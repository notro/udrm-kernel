/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <linux/dma-buf.h>

#include <uapi/drm/udrm.h>

#include "udrm.h"

static int udrm_fb_create_event(struct drm_framebuffer *fb)
{
	struct udrm_device *udev = drm_to_udrm(fb->dev);
	struct udrm_event_fb ev = {
		.base = {
			.type = UDRM_EVENT_FB_CREATE,
			.length = sizeof(ev),
		},
		.fb_id = fb->base.id,
	};
	int ret;

	DRM_DEBUG_KMS("[FB:%d]\n", fb->base.id);

	/* Needed because the id is gone in &drm_framebuffer_funcs->destroy */
	ret = idr_alloc(&udev->idr, fb, fb->base.id, fb->base.id + 1, GFP_KERNEL);
	if (ret < 1) {
		DRM_ERROR("[FB:%d]: failed to allocate idr %d\n", fb->base.id, ret);
		return ret;
	}

	ret = udrm_send_event(udev, &ev);

	return ret;
}

static void tinydrm_merge_clips(struct drm_clip_rect *dst,
			 struct drm_clip_rect *src, unsigned int num_clips,
			 unsigned int flags, u32 max_width, u32 max_height)
{
	unsigned int i;

	if (!src || !num_clips) {
		dst->x1 = 0;
		dst->x2 = max_width;
		dst->y1 = 0;
		dst->y2 = max_height;
		return;
	}

	dst->x1 = ~0;
	dst->y1 = ~0;
	dst->x2 = 0;
	dst->y2 = 0;

	for (i = 0; i < num_clips; i++) {
		if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY)
			i++;
		dst->x1 = min(dst->x1, src[i].x1);
		dst->x2 = max(dst->x2, src[i].x2);
		dst->y1 = min(dst->y1, src[i].y1);
		dst->y2 = max(dst->y2, src[i].y2);
	}

	if (dst->x2 > max_width || dst->y2 > max_height ||
	    dst->x1 >= dst->x2 || dst->y1 >= dst->y2) {
		DRM_DEBUG_KMS("Illegal clip: x1=%u, x2=%u, y1=%u, y2=%u\n",
			      dst->x1, dst->x2, dst->y1, dst->y2);
		dst->x1 = 0;
		dst->y1 = 0;
		dst->x2 = max_width;
		dst->y2 = max_height;
	}
}

static void udrm_buf_memcpy(void *dst, void *vaddr, unsigned int pitch,
			    unsigned int cpp, struct drm_clip_rect *clip)
{
	void *src = vaddr + (clip->y1 * pitch) + (clip->x1 * cpp);
	size_t len = (clip->x2 - clip->x1) * cpp;
	unsigned int y;

	for (y = clip->y1; y < clip->y2; y++) {
		memcpy(dst, src, len);
		src += pitch;
		dst += len;
	}
}

static void udrm_buf_swab16(u16 *dst, void *vaddr, unsigned int pitch,
			    struct drm_clip_rect *clip)
{
	unsigned int x, y;
	u16 *src;

	for (y = clip->y1; y < clip->y2; y++) {
		src = vaddr + (y * pitch);
		src += clip->x1;
		for (x = clip->x1; x < clip->x2; x++)
			*dst++ = swab16(*src++);
	}
}

static void udrm_buf_emul_xrgb888(void *dst, void *vaddr, unsigned int pitch,
			u32 buf_mode, struct drm_clip_rect *clip)
{
	bool swap = (buf_mode & 7) == UDRM_BUF_MODE_SWAP_BYTES;
	u16 val16, *dst16 = dst;
	unsigned int x, y;
	u32 *src;

	for (y = clip->y1; y < clip->y2; y++) {
		src = vaddr + (y * pitch);
		src += clip->x1;
		for (x = clip->x1; x < clip->x2; x++) {
			val16 = ((*src & 0x00F80000) >> 8) |
				((*src & 0x0000FC00) >> 5) |
				((*src & 0x000000F8) >> 3);
			src++;
			if (swap)
				*dst16++ = swab16(val16);
			else
				*dst16++ = val16;
		}
	}
}

static bool udrm_fb_dirty_buf_copy(struct udrm_device *udev,
				   struct drm_framebuffer *fb,
				   struct drm_clip_rect *clip)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	unsigned int cpp = drm_format_plane_cpp(fb->pixel_format, 0);
	unsigned int pitch = fb->pitches[0];
	void *dst, *src = cma_obj->vaddr;
	int ret = 0;

	if (cma_obj->base.import_attach) {
		ret = dma_buf_begin_cpu_access(cma_obj->base.import_attach->dmabuf,
					       DMA_FROM_DEVICE);
		if (ret)
			return false;
	}

	dst = dma_buf_vmap(udev->dmabuf);
	if (!dst) {
		ret = -ENOMEM;
		goto out_end_access;
	}

	if (udev->emulate_xrgb8888_format &&
	    fb->pixel_format == DRM_FORMAT_XRGB8888) {
		udrm_buf_emul_xrgb888(dst, src, pitch, udev->buf_mode, clip);
		goto out;
	}

	switch (udev->buf_mode & 7) {
	case UDRM_BUF_MODE_PLAIN_COPY:
		udrm_buf_memcpy(dst, src, pitch, cpp, clip);
		break;
	case UDRM_BUF_MODE_SWAP_BYTES:
		/* FIXME support more */
		if (cpp == 2)
			udrm_buf_swab16(dst, src, pitch, clip);
		else
			ret = -EINVAL;
		break;
	default:
		ret = -EINVAL;
		break;
	}
out:
	dma_buf_vunmap(udev->dmabuf, dst);
