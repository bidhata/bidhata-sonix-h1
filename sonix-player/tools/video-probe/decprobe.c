// decprobe -- software video decode probe for the R1 / R3 Pro II.
//
// fbprobe showed what the panel can take with no codec in the loop. This one
// puts the codec in: it decodes a real clip with libavcodec, converts every
// frame from YUV 4:2:0 to RGB565 straight into the off-screen framebuffer page
// (rotated a quarter turn, so a landscape picture fills the portrait panel),
// and pans to it -- the work a player would do for each frame, with nothing
// held back to keep time. Every stage is timed on its own.
//
// What it answers, per clip: how many frames a second this device can decode
// and show, against how many the clip needs.
//
// Build: tools/video-probe/build-decprobe.sh (cross-builds a cut-down FFmpeg).
//
// Usage on device:
//   ./decprobe [-t seconds] [-j threads] [-n] [-r] [-l] [-f] clip [clip ...]
//     -t N   stop each clip after N seconds of wall time (default 8, 0 = whole clip)
//     -j N   decoder threads (default 1; the X1600 has one core)
//     -n     no framebuffer: convert into ordinary memory instead
//     -r     no rotation: picture upright, top-left of the panel
//     -l     H.264: skip the in-loop deblocking filter (faster, not bit-exact)
//     -f     allow the decoder's non-compliant speed-ups (AV_CODEC_FLAG2_FAST)
//     -o F   with -n: write the last converted frame to F as raw RGB565, and
//            the first bytes of its three planes to stdout, to check the picture
//
// Stop the player first: both would be panning the same framebuffer.

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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------- output ----

static int fb_fd = -1;
static struct fb_var_screeninfo fb_var;
static uint8_t *fb_mem;
static size_t fb_page_size;
static int fb_pages;
static int out_w = 480, out_h = 800; // the panel, or a stand-in for it with -n
static uint16_t *ram_page;

// The player's own way in (try_panned_display in src/main.c): 16 bits, tight
// stride, two pages, flip with FBIOPAN_DISPLAY.
static int fb_open(void) {
	struct fb_fix_screeninfo fix;

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		perror("/dev/fb0");
		return -1;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("fb: screen info");
		return -1;
	}
	if (fb_var.bits_per_pixel != 16 || fix.line_length != fb_var.xres * 2) {
		fprintf(stderr, "fb: not 16 bits with a tight stride\n");
		return -1;
	}

	fb_var.yres_virtual = fb_var.yres * 2;
	fb_var.xres_virtual = fb_var.xres;
	fb_var.yoffset = 0;
	fb_pages = 2;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var) < 0)
		fb_pages = 1;
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);

	fb_page_size = (size_t)fix.line_length * fb_var.yres;
	if ((size_t)fix.smem_len < fb_page_size * 2)
		fb_pages = 1;

	fb_mem = mmap(NULL, fb_page_size * fb_pages, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		perror("fb: mmap");
		return -1;
	}
	out_w = (int)fb_var.xres;
	out_h = (int)fb_var.yres;
	return 0;
}

// ------------------------------------------------------------ conversion ----
//
// BT.601, limited range, by table: what a player on a core with no SIMD would
// do. One table lookup per term, one more to clamp and pack each channel.

static int32_t tab_y[256], tab_rv[256], tab_gu[256], tab_gv[256], tab_bu[256];
#define CLAMP_OFF 512
static uint16_t pack_r[CLAMP_OFF * 2 + 256], pack_g[CLAMP_OFF * 2 + 256], pack_b[CLAMP_OFF * 2 + 256];

static void tables_init(int full_range) {
	for (int i = 0; i < 256; i++) {
		if (full_range) {
			tab_y[i] = i;
			tab_rv[i] = (int32_t)(1.402 * (i - 128));
			tab_gu[i] = (int32_t)(-0.344 * (i - 128));
			tab_gv[i] = (int32_t)(-0.714 * (i - 128));
			tab_bu[i] = (int32_t)(1.772 * (i - 128));
		} else {
			tab_y[i] = (int32_t)(1.164 * (i - 16));
			tab_rv[i] = (int32_t)(1.596 * (i - 128));
			tab_gu[i] = (int32_t)(-0.392 * (i - 128));
			tab_gv[i] = (int32_t)(-0.813 * (i - 128));
			tab_bu[i] = (int32_t)(2.017 * (i - 128));
		}
	}
	for (int i = 0; i < CLAMP_OFF * 2 + 256; i++) {
		int v = i - CLAMP_OFF;
		if (v < 0)
			v = 0;
		if (v > 255)
			v = 255;
		pack_r[i] = (uint16_t)((v >> 3) << 11);
		pack_g[i] = (uint16_t)((v >> 2) << 5);
		pack_b[i] = (uint16_t)(v >> 3);
	}
}

