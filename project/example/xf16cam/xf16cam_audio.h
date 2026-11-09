#ifndef XF16CAM_AUDIO_H
#define XF16CAM_AUDIO_H

#include <stdint.h>

#define XF16CAM_AUDIO_SAMPLES_PER_PACKET (160)

typedef struct {
	uint32_t packets;
	uint32_t read_errors;
	uint16_t peak;
	uint16_t mean;
	uint8_t available;
	uint8_t active;
} XF16CamAudioInfo;

int xf16cam_audio_start(void);
int xf16cam_audio_acquire(void);
void xf16cam_audio_release(void);
int xf16cam_audio_http_start(int fd);
int xf16cam_audio_update_ready(void);
const XF16CamAudioInfo *xf16cam_audio_info(void);
uint32_t xf16cam_audio_cursor(void);
int xf16cam_audio_read(uint32_t *cursor, uint8_t *pcmu, uint32_t *timestamp);
uint32_t xf16cam_audio_stack_min_free(void);

#endif
