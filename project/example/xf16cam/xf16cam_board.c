#include <stdint.h>
#include <stdio.h>

#include "kernel/os/os.h"
#include "driver/chip/hal_adc.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_wdg.h"

#include "xf16cam_board.h"
#include "xf16cam_config.h"
#include "xf16cam_light.h"
#include "xf16cam_ptz.h"
#include "xf16cam_storage.h"
#include "xf16cam_sensor.h"

#define XF16CAM_RESET_BUTTON_PORT GPIO_PORT_B
#define XF16CAM_RESET_BUTTON_PIN  GPIO_PIN_19
#define XF16CAM_BUTTON_POLL_MS    (50)
#define XF16CAM_BUTTON_DEBOUNCE_MS (300)
#define XF16CAM_RESET_HOLD_MS     (3000)
#define XF16CAM_BOARD_STACK_SIZE  (1024)
#define XF16CAM_IR_LED_PORT       GPIO_PORT_A
#define XF16CAM_IR_LED_PIN        GPIO_PIN_22
#define XF16CAM_CDS_CHANNEL       ADC_CHANNEL_5
#define XF16CAM_CDS_SAMPLES       (10U)
#define XF16CAM_CDS_DARK_THRESHOLD (1500U)
#define XF16CAM_CDS_LIGHT_THRESHOLD (1300U)
#define XF16CAM_CDS_CHECK_MS      (5000U)

static OS_Thread_t g_board_thread;
static volatile int g_board_ready;
static volatile int g_board_sleeping;
static int g_ir_led_on;
static int g_cds_adc_ready;
static int g_cds_dark_state;

static int xf16cam_button_pressed(GPIO_Pin pin)
{
	return HAL_GPIO_ReadPin(XF16CAM_RESET_BUTTON_PORT, pin) == GPIO_PIN_LOW;
}

int xf16cam_board_mode_button_pressed(void)
{
	return 0;
}

int xf16cam_board_reset_button_pressed(void)
{
	return xf16cam_button_pressed(XF16CAM_RESET_BUTTON_PIN);
}

typedef struct {
	int candidate;
	int stable;
	unsigned int candidate_ms;
} XF16CamButtonState;

static int xf16cam_button_update(XF16CamButtonState *state, int pressed)
{
	if (pressed != state->candidate) {
		state->candidate = pressed;
		state->candidate_ms = 0;
		return 0;
	}
	if (state->candidate_ms < XF16CAM_BUTTON_DEBOUNCE_MS)
		state->candidate_ms += XF16CAM_BUTTON_POLL_MS;
	if (state->candidate_ms >= XF16CAM_BUTTON_DEBOUNCE_MS &&
	    state->stable != state->candidate) {
		state->stable = state->candidate;
		return 1;
	}
	return 0;
}

static void xf16cam_board_reboot_now(void)
{
	if (xf16cam_storage_unmount() != 0)
		printf("xf16cam board: SD eject failed before reboot\n");
	OS_MSleep(250);
	HAL_PRCM_SetCPUABootFlag(PRCM_CPUA_BOOT_FROM_COLD_RESET);
	HAL_WDG_Reboot();
}

static void xf16cam_board_reboot(void)
{
	if (xf16cam_update_begin() != 0) {
		printf("xf16cam reboot deferred: firmware update is active\n");
		return;
	}
	xf16cam_board_reboot_now();
}


