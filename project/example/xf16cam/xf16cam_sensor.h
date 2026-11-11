#ifndef XF16CAM_SENSOR_H
#define XF16CAM_SENSOR_H

#include <stdint.h>

#include "driver/component/csi_camera/camera_sensor.h"

HAL_Status xf16cam_sensor_init(SENSOR_ConfigParam *cfg);
void xf16cam_sensor_deinit(SENSOR_ConfigParam *cfg);
int xf16cam_sensor_configure_camera(uint16_t configured_width,
				    uint16_t configured_height);
void xf16cam_sensor_prepare_capture(void);
int xf16cam_sensor_switch_cam_sensor_mode(int night_mode);
const char *xf16cam_sensor_name(void);
int xf16cam_sensor_available(void);
uint16_t xf16cam_sensor_width(void);
uint16_t xf16cam_sensor_height(void);
int xf16cam_sensor_supports_vga(void);

#endif
