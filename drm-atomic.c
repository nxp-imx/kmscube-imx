/*
 * Copyright (c) 2017 Rob Clark <rclark@redhat.com>
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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "drm-common.h"

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

static int add_connector_property(const struct drm *drm, drmModeAtomicReq *req, uint32_t obj_id,
					const char *name, uint64_t value)
{
	struct connector *obj = drm->connector;
	unsigned int i;
	int prop_id = 0;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
		printf("no connector property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_crtc_property(const struct drm *drm, drmModeAtomicReq *req, uint32_t obj_id,
				const char *name, uint64_t value)
{
	struct crtc *obj = drm->crtc;
	unsigned int i;
	int prop_id = -1;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
		printf("no crtc property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_plane_property(const struct drm *drm, drmModeAtomicReq *req, uint32_t obj_id,
				const char *name, uint64_t value)
{
	struct plane *obj = drm->plane;
	unsigned int i;
	int prop_id = -1;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}


	if (prop_id < 0) {
		printf("no plane property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int drm_atomic_commit(struct drm *drm, int drm_fd, uint32_t fb_id, uint32_t flags)
{
	drmModeAtomicReq *req;
	uint32_t plane_id = drm->plane->plane->plane_id;
	uint32_t blob_id;
	int ret;

	req = drmModeAtomicAlloc();

	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		if (add_connector_property(drm, req, drm->connector_id, "CRTC_ID",
						drm->crtc_id) < 0)
				return -1;

		if (drmModeCreatePropertyBlob(drm_fd, drm->mode, sizeof(*drm->mode),
					      &blob_id) != 0)
			return -1;

		if (add_crtc_property(drm, req, drm->crtc_id, "MODE_ID", blob_id) < 0)
			return -1;

		if (add_crtc_property(drm, req, drm->crtc_id, "ACTIVE", 1) < 0)
			return -1;
	}

	add_plane_property(drm, req, plane_id, "FB_ID", fb_id);
	add_plane_property(drm, req, plane_id, "CRTC_ID", drm->crtc_id);
	add_plane_property(drm, req, plane_id, "SRC_X", 0);
	add_plane_property(drm, req, plane_id, "SRC_Y", 0);
	add_plane_property(drm, req, plane_id, "SRC_W", drm->mode->hdisplay << 16);
	add_plane_property(drm, req, plane_id, "SRC_H", drm->mode->vdisplay << 16);
	add_plane_property(drm, req, plane_id, "CRTC_X", 0);
	add_plane_property(drm, req, plane_id, "CRTC_Y", 0);
	add_plane_property(drm, req, plane_id, "CRTC_W", drm->mode->hdisplay);
	add_plane_property(drm, req, plane_id, "CRTC_H", drm->mode->vdisplay);

	if (drm->kms_in_fence_fd != -1) {
		add_crtc_property(drm, req, drm->crtc_id, "OUT_FENCE_PTR",
				VOID2U64(&drm->kms_out_fence_fd));
		add_plane_property(drm, req, plane_id, "IN_FENCE_FD", drm->kms_in_fence_fd);
	}

	ret = drmModeAtomicCommit(drm_fd, req, flags, NULL);
	if (ret)
		goto out;

	if (drm->kms_in_fence_fd != -1) {
		close(drm->kms_in_fence_fd);
		drm->kms_in_fence_fd = -1;
	}

out:
	drmModeAtomicFree(req);

	return ret;
}

static EGLSyncKHR create_fence(const struct egl *egl, int fd)
{
	EGLint attrib_list[] = {
		EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd,
		EGL_NONE,
	};
	EGLSyncKHR fence = egl->eglCreateSyncKHR(egl->display,
			EGL_SYNC_NATIVE_FENCE_ANDROID, attrib_list);
	assert(fence);
	return fence;
}

static int atomic_run(struct drm *drm, const struct gbm *gbm, struct egl *egl)
{
	struct gbm_bo *bo = NULL;
	struct drm_fb *fb;
	uint32_t i = 0;
	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
	int ret;

	if (egl_check(egl, eglDupNativeFenceFDANDROID) ||
	    egl_check(egl, eglCreateSyncKHR) ||
	    egl_check(egl, eglDestroySyncKHR) ||
	    egl_check(egl, eglWaitSyncKHR) ||
	    egl_check(egl, eglClientWaitSyncKHR))
		return -1;

	/* Allow a modeset change for the first commit only. */
	flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

	while (1) {
		struct gbm_bo *next_bo;
		EGLSyncKHR gpu_fence = NULL;   /* out-fence from gpu, in-fence to kms */
		EGLSyncKHR kms_fence = NULL;   /* in-fence to gpu, out-fence from kms */

		if (drm->kms_out_fence_fd != -1) {
			kms_fence = create_fence(egl, drm->kms_out_fence_fd);
			assert(kms_fence);

			/* driver now has ownership of the fence fd: */
			drm->kms_out_fence_fd = -1;

			/* wait "on the gpu" (ie. this won't necessarily block, but
			 * will block the rendering until fence is signaled), until
			 * the previous pageflip completes so we don't render into
			 * the buffer that is still on screen.
			 */
			egl->eglWaitSyncKHR(egl->display, kms_fence, 0);
		}

		egl->draw(egl, i++);

		/* insert fence to be singled in cmdstream.. this fence will be
		 * signaled when gpu rendering done
		 */
		gpu_fence = create_fence(egl, EGL_NO_NATIVE_FENCE_FD_ANDROID);
		assert(gpu_fence);

		eglSwapBuffers(egl->display, egl->surface);

		/* after swapbuffers, gpu_fence should be flushed, so safe
		 * to get fd:
		 */
		drm->kms_in_fence_fd = egl->eglDupNativeFenceFDANDROID(egl->display, gpu_fence);
		egl->eglDestroySyncKHR(egl->display, gpu_fence);
		assert(drm->kms_in_fence_fd != -1);

		next_bo = gbm_surface_lock_front_buffer(gbm->surface);
		if (!next_bo) {
			printf("Failed to lock frontbuffer\n");
			return -1;
		}
		fb = drm_fb_get_from_bo(next_bo);
		if (!fb) {
			printf("Failed to get a new framebuffer BO\n");
			return -1;
		}

		if (kms_fence) {
			EGLint status;

			/* Wait on the CPU side for the _previous_ commit to
			 * complete before we post the flip through KMS, as
			 * atomic will reject the commit if we post a new one
			 * whilst the previous one is still pending.
			 */
			do {
				status = egl->eglClientWaitSyncKHR(egl->display,
						kms_fence,
						0,
						EGL_FOREVER_KHR);
			} while (status != EGL_CONDITION_SATISFIED_KHR);

			egl->eglDestroySyncKHR(egl->display, kms_fence);
		}

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */
		ret = drm_atomic_commit(drm, drm->fd, fb->fb_id, flags);
		if (ret) {
			printf("failed to commit: %s\n", strerror(errno));
			return -1;
		}

		/* release last buffer to render on again: */
		if (bo)
			gbm_surface_release_buffer(gbm->surface, bo);
		bo = next_bo;

		/* Allow a modeset change for the first commit only. */
		flags &= ~(DRM_MODE_ATOMIC_ALLOW_MODESET);
	}

	return ret;
}

