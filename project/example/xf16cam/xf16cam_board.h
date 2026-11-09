#ifndef XF16CAM_BOARD_H
#define XF16CAM_BOARD_H

#include <stdint.h>

int xf16cam_board_init(void);
void xf16cam_board_set_ready(void);
void xf16cam_board_prepare_sleep(void);
int xf16cam_board_mode_button_pressed(void);
int xf16cam_board_reset_button_pressed(void);
uint32_t xf16cam_board_stack_min_free(void);

#endif
