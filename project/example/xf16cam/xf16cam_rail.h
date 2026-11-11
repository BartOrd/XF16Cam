#ifndef XF16CAM_RAIL_H
#define XF16CAM_RAIL_H

int xf16cam_rail_init(void);
int xf16cam_rail_acquire(void);
void xf16cam_rail_release(void);
void xf16cam_rail_restore(void);

#endif
