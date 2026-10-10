/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ff.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "kit_io.h"
#include "kit_logic.h"

#define MIC_SAMPLES 320U
#define MIC_BYTES (MIC_SAMPLES * sizeof(int16_t))
#define TX_FRAMES 160U
#define TX_BYTES (TX_FRAMES * 2U * sizeof(int16_t))
#define TX_SLAB_BLOCKS 6
#define SD_ROOT "/KITSD:/NU40KIT"

K_MEM_SLAB_DEFINE_STATIC(mic_slab, MIC_BYTES, 8, 4);
K_MEM_SLAB_DEFINE_STATIC(tx_slab, TX_BYTES, TX_SLAB_BLOCKS, 4);
K_MUTEX_DEFINE(state_lock);
K_SEM_DEFINE(record_done, 0, 1);
K_MSGQ_DEFINE(actions, sizeof(enum kit_action), 1, 4);

static const struct device *const mic = DEVICE_DT_GET(DT_NODELABEL(pdm0));
static const struct device *const speaker = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const strip = DEVICE_DT_GET(DT_NODELABEL(kit_strip));
static const struct device *const sd = DEVICE_DT_GET(DT_NODELABEL(kit_sdhc));
static const struct gpio_dt_spec keys[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key0), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key1), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key2), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key3), gpios),
};
static const struct gpio_dt_spec mic_sel = GPIO_DT_SPEC_GET(DT_NODELABEL(kit_mic_sel), gpios);
static const struct gpio_dt_spec amp = GPIO_DT_SPEC_GET(DT_NODELABEL(kit_amp), gpios);
static struct kit_status state;
static struct kit_buttons buttons;
static int16_t recording_pcm[KIT_RECORD_SAMPLES] __aligned(4);
static bool recording, link_connected;
static uint32_t next_file;
static FATFS fat_fs;
static struct fs_mount_t mount = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/KITSD:",
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};

void kit_snapshot(struct kit_status *out)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	*out = state;
	k_mutex_unlock(&state_lock);
}

void kit_set_link(bool connected)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	link_connected = connected;
	k_mutex_unlock(&state_lock);
}

static int mount_card(void)
{
	struct fs_dirent info;
	struct kit_status s;
	int ret;

	kit_snapshot(&s);
	if (s.sd_mounted) {
		return 0;
	}
	ret = device_is_ready(sd) ? sdhc_card_present(sd) : -ENODEV;
	if (ret <= 0) {
		return ret < 0 ? ret : -ENOMEDIUM;
	}
	ret = fs_mount(&mount);
	if (ret != 0) {
		return ret;
	}
	ret = fs_stat(SD_ROOT, &info);
	if (ret == -ENOENT) {
		ret = fs_mkdir(SD_ROOT);
	} else if (ret == 0 && info.type != FS_DIR_ENTRY_DIR) {
		ret = -ENOTDIR;
	}
	if (ret != 0) {
		(void)fs_unmount(&mount);
		return ret;
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	state.sd_mounted = true;
	k_mutex_unlock(&state_lock);
	return 0;
}

/* No format, truncation, deletion or replacement of an existing card file. */
static int unused_record_path(char path[40])
{
	struct fs_dirent info;

	while (next_file < 100000) {
		snprintk(path, 40, SD_ROOT "/R%05u.WAV", next_file++);
		int ret = fs_stat(path, &info);

		if (ret == -ENOENT) {
			return 0;
		}
		if (ret != 0) {
			return ret;
		}
	}
	return -ENOSPC;
}

static int verify_record(const char *path, uint32_t samples)
{
	struct fs_file_t file;
	uint8_t data[MIC_BYTES], header[KIT_WAV_HEADER_BYTES];
	int ret, closed;
	size_t offset = 0, bytes = samples * sizeof(int16_t);

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret != 0) {
		return ret;
	}
	ssize_t read = fs_read(&file, header, sizeof(header));

	ret = read < 0 ? read :
	      kit_wav_valid(header, (size_t)read, samples) ? 0 : -EBADMSG;
	while (ret == 0 && offset < bytes) {
		size_t count = MIN(sizeof(data), bytes - offset);

		read = fs_read(&file, data, count);
		if (read != count) {
			ret = read < 0 ? read : -EIO;
		} else if (memcmp(data, (uint8_t *)recording_pcm + offset, count)) {
			ret = -EBADMSG;
		}
		offset += count;
	}
	if (ret == 0) {
		read = fs_read(&file, data, 1);
		ret = read < 0 ? read : read == 0 ? 0 : -EBADMSG;
	}
	closed = fs_close(&file);
	return ret != 0 ? ret : closed;
}