#define PIXEL(yv, rr, gg, bb) (uint16_t)(pack_r[CLAMP_OFF + (yv) + (rr)] | pack_g[CLAMP_OFF + (yv) + (gg)] | pack_b[CLAMP_OFF + (yv) + (bb)])

// Upright: source row to destination row, cropped to the panel.
static void convert_upright(const AVFrame *f, uint16_t *dst) {
	int w = f->width < out_w ? f->width : out_w;
	int h = f->height < out_h ? f->height : out_h;
	w &= ~1;
	h &= ~1;

	for (int y = 0; y < h; y += 2) {
		const uint8_t *y0 = f->data[0] + (ptrdiff_t)y * f->linesize[0];
		const uint8_t *y1 = y0 + f->linesize[0];
		const uint8_t *u = f->data[1] + (ptrdiff_t)(y / 2) * f->linesize[1];
		const uint8_t *v = f->data[2] + (ptrdiff_t)(y / 2) * f->linesize[2];
		uint16_t *d0 = dst + (size_t)y * out_w;
		uint16_t *d1 = d0 + out_w;

		for (int x = 0; x < w; x += 2) {
			int rr = tab_rv[*v], gg = tab_gu[*u] + tab_gv[*v], bb = tab_bu[*u];
			int a = tab_y[y0[0]], b = tab_y[y0[1]], c = tab_y[y1[0]], e = tab_y[y1[1]];
			d0[0] = PIXEL(a, rr, gg, bb);
			d0[1] = PIXEL(b, rr, gg, bb);
			d1[0] = PIXEL(c, rr, gg, bb);
			d1[1] = PIXEL(e, rr, gg, bb);
			y0 += 2, y1 += 2, d0 += 2, d1 += 2, u++, v++;
		}
	}
}

// A quarter turn clockwise, centred: source column sx becomes destination row
// sx, read from the bottom of the picture up. The destination is written in
// order, two rows at a time; the source is read down its columns.
static void convert_rotated(const AVFrame *f, uint16_t *dst) {
	int w = f->width < out_h ? f->width : out_h;   // source width runs down the panel
	int h = f->height < out_w ? f->height : out_w; // source height runs across it
	w &= ~1;
	h &= ~1;
	int off_x = ((out_w - h) / 2) & ~1;
	int off_y = ((out_h - w) / 2) & ~1;
	ptrdiff_t ls = f->linesize[0], lsu = f->linesize[1], lsv = f->linesize[2];

	// In bands of BAND source rows. Going down a whole column at once touches
	// one cache line per row in each plane, more than the 32 KB data cache
	// holds, so every read misses (measured: 25 ms a frame at 800x450). Within
	// a band the same few dozen lines serve 32 columns in a row.
	enum { BAND = 32 };
	for (int i0 = 0; i0 < h; i0 += BAND) {
		int i1 = i0 + BAND < h ? i0 + BAND : h;
		for (int sx = 0; sx < w; sx += 2) {
			uint16_t *d0 = dst + (size_t)(off_y + sx) * out_w + off_x + i0;
			uint16_t *d1 = d0 + out_w;
			const uint8_t *yp = f->data[0] + (ptrdiff_t)(h - 1 - i0) * ls + sx;
			const uint8_t *up = f->data[1] + (ptrdiff_t)(h / 2 - 1 - i0 / 2) * lsu + sx / 2;
			const uint8_t *vp = f->data[2] + (ptrdiff_t)(h / 2 - 1 - i0 / 2) * lsv + sx / 2;

			for (int i = i0; i < i1; i += 2) {
				int rr = tab_rv[*vp], gg = tab_gu[*up] + tab_gv[*vp], bb = tab_bu[*up];
				const uint8_t *yq = yp - ls; // the row above
				int a = tab_y[yp[0]], b = tab_y[yp[1]], c = tab_y[yq[0]], e = tab_y[yq[1]];
				d0[0] = PIXEL(a, rr, gg, bb);
				d1[0] = PIXEL(b, rr, gg, bb);
				d0[1] = PIXEL(c, rr, gg, bb);
				d1[1] = PIXEL(e, rr, gg, bb);
				d0 += 2, d1 += 2;
				yp -= 2 * ls, up -= lsu, vp -= lsv;
			}
		}
	}
}

