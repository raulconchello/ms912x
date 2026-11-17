// SPDX-License-Identifier: GPL-2.0-only
/*
 * Portable XRGB8888 -> UYVY conversion. The math is shared with the AVX2
 * variant so both produce bit-identical output:
 *
 *   Y = (16.16 fixed point BT.601 limited range luma) >> 16
 *   U, V are computed from the *sum* of the two pixels of a pair and
 *   shifted by 17, i.e. the average chroma of the pair.
 */

#include <linux/unaligned.h>

#include "ms912x_convert.h"

#define MS912X_Y(r, g, b) \
	(((16u << 16) + 16763u * (r) + 32904u * (g) + 6391u * (b)) >> 16)
#define MS912X_U2(rs, gs, bs) \
	(((256u << 16) - 9676u * (rs) - 18996u * (gs) + 28672u * (bs)) >> 17)
#define MS912X_V2(rs, gs, bs) \
	(((256u << 16) + 28672u * (rs) - 24009u * (gs) - 4663u * (bs)) >> 17)

void ms912x_xrgb_to_uyvy_c(u8 *dst, const u32 *src, unsigned int width)
{
	unsigned int i;

	for (i = 0; i < width; i += 2) {
		u32 p0 = get_unaligned_le32(&src[i]);
		u32 p1 = get_unaligned_le32(&src[i + 1]);
		u32 r0 = (p0 >> 16) & 0xff, g0 = (p0 >> 8) & 0xff, b0 = p0 & 0xff;
		u32 r1 = (p1 >> 16) & 0xff, g1 = (p1 >> 8) & 0xff, b1 = p1 & 0xff;
		u32 rs = r0 + r1, gs = g0 + g1, bs = b0 + b1;

		*dst++ = MS912X_U2(rs, gs, bs);
		*dst++ = MS912X_Y(r0, g0, b0);
		*dst++ = MS912X_V2(rs, gs, bs);
		*dst++ = MS912X_Y(r1, g1, b1);
	}
}
