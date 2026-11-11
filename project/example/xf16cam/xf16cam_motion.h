#ifndef XF16CAM_MOTION_H
#define XF16CAM_MOTION_H

#include <stdint.h>

void xf16cam_motion_reset(void);
int xf16cam_motion_process_jpeg(const uint8_t *jpeg, uint32_t length, uint8_t sensitivity);

#endif
