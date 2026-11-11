#ifndef XF16CAM_ALARM_H
#define XF16CAM_ALARM_H

int xf16cam_alarm_init(void);
void xf16cam_alarm_camera_start(void);
void xf16cam_alarm_camera_stop(void);
void xf16cam_alarm_camera_close(void);
void xf16cam_alarm_trigger(void);

#endif