// ------------------------------------------------------------- the probe ----

static int opt_seconds = 8, opt_threads = 1, opt_nofb, opt_upright, opt_noloop, opt_fast;
static const char *opt_dump;

static int is_420(enum AVPixelFormat p) { return p == AV_PIX_FMT_YUV420P || p == AV_PIX_FMT_YUVJ420P; }

static int probe(const char *path) {
	AVFormatContext *fmt = NULL;
	AVCodecContext *dec = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int rc = 1;

	if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
		fprintf(stderr, "%s: cannot open\n", path);
		goto out;
	}
	if (avformat_find_stream_info(fmt, NULL) < 0) {
		fprintf(stderr, "%s: no stream info\n", path);
		goto out;
	}
	const AVCodec *codec = NULL;
	int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (vs < 0 || !codec) {
		fprintf(stderr, "%s: no video stream this build can decode\n", path);
		goto out;
	}
	AVStream *st = fmt->streams[vs];
	dec = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(dec, st->codecpar);
	dec->thread_count = opt_threads;
	if (opt_noloop)
		dec->skip_loop_filter = AVDISCARD_ALL;
	if (opt_fast)
		dec->flags2 |= AV_CODEC_FLAG2_FAST;
	if (avcodec_open2(dec, codec, NULL) < 0) {
		fprintf(stderr, "%s: cannot open the %s decoder\n", path, codec->name);
		goto out;
	}

	AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
	double clip_fps = fr.den ? av_q2d(fr) : 0;
	double budget = clip_fps > 0 ? 1000.0 / clip_fps : 0;
	double kbps = fmt->bit_rate > 0 ? fmt->bit_rate / 1000.0 : 0;

	double t_dec = 0, t_conv = 0, t_pan = 0, worst = 0;
	long frames = 0, late = 0, unconverted = 0;
	int page = 0, tables_ready = 0, eof = 0;
	double start = now_ms();

	while (!eof) {
		double t0 = now_ms();
		int r = av_read_frame(fmt, pkt);
		if (r < 0) {
			avcodec_send_packet(dec, NULL); // drain
			eof = 1;
		} else if (pkt->stream_index == vs) {
			avcodec_send_packet(dec, pkt);
		}
		if (r >= 0)
			av_packet_unref(pkt);

		for (;;) {
			r = avcodec_receive_frame(dec, frame);
			double t1 = now_ms();
			// Reading and demuxing count as decode time: a player pays for them too.
			if (r < 0) {
				t_dec += t1 - t0;
				break;
			}
			double d = t1 - t0, c = 0, p = 0;

			if (is_420(frame->format)) {
				if (!tables_ready) {
					tables_init(frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P);
					tables_ready = 1;
				}
				uint16_t *dst = fb_mem ? (uint16_t *)(fb_mem + (page ? fb_page_size : 0)) : ram_page;
				if (opt_upright)
					convert_upright(frame, dst);
				else
					convert_rotated(frame, dst);
				double t2 = now_ms();
				c = t2 - t1;
				if (opt_dump && frames == 60) {
					printf("  frame 60: linesize %d %d %d, Y %d %d %d %d, U %d %d %d %d, V %d %d %d %d (mid-picture)\n", frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->data[0][frame->linesize[0] * (frame->height / 2) + 100], frame->data[0][frame->linesize[0] * (frame->height / 2) + 200], frame->data[0][frame->linesize[0] * (frame->height / 2) + 300], frame->data[0][frame->linesize[0] * (frame->height / 2) + 400], frame->data[1][frame->linesize[1] * (frame->height / 4) + 50], frame->data[1][frame->linesize[1] * (frame->height / 4) + 100], frame->data[1][frame->linesize[1] * (frame->height / 4) + 150], frame->data[1][frame->linesize[1] * (frame->height / 4) + 200], frame->data[2][frame->linesize[2] * (frame->height / 4) + 50], frame->data[2][frame->linesize[2] * (frame->height / 4) + 100], frame->data[2][frame->linesize[2] * (frame->height / 4) + 150], frame->data[2][frame->linesize[2] * (frame->height / 4) + 200]);
					FILE *o = fopen(opt_dump, "wb");
					if (o) {
						fwrite(dst, 2, (size_t)out_w * out_h, o);
						fclose(o);
					}
				}
				if (fb_mem) {
					fb_var.yoffset = page ? fb_var.yres : 0;
					ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
					if (fb_pages == 2)
						page ^= 1;
					p = now_ms() - t2;
				}
			} else {
				unconverted++;
			}

			t_dec += d, t_conv += c, t_pan += p;
			if (d + c + p > worst)
				worst = d + c + p;
			if (budget > 0 && d + c + p > budget)
				late++;
			frames++;
			av_frame_unref(frame);
			t0 = now_ms();
		}

		if (opt_seconds > 0 && now_ms() - start > opt_seconds * 1000.0)
			break;
	}

	if (frames == 0) {
		fprintf(stderr, "%s: no frame came out\n", path);
		goto out;
	}

	double per = (t_dec + t_conv + t_pan) / frames;
	double can = 1000.0 / per;
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;

	printf("%s\n", name);
	printf("  %s %s, %dx%d, %s, %.2f fps, %.0f kbit/s\n", codec->name, avcodec_profile_name(dec->codec_id, dec->profile) ? avcodec_profile_name(dec->codec_id, dec->profile) : "-", dec->width, dec->height, av_get_pix_fmt_name(dec->pix_fmt) ? av_get_pix_fmt_name(dec->pix_fmt) : "?", clip_fps, kbps);
	printf("  per frame: decode %.1f ms + convert %.1f ms + pan %.1f ms = %.1f ms   (worst frame %.1f ms)\n", t_dec / frames, t_conv / frames, t_pan / frames, per, worst);
	if (clip_fps > 0) {
		printf("  can do %.1f fps, clip needs %.1f  ->  %.0f%% of real time, %ld of %ld frames over budget: %s\n", can, clip_fps, 100.0 * can / clip_fps, late, frames, can >= clip_fps * 1.25 ? "PLAYS" : can >= clip_fps ? "MARGINAL" : "TOO SLOW");
	} else {
		printf("  can do %.1f fps (%ld frames)\n", can, frames);
	}
	if (unconverted)
		printf("  note: pixel format is not 4:2:0, so no frame was converted or shown\n");
	// One line a script can pick out.
	printf("CSV,%s,%s,%dx%d,%.2f,%.0f,%.2f,%.2f,%.2f,%.1f,%ld,%ld\n", name, codec->name, dec->width, dec->height, clip_fps, kbps, t_dec / frames, t_conv / frames, t_pan / frames, can, late, frames);
	fflush(stdout);
	rc = 0;

