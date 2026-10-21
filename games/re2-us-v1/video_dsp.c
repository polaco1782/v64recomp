/* SPDX-License-Identifier: GPL-2.0-or-later */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - re2.c                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2016 Gilles Siberlin                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* Integer video cores from Salsa64 59cb166c99e7bee2dbf6467814dfab9a13625d1f,
 * src/rsp/jpeg/re2.c. Plugin dispatch and RDRAM integration are omitted.
 * Local integration supplies checked guest spans and full microcode hashes. */
#include "video_dsp.h"
#include <string.h>
static int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static uint32_t dw(const uint8_t *d, unsigned o) { return (uint32_t)d[o]<<24 | (uint32_t)d[o+1]<<16 | (uint32_t)d[o+2]<<8 | d[o+3]; }
static uint32_t dh(const uint8_t *d, unsigned o) { return (uint32_t)d[o]<<8 | d[o+1]; }
#define PHYS 0x1fffffff
/* Las constantes del segmento de datos con las que el núcleo está medido
 * (las cargan los vectores v29, v30 y v31 de la tarea): máscaras de 15 bits,
 * 3 bytes por píxel, pesos ×4, alfa 1, máscara de 5 bits 0xF8, el redondeo
 * de +4 (máscara 7) con tope 255 y los 960 bytes de una fila de origen; el
 * umbral del difuminado (v31[1]) es un parámetro. */
static bool resize_consts(const uint8_t *d)
{
	static const uint16_t v29[8] = { 0xFFFF, 0x7FFF, 3, 4, 1, 0xF8, 0x4000, 8 };
	for (unsigned i = 0; i < 8; i++)
		if (dh(d, 0x40 + 2 * i) != v29[i])
			return false;
	return dh(d, 0x50) == 0x80 && dh(d, 0x52) == 7 && dh(d, 0x54) == 0x8000 && dh(d, 0x56) == 0xFF &&
	       dh(d, 0x60) == RE2_RSP_SRC_ROW;
}

bool re2_rsp_resize_read(const uint8_t *d, struct re2_rsp_resize *t)
{
	t->src = dw(d, 0x00) & PHYS;
	t->dst = dw(d, 0x04) & PHYS;
	t->width = dw(d, 0x08);
	t->rows = dw(d, 0x0C);
	t->x_ratio = dw(d, 0x10);
	t->y_ratio = dw(d, 0x14);
	t->stride = dw(d, 0x18);
	t->x0 = dw(d, 0x20);
	t->y0 = dw(d, 0x24);
	t->thresh = (int16_t)dh(d, 0x62);
	/* El ancho múltiplo de 4 (filas de salida de 8 bytes múltiplos, para el DMA del SPE). */
	if (!resize_consts(d) || t->width < 4 || t->width > 1024 || (t->width & 3) || t->rows < 1 || t->rows > 4096 ||
	    t->stride < 2 * t->width || t->stride > 65536 || (t->stride & 7) || (t->src | t->dst) & 7)
		return false;
	/* El paso por grupo de 8 píxeles, en el segmento de datos, es 8 · x_ratio. */
	if (((dh(d, 0x58) << 16) | dh(d, 0x5A)) != 8 * t->x_ratio)
		return false;
	/* Los píxeles de un grupo: 8 · ⌈ancho / 8⌉; el vecino de abajo a la derecha
	 * (3 · x + 963 + 2) tiene que quedar antes de 0xF80 (la memoria de trabajo
	 * del microcódigo) y x no puede dar la vuelta. */
	uint64_t px = (uint64_t)((t->width + 7) >> 3) * 8;
	uint64_t xmax = t->x0 + (px - 1) * (uint64_t)t->x_ratio;
	if (xmax >= (1ull << 32) || 3 * (xmax >> 16) + 965 >= 0xF80u)
		return false;
	return true;
}

void re2_rsp_resize_begin(struct re2_rsp_dither *st, uint32_t dram_stack, uint32_t dram_stack_size)
{
	memset(st->v, 0, sizeof st->v);
	st->last[0] = (int16_t)(dram_stack >> 16);
	st->last[1] = (int16_t)dram_stack;
	st->last[2] = (int16_t)(dram_stack_size >> 16);
}

/* VABS: valor absoluto con -32768 → 32767. */
static inline int32_t absat(int32_t d)
{
	return d < 0 ? (d == -32768 ? 32767 : -d) : d;
}

