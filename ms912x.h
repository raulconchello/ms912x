/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef MS912X_H
#define MS912X_H

#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/timer_types.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#define DRIVER_NAME "ms912x"
#define DRIVER_DESC "MacroSilicon USB to VGA/HDMI"

#define DRIVER_MAJOR 1
#define DRIVER_MINOR 0

#define MS912X_BULK_OUT_ENDPOINT	4

/* Longest a new frame waits for a keepalive resend of its buffer */
#define MS912X_KEEPALIVE_WAIT_MS	500

#define MS912X_REQ_TYPE_WRITE_6_BYTES	0xa6
#define MS912X_REQ_TYPE_READ_BYTE	0xb5
#define MS912X_REQ_TYPE_WRITE_BYTE	0xb6
#define MS912X_REQ_TYPE_READ_FLASH	0xf5

#define MS912X_REG_VIDEO_PORT		0x0031
#define MS912X_REG_DISPLAY_STATUS	0x0032
#define MS912X_REG_EDID_BASE		0xc000

#define MS912X_REG_MODE_SEQUENCE_0	0x0030
#define MS912X_REG_MODE_SEQUENCE_1	0x0033
#define MS912X_REG_MODE_SEQUENCE_2	0xc620

/* Video sub-commands, names from the MacroSilicon vendor driver */
#define MS912X_CMD_RESOLUTION		0x01 /* video in info */
#define MS912X_CMD_MODE			0x02 /* video out info */
#define MS912X_CMD_UNKNOWN1		0x03 /* transfer mode */
#define MS912X_CMD_UNKNOWN2		0x04 /* transfer enable */
#define MS912X_CMD_OUTPUT_ENABLE	0x05 /* video enable */
#define MS912X_CMD_POWER		0x07

#define MS912X_TRANS_MODE_FRAME			0
#define MS912X_TRANS_MODE_MANUAL_BLOCK		3
#define MS912X_TRANS_MODE_BYPASS_FRAME		4
#define MS912X_TRANS_MODE_BYPASS_MANUAL_BLOCK	5

#define MS913X_REG_CHIP_ID			0xff00
#define MS912X_REG_CHIP_ID			0xf000
#define MS913X_CHIP_ID_SIGNATURE_MSB		0x13
#define MS912X_CHIP_ID_SIGNATURE_MSB		0x16
#define MS91XX_CHIP_ID_SIGNATURE_LSB		0x0a
#define MS913X_CUSTOM_TIMING_BASE		0xfc50
#define MS912X_CUSTOM_TIMING_BASE		0x1c00
#define MS912X_CUSTOM_TIMING_OFFSET		0x10
#define MS912X_CUSTOM_TIMING_STRIDE		0x20

#define MS912X_TIMING_PROGRESSIVE		BIT(0)
#define MS912X_TIMING_POSITIVE_HSYNC		BIT(1)
#define MS912X_TIMING_POSITIVE_VSYNC		BIT(2)

enum ms912x_video_port {
	MS912X_VIDEO_PORT_CVBS = 0,
	MS912X_VIDEO_PORT_SVIDEO = 1,
	MS912X_VIDEO_PORT_VGA = 2,
	MS912X_VIDEO_PORT_YPBPR = 3,
	MS912X_VIDEO_PORT_CVBS_SVIDEO = 4,
	MS912X_VIDEO_PORT_HDMI = 5,
	MS912X_VIDEO_PORT_DIGITAL = 6,
	MS912X_VIDEO_PORT_UNKNOWN = 0xff,
};

struct drm_pending_vblank_event;

struct ms912x_usb_request {
	void *transfer_buffer;
	/* flip event completed when this transfer is done (may be NULL) */
	struct drm_pending_vblank_event *event;
	ktime_t t_start; /* conversion start, for latency stats */
	ktime_t t_queued; /* conversion done */
	struct hrtimer flip_timer; /* just-in-time flip completion */
	struct ms912x_device *ms912x;
	size_t transfer_len;
	struct sg_table transfer_sgt;
	struct usb_sg_request sgr;
	struct work_struct work;
	struct timer_list timer;
	struct completion done;
	bool keepalive; /* being resent by the keepalive */
};

struct ms912x_mode {
	int width;
	int height;
	int hz;
	int mode;
};

struct ms912x_custom_mode {
	struct ms912x_mode mode;
	struct drm_display_mode display_mode;
};

#define MS912X_PIXFMT_RGB888		0x11
#define MS912X_PIXFMT_UYVY		0x22
#define MS912X_BYTE_SELECT_UYVY		0x00

struct ms912x_stat {
	u64 sum_ns;
	u64 max_ns;
	u32 n;
};

enum ms912x_flip_mode {
	MS912X_FLIP_IMMEDIATE = 0,	/* after conversion: max throughput */
	MS912X_FLIP_ON_DONE = 1,	/* after the USB transfer */
	MS912X_FLIP_JIT = 2,		/* predicted, see ms912x_flip_deadline() */
};

struct ms912x_stats {
	struct ms912x_stat cpu_access;	/* begin_cpu_access (fence wait/flush) */
	struct ms912x_stat convert;	/* XRGB -> UYVY */
	struct ms912x_stat queue;	/* conversion done -> USB submit */
	struct ms912x_stat usb;		/* USB bulk transfer */
	struct ms912x_stat total;	/* conversion start -> USB done */
	struct ms912x_stat lead;	/* flip event -> next frame converted */
	u32 flip_early;			/* flips completed before USB done */
	u64 bytes;
	u32 frames;
	u32 full_frames;
	u32 dropped;
	u32 errors;
};