out:
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
	return rc;
}

int main(int argc, char **argv) {
	int c;
	while ((c = getopt(argc, argv, "t:j:nrlfo:")) != -1) {
		switch (c) {
		case 't':
			opt_seconds = atoi(optarg);
			break;
		case 'j':
			opt_threads = atoi(optarg);
			break;
		case 'n':
			opt_nofb = 1;
			break;
		case 'r':
			opt_upright = 1;
			break;
		case 'l':
			opt_noloop = 1;
			break;
		case 'f':
			opt_fast = 1;
			break;
		case 'o':
			opt_dump = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-t seconds] [-j threads] [-n] [-r] [-l] [-f] clip [clip ...]\n", argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-t seconds] [-j threads] [-n] [-r] [-l] [-f] clip [clip ...]\n", argv[0]);
		return 2;
	}

	av_log_set_level(AV_LOG_ERROR);

	if (!opt_nofb && fb_open() < 0) {
		fprintf(stderr, "no framebuffer: converting into memory instead\n");
		fb_mem = NULL;
	}
	if (fb_mem == MAP_FAILED)
		fb_mem = NULL;
	if (!fb_mem) {
		ram_page = malloc((size_t)out_w * out_h * 2);
		if (!ram_page)
			return 1;
	}

	printf("decprobe: %s, output %dx%d %s, %s, %d thread(s)%s%s\n", av_version_info(), out_w, out_h, fb_mem ? (fb_pages == 2 ? "framebuffer, two pages" : "framebuffer, one page") : "memory", opt_upright ? "upright" : "rotated", opt_threads, opt_noloop ? ", no loop filter" : "", opt_fast ? ", fast flag" : "");
	printf("CSV,clip,codec,size,clip_fps,kbps,decode_ms,convert_ms,pan_ms,can_fps,late,frames\n");

	int failed = 0;
	for (int i = optind; i < argc; i++) {
		if (fb_mem)
			memset(fb_mem, 0, fb_page_size * fb_pages);
		failed |= probe(argv[i]);
	}
	return failed;
}
