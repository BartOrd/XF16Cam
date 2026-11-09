#include <stdio.h>

#include "kernel/os/os.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_wdg.h"

#include "xf16cam_board.h"
#include "xf16cam_config.h"
#include "xf16cam_storage.h"

#define XF16CAM_LED_PIN              GPIO_PIN_21
#define XF16CAM_MODE_BUTTON_PIN      GPIO_PIN_15
#define XF16CAM_RESET_BUTTON_PIN     GPIO_PIN_20
#define XF16CAM_BUTTON_POLL_MS       (50)
#define XF16CAM_BUTTON_DEBOUNCE_MS   (300)
#define XF16CAM_RESET_HOLD_MS        (3000)
#define XF16CAM_BOARD_STACK_SIZE     (1024)

static OS_Thread_t g_board_thread;
static volatile int g_board_ready;
static volatile int g_board_sleeping;

static int xf16cam_button_pressed(GPIO_Pin pin)
{
	return HAL_GPIO_ReadPin(GPIO_PORT_A, pin) == GPIO_PIN_LOW;
}

int xf16cam_board_mode_button_pressed(void)
{
	return xf16cam_button_pressed(XF16CAM_MODE_BUTTON_PIN);
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
	if (state->candidate_ms < XF16CAM_BUTTON_DEBOUNCE_MS) {
		state->candidate_ms += XF16CAM_BUTTON_POLL_MS;
	}
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
		printf("xf16cam board: SD eject failed before reboot\\n");
	OS_MSleep(250);
	HAL_PRCM_SetCPUABootFlag(PRCM_CPUA_BOOT_FROM_COLD_RESET);
	HAL_WDG_Reboot();
}

static void xf16cam_board_reboot(void)
{
	if (xf16cam_update_begin() != 0) {
		printf("xf16cam reboot deferred: firmware update is active\\n");
		return;
	}
	xf16cam_board_reboot_now();
}

static void xf16cam_board_task(void *arg)
{
	XF16CamButtonState mode_button = {0};
	XF16CamButtonState reset_button = {0};
	unsigned int reset_held_ms = 0;
	unsigned int blink_ms = 0;
	int mode_armed = 0;
	int reset_armed = 0;
	int reset_handled = 0;
	int buttons_initialized = 0;
	int led = 0;

	(void)arg;
	while (1) {
		int mode_pressed = xf16cam_board_mode_button_pressed();
		int reset_pressed = xf16cam_board_reset_button_pressed();

		if (g_board_sleeping) {
			if (led) {
				led = 0;
				HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN, GPIO_PIN_LOW);
			}
			buttons_initialized = 0;
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}
		if (!g_board_ready) {
			blink_ms += XF16CAM_BUTTON_POLL_MS;
			if (blink_ms >= 250) {
				blink_ms = 0;
				led = !led;
				HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN,
				                  led ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
			}
			buttons_initialized = 0;
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}
		if (!led) {
			led = 1;
			HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN, GPIO_PIN_HIGH);
		}

		if (!buttons_initialized) {
			mode_button.candidate = mode_pressed;
			mode_button.stable = mode_pressed;
			mode_button.candidate_ms = 0;
			reset_button.candidate = reset_pressed;
			reset_button.stable = reset_pressed;
			reset_button.candidate_ms = 0;
			mode_armed = !mode_pressed;
			reset_armed = !reset_pressed;
			reset_held_ms = 0;
			reset_handled = 0;
			buttons_initialized = 1;
		} else {
			int mode_changed = xf16cam_button_update(&mode_button, mode_pressed);
			int reset_changed = xf16cam_button_update(&reset_button, reset_pressed);

			if (mode_changed) {
				if (!mode_button.stable) {
					mode_armed = 1;
				} else if (mode_armed) {
					XF16CamMediaMode next =
						xf16cam_config_get()->media_mode == XF16CAM_MEDIA_WEB ?
						XF16CAM_MEDIA_RTSP : XF16CAM_MEDIA_WEB;

					mode_armed = 0;
					printf("xf16cam PA15: stable press, switching media mode to %s\\n",
					       next == XF16CAM_MEDIA_WEB ? "WEB" : "RTSP");
					if (xf16cam_config_save_media_and_begin_update(next) == 0)
						xf16cam_board_reboot_now();
					else
						printf("xf16cam PA15: mode switch ignored (busy or flash write failed)\\n");
				}
			}

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
					printf("xf16cam PA20: restoring setup AP\\n");
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

	HAL_GPIO_Init(GPIO_PORT_A, XF16CAM_MODE_BUTTON_PIN, &input);
	HAL_GPIO_Init(GPIO_PORT_A, XF16CAM_RESET_BUTTON_PIN, &input);
	HAL_GPIO_Init(GPIO_PORT_A, XF16CAM_LED_PIN, &output);
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN, GPIO_PIN_LOW);
	printf("xf16cam board: PA15 mode=%s PA20 reset=%s PA21 status LED\n",
	       xf16cam_board_mode_button_pressed() ? "pressed" : "released",
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
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN, GPIO_PIN_LOW);
}

__xip_text
uint32_t xf16cam_board_stack_min_free(void)
{
	return OS_ThreadGetStackMinFreeSize(&g_board_thread);
}
