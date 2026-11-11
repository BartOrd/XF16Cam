#include <stdint.h>
#include <stdio.h>

#include "xf16cam_motion.h"

int main(void)
{
	static const uint8_t invalid_jpeg[] = { 0x00, 0x01, 0x02, 0x03 };

	xf16cam_motion_reset();
	if (xf16cam_motion_process_jpeg(NULL, 0U, 55U) != 0 ||
	    xf16cam_motion_process_jpeg(invalid_jpeg, sizeof(invalid_jpeg), 55U) != 0) {
		fputs("motion detector accepted an invalid JPEG\n", stderr);
		return 1;
	}
	puts("JPEG motion detector input tests passed");
	return 0;
}
