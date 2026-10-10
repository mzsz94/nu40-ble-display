/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <hal/nrf_power.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#define MIC_RATE 16000
#define MIC_BLOCK_BYTES (320 * sizeof(int16_t)) /* 20 ms, mono. */
#define TX_FRAMES 160
#define TX_BYTES (TX_FRAMES * 2 * sizeof(int16_t))
#define TX_BLOCKS 30
#define TX_SLAB_BLOCKS 6

K_MEM_SLAB_DEFINE_STATIC(mic_slab, MIC_BLOCK_BYTES, 8, 4);
K_MEM_SLAB_DEFINE_STATIC(tx_slab, TX_BYTES, TX_SLAB_BLOCKS, 4);

static const struct device *const oled = DEVICE_DT_GET(DT_NODELABEL(kit_oled));
static const struct device *const mic = DEVICE_DT_GET(DT_NODELABEL(pdm0));
static const struct device *const speaker = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const strip = DEVICE_DT_GET(DT_NODELABEL(kit_strip));
static const struct gpio_dt_spec keys[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key0), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key1), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key2), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(kit_key3), gpios),
};
static const struct gpio_dt_spec mic_sel = GPIO_DT_SPEC_GET(DT_NODELABEL(kit_mic_sel), gpios);
static const struct gpio_dt_spec amp = GPIO_DT_SPEC_GET(DT_NODELABEL(kit_amp), gpios);
static bool oled_ready, mic_ready, speaker_ready, strip_ready;
static uint8_t keys_ready, pressed, seen, page;
static uint32_t blocks, peak, rms, mic_errors, oled_writes;
static uint32_t window_peak, window_rms_max, window_blocks;
static uint64_t window_rms_sum;
static int64_t window_start;
static atomic_t beep_count, beep_error;
static atomic_t sd_result = ATOMIC_INIT(-EINPROGRESS), sd_present = ATOMIC_INIT(-1);

static void summary(bool reset_window)
{
	printk("KIT|STATUS keys_ready=0x%x pressed=0x%x seen=0x%x oled_write_ok=%u "
	       "mic_blocks=%u mic_errors=%u beep_tx_ok=%u beep_error=%d\n",
	       keys_ready, pressed, seen, oled_writes, blocks, mic_errors,
	       (uint32_t)atomic_get(&beep_count), (int)atomic_get(&beep_error));
	printk("KIT|MIC rate=16000 width=16 channels=1 peak=%u rms=%u blocks=%u "
	       "window_ms=%u window_blocks=%u rms_max=%u last_peak=%u last_rms=%u "
	       "sound_response=unverified\n", window_peak,
	       window_blocks ? (uint32_t)(window_rms_sum / window_blocks) : 0, blocks,
	       (uint32_t)(k_uptime_get() - window_start), window_blocks, window_rms_max, peak, rms);
	printk("KIT|SD present=%d read_result=%d readonly=1\n",
	       (int)atomic_get(&sd_present), (int)atomic_get(&sd_result));
	if (reset_window) {
		window_peak = window_rms_max = window_blocks = 0;
		window_rms_sum = 0;
		window_start = k_uptime_get();
	}
}