void re2_rsp_resize_row(uint8_t *dm, const struct re2_rsp_resize *t, struct re2_rsp_dither *st,
			    uint32_t n)
{
	const uint32_t gp = t->y0 + n * t->y_ratio;
	const uint32_t wy = (gp >> 1) & 0x7FFF, wyc = 0x7FFF - wy;
	const unsigned groups = (t->width + 7) >> 3;
	uint32_t x = t->x0;
	for (unsigned g = 0; g < groups; g++) {
		int32_t s[3][8];
		for (unsigned i = 0; i < 8; i++, x += t->x_ratio) {
			const uint32_t wx = (x & 0xFFFF) >> 1, wxc = 0x7FFF - wx;
			const uint32_t wa = ((wxc * wyc) >> 16) << 2, wb = ((wx * wyc) >> 16) << 2;
			const uint32_t wc = ((wxc * wy) >> 16) << 2, wd = ((wx * wy) >> 16) << 2;
			const uint8_t *a = dm + 3 * (x >> 16), *b = a + 3, *c = a + RE2_RSP_SRC_ROW, *e = c + 3;
			for (unsigned k = 0; k < 3; k++)
				s[k][i] = (int32_t)(((a[k] * wa) >> 16) + ((b[k] * wb) >> 16) + ((c[k] * wc) >> 16) +
						    ((e[k] * wd) >> 16));
		}
		/* El difuminado: por canal, lo acumulado por carril (VADD con lo del grupo anterior),
		 * rotado un carril (el 0 recibe el último del grupo anterior), y qué carriles quedan
		 * a menos del umbral de su suma en los tres canales (VCC). */
		int32_t rot[3][8];
		bool near[8];
		for (unsigned k = 0; k < 3; k++) {
			int32_t acc[8];
			for (unsigned i = 0; i < 8; i++)
				acc[i] = sat16(st->v[k][i] + s[k][i]);
			rot[k][0] = st->last[k];
			for (unsigned i = 1; i < 8; i++)
				rot[k][i] = acc[i - 1];
			st->last[k] = acc[7];
		}
		for (unsigned i = 0; i < 8; i++)
			near[i] = absat(sat16(s[0][i] - rot[0][i])) < t->thresh &&
				  absat(sat16(s[1][i] - rot[1][i])) < t->thresh &&
				  absat(sat16(s[2][i] - rot[2][i])) < t->thresh;
		uint8_t grp[16];
		for (unsigned i = 0; i < 8; i++) {
			uint32_t ch[3];
			for (unsigned k = 0; k < 3; k++) {
				st->v[k][i] = near[i] ? (rot[k][i] & 7) : 4;
				int32_t o = s[k][i] + st->v[k][i];
				ch[k] = (uint32_t)(o > 255 ? 255 : o) & 0xF8;
			}
			/* byte 0 → azul, 1 → verde, 2 → rojo; RGBA5551 con el alfa en 1 */
			uint32_t p = (ch[2] << 8) | (ch[1] << 3) | (ch[0] >> 2) | 1;
			grp[2 * i] = (uint8_t)(p >> 8);
			grp[2 * i + 1] = (uint8_t)p;
		}
		memcpy(dm + RE2_RSP_OUT_BASE + 16 * g, grp, sizeof grp);
	}
}


/* Decode core from the same pinned Salsa64 source. */
bool re2_rsp_decode_read(const uint8_t *d, struct re2_rsp_decode *t)
{
	t->y = dw(d, 0x00) & PHYS;
	t->cb = dw(d, 0x04) & PHYS;
	t->cr = dw(d, 0x08) & PHYS;
	t->dst = dw(d, 0x0C) & PHYS;
	t->width = dw(d, 0x10);
	t->pairs = dw(d, 0x1C);
	t->step = dw(d, 0x24);
	for (unsigned i = 0; i < 5; i++)
		t->k[i] = dh(d, 0x40 + 2 * i);
	t->zero = (int16_t)dh(d, 0x50);
	t->max = (int16_t)dh(d, 0x56);
	const uint32_t height = dw(d, 0x14), passes = dw(d, 0x18), dma = dw(d, 0x20);
	/* El microcódigo trae con un DMA los planos de un par de filas (Y a 0, Cb a
	 * 0x200, Cr a 0x280) y escribe las dos filas de salida a partir de 0x300 y
	 * 0x300 + 4 · ancho; el alfa de las dos zonas queda en cero (se limpian
	 * hasta 0xAC0) salvo lo que deja el DMA de Cr: ancho ≤ 240 y múltiplo de 16
	 * (cada pasada hace 16 píxeles). La salida es un DMA de 2 filas de 4 · ancho
	 * bytes con el salto de la palabra [0x20]; el paso entre pares de filas es
	 * el de las dos. */
	if (t->width < 16 || t->width > 240 || (t->width & 15) || t->pairs < 1 || t->pairs > 4096 ||
	    height != 2 * t->pairs || passes < 1 || passes > 2 || ((t->y | t->cb | t->cr | t->dst) & 7))
		return false;
	const uint32_t len = (dma & 0xFFF) + 1, count = (dma >> 12) & 0xFF, skip = (dma >> 20) & 0xFFF;
	if (len != 4 * t->width || count != 1 || (skip & 7) || t->step != 2 * (len + skip))
		return false;
	return true;
}

