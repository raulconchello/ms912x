// SPDX-License-Identifier: GPL-2.0-only

#include <asm/byteorder.h>
#include <linux/align.h>
#include <linux/completion.h>
#include <linux/container_of.h>
#include <linux/dma-direction.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#ifdef MS912X_HAVE_AVX2
#include <asm/fpu/api.h>
#endif

#include "ms912x.h"
#include "ms912x_convert.h"

void ms912x_stat_add(struct ms912x_stat *stat, u64 ns)
{
	stat->sum_ns += ns;
	stat->n++;
	if (ns > stat->max_ns)
		stat->max_ns = ns;
}

static void ms912x_request_timeout(struct timer_list *t)
{
	struct ms912x_usb_request *request = timer_container_of(request, t, timer);

	usb_sg_cancel(&request->sgr);
}

void ms912x_send_event(struct ms912x_device *ms912x,
		       struct drm_pending_vblank_event **event)
{
	struct drm_device *dev = &ms912x->drm;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (*event) {
		drm_crtc_send_vblank_event(&ms912x->crtc, *event);
		*event = NULL;
		ms912x->t_last_flip = ktime_get();
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static enum hrtimer_restart ms912x_flip_timer_fn(struct hrtimer *timer)
{
	struct ms912x_usb_request *request =
		container_of(timer, struct ms912x_usb_request, flip_timer);

	if (request->event)
		request->ms912x->stats.flip_early++;
	ms912x_send_event(request->ms912x, &request->event);
	return HRTIMER_NORESTART;
}

#define MS912X_EWMA(avg, sample) ((avg) - ((avg) >> 3) + ((sample) >> 3))
/* Lead samples above this are idle gaps, not render time. */
#define MS912X_LEAD_MAX_NS (40 * NSEC_PER_MSEC)

/*
 * Just-in-time flip completion.
 *
 * The device has no vblank; a flip completes when we say so, and the
 * compositor starts its next frame right after that. Completing it after
 * the USB transfer leaves the link idle while the compositor renders,
 * completing it early lets frames queue up behind the transfer and go
 * stale. So predict when the transfer will finish and complete the flip
 * one "lead" earlier, the lead being the measured time from completing a
 * flip until the next frame has been rendered and converted. The next
 * frame then arrives just as the link becomes free.
 */
static ktime_t ms912x_flip_deadline(struct ms912x_device *ms912x,
				    struct ms912x_usb_request *request,
				    ktime_t t_submit)
{
	u64 usb_ns = div_u64((u64)request->transfer_len *
				     ms912x->usb_ps_per_byte, 1000);

	return ktime_add_ns(t_submit, usb_ns) - (s64)ms912x->lead_ns;
}

static void ms912x_flip_arm(struct ms912x_device *ms912x,
			    struct ms912x_usb_request *request,
			    ktime_t t_submit)
{
	ktime_t deadline;

	if (!request->event)
		return;

	switch (ms912x->flip_mode) {
	case MS912X_FLIP_IMMEDIATE:
		ms912x_send_event(ms912x, &request->event);
		return;
	case MS912X_FLIP_JIT:
		deadline = ms912x_flip_deadline(ms912x, request, t_submit);
		if (ktime_compare(deadline, ktime_get()) <= 0) {
			ms912x->stats.flip_early++;
			ms912x_send_event(ms912x, &request->event);
		} else {
			hrtimer_start(&request->flip_timer, deadline,
				      HRTIMER_MODE_ABS_HARD);
		}
		return;
	default:
		return; /* sent when the transfer is done */
	}
}

static void ms912x_update_estimates(struct ms912x_device *ms912x,
				    struct ms912x_usb_request *request,
				    ktime_t t_submit, ktime_t t_done)
{
	u64 usb_ns = ktime_to_ns(ktime_sub(t_done, t_submit));
	u64 ps_per_byte;

	/* Only large transfers say something about the link throughput. */
	if (request->transfer_len >= 256 * 1024) {
		ps_per_byte = div_u64(usb_ns * 1000, request->transfer_len);
		ms912x->usb_ps_per_byte =
			MS912X_EWMA(ms912x->usb_ps_per_byte, ps_per_byte);
	}
}

static int ms912x_request_send(struct ms912x_usb_request *request)
{
	struct ms912x_device *ms912x = request->ms912x;
	struct usb_device *usbdev = interface_to_usbdev(ms912x->intf);
	struct usb_sg_request *sgr = &request->sgr;
	struct sg_table *transfer_sgt = &request->transfer_sgt;
	unsigned int maxpacket;
	int ret, actual;

	ret = usb_sg_init(sgr, usbdev, ms912x->bulk_pipe, 0, transfer_sgt->sgl,
			  transfer_sgt->nents, request->transfer_len,
			  GFP_KERNEL);
	if (ret)
		return ret;

	mod_timer(&request->timer, jiffies + msecs_to_jiffies(5000));
	usb_sg_wait(sgr);

	if (!timer_delete_sync(&request->timer))
		return -ETIMEDOUT;
	if (sgr->status < 0)
		return sgr->status;
	if (sgr->bytes != request->transfer_len)
		return -EIO;

	/* Terminate the bulk transfer like the vendor driver does. */
	maxpacket = usb_maxpacket(usbdev, ms912x->bulk_pipe);
	if (maxpacket && !(request->transfer_len % maxpacket))
		usb_bulk_msg(usbdev, ms912x->bulk_pipe, NULL, 0, &actual, 100);

	return 0;
}

static void ms912x_request_work(struct work_struct *work)
{
	struct ms912x_usb_request *request =
		container_of(work, struct ms912x_usb_request, work);
	struct ms912x_device *ms912x = request->ms912x;
	struct ms912x_stats *stats = &ms912x->stats;
	ktime_t t_submit, t_done;
	int idx, ret;

	if (!drm_dev_enter(&ms912x->drm, &idx)) {
		ret = -ENODEV;
		goto complete;
	}

	t_submit = ktime_get();
	ms912x_flip_arm(ms912x, request, t_submit);
	ret = ms912x_request_send(request);
	t_done = ktime_get();
	hrtimer_cancel(&request->flip_timer);
	if (!ret)
		ms912x_update_estimates(ms912x, request, t_submit, t_done);

	ms912x_stat_add(&stats->queue,
			ktime_to_ns(ktime_sub(t_submit, request->t_queued)));
	if (!ret) {
		ms912x_stat_add(&stats->usb, ktime_to_ns(ktime_sub(t_done, t_submit)));
		ms912x_stat_add(&stats->total,
				ktime_to_ns(ktime_sub(t_done, request->t_start)));
		stats->bytes += request->transfer_len;
		stats->frames++;
	} else {
		stats->errors++;
	}

	if (!ret && ms912x->video_pending) {
		/*
		 * First (full) frame after a mode set: now turn the video
		 * output on. The device only shows frames received after
		 * that, and the compositor sends nothing more while the
		 * screen is static, so send this frame again, to both of the
		 * device's buffers.
		 */
		ret = ms912x_hw_start_video(ms912x);
		if (!ret)
			ret = ms912x_request_send(request);
		if (!ret)
			ret = ms912x_request_send(request);
		if (!ret) {
			ms912x->video_pending = false;
			drm_info(&ms912x->drm, "video output started\n");
		}
	}

	drm_dev_exit(idx);
complete:
	if (ret < 0 && ret != -ENODEV)
		drm_err_ratelimited(&ms912x->drm,
				    "failed to send framebuffer: %d\n", ret);
	/* Let the compositor render the next frame now that the link is free */
	ms912x_send_event(ms912x, &request->event);
	complete(&request->done);
}

void ms912x_free_request(struct ms912x_usb_request *request)
{
	if (!request->transfer_buffer)
		return;

	timer_shutdown_sync(&request->timer);
	hrtimer_cancel(&request->flip_timer);
	sg_free_table(&request->transfer_sgt);
	vfree(request->transfer_buffer);
	request->transfer_buffer = NULL;
}

int ms912x_init_request(struct ms912x_device *ms912x,
			struct ms912x_usb_request *request, size_t len)
{
	int ret;
	unsigned int i, num_pages;
	void *data;
	struct page **pages;
	void *ptr;