static void check_sd(void *p1, void *p2, void *p3)
{
	const struct device *host = DEVICE_DT_GET(DT_NODELABEL(kit_sdhc));
	static uint8_t sector[512] __aligned(4);
	uint32_t size = 0, count = 0;
	int detected, ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	detected = device_is_ready(host) ? sdhc_card_present(host) : -ENODEV;
	atomic_set(&sd_present, detected);
	printk("KIT|SD detect_result=%d polarity=active_high readonly=1\n", detected);
	if (detected <= 0) {
		atomic_set(&sd_result, detected < 0 ? detected : -ENOMEDIUM);
		printk("KIT|SD skipped=1 other_tests_continue=1\n");
		return;
	}
	ret = disk_access_init("KITSD");
	printk("KIT|SD init_result=%d readonly=1\n", ret);
	if (ret == 0) {
		ret = disk_access_ioctl("KITSD", DISK_IOCTL_GET_SECTOR_SIZE, &size);
	}
	if (ret == 0) {
		ret = disk_access_ioctl("KITSD", DISK_IOCTL_GET_SECTOR_COUNT, &count);
	}
	if (ret == 0 && size != sizeof(sector)) {
		ret = -ENOTSUP;
	}
	if (ret == 0) {
		/* Exactly one sector is read into RAM; never mount, format, write, or erase. */
		ret = disk_access_read("KITSD", sector, 0, 1);
	}
	atomic_set(&sd_result, ret);
	printk("KIT|SD read_result=%d sectors=%u sector_bytes=%u lba=0 count=1 readonly=1\n",
	       ret, count, size);
}
/* A slow or absent card must not block microphone, button, or display handling. */
K_THREAD_DEFINE(sd_thread, 2048, check_sd, NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

static void draw_status(void)
{
	char top[20], bottom[20];
	int ret;

	if (!oled_ready) {
		return;
	}
	if (page == 1) {
		snprintk(top, sizeof(top), "MIC %u", blocks);
		snprintk(bottom, sizeof(bottom), "P%u%% R%u%%", peak * 100 / 32768,
			 rms * 100 / 32768);
	} else if (page == 3) {
		snprintk(top, sizeof(top), "KIT RUNNING");
		snprintk(bottom, sizeof(bottom), "O%d M%d L%d", oled_ready, mic_ready, strip_ready);
	} else {
		snprintk(top, sizeof(top), "KEY 0 1 2 3");
		snprintk(bottom, sizeof(bottom), "    %d %d %d %d", !!(pressed & BIT(0)),
			 !!(pressed & BIT(1)), !!(pressed & BIT(2)), !!(pressed & BIT(3)));
	}
	ret = cfb_framebuffer_clear(oled, false);
	if (ret == 0) {
		ret = cfb_print(oled, top, 0, 0);
	}
	if (ret == 0) {
		ret = cfb_print(oled, bottom, 0, 16);
	}
	if (ret == 0) {
		ret = cfb_framebuffer_finalize(oled);
	}
	if (ret != 0) {
		printk("KIT|OLED write_error=%d other_tests_continue=1\n", ret);
		oled_ready = false;
	} else {
		oled_writes++;
	}
}

static void update_rgb(void)
{
	struct led_rgb pixels[4] = {0};
	int ret;

	if (!strip_ready) {
		return;
	}
	for (int i = 0; i < ARRAY_SIZE(pixels); i++) {
		pixels[i].g = (pressed & BIT(i)) ? 8 : 0;
		pixels[i].b = (pressed & BIT(i)) ? 0 : 2;
	}
	ret = led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
	if (ret != 0) {
		strip_ready = false;
		printk("KIT|RGB write_error=%d\n", ret);
	}
}

/* Integer square root avoids floating point formatting and a libm dependency. */
static uint32_t isqrt(uint64_t value)
{
	uint64_t result = 0, bit = (uint64_t)1 << 62;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit != 0) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	return result;
}

static void read_microphone(void)
{
	void *buffer;
	size_t size;
	int ret;

	if (!mic_ready) {
		return;
	}
	/* Drain queued blocks without delaying button handling. */
	while ((ret = dmic_read(mic, 0, &buffer, &size, 0)) == 0) {
		const int16_t *samples = buffer;
		uint64_t squares = 0;
		uint32_t maximum = 0;

		for (size_t i = 0; i < size / sizeof(*samples); i++) {
			int32_t value = samples[i];
			uint32_t magnitude = value < 0 ? -value : value;

			maximum = MAX(maximum, magnitude);
			squares += (int64_t)value * value;
		}
		peak = maximum;
		rms = size ? isqrt(squares / (size / sizeof(*samples))) : 0;
		blocks++;
		window_peak = MAX(window_peak, peak);
		window_rms_max = MAX(window_rms_max, rms);
		window_rms_sum += rms;
		window_blocks++;
		k_mem_slab_free(&mic_slab, buffer);
	}
	if (ret != -ENOMSG && ret != -EAGAIN) {
		mic_errors++;
		mic_ready = false;
		printk("KIT|MIC read_error=%d other_tests_continue=1\n", ret);
	}
}

