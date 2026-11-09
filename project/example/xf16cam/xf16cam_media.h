#ifndef XF16CAM_MEDIA_H
#define XF16CAM_MEDIA_H

#include <stdint.h>

typedef struct {
	uint32_t frames;
	uint32_t largest_jpeg;
	uint32_t capture_errors;
	uint32_t jpeg_capacity;
} XF16CamMediaInfo;

int xf16cam_mjpeg_start(int fd);
int xf16cam_media_client_connected(int fd);
int xf16cam_media_quiesce_for_update(uint32_t timeout_ms);
const XF16CamMediaInfo *xf16cam_media_info(void);

#endif