	data = vmalloc_32(len);
	if (!data)
		return -ENOMEM;

	num_pages = DIV_ROUND_UP(len, PAGE_SIZE);
	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_vfree;
	}

	for (i = 0, ptr = data; i < num_pages; i++, ptr += PAGE_SIZE)
		pages[i] = vmalloc_to_page(ptr);
	ret = sg_alloc_table_from_pages(&request->transfer_sgt, pages,
					num_pages, 0, len, GFP_KERNEL);
	kfree(pages);
	if (ret)
		goto err_vfree;

	request->transfer_buffer = data;
	request->ms912x = ms912x;

	init_completion(&request->done);
	complete(&request->done);
	timer_setup(&request->timer, ms912x_request_timeout, 0);
	hrtimer_setup(&request->flip_timer, ms912x_flip_timer_fn,
		      CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD);
	INIT_WORK(&request->work, ms912x_request_work);
	return 0;

err_vfree:
	vfree(data);
	return ret;
}

static const u8 ms912x_end_of_buffer[] = { 0xff, 0xc0, 0x00, 0x00,
				    0x00, 0x00, 0x00, 0x00 };

#define MS912X_FPU_LINES 16
#define MS912X_MAX_CONV_THREADS 8
/* Below this many pixels the thread hand-off costs more than it saves. */
#define MS912X_MT_MIN_PIXELS (256 * 1024)

struct ms912x_conv_job {
	struct work_struct work;
	struct completion done;
	u8 *dst;
	const u8 *src;
	unsigned int pitch;
	unsigned int width;
	unsigned int lines;
	bool avx2;
};

static void ms912x_convert_lines(u8 *dst, const u8 *src, unsigned int pitch,
				 unsigned int width, unsigned int lines,
				 bool avx2)
{
	unsigned int i;

	for (i = 0; i < lines; i++) {
#ifdef MS912X_HAVE_AVX2
		if (avx2) {
			/* Keep non-preemptible sections short. */
			if (!(i % MS912X_FPU_LINES))
				kernel_fpu_begin();
			ms912x_xrgb_to_uyvy_avx2(dst, (const u32 *)src, width);
			if (i % MS912X_FPU_LINES == MS912X_FPU_LINES - 1 ||
			    i == lines - 1)
				kernel_fpu_end();
		} else
#endif
			ms912x_xrgb_to_uyvy_c(dst, (const u32 *)src, width);
		dst += width * 2;
		src += pitch;
	}
}

static void ms912x_conv_work(struct work_struct *work)
{
	struct ms912x_conv_job *job =
		container_of(work, struct ms912x_conv_job, work);

	ms912x_convert_lines(job->dst, job->src, job->pitch, job->width,
			     job->lines, job->avx2);
	complete(&job->done);
}

/* Convert a system memory region, split across worker threads. */
static void ms912x_convert_parallel(struct ms912x_device *ms912x, u8 *dst,
				    const u8 *src, unsigned int pitch,
				    unsigned int width, unsigned int lines,
				    bool avx2)
{
	struct ms912x_conv_job jobs[MS912X_MAX_CONV_THREADS];
	unsigned int n = ms912x->conv_threads, i, line = 0, per;

	if (n > 1 && ms912x->conv_workqueue &&
	    width * lines >= MS912X_MT_MIN_PIXELS) {
		n = min_t(unsigned int, n, lines / 8);
	} else {
		n = 1;
	}

	if (n <= 1) {
		ms912x_convert_lines(dst, src, pitch, width, lines, avx2);
		return;
	}

	per = DIV_ROUND_UP(lines, n);
	for (i = 0; i < n; i++) {
		struct ms912x_conv_job *job = &jobs[i];
		unsigned int count = min(per, lines - line);

		job->dst = dst + (size_t)line * width * 2;
		job->src = src + (size_t)line * pitch;
		job->pitch = pitch;
		job->width = width;
		job->lines = count;
		job->avx2 = avx2;
		line += count;

		/* Band 0 runs on this thread. */
		if (i) {
			INIT_WORK_ONSTACK(&job->work, ms912x_conv_work);
			init_completion(&job->done);
			queue_work(ms912x->conv_workqueue, &job->work);
		}
	}

	ms912x_convert_lines(jobs[0].dst, jobs[0].src, pitch, width,
			     jobs[0].lines, avx2);

	for (i = 1; i < n; i++) {
		wait_for_completion(&jobs[i].done);
		destroy_work_on_stack(&jobs[i].work);
	}
}

static int ms912x_fb_xrgb8888_to_yuv422(struct ms912x_device *ms912x,
					void *dst,
					const struct iosys_map *src,
					struct drm_framebuffer *fb,
					const struct drm_rect *rect,
					struct drm_format_conv_state *fmtcnv_state)
{
	struct ms912x_frame_update_header *header = dst;
	unsigned int pitch = fb->pitches[0];
	u32 position, dimensions;
	int i, x, y1, y2, width;
	bool avx2 = false;

