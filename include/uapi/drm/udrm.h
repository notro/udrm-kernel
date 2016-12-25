/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _UAPI__UDRM_H_
#define _UAPI__UDRM_H_

#if defined(__KERNEL__)
#include <uapi/drm/drm_mode.h>
#include <linux/types.h>
#else
#include <linux/types.h>
#include <drm/drm_mode.h>
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#define UDRM_MAX_NAME_SIZE    80

/* FIXME: Update Documentation/ioctl/ioctl-number.txt */
#define UDRM_IOCTL_BASE       0xB5

#define UDRM_BUF_MODE_NONE		0
#define UDRM_BUF_MODE_PLAIN_COPY	1
#define UDRM_BUF_MODE_SWAP_BYTES	2

#define UDRM_BUF_MODE_EMUL_XRGB8888	BIT(8)

struct udrm_dev_create {
	char name[UDRM_MAX_NAME_SIZE];
	struct drm_mode_modeinfo mode;
	__u64 formats;
	__u32 num_formats;
	__u32 buf_mode;
	__s32 buf_fd;

	__u32 index;
};

#define UDRM_DEV_CREATE       _IOWR(UDRM_IOCTL_BASE, 1, struct udrm_dev_create)

struct udrm_event {
	__u32 type;
	__u32 length;
};

#define UDRM_EVENT_PIPE_ENABLE	1
#define UDRM_EVENT_PIPE_DISABLE	2

#define UDRM_EVENT_FB_CREATE	3
#define UDRM_EVENT_FB_DESTROY	4

struct udrm_event_fb {
	struct udrm_event base;
	__u32 fb_id;
};

#define UDRM_EVENT_FB_DIRTY 	5

struct udrm_event_fb_dirty {
	struct udrm_event base;
	struct drm_mode_fb_dirty_cmd fb_dirty_cmd;
	struct drm_clip_rect clips[];
};

#define UDRM_PRIME_HANDLE_TO_FD 0x01
#define DRM_IOCTL_UDRM_PRIME_HANDLE_TO_FD    DRM_IOWR(DRM_COMMAND_BASE + UDRM_PRIME_HANDLE_TO_FD, struct drm_prime_handle)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI__UDRM_H_ */
