#ifndef XF16CAM_PTZ_H
#define XF16CAM_PTZ_H

#include <stdint.h>

void xf16cam_ptz_init(void);
void xf16cam_ptz_power_down(void);
void ptz_move_left(void);
void ptz_move_right(void);
void ptz_move_up(void);
void ptz_move_down(void);
int xf16cam_ptz_start(int direction);
void xf16cam_ptz_stop(void);

extern volatile int g_ptz_ready;
extern volatile uint32_t g_last_ptz_time;

#define PTZ_IDLE_TIMEOUT_MS 10000U
#define XF16CAM_PTZ_LEFT  1
#define XF16CAM_PTZ_RIGHT 2
#define XF16CAM_PTZ_UP    3
#define XF16CAM_PTZ_DOWN  4

#endif