static void beep(struct k_work *work)
{
	/* Nominal 1000 Hz sine, -18 dBFS; physical audibility requires observation. */
	static const int16_t sine[16] = {
		0, 1567, 2896, 3784, 4096, 3784, 2896, 1567,
		0, -1567, -2896, -3784, -4096, -3784, -2896, -1567,
	};
	int16_t frame[TX_FRAMES * 2];
	int ret = -ENODEV;
	bool drained = false;

	ARG_UNUSED(work);
	if (!speaker_ready) {
		goto done;
	}
	for (int i = 0; i < TX_FRAMES; i++) {
		frame[i * 2] = sine[i % ARRAY_SIZE(sine)];
		frame[i * 2 + 1] = frame[i * 2];
	}
	ret = gpio_pin_set_dt(&amp, 1);
	for (int i = 0; ret == 0 && i < TX_BLOCKS; i++) {
		ret = i2s_buf_write(speaker, frame, sizeof(frame));
		if (ret == 0 && i == 1) {
			ret = i2s_trigger(speaker, I2S_DIR_TX, I2S_TRIGGER_START);
		}
	}
	if (ret == 0) {
		ret = i2s_trigger(speaker, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	}
	if (ret == 0) {
		int64_t deadline = k_uptime_get() + 500;

		/* nrfx releases its final slab block only after the transfer has stopped. */
		while (k_mem_slab_num_free_get(&tx_slab) != TX_SLAB_BLOCKS &&
		       k_uptime_get() < deadline) {
			k_sleep(K_MSEC(2));
		}
		drained = k_mem_slab_num_free_get(&tx_slab) == TX_SLAB_BLOCKS;
		ret = drained ? 0 : -ETIMEDOUT;
	}
	(void)gpio_pin_set_dt(&amp, 0);
	if (ret != 0) {
		(void)i2s_trigger(speaker, I2S_DIR_TX, I2S_TRIGGER_DROP);
		k_sleep(K_MSEC(2));
	}
done:
	atomic_set(&beep_error, ret);
	if (ret == 0) {
		atomic_inc(&beep_count);
	}
	printk("KIT|SPEAKER tx_result=%d nominal_hz=1000 duration_ms=300 amplitude=4096 "
	       "dbfs=-18 drain_complete=%d audible=unverified\n", ret, drained);
}
K_WORK_DEFINE(beep_work, beep);

static void enter_uf2_bootloader(void)
{
	printk("KIT|CMD uf2 accepted=1 gpregret=0x57 reboot=cold\n");
	(void)gpio_pin_set_dt(&amp, 0);
	k_msleep(50);
	nrf_power_gpregret_set(NRF_POWER, 0U, 0x57U);
	sys_reboot(SYS_REBOOT_COLD);
}

static void poll_commands(const struct device *console)
{
	static char command[16];
	static size_t used;
	static bool overflow;
	uint32_t baudrate;
	unsigned char c;

	/* Match the verified NU40DK KIT example's 1200-baud bootloader request. */
	if (uart_line_ctrl_get(console, UART_LINE_CTRL_BAUD_RATE, &baudrate) == 0 &&
	    baudrate == 1200) {
		enter_uf2_bootloader();
	}
	for (int i = 0; i < 64 && uart_poll_in(console, &c) == 0; i++) {
		if (c == '\r' || c == '\n') {
			command[used] = '\0';
			if (overflow) {
				printk("KIT|CMD error=line_too_long\n");
			} else if (strcmp(command, "status") == 0) {
				summary(false);
			} else if (strcmp(command, "beep") == 0) {
				printk("KIT|CMD beep queued=%d\n", k_work_submit(&beep_work));
			} else if (strcmp(command, "uf2") == 0) {
				enter_uf2_bootloader();
			} else if (used != 0) {
				printk("KIT|CMD error=unknown supported=status,beep,uf2\n");
			}
			used = 0;
			overflow = false;
		} else if (used < sizeof(command) - 1) {
			command[used++] = c;
		} else {
			overflow = true;
		}
	}
}

static void init_devices(void)
{
	int ret;
	struct pcm_stream_cfg stream = {
		.pcm_rate = MIC_RATE, .pcm_width = 16,
		.block_size = MIC_BLOCK_BYTES, .mem_slab = &mic_slab,
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
		.frame_clk_freq = MIC_RATE, .mem_slab = &tx_slab,
		.block_size = TX_BYTES, .timeout = 100,
	};

	for (int i = 0; i < ARRAY_SIZE(keys); i++) {
		ret = gpio_is_ready_dt(&keys[i]) ? gpio_pin_configure_dt(&keys[i], GPIO_INPUT)
					       : -ENODEV;
		keys_ready |= ret == 0 ? BIT(i) : 0;
		printk("KIT|BUTTON key=%d init_result=%d input_observation=pending\n", i, ret);
	}
	ret = device_is_ready(oled) ? cfb_framebuffer_init(oled) : -ENODEV;
	if (ret == 0) {
		ret = display_blanking_off(oled);
	}
	if (ret == 0) {
		ret = cfb_framebuffer_set_font(oled, 0); /* Built-in 10x16, two lines. */
	}
	oled_ready = ret == 0;
	printk("KIT|OLED init_result=%d addr=0x3c visible=unverified\n", ret);
	ret = gpio_is_ready_dt(&mic_sel) ?
		gpio_pin_configure_dt(&mic_sel, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	if (ret == 0) {
		ret = device_is_ready(mic) ? dmic_configure(mic, &cfg) : -ENODEV;
	}
	if (ret == 0) {
		ret = dmic_trigger(mic, DMIC_TRIGGER_START);
	}
	mic_ready = ret == 0;
	printk("KIT|MIC start_result=%d rate=16000 width=16 channels=1 sel=low\n", ret);
	ret = gpio_is_ready_dt(&amp) ?
		gpio_pin_configure_dt(&amp, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	if (ret == 0) {
		ret = device_is_ready(speaker) ? i2s_configure(speaker, I2S_DIR_TX, &tx) : -ENODEV;
	}
	speaker_ready = ret == 0;
	printk("KIT|SPEAKER init_result=%d amp=muted trigger=key2\n", ret);
	strip_ready = device_is_ready(strip);
	update_rgb();
	printk("KIT|RGB driver_ready=%d brightness_max=8 visible=unverified\n", strip_ready);
}

int main(void)
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;
	uint8_t candidate = 0;
	int64_t change_time = 0, report_time = 0, screen_time = 0;

	for (int i = 0; i < 150 && dtr == 0; i++) {
		(void)uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(20));
	}
	printk("KIT|BOOT NU40DK-KIT selftest firmware=3 observation=pending\n");
	printk("KIT|BOOT key0=keys key1=mic key2=beep key3=summary commands=status,beep,uf2\n");
	init_devices();
	window_start = report_time = k_uptime_get();
	k_thread_start(sd_thread);
	while (true) {
		uint8_t raw = 0;
		int64_t now = k_uptime_get();

		for (int i = 0; i < ARRAY_SIZE(keys); i++) {
			if ((keys_ready & BIT(i)) && gpio_pin_get_dt(&keys[i]) > 0) {
				raw |= BIT(i);
			}
		}
		if (raw != candidate) {
			candidate = raw;
			change_time = now;
		}
		if (candidate != pressed && now - change_time >= 25) {
			uint8_t rising = candidate & ~pressed;

			pressed = candidate;
			seen |= rising;
			printk("KIT|BUTTON pressed=0x%x seen=0x%x\n", pressed, seen);
			if (rising & BIT(0)) {
				page = 0;
			}
			if (rising & BIT(1)) {
				page = 1;
			}
			if (rising & BIT(2)) {
				(void)k_work_submit(&beep_work);
			}
			if (rising & BIT(3)) {
				page = 3;
				summary(false);
			}
			update_rgb();
		}
		read_microphone();
		poll_commands(console);
		if (now - screen_time >= 250) {
			draw_status();
			screen_time = now;
		}
		if (now - report_time >= 2000) {
			summary(true);
			report_time = now;
		}
		k_sleep(K_MSEC(5));
	}
	return 0;
}