/* Un canal: ⌊Y'⌋ ± términos, con VADD/VSUB saturados a 16 bits, VGE contra 0 y VLT contra el
 * tope. */
static inline uint8_t chan(int32_t v, int32_t max)
{
	v = sat16(v);
	v = v < 0 ? 0 : v;
	v = v > max ? max : v;
	return (uint8_t)(v >> 7);
}

void re2_rsp_decode_pair(const struct re2_rsp_decode *t, const uint8_t *y, const uint8_t *cb,
			     const uint8_t *cr, uint8_t *row0, uint8_t *row1)
{
	const unsigned w = t->width;
	const int32_t k0 = (int32_t)t->k[0], k1 = (int32_t)t->k[1], k2 = (int32_t)t->k[2], k3 = (int32_t)t->k[3];
	const int32_t k4 = (int32_t)t->k[4], max = t->max;
	for (unsigned c = 0; c < w / 2; c++) {
		/* VSUB: croma · 128 (LUV) menos el cero, saturado; VMUDM: producto con signo por la
		 * constante sin signo, la mitad alta (piso). */
		const int32_t dcr = sat16((int32_t)cr[c] * 128 - t->zero), dcb = sat16((int32_t)cb[c] * 128 - t->zero);
		const int32_t tr = (dcr * k0) >> 16, tg1 = (dcr * k1) >> 16, tg2 = (dcb * k2) >> 16, tb = (dcb * k3) >> 16;
		for (unsigned i = 0; i < 2; i++) {
			const unsigned x = 2 * c + i;
			const int32_t y0 = ((int32_t)y[x] * 128 * k4) >> 16, y1 = ((int32_t)y[w + x] * 128 * k4) >> 16;
			uint8_t *p0 = row0 + 4 * x, *p1 = row1 + 4 * x;
			p0[0] = chan(y0 + tr, max);
			p0[1] = chan(sat16(y0 - tg1) - tg2, max);
			p0[2] = chan(y0 + tb, max);
			p1[0] = chan(y1 + tr, max);
			p1[1] = chan(sat16(y1 - tg1) - tg2, max);
			p1[2] = chan(y1 + tb, max);
			/* El cuarto byte: ver la cabecera. */
			unsigned a = 0x80 + 4 * x + 3;
			p0[3] = a < w ? cr[a] : 0;
			p1[3] = 0;
		}
	}
}


/* Blend core from the same pinned source; local capacity arithmetic is wide. */
bool re2_rsp_fill_read(const uint8_t *d, struct re2_rsp_fill *t)
{
	t->src = dw(d, 0x00) & PHYS;
	t->dst = dw(d, 0x04) & PHYS;
	const uint32_t bytes = dw(d, 0x08), dma = dw(d, 0x18);
	t->rows = dw(d, 0x0C);
	t->iters = dw(d, 0x10);
	t->advance = dw(d, 0x1C);
	t->len = (dma & 0xFFF) + 1;
	t->skip = (dma >> 20) & 0xFFF;
	/* El microcódigo trae t->rows filas de cada cuadro (DMA con el salto de la palabra, el
	 * número de filas lo pone el segmento de datos) a 0 y a 0x800, promedia `bytes` bytes y
	 * escribe las filas del segundo. */
	if (((dma >> 12) & 0xFF) != 0 || t->rows < 1 || (uint64_t)t->rows * t->len > 2048 || bytes != t->rows * t->len ||
	    (t->len & 7) || (t->skip & 7) || t->iters < 1 || t->iters > 65536 || t->advance != t->rows * (t->len + t->skip) ||
	    ((t->src | t->dst) & 7))
		return false;
	/* La constante de redondeo y la del producto (v10[3] y v10[4]): (s + d) >> 1. */
	return dh(d, 0x26) == 1 && dh(d, 0x28) == 0x8000;
}

void re2_rsp_fill_row(const uint8_t *src, uint8_t *dst, uint32_t n)
{

	uint32_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t a, b;
		memcpy(&a, src + i, 8);
		memcpy(&b, dst + i, 8);
		a = (a & b) + (((a ^ b) >> 1) & 0x7F7F7F7F7F7F7F7Full);
		memcpy(dst + i, &a, 8);
	}
	for (; i < n; i++)
		dst[i] = (uint8_t)((src[i] + dst[i]) >> 1);
}

