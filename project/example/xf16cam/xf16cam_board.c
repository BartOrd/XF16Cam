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
#define XF16CAM_BUTTON_DEBOUNCE_MS   (100)
#define XF16CAM_RESET_HOLD_MS        (3000)
#define XF16CAM_BOARD_STACK_SIZE     (1024)

static OS_Thread_t g_board_thread;
static volatile int g_board_ready;
static volatile int g_board_sleeping;
static volatile int g_gpio_test_active;

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

static void xf16cam_board_reboot(void)
{
	if (xf16cam_update_begin() != 0) {
		printf("xf16cam reboot deferred: firmware update is active\n");
		return;
	}
	if (xf16cam_storage_unmount() != 0)
		printf("xf16cam board: SD eject failed before reboot\n");
	OS_MSleep(250);
	HAL_PRCM_SetCPUABootFlag(PRCM_CPUA_BOOT_FROM_COLD_RESET);
	HAL_WDG_Reboot();
}

static void xf16cam_board_task(void *arg)
{
	unsigned int mode_held_ms = 0;
	unsigned int reset_held_ms = 0;
	unsigned int blink_ms = 0;
	int mode_handled = 0;
	int reset_handled = 0;
	int led = 0;

	(void)arg;
	while (1) {
		int mode_pressed = xf16cam_board_mode_button_pressed();
		int reset_pressed = xf16cam_board_reset_button_pressed();
		int led_enabled = (xf16cam_config_get()->reserved & XF16CAM_CONFIG_LED_ENABLED) != 0;

		if (g_gpio_test_active) {
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}

		if (g_board_sleeping) {
			if (led) {
				led = 0;
				HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN, GPIO_PIN_LOW);
			}
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}
		if (!g_board_ready) {
			if (led_enabled) {
				blink_ms += XF16CAM_BUTTON_POLL_MS;
				if (blink_ms >= 250) {
					blink_ms = 0;
					led = !led;
					HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN,
					                  led ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
				}
			} else if (led) {
				led = 0;
				HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN, GPIO_PIN_LOW);
			}
		} else if (led != led_enabled) {
			led = led_enabled;
			HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_LED_PIN,
			                  led ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
		}
		if (!g_board_ready) {
			mode_held_ms = 0;
			reset_held_ms = 0;
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}

		if (mode_pressed) {
			mode_held_ms += XF16CAM_BUTTON_POLL_MS;
		} else {
			if (!mode_handled && mode_held_ms >= XF16CAM_BUTTON_DEBOUNCE_MS) {
				XF16CamMediaMode next = xf16cam_config_get()->media_mode == XF16CAM_MEDIA_WEB ?
				                          XF16CAM_MEDIA_RTSP : XF16CAM_MEDIA_WEB;
				printf("xf16cam PA15: switching media mode to %s\n",
				       next == XF16CAM_MEDIA_WEB ? "WEB" : "RTSP");
				if (xf16cam_config_save_media(next) == 0)
					xf16cam_board_reboot();
			}
			mode_held_ms = 0;
			mode_handled = 0;
		}

		if (reset_pressed) {
			reset_held_ms += XF16CAM_BUTTON_POLL_MS;
			if (!reset_handled && reset_held_ms >= XF16CAM_RESET_HOLD_MS) {
				reset_handled = 1;
				printf("xf16cam PA20: restoring setup AP\n");
				if (xf16cam_config_save_ap() == 0)
					xf16cam_board_reboot();
			}
		} else {
			reset_held_ms = 0;
			reset_handled = 0;
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

void xf16cam_board_gpio_test(int enabled)
{
	g_gpio_test_active = enabled != 0;
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