	y1 = rect->y1;
	y2 = min_t(unsigned int, rect->y2, fb->height);
	x = rect->x1;
	width = drm_rect_width(rect);

	header->marker = cpu_to_be16(0xff00);
	position = ((x & 0xfff) << 12) | (y1 & 0xfff);
	dimensions = ((width & 0xfff) << 12) |
		     (drm_rect_height(rect) & 0xfff);
	put_unaligned_be24(position, header->position);
	put_unaligned_be24(dimensions, header->dimensions);
	dst += sizeof(*header);

#ifdef MS912X_HAVE_AVX2
	avx2 = ms912x->use_avx2;
#endif

	if (!src->is_iomem) {
		ms912x_convert_parallel(ms912x, dst,
					(const u8 *)src->vaddr + y1 * pitch + x * 4,
					pitch, width, y2 - y1, avx2);
		dst += (size_t)width * 2 * (y2 - y1);
	} else {
		u32 *temp_buffer;

		temp_buffer = drm_format_conv_state_reserve(fmtcnv_state,
							    width * sizeof(u32),
							    GFP_KERNEL);
		if (!temp_buffer)
			return -ENOMEM;

		for (i = y1; i < y2; i++) {
			struct iosys_map map =
				IOSYS_MAP_INIT_OFFSET(src, i * pitch + x * 4);

			iosys_map_memcpy_from(temp_buffer, &map, 0, width * 4);
			ms912x_convert_lines(dst, (const u8 *)temp_buffer, 0,
					     width, 1, avx2);
			dst += width * 2;
		}
	}

