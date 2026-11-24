// fbprobe -- framebuffer blit ceiling probe for the R1 / R3 Pro II.
//
// Measures the ABSOLUTE upper bound for on-device video playback: how fast
// full-panel RGB565 frames can be pushed to the screen with ZERO decoding.
//
// If this probe cannot sustain the target frame rate with no codec in the
// loop, then software-decoded video at that resolution/rate is impossible on
// this hardware, because a real player must do all of this PLUS decode.
//
// It reproduces the player's real output path (src/main.c): a double-height
// framebuffer, draw into the off-screen page, then FBIOPAN_DISPLAY which the
// display driver latches on vsync.
//
// Three measurements, each for N seconds:
//   1. pan-only:   flip between two already-filled pages (vsync cap of the panel)
//   2. fill+pan:   write a fresh full-panel frame into the off-screen page,
//                  then pan -- the true "new frame every flip" ceiling
//   3. memfill:    just write full-panel frames to RAM, no pan -- raw CPU/mem
//                  write bandwidth, to separate it from the vsync cap
//
// Build: cross-compile with the repo toolchain, must pass the glibc-2.22 ABI
// gate (see tools/video-probe/build.sh).
//
// Usage on device:  ./fbprobe [seconds]   (default 3)

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Fill a full page with a moving gradient so the compiler cannot elide the
// writes and so the picture visibly changes (confirms real output).
static void fill_frame(uint16_t *dst, int w, int h, int phase) {
	for (int y = 0; y < h; y++) {
		uint16_t *row = dst + (size_t)y * w;
		for (int x = 0; x < w; x++) {
			int v = (x + y + phase);
			// RGB565 from a cheap pattern
			uint16_t r = (uint16_t)((v >> 1) & 0x1F);
			uint16_t g = (uint16_t)((v >> 0) & 0x3F);
			uint16_t b = (uint16_t)((v >> 2) & 0x1F);
			row[x] = (uint16_t)((r << 11) | (g << 5) | b);
		}
	}
}

int main(int argc, char **argv) {
	int seconds = (argc > 1) ? atoi(argv[1]) : 3;
	if (seconds < 1)
		seconds = 1;

	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/fb0");
		return 1;
	}

	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_*SCREENINFO");
		close(fd);
		return 1;
	}

	int w = var.xres, h = var.yres;
	int bpp = var.bits_per_pixel;
	size_t page_bytes = (size_t)fix.line_length * h;
	size_t map_bytes = page_bytes * 2; // double buffer, as the player uses

	printf("panel      : %dx%d, %d bpp, line_length=%u\n", w, h, bpp, fix.line_length);
	printf("page bytes : %zu  (double-buffer map %zu)\n", page_bytes, map_bytes);
	printf("pixels/frame: %d\n", w * h);

	if (bpp != 16) {
		printf("WARNING: expected 16bpp RGB565; got %d bpp. Results still indicative.\n", bpp);
	}

	uint8_t *fb = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		// Some drivers only map one page; fall back to one page (pan-only test
		// then just re-pans the same page, still a valid vsync measurement).
		map_bytes = page_bytes;
		fb = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (fb == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return 1;
		}
		printf("NOTE: only one page could be mapped; pan test re-pans page 0.\n");
	}
	int two_pages = (map_bytes >= page_bytes * 2);

	uint16_t *page0 = (uint16_t *)fb;
	uint16_t *page1 = two_pages ? (uint16_t *)(fb + page_bytes) : page0;

	// Pre-fill both pages for the pan-only test.
	fill_frame(page0, w, h, 0);
	fill_frame(page1, w, h, 128);

	// ---- Test 1: pan-only (vsync cap) ----
	{
		double t0 = now_s(), t1;
		long frames = 0;
		int page = 0;
		do {
			var.yoffset = page ? var.yres : 0;
			ioctl(fd, FBIOPAN_DISPLAY, &var);
			page ^= two_pages ? 1 : 0;
			frames++;
			t1 = now_s();
		} while (t1 - t0 < seconds);
		printf("\n[1] pan-only   : %.1f fps  (%ld flips in %.2fs) -- panel vsync ceiling\n", frames / (t1 - t0), frames, t1 - t0);
	}

	// ---- Test 2: fill a fresh frame + pan (real per-frame ceiling) ----
	{
		double t0 = now_s(), t1;
		long frames = 0;
		int page = 0, phase = 0;
		do {
			uint16_t *dst = page ? page1 : page0;
			fill_frame(dst, w, h, phase++);
			var.yoffset = page ? var.yres : 0;
			ioctl(fd, FBIOPAN_DISPLAY, &var);
			page ^= two_pages ? 1 : 0;
			frames++;
			t1 = now_s();
		} while (t1 - t0 < seconds);
		printf("[2] fill+pan   : %.1f fps  (%ld frames in %.2fs) -- new full-panel frame every flip\n", frames / (t1 - t0), frames, t1 - t0);
	}

	// ---- Test 3: memfill only (raw write bandwidth, no pan) ----
	{
		double t0 = now_s(), t1;
		long frames = 0;
		int phase = 0;
		do {
			fill_frame(page0, w, h, phase++);
			frames++;
			t1 = now_s();
		} while (t1 - t0 < seconds);
		double fps = frames / (t1 - t0);
		double mbps = fps * (double)(w * h * 2) / (1024.0 * 1024.0);
		printf("[3] memfill    : %.1f fps  (%.1f MB/s of pixel writes) -- CPU/mem write bound\n", fps, mbps);
	}

	printf("\nTarget for full-panel video: 25-30 fps.\n");
	printf("Read [2]: if it is below ~30, a decoder on top only makes it slower.\n");

	munmap(fb, map_bytes);
	close(fd);
	return 0;
}
