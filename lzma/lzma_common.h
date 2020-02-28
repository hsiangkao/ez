/* SPDX-License-Identifier: Unlicense */
/*
 * lzma/lzma_common.h - common definitions of LZMA encoder
 *
 * Copyright (C) 2019-2020 Gao Xiang <hsiangkao@aol.com>
 ×
 * Authors: Igor Pavlov <http://7-zip.org/>
 *          Gao Xiang <hsiangkao@aol.com>
 */
#ifndef __EZ_LZMA_LZMA_COMMON_H
#define __EZ_LZMA_LZMA_COMMON_H

#include <ez/defs.h>
#include <ez/unaligned.h>

/*
 * LZMA Matchlength
 */

/* Minimum length of a match is two bytes. */
#define kMatchMinLen	2

/*
 * Match length is encoded with 4, 5, or 10 bits.
 *
 * Length    Bits
 *    2-9     4 = (Choice = 0) + 3 bits
 *  10-17     5 = (Choice = 1) + (Choice2 = 0) + 3 bits
 * 18-273    10 = (Choice = 1) + (Choice2 = 1) + 8 bits
 */
#define kLenNumLowBits		3
#define kLenNumLowSymbols	(1 << kLenNumLowBits)
#define kLenNumHighBits		8
#define kLenNumHighSymbols	(1 << kLenNumHighBits)
#define kLenNumSymbolsTotal	(kLenNumLowSymbols * 2 + kLenNumHighSymbols)

/*
 * Maximum length of a match is 273 which is a result
 * of the encoding described above.
 */
#define kMatchMaxLen	(kMatchMinLen + kLenNumSymbolsTotal - 1)

/*
 * LZMA remembers the four most recent match distances.
 * Reusing these distances tend to take less space than
 * re-encoding the actual distance value.
 */
#define LZMA_NUM_REPS	4

#define MARK_LIT ((uint32_t)-1)

/*
 * LZMA_REQUIRED_INPUT_MAX = number of required input bytes for worst case.
 * Num bits = log2((2^11 / 31) ^ 22) + 26 < 134 + 26 = 160;
 */
#define LZMA_REQUIRED_INPUT_MAX 20

#endif