/* Pick a plane.. something that at a minimum can be connected to
 * the chosen crtc, but prefer primary plane.
 *
 * Seems like there is some room for a drmModeObjectGetNamedProperty()
 * type helper in libdrm->.
 */
static int get_plane_id(struct drm *drm, int drm_fd)
{
	drmModePlaneResPtr plane_resources;
	uint32_t i, j;
	int ret = -EINVAL;
	int found_primary = 0;

	plane_resources = drmModeGetPlaneResources(drm_fd);
	if (!plane_resources) {
		printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; (i < plane_resources->count_planes) && !found_primary; i++) {
		uint32_t id = plane_resources->planes[i];
		drmModePlanePtr plane = drmModeGetPlane(drm_fd, id);
		if (!plane) {
			printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
			continue;
		}

		if (plane->possible_crtcs & (1 << drm->crtc_index)) {
			drmModeObjectPropertiesPtr props =
				drmModeObjectGetProperties(drm_fd, id, DRM_MODE_OBJECT_PLANE);

			/* primary or not, this plane is good enough to use: */
			ret = id;

			for (j = 0; j < props->count_props; j++) {
				drmModePropertyPtr p =
					drmModeGetProperty(drm_fd, props->props[j]);

				if ((strcmp(p->name, "type") == 0) &&
						(props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)) {
					/* found our primary plane, lets use that: */
					found_primary = 1;
				}

				drmModeFreeProperty(p);
			}

			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_resources);

	return ret;
}

struct drm * init_drm_atomic(int drm_fd, int leased_fd)
{
	uint32_t plane_id;
	int ret;

	struct drm *drm = calloc(1, sizeof(*drm));

	ret = init_drm(drm, drm_fd, leased_fd);
	if (ret)
		return NULL;

	ret = drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		printf("no atomic modesetting support: %s\n", strerror(errno));
		return NULL;
	}

	ret = get_plane_id(drm, drm_fd);
	if (!ret) {
		printf("could not find a suitable plane\n");
		return NULL;
	} else {
		plane_id = ret;
	}

	/* We only do single plane to single crtc to single connector, no
	 * fancy multi-monitor or multi-plane stuff.  So just grab the
	 * plane/crtc/connector property info for one of each:
	 */
	drm->plane = calloc(1, sizeof(*drm->plane));
	drm->crtc = calloc(1, sizeof(*drm->crtc));
	drm->connector = calloc(1, sizeof(*drm->connector));

#define get_resource(type, Type, id) do { 					\
		drm->type->type = drmModeGet##Type(drm_fd, id);		\
		if (!drm->type->type) {								\
			printf("could not get %s %i: %s\n",				\
					#type, id, strerror(errno));			\
			return NULL;									\
		}													\
	} while (0)

	get_resource(plane, Plane, plane_id);
	get_resource(crtc, Crtc, drm->crtc_id);
	get_resource(connector, Connector, drm->connector_id);

#define get_properties(type, TYPE, id) do {						\
		uint32_t i;												\
		drm->type->props = drmModeObjectGetProperties(drm_fd,	\
				id, DRM_MODE_OBJECT_##TYPE);					\
		if (!drm->type->props) {									\
			printf("could not get %s %u properties: %s\n",		\
					#type, id, strerror(errno));				\
			return NULL;										\
		}														\
		drm->type->props_info = calloc(drm->type->props->count_props, \
				sizeof(drm->type->props_info));					\
		for (i = 0; i < drm->type->props->count_props; i++) {	\
			drm->type->props_info[i] = drmModeGetProperty(drm_fd,\
					drm->type->props->props[i]);					\
		}														\
	} while (0)


	get_properties(plane, PLANE, plane_id);
	get_properties(crtc, CRTC, drm->crtc_id);
	get_properties(connector, CONNECTOR, drm->connector_id);
	drm->kms_out_fence_fd = -1;

	drm->run = atomic_run;

	return drm;
}
