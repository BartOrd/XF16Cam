#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/manager/audio_manager.h"
#include "audio/pcm/audio_pcm.h"
#include "kernel/os/os.h"

#include "xf16cam_alarm.h"
#include "xf16cam_rail.h"

#define XF16CAM_ALARM_RATE          (16000U)
#define XF16CAM_ALARM_BLOCK_SAMPLES (160U)
#define XF16CAM_ALARM_STACK_SIZE    (1536U)

static OS_Thread_t g_alarm_thread;
static OS_Mutex_t g_alarm_lock;
static volatile int g_alarm_initialized;
static volatile int g_alarm_camera_active;
static volatile int g_alarm_pending;
static int g_alarm_output_open;
static int g_alarm_output_attempted;

static int xf16cam_alarm_output_open(void)
{
	struct pcm_config config;

	if (g_alarm_output_open)
		return 0;
	if (g_alarm_output_attempted)
		return -1;
	g_alarm_output_attempted = 1;
	memset(&config, 0, sizeof(config));
	config.channels = 1;
	config.format = PCM_FORMAT_S16_LE;
	config.period_count = 2;
	config.period_size = XF16CAM_ALARM_BLOCK_SAMPLES;
	config.rate = XF16CAM_ALARM_RATE;
	if (snd_pcm_open(AUDIO_SND_CARD_DEFAULT, PCM_OUT, &config) != 0) {
		printf("xf16cam alarm: speaker output open failed\n");
		return -1;
	}
	g_alarm_output_open = 1;
	audio_manager_handler(AUDIO_SND_CARD_DEFAULT, AUDIO_MANAGER_SET_VOLUME_LEVEL,
	                      AUDIO_OUT_DEV_SPK, VOLUME_LEVEL8);
	return 0;
}

static int xf16cam_alarm_write_block(const int16_t *samples)
{
	return snd_pcm_write(AUDIO_SND_CARD_DEFAULT, (void *)samples,
	                     XF16CAM_ALARM_BLOCK_SAMPLES * sizeof(samples[0])) ==
	       (int)(XF16CAM_ALARM_BLOCK_SAMPLES * sizeof(samples[0])) ? 0 : -1;
}

static void xf16cam_alarm_play_tone(void)
{
	static const int16_t wave[20] = {
		0, 1854, 3526, 4854, 5706, 6000, 5706, 4854, 3526, 1854,
		0, -1854, -3526, -4854, -5706, -6000, -5706, -4854, -3526, -1854
	};
	int16_t samples[XF16CAM_ALARM_BLOCK_SAMPLES];
	unsigned int block;
	unsigned int sample;
	unsigned int phase = 0;
	unsigned int beep;

	for (beep = 0; beep < 2U; ++beep) {
		for (block = 0; block < 18U; ++block) {
			for (sample = 0; sample < XF16CAM_ALARM_BLOCK_SAMPLES; ++sample) {
				samples[sample] = wave[phase];
				if (++phase == 20U)
					phase = 0;
			}
			if (xf16cam_alarm_write_block(samples) != 0)
				goto flush;
		}
		if (beep == 0U) {
			memset(samples, 0, sizeof(samples));
			for (block = 0; block < 10U; ++block) {
				if (xf16cam_alarm_write_block(samples) != 0)
					goto flush;
			}
		}
	}

flush:
	snd_pcm_flush(AUDIO_SND_CARD_DEFAULT);
}

static void xf16cam_alarm_task(void *arg)
{
	(void)arg;
	while (1) {
		if (g_alarm_pending && g_alarm_camera_active) {
			if (OS_MutexLock(&g_alarm_lock, OS_WAIT_FOREVER) == OS_OK) {
				if (g_alarm_pending && g_alarm_camera_active) {
					g_alarm_pending = 0;
					if (xf16cam_alarm_output_open() == 0)
						xf16cam_alarm_play_tone();
				}
				OS_MutexUnlock(&g_alarm_lock);
			}
		}
		OS_MSleep(10);
	}
}

int xf16cam_alarm_init(void)
{
	if (g_alarm_initialized)
		return 0;
	if (OS_MutexCreate(&g_alarm_lock) != OS_OK)
		return -1;
	if (OS_ThreadCreate(&g_alarm_thread, "xf16cam-alarm",
	                    xf16cam_alarm_task, NULL, OS_THREAD_PRIO_APP,
	                    XF16CAM_ALARM_STACK_SIZE) != OS_OK) {
		OS_MutexDelete(&g_alarm_lock);
		return -1;
	}
	g_alarm_initialized = 1;
	return 0;
}

void xf16cam_alarm_camera_start(void)
{
	if (!g_alarm_initialized ||
	    OS_MutexLock(&g_alarm_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	if (!g_alarm_camera_active) {
		g_alarm_camera_active = 1;
		g_alarm_output_attempted = 0;
	}
	OS_MutexUnlock(&g_alarm_lock);
}

void xf16cam_alarm_camera_stop(void)
{
	if (!g_alarm_initialized ||
	    OS_MutexLock(&g_alarm_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	g_alarm_camera_active = 0;
	g_alarm_pending = 0;
	OS_MutexUnlock(&g_alarm_lock);
}

void xf16cam_alarm_camera_close(void)
{
	if (!g_alarm_initialized ||
	    OS_MutexLock(&g_alarm_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	g_alarm_pending = 0;
	if (g_alarm_output_open) {
		snd_pcm_flush(AUDIO_SND_CARD_DEFAULT);
		snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_OUT);
		g_alarm_output_open = 0;
		xf16cam_rail_restore();
	}
	g_alarm_output_attempted = 0;
	OS_MutexUnlock(&g_alarm_lock);
}

void xf16cam_alarm_trigger(void)
{
	if (g_alarm_camera_active)
		__sync_lock_test_and_set(&g_alarm_pending, 1);
}