	memcpy(dst, ms912x_end_of_buffer, sizeof(ms912x_end_of_buffer));
	return 0;
}

/*
 * Convert and queue @rect of @fb. If @event is non-NULL and the frame is
 * queued, the event is taken over and sent once the USB transfer is done;
 * otherwise it is left for the caller.
 */
int ms912x_fb_send_rect(struct drm_framebuffer *fb, const struct iosys_map *map,
			struct drm_format_conv_state *fmtcnv_state,
			struct drm_rect *rect,
			struct drm_pending_vblank_event **event)
{
	int ret = 0, idx;
	struct ms912x_device *ms912x = to_ms912x(fb->dev);
	struct drm_device *drm = &ms912x->drm;
	struct ms912x_stats *stats = &ms912x->stats;
	struct ms912x_usb_request *current_request;
	ktime_t t0, t1, t2;

	/*
	 * Block alignment used by the vendor driver: x in units of 4 pixels,
	 * y in units of 2 lines.
	 */
	rect->x1 = ALIGN_DOWN(rect->x1, 4);
	rect->x2 = min_t(int, ALIGN(rect->x2, 4), ALIGN_DOWN(fb->width, 2));
	rect->y1 = ALIGN_DOWN(rect->y1, 2);
	rect->y2 = min_t(int, ALIGN(rect->y2, 2), fb->height);
	if (rect->x2 <= rect->x1 || rect->y2 <= rect->y1)
		return 0;

	current_request = &ms912x->requests[ms912x->current_request];

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	/*
	 * Transfer buffer still in use, drop this frame. Unless the keepalive
	 * is resending it: that is over within one transfer, and a dropped
	 * frame would leave the output stale until the next update.
	 */
	if (!wait_for_completion_timeout(&current_request->done,
					 msecs_to_jiffies(10)) &&
	    (!READ_ONCE(current_request->keepalive) ||
	     !wait_for_completion_timeout(&current_request->done,
					  msecs_to_jiffies(MS912X_KEEPALIVE_WAIT_MS)))) {
		stats->dropped++;
		ret = -ETIMEDOUT;
		goto dev_exit;
	}

	t0 = ktime_get();
	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto request_complete;
	t1 = ktime_get();

	ret = ms912x_fb_xrgb8888_to_yuv422(ms912x,
					   current_request->transfer_buffer,
					   map, fb, rect, fmtcnv_state);

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto request_complete;
	t2 = ktime_get();

	ms912x_stat_add(&stats->cpu_access, ktime_to_ns(ktime_sub(t1, t0)));
	ms912x_stat_add(&stats->convert, ktime_to_ns(ktime_sub(t2, t1)));
	if (drm_rect_width(rect) == fb->width && drm_rect_height(rect) == fb->height)
		stats->full_frames++;

	/* Time from the last flip until this frame was ready to send */
	if (event && *event && ms912x->t_last_flip) {
		u64 lead = ktime_to_ns(ktime_sub(t2, ms912x->t_last_flip));

		if (lead < MS912X_LEAD_MAX_NS) {
			ms912x->lead_ns = MS912X_EWMA(ms912x->lead_ns, lead);
			ms912x_stat_add(&stats->lead, lead);
		}
	}

	current_request->t_start = t0;
	current_request->t_queued = t2;
	if (event) {
		current_request->event = *event;
		*event = NULL;
	}
	current_request->transfer_len =
		drm_rect_width(rect) * 2 * drm_rect_height(rect) +
		MS912X_FRAME_OVERHEAD;
	queue_work(ms912x->workqueue, &current_request->work);
	ms912x->current_request = 1 - ms912x->current_request;

