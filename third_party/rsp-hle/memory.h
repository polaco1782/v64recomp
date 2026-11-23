/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - memory.h                                        *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
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

#ifndef MEMORY_H
#define MEMORY_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "hle_internal.h"

#ifdef M64P_BIG_ENDIAN
#define S16 0
#else
#define S16 2
#endif

enum {
    TASK_FLAGS = 0xfc4,
    TASK_DATA_PTR = 0xff0
};

static inline uint16_t* u16(const unsigned char* buffer, unsigned address)
{
    assert((address & 1) == 0);
    return (uint16_t*)(buffer + (address ^ S16));
}

static inline uint32_t* u32(const unsigned char* buffer, unsigned address)
{
    assert((address & 3) == 0);
    return (uint32_t*)(buffer + address);
}

void load_u16(uint16_t* dst, const unsigned char* buffer, unsigned address, size_t count);
void store_u16(unsigned char* buffer, unsigned address, const uint16_t* src, size_t count);


/* convenient functions for DMEM access */
static inline uint32_t* dmem_u32(struct hle_t* hle, uint16_t address)
{
    return u32(hle->dmem, address & 0xfff);
}

/* convenient functions DRAM access */
static inline uint32_t* dram_u32(struct hle_t* hle, uint32_t address)
{
    return u32(hle->dram, address & 0xffffff);
}

static inline void dram_load_u16(struct hle_t* hle, uint16_t* dst, uint32_t address, size_t count)
{
    load_u16(dst, hle->dram, address & 0xffffff, count);
}

static inline void dram_store_u16(struct hle_t* hle, const uint16_t* src, uint32_t address, size_t count)
{
    store_u16(hle->dram, address & 0xffffff, src, count);
}

#endif

