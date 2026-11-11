#include "driver/chip/hal_gpio.h"

#include "xf16cam_config.h"
#include "xf16cam_light.h"
#include "xf16cam_ptz.h"
#include "xf16cam_board.h"

#define XF16CAM_LIGHT_ENABLE_PIN  GPIO_PIN_20
#define XF16CAM_LIGHT_LEVEL_PIN   GPIO_PIN_3

void xf16cam_light_apply(void)
{
	const XF16CamConfig *config = xf16cam_config_get();
	GPIO_InitParam output = {
		.mode = GPIOx_Pn_F1_OUTPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};
	GPIO_InitParam input = {
		.mode = GPIOx_Pn_F0_INPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};
	GPIO_InitParam current;
	GPIO_PinState light_state = xf16cam_board_white_boost_active() ||
	                           (!XF16CAM_AUTO_DAY_NIGHT(config) && XF16CAM_LIGHT_ENABLED(config)) ?
	                           GPIO_PIN_HIGH : GPIO_PIN_LOW;

	HAL_GPIO_GetConfig(GPIO_PORT_B, XF16CAM_LIGHT_ENABLE_PIN, &current);
	if (current.mode != output.mode || current.driving != output.driving ||
	    current.pull != output.pull) {
		HAL_GPIO_WritePin(GPIO_PORT_B, XF16CAM_LIGHT_ENABLE_PIN, light_state);
		HAL_GPIO_Init(GPIO_PORT_B, XF16CAM_LIGHT_ENABLE_PIN, &output);
	}
	HAL_GPIO_WritePin(GPIO_PORT_B, XF16CAM_LIGHT_ENABLE_PIN, light_state);

	if (g_ptz_ready)
		return;

	HAL_GPIO_GetConfig(GPIO_PORT_B, XF16CAM_LIGHT_LEVEL_PIN, &current);
	if (XF16CAM_LIGHT_STRONGER(config)) {
		if (current.mode != output.mode || current.driving != output.driving ||
		    current.pull != output.pull) {
			HAL_GPIO_WritePin(GPIO_PORT_B, XF16CAM_LIGHT_LEVEL_PIN, GPIO_PIN_LOW);
			HAL_GPIO_Init(GPIO_PORT_B, XF16CAM_LIGHT_LEVEL_PIN, &output);
		}
		HAL_GPIO_WritePin(GPIO_PORT_B, XF16CAM_LIGHT_LEVEL_PIN, GPIO_PIN_LOW);
	} else if (current.mode != input.mode || current.driving != input.driving ||
	           current.pull != input.pull) {
		HAL_GPIO_Init(GPIO_PORT_B, XF16CAM_LIGHT_LEVEL_PIN, &input);
	}
}
