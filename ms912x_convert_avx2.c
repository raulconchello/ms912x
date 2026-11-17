// SPDX-License-Identifier: GPL-2.0-only
/*
 * AVX2 XRGB8888 -> UYVY conversion, written with GCC vector extensions so
 * no userspace intrinsic headers are needed. This file is compiled with
 * AVX2 enabled (see Makefile) and must only be called inside
 * kernel_fpu_begin()/kernel_fpu_end() on CPUs with X86_FEATURE_AVX2.
 *
 * 16 pixels are processed per iteration: two 256-bit loads, split into
 * even and odd pixels, and one 256-bit store of 8 UYVY macropixels.
 */

#include "ms912x_convert.h"

typedef u32 v8u __attribute__((vector_size(32)));
typedef u32 v8u_ua __attribute__((vector_size(32), aligned(1)));
typedef int v8i __attribute__((vector_size(32)));

void ms912x_xrgb_to_uyvy_avx2(u8 *dst, const u32 *src, unsigned int width)
{
	const v8i even_idx = { 0, 2, 4, 6, 8, 10, 12, 14 };
	const v8i odd_idx = { 1, 3, 5, 7, 9, 11, 13, 15 };
	unsigned int i;

	for (i = 0; i + 16 <= width; i += 16) {
		v8u a = *(const v8u_ua *)(src + i);
		v8u b = *(const v8u_ua *)(src + i + 8);
		v8u e = __builtin_shuffle(a, b, even_idx);
		v8u o = __builtin_shuffle(a, b, odd_idx);
		v8u re = (e >> 16) & 0xff, ge = (e >> 8) & 0xff, be = e & 0xff;
		v8u ro = (o >> 16) & 0xff, go = (o >> 8) & 0xff, bo = o & 0xff;
		v8u rs = re + ro, gs = ge + go, bs = be + bo;
		v8u ye, yo, u, v;

		ye = ((16u << 16) + 16763u * re + 32904u * ge + 6391u * be) >> 16;
		yo = ((16u << 16) + 16763u * ro + 32904u * go + 6391u * bo) >> 16;
		u = ((256u << 16) - 9676u * rs - 18996u * gs + 28672u * bs) >> 17;
		v = ((256u << 16) + 28672u * rs - 24009u * gs - 4663u * bs) >> 17;

		*(v8u_ua *)(dst + i * 2) = u | (ye << 8) | (v << 16) | (yo << 24);
	}

	if (i < width)
		ms912x_xrgb_to_uyvy_c(dst + i * 2, src + i, width - i);
}
