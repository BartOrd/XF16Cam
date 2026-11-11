#include <stdint.h>
#include <stdio.h>

#include "compiler.h"
#include "kernel/os/os.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"

#include "xf16cam_rail.h"

#define XF16CAM_CAMERA_CTRL_PORT    GPIO_PORT_A
#define XF16CAM_CAMERA_CTRL_PIN     GPIO_PIN_14
#define XF16CAM_MEDIA_RAIL_PORT     GPIO_PORT_A
#define XF16CAM_MEDIA_RAIL_PIN      GPIO_PIN_23
#define XF16CAM_MEDIA_RAIL_PULSE_MS (100U)
#define XF16CAM_MEDIA_RAIL_SETTLE_MS (100U)

static OS_Mutex_t g_rail_lock;
static int g_rail_lock_ready;
static uint32_t g_rail_users;

int xf16cam_rail_init(void)
{
	if (g_rail_lock_ready)
		return 0;
	if (OS_MutexCreate(&g_rail_lock) != OS_OK)
		return -1;
	g_rail_lock_ready = 1;
	return 0;
}

__xip_text
int xf16cam_rail_acquire(void)
{
	GPIO_InitParam output = {
		.mode = GPIOx_Pn_F1_OUTPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};

	if (!g_rail_lock_ready ||
	    OS_MutexLock(&g_rail_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (g_rail_users == 0) {
		/* PA23 supplies both the sensor and SD socket. Keep the sensor in
		 * power-down when a storage-only operation raises the shared rail. */
		HAL_PRCM_SelectEXTLDOVolt(PRCM_EXT_LDO_3V3);
		HAL_PRCM_SetEXTLDOMode(PRCM_EXTLDO_ALWAYS_ON);
		HAL_PRCM_SetTOPLDOVoltage(PRCM_TOPLDO_VOLT_2V8);
		HAL_GPIO_Init(XF16CAM_CAMERA_CTRL_PORT, XF16CAM_CAMERA_CTRL_PIN,
		              &output);
		HAL_GPIO_WritePin(XF16CAM_CAMERA_CTRL_PORT, XF16CAM_CAMERA_CTRL_PIN,
		                  GPIO_PIN_HIGH);
		HAL_GPIO_Init(XF16CAM_MEDIA_RAIL_PORT, XF16CAM_MEDIA_RAIL_PIN, &output);
		HAL_GPIO_WritePin(XF16CAM_MEDIA_RAIL_PORT, XF16CAM_MEDIA_RAIL_PIN,
		                  GPIO_PIN_LOW);
		OS_MSleep(XF16CAM_MEDIA_RAIL_PULSE_MS);
		HAL_GPIO_WritePin(XF16CAM_MEDIA_RAIL_PORT, XF16CAM_MEDIA_RAIL_PIN,
		                  GPIO_PIN_HIGH);
		OS_MSleep(XF16CAM_MEDIA_RAIL_SETTLE_MS);
		printf("xf16cam media rail: PA23 on\n");
	}
	++g_rail_users;
	OS_MutexUnlock(&g_rail_lock);
	return 0;
}

__xip_text
void xf16cam_rail_release(void)
{
	if (!g_rail_lock_ready ||
	    OS_MutexLock(&g_rail_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	if (g_rail_users > 0)
		--g_rail_users;
	if (g_rail_users == 0) {
		HAL_GPIO_WritePin(XF16CAM_MEDIA_RAIL_PORT, XF16CAM_MEDIA_RAIL_PIN,
		                  GPIO_PIN_LOW);
		printf("xf16cam media rail: PA23 off\n");
	}
	OS_MutexUnlock(&g_rail_lock);
}

__xip_text
void xf16cam_rail_restore(void)
{
	GPIO_InitParam output = {
		.mode = GPIOx_Pn_F1_OUTPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};

	if (!g_rail_lock_ready ||
	    OS_MutexLock(&g_rail_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	if (g_rail_users > 0) {
		HAL_GPIO_Init(XF16CAM_MEDIA_RAIL_PORT, XF16CAM_MEDIA_RAIL_PIN, &output);
		HAL_GPIO_WritePin(XF16CAM_MEDIA_RAIL_PORT, XF16CAM_MEDIA_RAIL_PIN,
		                  GPIO_PIN_HIGH);
	}
	OS_MutexUnlock(&g_rail_lock);
}