static int save_record(uint32_t samples)
{
	struct fs_file_t file;
	uint8_t header[KIT_WAV_HEADER_BYTES];
	char path[40];
	int ret = mount_card(), closed;

	if (ret == 0) {
		ret = unused_record_path(path);
	}
	if (ret != 0) {
		return ret;
	}
	if (!kit_wav_header(header, samples)) {
		return -EINVAL;
	}
	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret != 0) {
		return ret;
	}
	ssize_t written = fs_write(&file, header, sizeof(header));

	ret = written < 0 ? written : written == sizeof(header) ? 0 : -ENOSPC;
	if (ret == 0) {
		written = fs_write(&file, recording_pcm, samples * sizeof(int16_t));
		ret = written < 0 ? written : written == samples * sizeof(int16_t) ? 0 : -ENOSPC;
	}
	if (ret == 0) {
		ret = fs_sync(&file);
	}
	closed = fs_close(&file);
	ret = ret != 0 ? ret : closed;
	if (ret == 0) {
		ret = verify_record(path, samples);
	}
	if (ret == 0) {
		k_mutex_lock(&state_lock, K_FOREVER);
		state.sd_saves++;
		state.sd_reads++;
		memcpy(state.last_file, path, sizeof(path));
		k_mutex_unlock(&state_lock);
	}
	printk("KIT|SD file=%s save_readback_result=%d samples=%u\n", path, ret, samples);
	return ret;
}

static void microphone_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (true) {
		void *buffer;
		size_t bytes;
		int ret = dmic_read(mic, 0, &buffer, &bytes, 200);

		if (ret == 0) {
			const int16_t *pcm = buffer;
			uint32_t peak = 0;
			uint64_t squares = 0;
			size_t samples = bytes / sizeof(int16_t);

			for (size_t i = 0; i < samples; i++) {
				int32_t value = pcm[i];
				uint32_t magnitude = value < 0 ? -value : value;

				peak = MAX(peak, magnitude);
				squares += (int64_t)value * value;
			}
			k_mutex_lock(&state_lock, K_FOREVER);
			state.peak = peak;
			state.rms = samples ? kit_isqrt(squares / samples) : 0;
			state.mic_blocks++;
			state.mic_result = 0;
			if (recording) {
				size_t count = MIN(samples, KIT_RECORD_SAMPLES - state.recorded_samples);

				memcpy(recording_pcm + state.recorded_samples, pcm,
				       count * sizeof(int16_t));
				state.recorded_samples += count;
				if (state.recorded_samples == KIT_RECORD_SAMPLES) {
					recording = false;
					k_sem_give(&record_done);
				}
			}
			k_mutex_unlock(&state_lock);
			k_mem_slab_free(&mic_slab, buffer);
			continue;
		}
		k_mutex_lock(&state_lock, K_FOREVER);
		state.mic_errors++;
		state.mic_result = ret;
		k_mutex_unlock(&state_lock);
		printk("KIT|MIC read_error=%d retry=1\n", ret);
		(void)dmic_trigger(mic, DMIC_TRIGGER_STOP);
		k_msleep(50);
		while (dmic_read(mic, 0, &buffer, &bytes, 0) == 0) {
			k_mem_slab_free(&mic_slab, buffer);
		}
		(void)dmic_trigger(mic, DMIC_TRIGGER_START);
		k_msleep(50);
	}
}
K_THREAD_DEFINE(mic_thread_id, 2048, microphone_thread, NULL, NULL, NULL, 5, 0, SYS_FOREVER_MS);