__xip_text
void xf16cam_board_set_ir_led(int on)
{
	on = on != 0;
	if (g_ir_led_on == on)
		return;
	HAL_GPIO_WritePin(XF16CAM_IR_LED_PORT, XF16CAM_IR_LED_PIN,
	                  on ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
	g_ir_led_on = on;
	xf16cam_sensor_switch_cam_sensor_mode(on);
}

int xf16cam_board_ir_led_on(void)
{
	return g_ir_led_on;
}

__xip_text
static int xf16cam_board_cds_read(uint16_t *average)
{
	ADC_InitParam param;
	uint16_t samples[XF16CAM_CDS_SAMPLES];
	uint32_t sample;
	uint32_t total = 0;
	unsigned int index;

	if (!g_cds_adc_ready) {
		memset(&param, 0, sizeof(param));
		param.delay = 10;
		param.freq = 500000;
		param.vref_mode = 1;
		param.mode = ADC_CONTI_CONV;
		if (HAL_ADC_Init(&param) != HAL_OK)
			return -1;
		g_cds_adc_ready = 1;
	}

	for (index = 0; index < XF16CAM_CDS_SAMPLES; ++index) {
		if (HAL_ADC_Conv_Polling(XF16CAM_CDS_CHANNEL, &sample, 100) != HAL_OK)
			return -1;
		samples[index] = (uint16_t)(sample & 0x0fffU);
	}

	for (index = 1; index < XF16CAM_CDS_SAMPLES; ++index) {
		uint16_t value = samples[index];
		unsigned int sorted = index;

		while (sorted > 0 && samples[sorted - 1] > value) {
			samples[sorted] = samples[sorted - 1];
			--sorted;
		}
		samples[sorted] = value;
	}

	for (index = 1; index < XF16CAM_CDS_SAMPLES - 1; ++index)
		total += samples[index];

	*average = (uint16_t)(total / (XF16CAM_CDS_SAMPLES - 2U));
	return 0;
}

__xip_text
static void xf16cam_board_daynight_poll(void)
{
	uint16_t level;

	if (!XF16CAM_AUTO_DAY_NIGHT(xf16cam_config_get())) {
		g_cds_dark_state = 0;
		xf16cam_board_set_ir_led(0);
		return;
	}

	if (xf16cam_board_cds_read(&level) != 0)
		return;

	if (g_cds_dark_state) {
		if (level < XF16CAM_CDS_LIGHT_THRESHOLD)
			g_cds_dark_state = 0;
	} else if (level > XF16CAM_CDS_DARK_THRESHOLD) {
		g_cds_dark_state = 1;
	}

	xf16cam_board_set_ir_led(g_cds_dark_state);
}

static void xf16cam_board_task(void *arg)
{
	XF16CamButtonState reset_button = {0};
	unsigned int reset_held_ms = 0;
	int reset_armed = 0;
	int reset_handled = 0;
	int buttons_initialized = 0;
	unsigned int cds_elapsed_ms = 0;

	(void)arg;
	while (1) {
		int reset_pressed = xf16cam_board_reset_button_pressed();

		if (g_ptz_ready) {
			uint32_t now = OS_TicksToMSecs(OS_GetTicks());
			if ((uint32_t)(now - g_last_ptz_time) >= PTZ_IDLE_TIMEOUT_MS) {
				xf16cam_ptz_power_down();
				xf16cam_light_apply();
			}
		}

		if (g_board_sleeping || !g_board_ready) {
			buttons_initialized = 0;
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}

		cds_elapsed_ms += XF16CAM_BUTTON_POLL_MS;
		if (cds_elapsed_ms >= XF16CAM_CDS_CHECK_MS) {
			cds_elapsed_ms = 0;
			xf16cam_board_daynight_poll();
		}

		if (!buttons_initialized) {
			reset_button.candidate = reset_pressed;
			reset_button.stable = reset_pressed;
			reset_button.candidate_ms = 0;
			reset_armed = !reset_pressed;
			reset_held_ms = 0;
			reset_handled = 0;
			buttons_initialized = 1;
		} else {
			int reset_changed = xf16cam_button_update(&reset_button, reset_pressed);

			if (reset_changed && !reset_button.stable) {
				reset_armed = 1;
				reset_held_ms = 0;
				reset_handled = 0;
			}
			if (reset_button.stable && reset_armed) {
				reset_held_ms += XF16CAM_BUTTON_POLL_MS;
				if (!reset_handled && reset_held_ms >= XF16CAM_RESET_HOLD_MS) {
					reset_handled = 1;
					reset_armed = 0;
					printf("xf16cam PB19: restoring setup AP\n");
					if (xf16cam_config_save_ap() == 0)
						xf16cam_board_reboot();
				}
			}
		}

		OS_MSleep(XF16CAM_BUTTON_POLL_MS);
	}
}

int xf16cam_board_init(void)
{
	GPIO_InitParam input = {
		.mode = GPIOx_Pn_F0_INPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_UP,
	};

	GPIO_InitParam output = {
		.mode = GPIOx_Pn_F1_OUTPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};

	HAL_GPIO_Init(XF16CAM_IR_LED_PORT, XF16CAM_IR_LED_PIN, &output);
	HAL_GPIO_WritePin(XF16CAM_IR_LED_PORT, XF16CAM_IR_LED_PIN, GPIO_PIN_LOW);
	HAL_GPIO_Init(XF16CAM_RESET_BUTTON_PORT, XF16CAM_RESET_BUTTON_PIN, &input);
	printf("xf16cam board: PB19 reset=%s; PTZ motors initialise on first movement\n",
	       xf16cam_board_reset_button_pressed() ? "pressed" : "released");

	if (OS_ThreadCreate(&g_board_thread, "xf16cam-board", xf16cam_board_task,
	                    NULL, OS_THREAD_PRIO_APP, XF16CAM_BOARD_STACK_SIZE) != OS_OK) {
		printf("xf16cam board thread create failed\n");
		return -1;
	}
	return 0;
}

void xf16cam_board_set_ready(void)
{
	g_board_ready = 1;
}

void xf16cam_board_prepare_sleep(void)
{
	g_board_sleeping = 1;
	g_board_ready = 0;
	if (g_ptz_ready)
		xf16cam_ptz_power_down();
	xf16cam_board_set_ir_led(0);
}

__xip_text
uint32_t xf16cam_board_stack_min_free(void)
{
	return OS_ThreadGetStackMinFreeSize(&g_board_thread);
}