#define MS912X_MODE(w, h, z, m)                                               \
	{                                                                      \
		.width = w, .height = h, .hz = z, .mode = m                    \
	}

struct ms912x_device {
	struct drm_device drm;
	struct usb_interface *intf;
	unsigned int bulk_pipe;
	enum ms912x_video_port port_type;
	bool is_913x;		/* MS9132/MS9133/MS9135 register layout */
	bool video_pending;	/* video output enabled after first frame */
	bool use_avx2;
	struct ms912x_stats stats;

	/*
	 * Keepalive: the device turns its video output off when it receives
	 * no frames for a few seconds, so resend the last transfer when the
	 * screen is idle. That transfer carries the damage of the last two
	 * frames, so it brings either of the device's buffers up to date.
	 */
	struct ms912x_usb_request *last_request;
	struct delayed_work keepalive_work;
	u32 keepalives;

	/* Just-in-time flip pacing state */
	unsigned int flip_mode;
	ktime_t t_last_flip;	/* when the last flip event was sent */
	u64 lead_ns;		/* EWMA: flip event -> next frame ready to send */
	u64 usb_ps_per_byte;	/* EWMA: USB transfer cost, picoseconds/byte */
	struct workqueue_struct *workqueue;
	struct workqueue_struct *conv_workqueue;
	unsigned int conv_threads;
	/* Serializes HID control request/response sequences */
	struct mutex ctrl_lock;

	struct drm_connector connector;
	struct drm_encoder encoder;
	struct drm_crtc crtc;
	struct drm_plane plane;

	struct ms912x_custom_mode custom_modes[2];
	unsigned int num_custom_modes;

	struct drm_rect update_rect;

	/* Double buffer to allow memcpy and transfer
	 * to happen in parallel
	 */
	int current_request;
	struct ms912x_usb_request requests[2];
};

struct ms912x_request {
	u8 type;
	__be16 addr;
	u8 data[5];
} __packed;

struct ms912x_write_request {
	u8 type;
	u8 addr;
	u8 data[6];
} __packed;

struct ms912x_resolution_request {
	__be16 width;
	__be16 height;
	u8 pixel_format;
	u8 byte_select;
} __packed;

struct ms912x_mode_request {
	u8 mode;
	u8 pixel_format;
	__be16 width;
	__be16 height;
} __packed;

struct ms912x_frame_update_header {
	__be16 marker;
	u8 position[3]; /* x:y, packed 12-bit big-endian values */
	u8 dimensions[3]; /* width:height, packed 12-bit big-endian values */
} __packed;

struct ms912x_flash_read_request {
	u8 type;
	u8 addr[3];
	u8 reserved[4];
} __packed;

struct ms912x_custom_timing_record {
	u8 vic;
	u8 polarity;
	__le16 htotal;
	__le16 vtotal;
	__le16 hactive;
	__le16 vactive;
	__le16 pixclk;
	__le16 vfreq;
	__le16 hoffset;
	__le16 voffset;
	__le16 hsyncwidth;
	__le16 vsyncwidth;
} __packed;

#define MS912X_FRAME_OVERHEAD 16
#define MS912X_MAX_WIDTH 1920
#define MS912X_MAX_HEIGHT 1200
#define MS912X_MAX_TRANSFER_LEN \
	(MS912X_MAX_WIDTH * MS912X_MAX_HEIGHT * 2 + MS912X_FRAME_OVERHEAD)

#define to_ms912x(x) container_of(x, struct ms912x_device, drm)

int ms912x_read_byte(struct ms912x_device *ms912x, u16 address);
int ms912x_read_custom_timing(struct ms912x_device *ms912x);
int ms912x_connector_init(struct ms912x_device *ms912x);
int ms912x_set_resolution(struct ms912x_device *ms912x,
			  const struct ms912x_mode *mode);

int ms912x_power_on(struct ms912x_device *ms912x);
int ms912x_power_off(struct ms912x_device *ms912x);
int ms912x_detect_chip(struct ms912x_device *ms912x);
int ms912x_hw_enable(struct ms912x_device *ms912x,
		     const struct ms912x_mode *mode, u8 trans_mode);
int ms912x_hw_start_video(struct ms912x_device *ms912x);
int ms912x_hw_disable(struct ms912x_device *ms912x);
void ms912x_stat_add(struct ms912x_stat *stat, u64 ns);

int ms912x_fb_send_rect(struct drm_framebuffer *fb, const struct iosys_map *map,
			struct drm_format_conv_state *fmtcnv_state,
			struct drm_rect *rect,
			struct drm_pending_vblank_event **event);
extern uint ms912x_keepalive_ms;
extern bool ms912x_debug_hold;

void ms912x_keepalive_work(struct work_struct *work);
void ms912x_keepalive_arm(struct ms912x_device *ms912x);
int ms912x_send_fill(struct ms912x_device *ms912x, unsigned int width,
		     unsigned int height, u32 rgb, u64 *t_done_ns);
void ms912x_send_event(struct ms912x_device *ms912x,
		       struct drm_pending_vblank_event **event);

void ms912x_free_request(struct ms912x_usb_request *request);
int ms912x_init_request(struct ms912x_device *ms912x,
			struct ms912x_usb_request *request, size_t len);
#endif
