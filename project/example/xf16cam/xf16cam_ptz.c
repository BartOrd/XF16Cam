#include <stdint.h>

#include "kernel/os/os.h"
#include "driver/chip/hal_gpio.h"

#include "xf16cam_ptz.h"

#define MOTOR_GPIO_PORT GPIO_PORT_B
#define PTZ_STEP_DELAY_MS 2U
#define HORIZONTAL_STEPS 50U
#define VERTICAL_STEPS 35U

volatile int g_ptz_ready;
volatile uint32_t g_last_ptz_time;

static const GPIO_Pin g_horizontal_motor_pins[4] = {
	GPIO_PIN_3, GPIO_PIN_5, GPIO_PIN_2, GPIO_PIN_6,
};

static const GPIO_Pin g_vertical_motor_pins[4] = {
	GPIO_PIN_14, GPIO_PIN_15, GPIO_PIN_4, GPIO_PIN_7,
};

static const uint8_t g_ptz_halfstep[8] = {
	0x09, 0x08, 0x0C, 0x04, 0x06, 0x02, 0x03, 0x01,
};

static unsigned int g_horizontal_step;
static unsigned int g_vertical_step;

__xip_text
void xf16cam_ptz_init(void)
{
	GPIO_InitParam output = {
		.mode = GPIOx_Pn_F1_OUTPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};
	unsigned int index;

	for (index = 0; index < 4; ++index) {
		HAL_GPIO_Init(MOTOR_GPIO_PORT, g_horizontal_motor_pins[index], &output);
		HAL_GPIO_Init(MOTOR_GPIO_PORT, g_vertical_motor_pins[index], &output);
		HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_horizontal_motor_pins[index], GPIO_PIN_LOW);
		HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_vertical_motor_pins[index], GPIO_PIN_LOW);
	}

	g_horizontal_step = 0;
	g_vertical_step = 0;
	g_ptz_ready = 1;
	g_last_ptz_time = OS_TicksToMSecs(OS_GetTicks());
}

__xip_text
void xf16cam_ptz_power_down(void)
{
	unsigned int index;

	if (!g_ptz_ready)
		return;

	for (index = 0; index < 4; ++index) {
		HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_horizontal_motor_pins[index], GPIO_PIN_LOW);
		HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_vertical_motor_pins[index], GPIO_PIN_LOW);
		HAL_GPIO_DeInit(MOTOR_GPIO_PORT, g_horizontal_motor_pins[index]);
		HAL_GPIO_DeInit(MOTOR_GPIO_PORT, g_vertical_motor_pins[index]);
	}

	g_ptz_ready = 0;
	g_last_ptz_time = 0;
}

__xip_text
static void ptz_write_phase(const GPIO_Pin *pins, unsigned int phase)
{
	unsigned int bit;

	for (bit = 0; bit < 4; ++bit) {
		HAL_GPIO_WritePin(MOTOR_GPIO_PORT, pins[bit],
		                  (g_ptz_halfstep[phase] & (1U << (3U - bit))) ?
		                  GPIO_PIN_HIGH : GPIO_PIN_LOW);
	}
}

__xip_text
static void ptz_move(const GPIO_Pin *pins, unsigned int *step,
                     unsigned int steps, int direction)
{
	unsigned int index;

	if (!g_ptz_ready)
		xf16cam_ptz_init();

	g_last_ptz_time = OS_TicksToMSecs(OS_GetTicks());

	for (index = 0; index < steps; ++index) {
		*step = direction > 0 ? (*step + 1U) % 8U : (*step + 7U) % 8U;
		ptz_write_phase(pins, *step);
		OS_MSleep(PTZ_STEP_DELAY_MS);
	}

	g_last_ptz_time = OS_TicksToMSecs(OS_GetTicks());
}

__xip_text
void ptz_move_left(void)
{
	ptz_move(g_horizontal_motor_pins, &g_horizontal_step,
	         HORIZONTAL_STEPS, -1);
}

__xip_text
void ptz_move_right(void)
{
	ptz_move(g_horizontal_motor_pins, &g_horizontal_step,
	         HORIZONTAL_STEPS, 1);
}

__xip_text
void ptz_move_up(void)
{
	ptz_move(g_vertical_motor_pins, &g_vertical_step,
	         VERTICAL_STEPS, 1);
}

__xip_text
void ptz_move_down(void)
{
	ptz_move(g_vertical_motor_pins, &g_vertical_step,
	         VERTICAL_STEPS, -1);
}
