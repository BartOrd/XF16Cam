#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#include "xf16cam_motion.h"
#include "xf16cam_alarm.h"

#define XF16CAM_RESET_BUTTON_PORT GPIO_PORT_B
#define XF16CAM_RESET_BUTTON_PIN  GPIO_PIN_19
#define XF16CAM_BUTTON_POLL_MS    (50)
#define XF16CAM_BUTTON_DEBOUNCE_MS (300)
#define XF16CAM_RESET_HOLD_MS     (3000)
#define XF16CAM_BOARD_STACK_SIZE  (1024)
#define XF16CAM_IR_LED_PORT       GPIO_PORT_A
#define XF16CAM_IR_LED_PIN        GPIO_PIN_22
#define XF16CAM_IRCUT_DAY_PIN     GPIO_PIN_12
#define XF16CAM_IRCUT_NIGHT_PIN   GPIO_PIN_13
#define XF16CAM_IRCUT_PULSE_MS    (120U)
#define XF16CAM_MOTION_LIGHT_MS   (30000U)
#define XF16CAM_MOTION_EVENT_GAP_MS (2000U)
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
static int g_ircut_night = -1;
static volatile uint32_t g_motion_events;
static volatile uint32_t g_motion_last_event_ms;
static volatile uint32_t g_white_boost_until_ms;

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
static void xf16cam_board_set_ircut(int night)
{
	GPIO_PinState day_level = night ? GPIO_PIN_LOW : GPIO_PIN_HIGH;
	GPIO_PinState night_level = night ? GPIO_PIN_HIGH : GPIO_PIN_LOW;

	if (g_ircut_night == night)
		return;
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IRCUT_DAY_PIN, day_level);
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IRCUT_NIGHT_PIN, night_level);
	OS_MSleep(XF16CAM_IRCUT_PULSE_MS);
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IRCUT_DAY_PIN, GPIO_PIN_LOW);
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IRCUT_NIGHT_PIN, GPIO_PIN_LOW);
	g_ircut_night = night;
}

__xip_text
void xf16cam_board_set_ir_led(int on)
{
	on = on != 0;
	if (g_ir_led_on == on && g_ircut_night == on)
		return;
	HAL_GPIO_WritePin(XF16CAM_IR_LED_PORT, XF16CAM_IR_LED_PIN,
	                  on ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
	g_ir_led_on = on;
	xf16cam_sensor_switch_cam_sensor_mode(on);
	xf16cam_board_set_ircut(on);
	xf16cam_motion_reset();
}

int xf16cam_board_ir_led_on(void)
{
	return g_ir_led_on;
}

int xf16cam_board_ircut_night(void)
{
	return g_ircut_night > 0;
}

int xf16cam_board_night_mode(void)
{
	return g_cds_dark_state;
}

int xf16cam_board_white_boost_active(void)
{
	uint32_t now = OS_TicksToMSecs(OS_GetTicks());
	return g_white_boost_until_ms != 0U &&
	       (int32_t)(g_white_boost_until_ms - now) > 0;
}

void xf16cam_board_motion_detected(void)
{
	uint32_t now;
	const XF16CamConfig *config = xf16cam_config_get();

	if (!XF16CAM_MOTION_ENABLED(config) &&
	    !XF16CAM_MOTION_ALARM_ENABLED(config))
		return;
	now = OS_TicksToMSecs(OS_GetTicks());
	if (g_motion_events != 0U &&
	    (uint32_t)(now - g_motion_last_event_ms) < XF16CAM_MOTION_EVENT_GAP_MS)
		return;
	__sync_add_and_fetch(&g_motion_events, 1U);
	g_motion_last_event_ms = now;
	if (XF16CAM_MOTION_ENABLED(config) && g_cds_dark_state) {
		g_white_boost_until_ms = now + XF16CAM_MOTION_LIGHT_MS;
		xf16cam_board_set_ir_led(0);
		xf16cam_light_apply();
	}
	if (XF16CAM_MOTION_ALARM_ENABLED(config))
		xf16cam_alarm_trigger();
}

uint32_t xf16cam_board_motion_events(void)
{
	return g_motion_events;
}

uint32_t xf16cam_board_motion_age_ms(void)
{
	if (g_motion_events == 0U)
		return 0xffffffffUL;
	return OS_TicksToMSecs(OS_GetTicks()) - g_motion_last_event_ms;
}

int xf16cam_board_motion_active(void)
{
	return xf16cam_board_motion_age_ms() < 5000U;
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
	int previous_night = g_cds_dark_state;
	int auto_day_night = XF16CAM_AUTO_DAY_NIGHT(xf16cam_config_get());

	if (xf16cam_board_cds_read(&level) != 0)
		return;

	if (g_cds_dark_state) {
		if (level < XF16CAM_CDS_LIGHT_THRESHOLD)
			g_cds_dark_state = 0;
	} else if (level > XF16CAM_CDS_DARK_THRESHOLD) {
		g_cds_dark_state = 1;
	}

	if (previous_night && !g_cds_dark_state)
		g_white_boost_until_ms = 0;
	if (!auto_day_night) {
		if (!xf16cam_board_white_boost_active())
			xf16cam_board_set_ir_led(0);
		xf16cam_light_apply();
		return;
	}
	if (!xf16cam_board_white_boost_active())
		xf16cam_board_set_ir_led(g_cds_dark_state);
	xf16cam_light_apply();
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

		if (g_white_boost_until_ms != 0U) {
			uint32_t now = OS_TicksToMSecs(OS_GetTicks());
			if ((int32_t)(now - g_white_boost_until_ms) >= 0) {
				g_white_boost_until_ms = 0;
				if (XF16CAM_AUTO_DAY_NIGHT(xf16cam_config_get()) && g_cds_dark_state)
					xf16cam_board_set_ir_led(1);
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
			cds_elapsed_ms = XF16CAM_CDS_CHECK_MS - XF16CAM_BUTTON_POLL_MS;
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
	HAL_GPIO_Init(GPIO_PORT_A, XF16CAM_IRCUT_DAY_PIN, &output);
	HAL_GPIO_Init(GPIO_PORT_A, XF16CAM_IRCUT_NIGHT_PIN, &output);
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IRCUT_DAY_PIN, GPIO_PIN_LOW);
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IRCUT_NIGHT_PIN, GPIO_PIN_LOW);
	g_ircut_night = -1;
	xf16cam_board_set_ircut(0);
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
	g_white_boost_until_ms = 0;
	if (g_ptz_ready)
		xf16cam_ptz_power_down();
	xf16cam_board_set_ir_led(0);
}

__xip_text
uint32_t xf16cam_board_stack_min_free(void)
{
	return OS_ThreadGetStackMinFreeSize(&g_board_thread);
}
