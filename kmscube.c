/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Based on a egl cube test app originally written by Arvin Schnell */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "common.h"
#include "drm-common.h"

#ifdef HAVE_GST
#include <gst/gst.h>
GST_DEBUG_CATEGORY(kmscube_debug);
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static const char *device = "/dev/dri/card0";
static const char *video = NULL;
static enum mode mode = SMOOTH;
static uint64_t modifier = DRM_FORMAT_MOD_INVALID;
static int atomic = 0;

static const char *shortopts = "AD:M:m:V:l";

struct thread_data {
	struct drm *drm;
	enum mode mode;
	uint64_t modifier;
	const char *video;
};

static const struct option longopts[] = {
	{"atomic", no_argument,       0, 'A'},
	{"device", required_argument, 0, 'D'},
	{"mode",   required_argument, 0, 'M'},
	{"modifier", required_argument, 0, 'm'},
	{"video",  required_argument, 0, 'V'},
	{"lease", no_argument, 0, 'l' },
	{0, 0, 0, 0}
};

static void usage(const char *name)
{
	printf("Usage: %s [-ADMmV]\n"
			"\n"
			"options:\n"
			"    -A, --atomic             use atomic modesetting and fencing\n"
			"    -D, --device=DEVICE      use the given device\n"
			"    -M, --mode=MODE          specify mode, one of:\n"
			"        smooth    -  smooth shaded cube (default)\n"
			"        rgba      -  rgba textured cube\n"
			"        nv12-2img -  yuv textured (color conversion in shader)\n"
			"        nv12-1img -  yuv textured (single nv12 texture)\n"
			"    -m, --modifier=MODIFIER  hardcode the selected modifier\n"
			"    -V, --video=FILE         video textured cube\n"
			"    -l, lease		     Uses DRM leases to display two cubes\n",
			name);
}

void
run(int drm_fd, int leased_fd)
{
	struct gbm *gbm;
	struct drm *drm;
	struct egl *egl;

	if (atomic)
		drm = init_drm_atomic(drm_fd, leased_fd);
	else
		drm = init_drm_legacy(drm_fd, leased_fd);

	drm->fd = drm_fd;
	drm->leased_fd = leased_fd;

	if (!drm) {
		printf("failed to initialize %s DRM\n", atomic ? "atomic" : "legacy");
		exit(EXIT_FAILURE);
	}

	gbm = init_gbm(drm_fd, drm->mode->hdisplay, drm->mode->vdisplay,
			modifier);
	if (!gbm) {
		printf("failed to initialize GBM\n");
		return;
	}

	fprintf(stdout, "gbm @ %p\n", gbm);

	if (mode == SMOOTH)
		egl = init_cube_smooth(gbm);
	else if (mode == VIDEO)
		egl = init_cube_video(gbm, video);
	else
		egl = init_cube_tex(gbm, mode);

	if (!egl) {
		printf("failed to initialize EGL\n");
		return;
	}

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	drm->run(drm, gbm, egl);
}

static void *
thread_run(void *arg)
{
	int leased_fd = (intptr_t) arg;

	/* passing -1 will not "force" to seek other connector, meaning that we
	 * can use whatever objects have been leased */
	run(leased_fd, -1);
	return NULL;
}

int main(int argc, char *argv[])
{
	int lease = 0;
	int opt;

#ifdef HAVE_GST
	gst_init(&argc, &argv);
	GST_DEBUG_CATEGORY_INIT(kmscube_debug, "kmscube", 0, "kmscube video pipeline");
#endif

	while ((opt = getopt_long_only(argc, argv, shortopts, longopts, NULL)) != -1) {
		switch (opt) {
		case 'A':
			atomic = 1;
			break;
		case 'D':
			device = optarg;
			break;
		case 'M':
			if (strcmp(optarg, "smooth") == 0) {
				mode = SMOOTH;
			} else if (strcmp(optarg, "rgba") == 0) {
				mode = RGBA;
			} else if (strcmp(optarg, "nv12-2img") == 0) {
				mode = NV12_2IMG;
			} else if (strcmp(optarg, "nv12-1img") == 0) {
				mode = NV12_1IMG;
			} else {
				printf("invalid mode: %s\n", optarg);
				usage(argv[0]);
				return -1;
			}
			break;
		case 'm':
			modifier = strtoull(optarg, NULL, 0);
			break;
		case 'V':
			mode = VIDEO;
			video = optarg;
			break;
		case 'l':
			lease = 1;
			break;
		default:
			usage(argv[0]);
			return -1;
		}
	}

	int leased_fd = -1;
	int drm_fd = open(device, O_RDWR);

	if (lease) {
		struct drm_resources drm_resources;
		uint32_t objects[3];
		uint32_t lessee_id;
		pthread_t thread;
		int i = 0;

		find_drm_resources(&drm_resources, drm_fd, -1);

		if (drm_resources.connector_id) {
			objects[i++] = drm_resources.connector_id;
		}

		if (drm_resources.crtc_id) {
			objects[i++] = drm_resources.crtc_id;
		}

		/* 
		 * we create a lease using the connector_id and crtc_id. The
		 * thread will initialize drm using these values.
		 */
		leased_fd = drmModeCreateLease(drm_fd, objects, i, 0, &lessee_id);
		if (leased_fd < 0) {
			fprintf(stderr, "Failed to create lease\n");
			exit(EXIT_FAILURE);
		}

		pthread_create(&thread, NULL, thread_run, (void *) (intptr_t) leased_fd);

		/* wait until the first one starts */
		nanosleep(&(struct timespec) { .tv_sec = 1, .tv_nsec = 1024 * 1024 * 10 }, NULL);
	}


	/* we know have drm_fd and leased_fd. When we're going to initalize the
	 * drm system we'll choose the other connector available */
	run(drm_fd, leased_fd);
	return 0;
}
