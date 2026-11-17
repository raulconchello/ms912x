/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef MS912X_CONVERT_H
#define MS912X_CONVERT_H

#include <linux/types.h>

/*
 * XRGB8888 -> UYVY (YUV 4:2:2, BT.601 limited range) line converters.
 *
 * @dst:   output, width * 2 bytes
 * @src:   input, width XRGB8888 pixels (little endian), may be unaligned
 * @width: number of pixels, must be even
 *
 * The AVX2 variant must be called between kernel_fpu_begin() and
 * kernel_fpu_end().
 */
void ms912x_xrgb_to_uyvy_c(u8 *dst, const u32 *src, unsigned int width);
void ms912x_xrgb_to_uyvy_avx2(u8 *dst, const u32 *src, unsigned int width);

#endif
