#include <stdint.h>
#include <stdio.h>

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
static void ptz_task(void *arg);
static OS_Thread_t g_ptz_thread;
static volatile int g_ptz_task_started;
static volatile int g_ptz_direction;
static int g_ptz_coils_active;
#define PTZ_THREAD_STACK_SIZE 1024U

__xip_text
void xf16cam_ptz_init(void)
{
	GPIO_InitParam output = {
		.mode = GPIOx_Pn_F1_OUTPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};
	unsigned int index;

	if (!g_ptz_ready) {
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

	if (!g_ptz_task_started) {
		if (OS_ThreadCreate(&g_ptz_thread, "xf16cam-ptz", ptz_task, NULL,
		                    OS_THREAD_PRIO_APP, PTZ_THREAD_STACK_SIZE) == OS_OK)
			g_ptz_task_started = 1;
		else
			printf("xf16cam PTZ: movement thread create failed\\n");
	}
}

__xip_text
void xf16cam_ptz_power_down(void)
{
	unsigned int index;

	g_ptz_direction = 0;
	if (!g_ptz_ready)
		return;

	for (index = 0; index < 4; ++index) {
		HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_horizontal_motor_pins[index], GPIO_PIN_LOW);
		HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_vertical_motor_pins[index], GPIO_PIN_LOW);
		HAL_GPIO_DeInit(MOTOR_GPIO_PORT, g_horizontal_motor_pins[index]);
		HAL_GPIO_DeInit(MOTOR_GPIO_PORT, g_vertical_motor_pins[index]);
	}

	g_ptz_ready = 0;
	g_ptz_coils_active = 0;
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


static void ptz_task(void *arg)
{
	(void)arg;
	while (1) {
		const GPIO_Pin *pins = NULL;
		unsigned int *step = NULL;
		int direction = g_ptz_direction;
		int rotation = 1;

		if (!g_ptz_ready || direction == 0) {
			if (g_ptz_ready && g_ptz_coils_active) {
				unsigned int index;
				for (index = 0; index < 4; ++index) {
					HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_horizontal_motor_pins[index], GPIO_PIN_LOW);
					HAL_GPIO_WritePin(MOTOR_GPIO_PORT, g_vertical_motor_pins[index], GPIO_PIN_LOW);
				}
				g_ptz_coils_active = 0;
			}
			OS_MSleep(4);
			continue;
		}

		switch (direction) {
		case XF16CAM_PTZ_LEFT:
			pins = g_horizontal_motor_pins;
			step = &g_horizontal_step;
			rotation = -1;
			break;
		case XF16CAM_PTZ_RIGHT:
			pins = g_horizontal_motor_pins;
			step = &g_horizontal_step;
			break;
		case XF16CAM_PTZ_UP:
			pins = g_vertical_motor_pins;
			step = &g_vertical_step;
			break;
		case XF16CAM_PTZ_DOWN:
			pins = g_vertical_motor_pins;
			step = &g_vertical_step;
			rotation = -1;
			break;
		default:
			g_ptz_direction = 0;
			continue;
		}

		if (g_ptz_direction != direction || !g_ptz_ready)
			continue;
		*step = rotation > 0 ? (*step + 1U) % 8U : (*step + 7U) % 8U;
		ptz_write_phase(pins, *step);
		g_ptz_coils_active = 1;
		g_last_ptz_time = OS_TicksToMSecs(OS_GetTicks());
		OS_MSleep(PTZ_STEP_DELAY_MS);
	}
}

__xip_text
static void ptz_move(const GPIO_Pin *pins, unsigned int *step,
                     unsigned int steps, int direction)
{
	unsigned int index;

	g_ptz_direction = 0;
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

int xf16cam_ptz_start(int direction)
{
	if (direction < XF16CAM_PTZ_LEFT || direction > XF16CAM_PTZ_DOWN)
		return -1;
	xf16cam_ptz_init();
	if (!g_ptz_task_started)
		return -1;
	g_ptz_direction = direction;
	g_last_ptz_time = OS_TicksToMSecs(OS_GetTicks());
	return 0;
}

void xf16cam_ptz_stop(void)
{
	g_ptz_direction = 0;
	if (g_ptz_ready)
		g_last_ptz_time = OS_TicksToMSecs(OS_GetTicks());
}