	WRITE_ONCE(ms912x->last_request, current_request);
	ms912x_keepalive_arm(ms912x);
	goto dev_exit;

request_complete:
	complete(&current_request->done);
dev_exit:
	drm_dev_exit(idx);
	return ret;
}

/* Module parameters, defined in ms912x_drv.c */

void ms912x_keepalive_arm(struct ms912x_device *ms912x)
{
	if (ms912x_keepalive_ms)
		mod_delayed_work(ms912x->workqueue, &ms912x->keepalive_work,
				 msecs_to_jiffies(ms912x_keepalive_ms));
}

/*
 * Runs on the ordered transfer workqueue, so it never overlaps a frame
 * transfer. It claims the buffer like a new frame does, so that a frame
 * converted into it meanwhile (only possible for commits that are not paced
 * by flip events) waits for the resend instead of overwriting the buffer
 * while it is being sent.
 */
void ms912x_keepalive_work(struct work_struct *work)
{
	struct ms912x_device *ms912x = container_of(to_delayed_work(work),
						    struct ms912x_device,
						    keepalive_work);
	struct ms912x_usb_request *request = READ_ONCE(ms912x->last_request);
	int idx, ret;

	if (!request || ms912x->video_pending || ms912x_debug_hold ||
	    !ms912x_keepalive_ms)
		return;

	/* A new frame holds the buffer and is about to be sent. */
	if (!try_wait_for_completion(&request->done))
		goto rearm;

	if (!drm_dev_enter(&ms912x->drm, &idx)) {
		complete(&request->done);
		return;
	}

	WRITE_ONCE(request->keepalive, true);
	ret = ms912x_request_send(request);
	drm_dev_exit(idx);
	complete(&request->done);
	/* Cleared after complete() so a waiting frame never misses it. */
	WRITE_ONCE(request->keepalive, false);

	if (ret)
		drm_dbg(&ms912x->drm, "keepalive failed: %d\n", ret);
	else
		ms912x->keepalives++;

rearm:
	ms912x_keepalive_arm(ms912x);
}

/*
 * Test hook: immediately send a full frame of one color, bypassing the
 * compositor. Used to measure device + display latency. Returns the
 * monotonic time at which the transfer completed in *t_done_ns.
 */
int ms912x_send_fill(struct ms912x_device *ms912x, unsigned int width,
		     unsigned int height, u32 rgb, u64 *t_done_ns)
{
	struct ms912x_usb_request *request;
	struct ms912x_frame_update_header *header;
	u32 pixel[2] = { rgb, rgb };
	u8 uyvy[4], *dst;
	size_t i, n;

	if (!width || width > MS912X_MAX_WIDTH || !height ||
	    height > MS912X_MAX_HEIGHT)
		return -EINVAL;
	ms912x_xrgb_to_uyvy_c(uyvy, pixel, 2);

	request = &ms912x->requests[ms912x->current_request];
	if (!wait_for_completion_timeout(&request->done,
					 msecs_to_jiffies(1000)))
		return -ETIMEDOUT;

	header = request->transfer_buffer;
	header->marker = cpu_to_be16(0xff00);
	put_unaligned_be24(0, header->position);
	put_unaligned_be24(((width & 0xfff) << 12) | (height & 0xfff),
			   header->dimensions);
	dst = request->transfer_buffer + sizeof(*header);
	n = (size_t)width * height / 2;
	for (i = 0; i < n; i++, dst += 4)
		memcpy(dst, uyvy, 4);
	memcpy(dst, ms912x_end_of_buffer, sizeof(ms912x_end_of_buffer));

	request->event = NULL;
	WRITE_ONCE(ms912x->last_request, NULL);
	request->t_start = request->t_queued = ktime_get();
	request->transfer_len = (size_t)width * height * 2 +
				MS912X_FRAME_OVERHEAD;
	queue_work(ms912x->workqueue, &request->work);
	ms912x->current_request = 1 - ms912x->current_request;

	if (!wait_for_completion_timeout(&request->done,
					 msecs_to_jiffies(1000)))
		return -ETIMEDOUT;
	/* Make the next compositor frame a full update. */
	ms912x->update_rect.x1 = 0;
	ms912x->update_rect.y1 = 0;
	ms912x->update_rect.x2 = width;
	ms912x->update_rect.y2 = height;
	*t_done_ns = ktime_get_ns();
	complete(&request->done);
	return 0;
}