static int output_audio(bool tone, uint32_t samples)
{
	static const int16_t sine[16] = {
		0, 1567, 2896, 3784, 4096, 3784, 2896, 1567,
		0, -1567, -2896, -3784, -4096, -3784, -2896, -1567,
	};
	int16_t frame[TX_FRAMES * 2];
	struct kit_status s;
	uint32_t blocks = DIV_ROUND_UP(samples, TX_FRAMES);
	int ret;

	kit_snapshot(&s);
	if (s.speaker_result != 0) {
		return s.speaker_result;
	}
	ret = gpio_pin_set_dt(&amp, 1);
	for (uint32_t block = 0; ret == 0 && block < blocks; block++) {
		for (uint32_t i = 0; i < TX_FRAMES; i++) {
			uint32_t index = block * TX_FRAMES + i;
			/* Cap playback gain at 1x; no boost of an unknown recording. */
			int16_t value = index >= samples ? 0 :
					tone ? sine[index % ARRAY_SIZE(sine)] : recording_pcm[index];

			frame[i * 2] = frame[i * 2 + 1] = value;
		}
		ret = i2s_buf_write(speaker, frame, sizeof(frame));
		if (ret == 0 && block == 1) {
			ret = i2s_trigger(speaker, I2S_DIR_TX, I2S_TRIGGER_START);
		}
	}
	if (ret == 0) {
		ret = i2s_trigger(speaker, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	}
	if (ret == 0) {
		int64_t deadline = k_uptime_get() + 500;

		while (k_mem_slab_num_free_get(&tx_slab) != TX_SLAB_BLOCKS &&
		       k_uptime_get() < deadline) {
			k_msleep(2);
		}
		ret = k_mem_slab_num_free_get(&tx_slab) == TX_SLAB_BLOCKS ? 0 : -ETIMEDOUT;
	}
	(void)gpio_pin_set_dt(&amp, 0);
	if (ret != 0) {
		(void)i2s_trigger(speaker, I2S_DIR_TX, I2S_TRIGGER_DROP);
		k_msleep(2);
	}
	return ret;
}

static int capture_record(void)
{
	struct kit_status s;
	int ret;

	kit_snapshot(&s);
	if (s.mic_result != 0) {
		return s.mic_result;
	}
	k_sem_reset(&record_done);
	k_mutex_lock(&state_lock, K_FOREVER);
	state.recorded_samples = 0;
	state.last_file[0] = '\0';
	recording = true;
	k_mutex_unlock(&state_lock);
	ret = k_sem_take(&record_done, K_SECONDS(4));
	k_mutex_lock(&state_lock, K_FOREVER);
	recording = false;
	if (ret == 0) {
		state.records++;
	} else {
		state.recorded_samples = 0;
	}
	k_mutex_unlock(&state_lock);
	if (ret == 0) {
		int saved = save_record(KIT_RECORD_SAMPLES);

		k_mutex_lock(&state_lock, K_FOREVER);
		state.sd_result = saved;
		k_mutex_unlock(&state_lock);
	}
	/* A card failure does not discard a successful RAM recording. */
	return ret;
}

static void audio_thread(void *a, void *b, void *c)
{
	enum kit_action action;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (true) {
		struct kit_status s;
		int ret = -EINVAL;

		(void)k_msgq_get(&actions, &action, K_FOREVER);
		kit_snapshot(&s);
		if (action == KIT_SD_RETRY) {
			ret = mount_card();
			k_mutex_lock(&state_lock, K_FOREVER);
			state.sd_result = ret;
			k_mutex_unlock(&state_lock);
		} else if (action == KIT_RECORD) {
			ret = capture_record();
		} else if (action == KIT_BEEP) {
			ret = output_audio(true, KIT_SAMPLE_RATE * 3 / 10);
		} else if (action == KIT_PLAY) {
			if (s.recorded_samples == 0) {
				ret = -ENODATA;
			} else {
				ret = s.last_file[0] ? verify_record(s.last_file, s.recorded_samples) : 0;
				if (s.last_file[0]) {
					k_mutex_lock(&state_lock, K_FOREVER);
					state.sd_result = ret;
					state.sd_reads += ret == 0;
					k_mutex_unlock(&state_lock);
				}
				if (ret == 0) {
					ret = output_audio(false, s.recorded_samples);
				}
			}
		}
		k_mutex_lock(&state_lock, K_FOREVER);
		state.audio_result = ret;
		state.beeps += action == KIT_BEEP && ret == 0;
		state.plays += action == KIT_PLAY && ret == 0;
		state.busy = 0;
		k_mutex_unlock(&state_lock);
		printk("KIT|ACTION action=%u result=%d\n", action, ret);
	}
}
K_THREAD_DEFINE(audio_thread_id, 4096, audio_thread, NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

int kit_request(enum kit_action action, uint32_t value)
{
	int ret = 0;

	if (action < KIT_BEEP || action > KIT_RGB ||
	    (action < KIT_PAGE && value != 0) ||
	    (action >= KIT_PAGE && value > 2 && value != UINT32_MAX)) {
		return -EINVAL;
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	if (action == KIT_PAGE) {
		state.page = value == UINT32_MAX ? (state.page + 1) % 3 : value;
	} else if (action == KIT_RGB) {
		state.rgb_mode = value == UINT32_MAX ? (state.rgb_mode + 1) % 3 : value;
	} else if (state.busy != 0) {
		ret = -EBUSY;
	} else {
		state.busy = action;
		ret = k_msgq_put(&actions, &action, K_NO_WAIT);
		if (ret != 0) {
			state.busy = 0;
		}
	}
	k_mutex_unlock(&state_lock);
	return ret;
}

void kit_poll(void)
{
	static uint32_t rgb_at;
	uint32_t now = k_uptime_get_32();
	uint8_t raw = 0;
	struct kit_status s;

	kit_snapshot(&s);
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		if ((s.keys_ready & BIT(i)) && gpio_pin_get_dt(&keys[i]) > 0) {
			raw |= BIT(i);
		}
	}
	struct kit_button_events e = kit_buttons_update(&buttons, raw, now);

	if (e.rising) {
		printk("KIT|BUTTON pressed=0x%x rising=0x%x\n", buttons.pressed, e.rising);
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	state.pressed = buttons.pressed;
	state.seen |= e.rising;
	k_mutex_unlock(&state_lock);
	if (e.rising & BIT(0)) {
		(void)kit_request(KIT_PAGE, UINT32_MAX);
	}
	if (e.rising & BIT(1)) {
		(void)kit_request(KIT_RECORD, 0);
	}
	if (e.rising & BIT(2)) {
		(void)kit_request(KIT_BEEP, 0);
	}
	if (e.short_release & BIT(3)) {
		(void)kit_request(KIT_PLAY, 0);
	}
	if (e.long_hold & BIT(3)) {
		(void)kit_request(KIT_RGB, UINT32_MAX);
	}
	if ((uint32_t)(now - rgb_at) >= 100 && s.rgb_result == 0) {
		struct led_rgb pixels[4] = {0};
		bool connected;

		rgb_at = now;
		k_mutex_lock(&state_lock, K_FOREVER);
		connected = link_connected;
		k_mutex_unlock(&state_lock);
		for (size_t i = 0; i < ARRAY_SIZE(pixels); i++) {
			if (s.rgb_mode == 2) {
				continue;
			}
			pixels[i].b = connected ? 0 : 2;
			pixels[i].g = s.rgb_mode == 1 ? MIN(8, s.rms / 128) : connected ? 3 : 0;
			pixels[i].r = s.busy == KIT_RECORD ? 8 : 0;
			if (buttons.pressed & BIT(i)) {
				pixels[i].r = pixels[i].g = pixels[i].b = 8;
			}
		}
		int ret = led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));

		if (ret != 0) {
			k_mutex_lock(&state_lock, K_FOREVER);
			state.rgb_result = ret;
			k_mutex_unlock(&state_lock);
			printk("KIT|RGB write_error=%d\n", ret);
		}
	}
}

void kit_local_screen(char out[85])
{
	struct kit_status s;

	kit_snapshot(&s);
	if (s.page == 1) {
		snprintk(out, 85, "MIC 16kHz MONO\nRMS:%u Peak:%u\nREC:%u/32000\n2:REC 3:BEEP 4:PLAY",
			 s.rms, s.peak, s.recorded_samples);
	} else {
		snprintk(out, 85, "NU40 KIT keys:%x\nSD:%s err:%d\nREC:%u PLAY:%u\nRGB:%u Busy:%u",
			 s.seen, s.sd_mounted ? "FAT" : "NONE", s.sd_result,
			 s.records % 10000, s.plays % 10000, s.rgb_mode, s.busy);
	}
}

void kit_report(void)
{
	struct kit_status s;

	kit_snapshot(&s);
	printk("KIT|STATUS keys_ready=0x%x pressed=0x%x seen=0x%x page=%u rgb_mode=%u "
	       "rgb_result=%d mic_result=%d mic_blocks=%u mic_errors=%u peak=%u rms=%u "
	       "speaker_result=%d beeps=%u records=%u plays=%u samples=%u busy=%u "
	       "audio_result=%d sd_mounted=%u sd_result=%d sd_saves=%u sd_reads=%u\n",
	       s.keys_ready, s.pressed, s.seen, s.page, s.rgb_mode, s.rgb_result, s.mic_result,
	       s.mic_blocks, s.mic_errors, s.peak, s.rms, s.speaker_result, s.beeps, s.records,
	       s.plays, s.recorded_samples, s.busy, s.audio_result, s.sd_mounted,
	       s.sd_result, s.sd_saves, s.sd_reads);
}

void kit_init(void)
{
	struct pcm_stream_cfg stream = {
		.pcm_rate = KIT_SAMPLE_RATE, .pcm_width = 16,
		.block_size = MIC_BYTES, .mem_slab = &mic_slab,
	};
	struct dmic_cfg cfg = {
		.io = {.min_pdm_clk_freq = 1000000, .max_pdm_clk_freq = 3500000,
		       .min_pdm_clk_dc = 40, .max_pdm_clk_dc = 60},
		.streams = &stream,
		.channel = {.req_num_streams = 1, .req_num_chan = 1,
			    .req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT)},
	};
	struct i2s_config tx = {
		.word_size = 16, .channels = 2, .format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
		.frame_clk_freq = KIT_SAMPLE_RATE, .mem_slab = &tx_slab,
		.block_size = TX_BYTES, .timeout = 100,
	};
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		ret = gpio_is_ready_dt(&keys[i]) ? gpio_pin_configure_dt(&keys[i], GPIO_INPUT) :
		      -ENODEV;
		state.keys_ready |= ret == 0 ? BIT(i) : 0;
	}
	ret = gpio_is_ready_dt(&mic_sel) ?
	      gpio_pin_configure_dt(&mic_sel, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	if (ret == 0) {
		ret = device_is_ready(mic) ? dmic_configure(mic, &cfg) : -ENODEV;
	}
	if (ret == 0) {
		ret = dmic_trigger(mic, DMIC_TRIGGER_START);
	}
	state.mic_result = ret;
	if (ret == 0) {
		k_thread_start(mic_thread_id);
	}
	ret = gpio_is_ready_dt(&amp) ? gpio_pin_configure_dt(&amp, GPIO_OUTPUT_INACTIVE) :
	      -ENODEV;
	if (ret == 0) {
		ret = device_is_ready(speaker) ? i2s_configure(speaker, I2S_DIR_TX, &tx) : -ENODEV;
	}
	state.speaker_result = ret;
	state.rgb_result = device_is_ready(strip) ? 0 : -ENODEV;
	state.sd_result = -EINPROGRESS;
	k_thread_start(audio_thread_id);
	(void)kit_request(KIT_SD_RETRY, 0);
	kit_report();
}
