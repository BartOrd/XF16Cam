#ifndef XF16CAM_PTZ_H
#define XF16CAM_PTZ_H

#include <stdint.h>

void xf16cam_ptz_init(void);
void xf16cam_ptz_power_down(void);
void ptz_move_left(void);
void ptz_move_right(void);
void ptz_move_up(void);
void ptz_move_down(void);

extern volatile int g_ptz_ready;
extern volatile uint32_t g_last_ptz_time;

#define PTZ_IDLE_TIMEOUT_MS 10000U

#endif
