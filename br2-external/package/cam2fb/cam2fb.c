/* SPDX-License-Identifier: MIT
 *
 * cam2fb — stream a UVC YUYV camera onto an RGB565 framebuffer.
 * Tuned for ESP32-S31 Function-CoreBoard-1 (320x240 /dev/fb0, ~16 MiB RAM).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_BUFS 3
#define CLAMP(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))

static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
	(void)sig;
	g_run = 0;
}

static uint16_t yuv_to_rgb565(int y, int u, int v)
{
	int c = y - 16;
	int d = u - 128;
	int e = v - 128;
	int r = (298 * c + 409 * e + 128) >> 8;
	int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
	int b = (298 * c + 516 * d + 128) >> 8;

	r = CLAMP(r);
	g = CLAMP(g);
	b = CLAMP(b);
	return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void yuyv_to_rgb565(const uint8_t *src, uint16_t *dst,
			   unsigned width, unsigned height)
{
	unsigned x, y;

	for (y = 0; y < height; y++) {
		const uint8_t *row = src + y * width * 2;
		uint16_t *out = dst + y * width;

		for (x = 0; x < width; x += 2) {
			int y0 = row[0], u = row[1], y1 = row[2], v = row[3];

			out[x] = yuv_to_rgb565(y0, u, v);
			out[x + 1] = yuv_to_rgb565(y1, u, v);
			row += 4;
		}
	}
}

/* Nearest-neighbor scale from cam WxH RGB565 into fb fbw x fbh. */
static void blit_scaled(const uint16_t *src, unsigned sw, unsigned sh,
			uint16_t *fb, unsigned fbw, unsigned fbh)
{
	unsigned dy, dx;

	for (dy = 0; dy < fbh; dy++) {
		unsigned sy = dy * sh / fbh;
		const uint16_t *srow = src + sy * sw;
		uint16_t *drow = fb + dy * fbw;

		for (dx = 0; dx < fbw; dx++)
			drow[dx] = srow[dx * sw / fbw];
	}
}

static int unbind_fbcon(void)
{
	const char *paths[] = {
		"/sys/class/vtconsole/vtcon1/bind",
		"/sys/class/vtconsole/vtcon0/bind",
		NULL,
	};
	unsigned i;

	for (i = 0; paths[i]; i++) {
		int fd = open(paths[i], O_WRONLY);

		if (fd < 0)
			continue;
		if (write(fd, "0\n", 2) == 2) {
			close(fd);
			return 0;
		}
		close(fd);
	}
	return -1;
}

struct cam_buf {
	void *start;
	size_t len;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static int open_fb(const char *path, uint16_t **map, unsigned *w, unsigned *h,
		   size_t *map_len)
{
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return -1;
	}
	if (xioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0 ||
	    xioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		perror("fb ioctl");
		close(fd);
		return -1;
	}
	if (vinfo.bits_per_pixel != 16) {
		fprintf(stderr, "fb0 is %u bpp; need RGB565\n",
			vinfo.bits_per_pixel);
		close(fd);
		return -1;
	}
	*w = vinfo.xres;
	*h = vinfo.yres;
	*map_len = finfo.smem_len;
	*map = mmap(NULL, *map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*map == MAP_FAILED) {
		perror("mmap fb");
		close(fd);
		return -1;
	}
	/* Keep fd open for the mmap lifetime; caller owns it via return. */
	return fd;
}

static int try_fmt(int fd, unsigned w, unsigned h)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		return -1;
	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *video = "/dev/video0";
	const char *fbdev = "/dev/fb0";
	static const unsigned try_sizes[][2] = {
		{ 320, 240 },
		{ 640, 480 },
		{ 160, 120 },
		{ 800, 600 },
		{ 1280, 720 },
	};
	int vfd = -1, fbfd = -1;
	uint16_t *fb = NULL;
	size_t fb_len = 0;
	unsigned fbw = 0, fbh = 0;
	unsigned cam_w = 0, cam_h = 0;
	struct v4l2_capability cap;
	struct v4l2_requestbuffers req;
	struct cam_buf bufs[MAX_BUFS];
	unsigned nbufs = 0, i;
	uint16_t *rgb = NULL;
	size_t rgb_bytes;
	enum v4l2_buf_type type;
	int rc = 1;

	if (argc >= 2)
		video = argv[1];
	if (argc >= 3)
		fbdev = argv[2];

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	unbind_fbcon();

	fbfd = open_fb(fbdev, &fb, &fbw, &fbh, &fb_len);
	if (fbfd < 0)
		return 1;
	printf("fb %ux%u rgb565\n", fbw, fbh);

	vfd = open(video, O_RDWR | O_NONBLOCK);
	if (vfd < 0) {
		perror(video);
		goto out;
	}
	if (xioctl(vfd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		goto out;
	}
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		fprintf(stderr, "%s is not a capture device\n", video);
		goto out;
	}

	for (i = 0; i < sizeof(try_sizes) / sizeof(try_sizes[0]); i++) {
		if (try_fmt(vfd, try_sizes[i][0], try_sizes[i][1]) == 0) {
			struct v4l2_format fmt;

			memset(&fmt, 0, sizeof(fmt));
			fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			xioctl(vfd, VIDIOC_G_FMT, &fmt);
			cam_w = fmt.fmt.pix.width;
			cam_h = fmt.fmt.pix.height;
			break;
		}
	}
	if (!cam_w) {
		fprintf(stderr, "camera has no YUYV mode we can use\n");
		goto out;
	}
	printf("cam %ux%u YUYV on %s\n", cam_w, cam_h, video);

	memset(&req, 0, sizeof(req));
	req.count = 2;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		perror("VIDIOC_REQBUFS");
		goto out;
	}
	nbufs = req.count;
	if (nbufs > MAX_BUFS)
		nbufs = MAX_BUFS;

	memset(bufs, 0, sizeof(bufs));
	for (i = 0; i < nbufs; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (xioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			goto out;
		}
		bufs[i].len = buf.length;
		bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, vfd, buf.m.offset);
		if (bufs[i].start == MAP_FAILED) {
			perror("mmap cam");
			bufs[i].start = NULL;
			goto out;
		}
		if (xioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			goto out;
		}
	}

	rgb_bytes = (size_t)cam_w * cam_h * sizeof(uint16_t);
	rgb = malloc(rgb_bytes);
	if (!rgb) {
		perror("malloc");
		goto out;
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		goto out;
	}

	printf("streaming — Ctrl-C to stop\n");
	while (g_run) {
		struct v4l2_buffer buf;
		fd_set fds;
		struct timeval tv;
		int sel;

		FD_ZERO(&fds);
		FD_SET(vfd, &fds);
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		sel = select(vfd + 1, &fds, NULL, NULL, &tv);
		if (sel < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			break;
		}
		if (!sel)
			continue;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				continue;
			perror("VIDIOC_DQBUF");
			break;
		}

		yuyv_to_rgb565(bufs[buf.index].start, rgb, cam_w, cam_h);
		if (cam_w == fbw && cam_h == fbh)
			memcpy(fb, rgb, rgb_bytes);
		else
			blit_scaled(rgb, cam_w, cam_h, fb, fbw, fbh);

		if (xioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			break;
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(vfd, VIDIOC_STREAMOFF, &type);
	rc = 0;

out:
	free(rgb);
	for (i = 0; i < nbufs; i++) {
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].len);
	}
	if (vfd >= 0)
		close(vfd);
	if (fb && fb != MAP_FAILED)
		munmap(fb, fb_len);
	if (fbfd >= 0)
		close(fbfd);
	return rc;
}