out_end_access:
	if (cma_obj->base.import_attach)
		ret = dma_buf_end_cpu_access(cma_obj->base.import_attach->dmabuf,
					     DMA_FROM_DEVICE);

	return ret ? false : true;
}

static int udrm_fb_dirty(struct drm_framebuffer *fb,
			     struct drm_file *file_priv,
			     unsigned int flags, unsigned int color,
			     struct drm_clip_rect *clips,
			     unsigned int num_clips)
{
	struct udrm_device *udev = drm_to_udrm(fb->dev);
	struct drm_mode_fb_dirty_cmd *dirty;
	struct udrm_event_fb_dirty *ev;
	struct drm_clip_rect clip;
	size_t size_clips, size;
	int ret;

	DRM_DEBUG("\n\n\n");

	/* don't return -EINVAL, xorg will stop flushing */
	if (!udev->prepared)
		return 0;

	/* fbdev can flush even when we're not interested */
	if (udev->pipe.plane.fb != fb)
		return 0;

	/* Make sure to flush everything the first time */
	if (!udev->enabled) {
		clips = NULL;
		num_clips = 0;
	}

	/* FIXME: if (fb == tdev->fbdev_helper->fb) */
	if (udev->fbdev_helper && !udev->fbdev_fb_sent) {
		udrm_fb_create_event(udev->fbdev_helper->fb);
		udev->fbdev_fb_sent = true;
	}

	udev->enabled = true;

	/*
	 * FIXME: are there any apps/libs that pass more than one clip rect?
	 *        should we support passing multi clips to the driver?
	 */
	tinydrm_merge_clips(&clip, clips, num_clips, flags,
			    fb->width, fb->height);
	clips = &clip;
	num_clips = 1;

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n", fb->base.id,
		  clips->x1, clips->x2, clips->y1, clips->y2);

	if (udev->dmabuf && num_clips == 1)
		udrm_fb_dirty_buf_copy(udev, fb, clips);

	size_clips = num_clips * sizeof(struct drm_clip_rect);
	size = sizeof(struct udrm_event_fb_dirty) + size_clips;
	ev = kzalloc(size, GFP_KERNEL);
	if (!ev)
		return -ENOMEM;

	ev->base.type = UDRM_EVENT_FB_DIRTY;
	ev->base.length = size;
	dirty = &ev->fb_dirty_cmd;

	dirty->fb_id = fb->base.id;
	dirty->flags = flags;
	dirty->color = color;
	dirty->num_clips = num_clips;

	if (num_clips)
		memcpy(ev->clips, clips, size_clips);

	ret = udrm_send_event(udev, ev);
	if (ret)
		pr_err_once("Failed to update display %d\n", ret);

	return ret;
}

static void udrm_fb_destroy(struct drm_framebuffer *fb)
{
	struct udrm_device *udev = drm_to_udrm(fb->dev);
	struct udrm_event_fb ev = {
		.base = {
			.type = UDRM_EVENT_FB_DESTROY,
			.length = sizeof(ev),
		},
	};
	struct drm_framebuffer *iter;
	int id;

	DRM_DEBUG_KMS("[FB:%d]\n", fb->base.id);

	idr_for_each_entry(&udev->idr, iter, id) {
		if (fb == iter)
			break;
	}

	if (!iter) {
		DRM_ERROR("failed to find idr\n");
		return;
	}

	ev.fb_id = id;
	idr_remove(&udev->idr, id);

	udrm_send_event(udev, &ev);

	drm_fb_cma_destroy(fb);
}

static const struct drm_framebuffer_funcs udrm_fb_funcs = {
	.destroy	= udrm_fb_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= udrm_fb_dirty,
};

struct drm_framebuffer *
udrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		   const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_framebuffer *fb;
	int ret;

	fb = drm_fb_cma_create_with_funcs(drm, file_priv, mode_cmd,
					  &udrm_fb_funcs);
	if (IS_ERR(fb))
		return fb;

	DRM_DEBUG_KMS("[FB:%d] pixel_format: %s\n", fb->base.id,
		      drm_get_format_name(fb->pixel_format));

	ret = udrm_fb_create_event(fb);

	return fb;
}

static int udrm_fbdev_create(struct drm_fb_helper *helper,
				struct drm_fb_helper_surface_size *sizes)
{
	struct udrm_device *udev = drm_to_udrm(helper->dev);
	int ret;

	ret = drm_fbdev_cma_create_with_funcs(helper, sizes, &udrm_fb_funcs);
	if (ret)
		return ret;

	strncpy(helper->fbdev->fix.id, helper->dev->driver->name, 16);
	udev->fbdev_helper = helper;

	DRM_DEBUG_KMS("fbdev: [FB:%d] pixel_format=%s\n", helper->fb->base.id,
		      drm_get_format_name(helper->fb->pixel_format));

	return 0;
}

static const struct drm_fb_helper_funcs udrm_fb_helper_funcs = {
	.fb_probe = udrm_fbdev_create,
};

int udrm_fbdev_init(struct udrm_device *udev)
{
	struct drm_device *drm = &udev->drm;
	struct drm_fbdev_cma *fbdev;
	int bpp;

	DRM_DEBUG_KMS("\n");

	bpp = drm->mode_config.preferred_depth;
	fbdev = drm_fbdev_cma_init_with_funcs(drm, bpp ? bpp : 32,
					      drm->mode_config.num_crtc,
					      drm->mode_config.num_connector,
					      &udrm_fb_helper_funcs);
	if (IS_ERR(fbdev))
		return PTR_ERR(fbdev);

	udev->fbdev_cma = fbdev;

	return 0;
}

void udrm_fbdev_fini(struct udrm_device *udev)
{
	drm_fbdev_cma_fini(udev->fbdev_cma);
	udev->fbdev_cma = NULL;
	udev->fbdev_helper = NULL;
}
