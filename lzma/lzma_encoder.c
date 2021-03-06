// SPDX-License-Identifier: Apache-2.0
/*
 * ez/lzma/lzma_encoder.c
 *
 * Copyright (C) 2019-2020 Gao Xiang <hsiangkao@aol.com>
 *
 * Authors: Igor Pavlov <http://7-zip.org/>
 *          Lasse Collin <lasse.collin@tukaani.org>
 *          Gao Xiang <hsiangkao@aol.com>
 */
#include <stdlib.h>
#include <ez/bitops.h>
#include "rc_encoder_ckpt.h"
#include "lzma_common.h"
#include "mf.h"

#define kNumBitModelTotalBits	11
#define kBitModelTotal		(1 << kNumBitModelTotalBits)
#define kProbInitValue		(kBitModelTotal >> 1)

#define kNumStates		12
#define LZMA_PB_MAX		4
#define LZMA_NUM_PB_STATES_MAX	(1 << LZMA_PB_MAX)

#define kNumLenToPosStates	4
#define kNumPosSlotBits		6

#define kStartPosModelIndex	4
#define kEndPosModelIndex	14
#define kNumFullDistances	(1 << (kEndPosModelIndex >> 1))

#define kNumAlignBits		4
#define kAlignTableSize		(1 << kNumAlignBits)
#define kAlignMask		(kAlignTableSize - 1)

#define kNumLenToPosStates	4

#define is_literal_state(state) ((state) < 7)

/* note that here dist is an zero-based distance */
static unsigned int get_pos_slot2(unsigned int dist)
{
	const unsigned int zz = fls(dist) - 1;

	return (zz + zz) + ((dist >> (zz - 1)) & 1);
}

static unsigned int get_pos_slot(unsigned int dist)
{
	return dist <= 4 ? dist : get_pos_slot2(dist);
}

/* aka. GetLenToPosState in LZMA */
static inline unsigned int get_len_state(unsigned int len)
{
	if (len < kNumLenToPosStates - 1 + kMatchMinLen)
		return len - kMatchMinLen;

	return kNumLenToPosStates - 1;
}

struct lzma_properties {
	uint32_t lc;	/* 0 <= lc <= 8, default = 3 */
	uint32_t lp;	/* 0 <= lp <= 4, default = 0 */
	uint32_t pb;	/* 0 <= pb <= 4, default = 2 */

	struct lzma_mf_properties mf;
};

struct lzma_length_encoder {
	probability low[LZMA_NUM_PB_STATES_MAX << (kLenNumLowBits + 1)];
	probability high[kLenNumHighSymbols];
};

struct lzma_encoder_destsize {
	struct lzma_rc_ckpt cp;

	uint8_t *op;
	uint32_t capacity;

	uint32_t esz;
	uint8_t ending[LZMA_REQUIRED_INPUT_MAX + 5];
};

struct lzma_encoder {
	struct lzma_mf mf;
	struct lzma_rc_encoder rc;

	uint8_t *op, *oend;
	bool finish;
	bool need_eopm;

	unsigned int state;

	/* the four most recent match distances */
	uint32_t reps[LZMA_NUM_REPS];

	unsigned int pbMask, lpMask;

	unsigned int lc, lp;

	/* the following names came from lzma-specification.txt */
	probability isMatch[kNumStates][LZMA_NUM_PB_STATES_MAX];
	probability isRep[kNumStates];
	probability isRepG0[kNumStates];
	probability isRepG1[kNumStates];
	probability isRepG2[kNumStates];
	probability isRep0Long[kNumStates][LZMA_NUM_PB_STATES_MAX];

	probability posSlotEncoder[kNumLenToPosStates][1 << kNumPosSlotBits];
	probability posEncoders[kNumFullDistances];
	probability posAlignEncoder[1 << kNumAlignBits];

	probability *literal;

	struct lzma_length_encoder lenEnc;
	struct lzma_length_encoder repLenEnc;

	struct {
		struct lzma_match matches[kMatchMaxLen];
		unsigned int matches_count;
	} fast;

	struct lzma_encoder_destsize *dstsize;
};

#define change_pair(smalldist, bigdist) (((bigdist) >> 7) > (smalldist))

static int lzma_get_optimum_fast(struct lzma_encoder *lzma,
				 uint32_t *back_res, uint32_t *len_res)
{
	struct lzma_mf *const mf = &lzma->mf;
	const uint32_t nice_len = mf->nice_len;

	unsigned int matches_count, i;
	unsigned int longest_match_length, longest_match_back;
	unsigned int best_replen, best_rep;
	const uint8_t *ip, *ilimit, *ista;
	uint32_t len;
	int ret;

	if (!mf->lookahead) {
		ret = lzma_mf_find(mf, lzma->fast.matches, lzma->finish);

		if (ret < 0)
			return ret;

		matches_count = ret;
	} else {
		matches_count = lzma->fast.matches_count;
	}

	ip = mf->buffer + mf->cur - mf->lookahead;

	/* no valid match found by matchfinder */
	if (!matches_count ||
	/* not enough input left to encode a match */
	   mf->iend - ip <= 2)
		goto out_literal;

	ilimit = (mf->iend <= ip + kMatchMaxLen ?
		  mf->iend : ip + kMatchMaxLen);

	best_replen = 0;

	/* look for all valid repeat matches */
	for (i = 0; i < LZMA_NUM_REPS; ++i) {
		const uint8_t *const repp = ip - lzma->reps[i];

		/* the first two bytes (MATCH_LEN_MIN == 2) do not match */
		if (get_unaligned16(ip) != get_unaligned16(repp))
			continue;

		len = ez_memcmp(ip + 2, repp + 2, ilimit) - ip;
		/* a repeated match at least nice_len, return it immediately */
		if (len >= nice_len) {
			*back_res = i;
			*len_res = len;
			lzma_mf_skip(mf, len - 1);
			return 0;
		}

		if (len > best_replen) {
			best_rep = i;
			best_replen = len;
		}
	}

	/*
	 * although we didn't find a long enough repeated match,
	 * the normal match is long enough to use directly.
	 */
	longest_match_length = lzma->fast.matches[matches_count - 1].len;
	longest_match_back = lzma->fast.matches[matches_count - 1].dist;
	if (longest_match_length >= nice_len) {
		/* it's encoded as 0-based match distances */
		*back_res = LZMA_NUM_REPS + longest_match_back - 1;
		*len_res = longest_match_length;
		lzma_mf_skip(mf, longest_match_length - 1);
		return 0;
	}

	while (matches_count > 1) {
		const struct lzma_match *const victim =
			&lzma->fast.matches[matches_count - 2];

		/* only (longest_match_length - 1) would be considered */
		if (longest_match_length > victim->len + 1)
			break;

		if (!change_pair(victim->dist, longest_match_back))
			break;

		--matches_count;
		longest_match_length = victim->len;
		longest_match_back = victim->dist;
	}

	if (longest_match_length > best_replen + 1) {
		best_replen = 0;

		if (longest_match_length < 3 &&
		    longest_match_back > 0x80)
			goto out_literal;
	} else {
		longest_match_length = best_replen;
		longest_match_back = 0;
	}

	ista = ip;

	while (1) {
		const struct lzma_match *victim;

		ret = lzma_mf_find(mf, lzma->fast.matches, lzma->finish);

		if (ret < 0) {
			lzma->fast.matches_count = 0;
			break;
		}

		lzma->fast.matches_count = ret;
		if (!ret)
			break;

		victim = &lzma->fast.matches[lzma->fast.matches_count - 1];

		/* both sides have eliminated '+ nlits' */
		if (victim->len + 1 < longest_match_length)
			break;

		if (!best_replen) {
			/* victim->len (should) >= longest_match_length - 1 */
			const uint8_t *ip1 = ip + 1;
			const uint32_t rl = max(2U, longest_match_length - 1);

			/* TODO: lazy match for this */
			for (i = 0; i < LZMA_NUM_REPS; ++i) {
				if (!memcmp(ip1, ip1 - lzma->reps[i], rl)) {
					*len_res = 0;
					return ip1 - ista;
				}
			}

			len = UINT32_MAX;
		} else {
			len = 0;
		}

		for (i = 0; i < LZMA_NUM_REPS; ++i) {
			if (lzma->reps[i] == victim->dist) {
				len = victim->len;
				break;
			}
		}

		/* if the previous match is a rep, this should be longer */
		if (len <= best_replen)
			break;

		/* if it's not a rep */
		if (len == UINT32_MAX) {
			if (victim->len + 1 == longest_match_length &&
			    !change_pair(victim->dist, longest_match_back))
				break;

			if (victim->len == longest_match_length &&
			    get_pos_slot(victim->dist - 1) >=
			    get_pos_slot(longest_match_back))
				break;
			len = 0;
		}
		longest_match_length = victim->len;
		longest_match_back = victim->dist;
		best_replen = len;
		best_rep = i;
		++ip;
	}

	/* it's encoded as 0-based match distances */
	if (best_replen)
		*back_res = best_rep;
	else
		*back_res = LZMA_NUM_REPS + longest_match_back - 1;

	*len_res = longest_match_length;
	lzma_mf_skip(mf, longest_match_length - 2 + (ret < 0));
	return ip - ista;

out_literal:
	*len_res = 0;
	return 1;
}

static void literal_matched(struct lzma_rc_encoder *rc, probability *probs,
			    uint32_t match_byte, uint32_t symbol)
{
	uint32_t offset = 0x100;

	symbol += 0x100;
	do {
		const unsigned int bit = (symbol >> 7) & 1;
		const unsigned int match_bit = (match_byte <<= 1) & offset;

		rc_bit(rc, &probs[offset + match_bit + (symbol >> 8)], bit);
		symbol <<= 1;
		offset &= ~(match_byte ^ symbol);
	} while (symbol < 0x10000);
}

static void literal(struct lzma_encoder *lzma, uint32_t position)
{
	static const unsigned char kLiteralNextStates[] = {
		0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5
	};
	struct lzma_mf *mf = &lzma->mf;
	const uint8_t *ptr = &mf->buffer[mf->cur - mf->lookahead];
	const unsigned int state = lzma->state;

	probability *probs = lzma->literal +
		3 * ((((position << 8) + ptr[-1]) & lzma->lpMask) << lzma->lc);

	if (is_literal_state(state)) {
		/*
		 * Previous LZMA-symbol was a literal. Encode a normal
		 * literal without a match byte.
		 */
		rc_bittree(&lzma->rc, probs, 8, *ptr);
	} else {
		/*
		 * Previous LZMA-symbol was a match. Use the byte + 1
		 * of the last match as a "match byte". That is, compare
		 * the bits of the current literal and the match byte.
		 */
		const uint8_t match_byte = *(ptr - lzma->reps[0]);

		literal_matched(&lzma->rc, probs, match_byte, *ptr);
	}

	lzma->state = kLiteralNextStates[state];
}

/* LenEnc_Encode */
static void length(struct lzma_rc_encoder *rc,
		   struct lzma_length_encoder *lc,
		   const uint32_t pos_state, const uint32_t len)
{
	uint32_t sym = len - kMatchMinLen;
	probability *probs = lc->low;

	if (sym >= kLenNumLowSymbols) {
		rc_bit(rc, probs, 1);
		probs += kLenNumLowSymbols;
		if (sym >= kLenNumLowSymbols * 2 /* + kLenNumMidSymbols */) {
			rc_bit(rc, probs, 1);
			rc_bittree(rc, lc->high, kLenNumHighBits,
				   sym - kLenNumLowSymbols * 2);
			return;
		}
		sym -= kLenNumLowSymbols;
	}
	rc_bit(rc, probs, 0);
	rc_bittree(rc, probs + (pos_state << (kLenNumLowBits + 1)),
		   kLenNumLowBits, sym);
}

/* Match */
static void match(struct lzma_encoder *lzma, const uint32_t pos_state,
		  const uint32_t dist, const uint32_t len)
{
	const uint32_t posSlot = get_pos_slot(dist);
	const uint32_t lenState = get_len_state(len);

	lzma->state = (is_literal_state(lzma->state) ? 7 : 10);
	length(&lzma->rc, &lzma->lenEnc, pos_state, len);

	/* - unsigned posSlot = PosSlotDecoder[lenState].Decode(&RangeDec); */
	rc_bittree(&lzma->rc, lzma->posSlotEncoder[lenState],
		   kNumPosSlotBits, posSlot);

	if (dist >= kStartPosModelIndex) {
		const uint32_t footer_bits = (posSlot >> 1) - 1;
		const uint32_t base = (2 | (posSlot & 1)) << footer_bits;

		if (dist < kNumFullDistances) {
			rc_bittree_reverse(&lzma->rc,
					   lzma->posEncoders + base,
					   footer_bits, dist);
		} else {
			const uint32_t dist_reduced = dist - base;

			rc_direct(&lzma->rc, dist_reduced >> kNumAlignBits,
				  footer_bits - kNumAlignBits);

#if 0
			rc_bittree_reverse(&lzma->rc, lzma->posAlignEncoder,
					   kNumAlignBits,
					   dist_reduced & kAlignMask);
#endif
			rc_bittree_reverse(&lzma->rc, lzma->posAlignEncoder,
					   kNumAlignBits, dist);
		}
	}
	lzma->reps[3] = lzma->reps[2];
	lzma->reps[2] = lzma->reps[1];
	lzma->reps[1] = lzma->reps[0];
	lzma->reps[0] = dist + 1;
}

static void rep_match(struct lzma_encoder *lzma, const uint32_t pos_state,
		      const uint32_t rep, const uint32_t len)
{
	const unsigned int state = lzma->state;

	if (rep == 0) {
		rc_bit(&lzma->rc, &lzma->isRepG0[state], 0);
		rc_bit(&lzma->rc, &lzma->isRep0Long[state][pos_state],
		       len != 1);
	} else {
		const uint32_t distance = lzma->reps[rep];

		rc_bit(&lzma->rc, &lzma->isRepG0[state], 1);
		if (rep == 1) {
			rc_bit(&lzma->rc, &lzma->isRepG1[state], 0);
		} else {
			rc_bit(&lzma->rc, &lzma->isRepG1[state], 1);
			rc_bit(&lzma->rc, &lzma->isRepG2[state], rep - 2);

			if (rep == 3)
				lzma->reps[3] = lzma->reps[2];
			lzma->reps[2] = lzma->reps[1];
		}
		lzma->reps[1] = lzma->reps[0];
		lzma->reps[0] = distance;
	}

	if (len == 1) {
		lzma->state = is_literal_state(state) ? 9 : 11;
	} else {
		length(&lzma->rc, &lzma->repLenEnc, pos_state, len);
		lzma->state = is_literal_state(state) ? 8 : 11;
	}
}

struct lzma_endstate {
	struct lzma_length_encoder lenEnc;

	probability simpleMatch[2];
	probability posSlot[kNumPosSlotBits];
	probability posAlign[kNumAlignBits];
};

static void encode_eopm_stateless(struct lzma_encoder *lzma,
				  struct lzma_endstate *endstate)
{
	const uint32_t pos_state =
		(lzma->mf.cur - lzma->mf.lookahead) & lzma->pbMask;
	const unsigned int state = lzma->state;
	unsigned int i;

	endstate->simpleMatch[0] = lzma->isMatch[state][pos_state];
	endstate->simpleMatch[1] = lzma->isRep[state];
	endstate->lenEnc = lzma->lenEnc;

	rc_bit(&lzma->rc, endstate->simpleMatch, 1);
	rc_bit(&lzma->rc, endstate->simpleMatch + 1, 0);
	length(&lzma->rc, &endstate->lenEnc, pos_state, kMatchMinLen);

	for (i = 0; i < kNumPosSlotBits; ++i) {
		endstate->posSlot[i] =
			lzma->posSlotEncoder[0][(1 << (i + 1)) - 1];
		rc_bit(&lzma->rc, endstate->posSlot + i, 1);
	}

	rc_direct(&lzma->rc, (1 << (30 - kNumAlignBits)) - 1,
		  30 - kNumAlignBits);

	for (i = 0; i < kNumAlignBits; ++i) {
		endstate->posAlign[i] =
			lzma->posAlignEncoder[(1 << (i + 1)) - 1];
		rc_bit(&lzma->rc, endstate->posAlign + i, 1);
	}
}

static void encode_eopm(struct lzma_encoder *lzma)
{
	const uint32_t pos_state =
		(lzma->mf.cur - lzma->mf.lookahead) & lzma->pbMask;
	const unsigned int state = lzma->state;

	rc_bit(&lzma->rc, &lzma->isMatch[state][pos_state], 1);
	rc_bit(&lzma->rc, &lzma->isRep[state], 0);
	match(lzma, pos_state, UINT32_MAX, kMatchMinLen);
}

static int __flush_symbol_destsize(struct lzma_encoder *lzma)
{
	uint8_t *op2;
	unsigned int symbols_size;
	unsigned int esz = 0;

	if (lzma->dstsize->capacity < 5)
		return -ENOSPC;

	if (!lzma->rc.pos) {
		rc_write_checkpoint(&lzma->rc, &lzma->dstsize->cp);
		lzma->dstsize->op = lzma->op;
	}

	if (rc_encode(&lzma->rc, &lzma->op, lzma->oend))
		return -ENOSPC;

	op2 = lzma->op;
	symbols_size = op2 - lzma->dstsize->op;
	if (lzma->dstsize->capacity < symbols_size + 5)
		goto err_enospc;

	if (!lzma->need_eopm)
		goto out;

	if (lzma->dstsize->capacity < symbols_size +
	    LZMA_REQUIRED_INPUT_MAX + 5) {
		struct lzma_rc_ckpt cp2;
		struct lzma_endstate endstate;
		uint8_t ending[sizeof(lzma->dstsize->ending)];
		uint8_t *ep;

		rc_write_checkpoint(&lzma->rc, &cp2);
		encode_eopm_stateless(lzma, &endstate);
		rc_flush(&lzma->rc);

		ep = ending;
		if (rc_encode(&lzma->rc, &ep, ending + sizeof(ending)))
			DBG_BUGON(1);

		esz = ep - ending;

		if (lzma->dstsize->capacity < symbols_size + esz)
			goto err_enospc;
		rc_restore_checkpoint(&lzma->rc, &cp2);

		memcpy(lzma->dstsize->ending, ending, sizeof(ending));
		lzma->dstsize->esz = esz;
	}

out:
	lzma->dstsize->capacity -= symbols_size;
	lzma->dstsize->esz = esz;
	return 0;

err_enospc:
	rc_restore_checkpoint(&lzma->rc, &lzma->dstsize->cp);
	lzma->op = lzma->dstsize->op;
	lzma->dstsize->capacity = 0;
	return -ENOSPC;
}

static int flush_symbol(struct lzma_encoder *lzma)
{
	if (lzma->rc.count && lzma->dstsize) {
		const unsigned int safemargin =
			5 + (LZMA_REQUIRED_INPUT_MAX << !!lzma->need_eopm);
		uint8_t *op;
		bool ret;

		if (lzma->dstsize->capacity < safemargin)
			return __flush_symbol_destsize(lzma);

		op = lzma->op;
		ret = rc_encode(&lzma->rc, &lzma->op, lzma->oend);

		lzma->dstsize->capacity -= lzma->op - op;
		return ret ? -ENOSPC : 0;
	}

	return rc_encode(&lzma->rc, &lzma->op, lzma->oend) ? -ENOSPC : 0;
}

static int encode_symbol(struct lzma_encoder *lzma, uint32_t back,
			 uint32_t len, uint32_t *position)
{
	int err = flush_symbol(lzma);

	if (!err) {
		const uint32_t pos_state = *position & lzma->pbMask;
		const unsigned int state = lzma->state;
		struct lzma_mf *const mf = &lzma->mf;

		if (back == MARK_LIT) {
			/* literal i.e. 8-bit byte */
			rc_bit(&lzma->rc, &lzma->isMatch[state][pos_state], 0);
			literal(lzma, *position);
			len = 1;
		} else {
			rc_bit(&lzma->rc, &lzma->isMatch[state][pos_state], 1);

			if (back < LZMA_NUM_REPS) {
				/* repeated match */
				rc_bit(&lzma->rc, &lzma->isRep[state], 1);
				rep_match(lzma, pos_state, back, len);
			} else {
				/* normal match */
				rc_bit(&lzma->rc, &lzma->isRep[state], 0);
				match(lzma, pos_state,
				      back - LZMA_NUM_REPS, len);
			}
		}

		/* len bytes has been consumed by encoder */
		DBG_BUGON(mf->lookahead < len);
		mf->lookahead -= len;
		*position += len;
	}
	return err;
}

/* encode sequence (literal, match) */
static int encode_sequence(struct lzma_encoder *lzma, unsigned int nliterals,
			   uint32_t back, uint32_t len, uint32_t *position)
{
	while (nliterals) {
		int err = encode_symbol(lzma, MARK_LIT, 0, position);

		if (err)
			return err;
		--nliterals;
	}
	if (!len)	/* no match */
		return 0;
	return encode_symbol(lzma, back, len, position);
}

static int __lzma_encode(struct lzma_encoder *lzma)
{
	uint32_t pos32 = lzma->mf.cur - lzma->mf.lookahead;
	int err;

	do {
		uint32_t back, len;
		int nlits;

		nlits = lzma_get_optimum_fast(lzma, &back, &len);

		if (nlits < 0) {
			err = nlits;
			break;
		}

		err = encode_sequence(lzma, nlits, back, len, &pos32);
	} while (!err);
	return err;
}

static void lzma_length_encoder_reset(struct lzma_length_encoder *lc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lc->low); i++)
		lc->low[i] = kProbInitValue;

	for (i = 0; i < ARRAY_SIZE(lc->high); i++)
		lc->high[i] = kProbInitValue;
}

static int lzma_encoder_reset(struct lzma_encoder *lzma,
			      const struct lzma_properties *props)
{
	unsigned int i, j, oldlclp, lclp;

	lzma_mf_reset(&lzma->mf, &props->mf);
	rc_reset(&lzma->rc);

	/* refer to "The main loop of decoder" of lzma specification */
	lzma->state = 0;
	lzma->reps[0] = lzma->reps[1] = lzma->reps[2] =
		lzma->reps[3] = 1;

	/* reset all LZMA probability matrices */
	for (i = 0; i < kNumStates; ++i) {
		for (j = 0; j < LZMA_NUM_PB_STATES_MAX; ++j) {
			lzma->isMatch[i][j] = kProbInitValue;
			lzma->isRep0Long[i][j] = kProbInitValue;
		}
		lzma->isRep[i] = kProbInitValue;
		lzma->isRepG0[i] = kProbInitValue;
		lzma->isRepG1[i] = kProbInitValue;
		lzma->isRepG2[i] = kProbInitValue;
	}

	for (i = 0; i < kNumLenToPosStates; ++i)
		for (j = 0; j < (1 << kNumPosSlotBits); j++)
			lzma->posSlotEncoder[i][j] = kProbInitValue;

	for (i = 0; i < ARRAY_SIZE(lzma->posEncoders); i++)
		lzma->posEncoders[i] = kProbInitValue;

	for (i = 0; i < ARRAY_SIZE(lzma->posAlignEncoder); i++)
		lzma->posAlignEncoder[i] = kProbInitValue;

	/* set up LZMA literal probabilities */
	oldlclp = lzma->lc + lzma->lp;
	lclp = props->lc + props->lp;
	lzma->lc = props->lc;
	lzma->lp = props->lp;

	if (lzma->literal && lclp != oldlclp) {
		free(lzma->literal);
		lzma->literal = NULL;
	}

	if (!lzma->literal) {
		lzma->literal = malloc((0x300 << lclp) * sizeof(probability));
		if (!lzma->literal)
			return -ENOMEM;
	}

	for (i = 0; i < (0x300 << lclp); i++)
		lzma->literal[i] = kProbInitValue;

	lzma->pbMask = (1 << props->pb) - 1;
	lzma->lpMask = (0x100 << props->lp) - (0x100 >> props->lc);

	lzma_length_encoder_reset(&lzma->lenEnc);
	lzma_length_encoder_reset(&lzma->repLenEnc);
	return 0;
}

void lzma_default_properties(struct lzma_properties *p, int level)
{
	if (level < 0)
		level = 5;

	p->lc = 3;
	p->lp = 0;
	p->pb = 2;
	p->mf.nice_len = (level < 7 ? 32 : 64);	/* LZMA SDK numFastBytes */
	p->mf.depth = (16 + (p->mf.nice_len >> 1)) >> 1;
}

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#if 0
const char text[] = "HABEABDABABABHHHEAAAAAAAA";
#elif 0
const char text[] = "abcde_bcdefgh_abcdefghxxxxxxx";
#else
const char text[] = "The only time we actually leave the path spinning is if we're truncating "
"a small amount and don't actually free an extent, which is not a common "
"occurrence.  We have to set the path blocking in order to add the "
"delayed ref anyway, so the first extent we find we set the path to "
"blocking and stay blocking for the duration of the operation.  With the "
"upcoming file extent map stuff there will be another case that we have "
"to have the path blocking, so just swap to blocking always.";
#endif

static const uint8_t lzma_header[] = {
	0x5D,				/* LZMA model properties (lc, lp, pb) in encoded form */
	0x00, 0x00, 0x80, 0x00,		/* Dictionary size (32-bit unsigned integer, little-endian) */
	0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF,		/* Uncompressed size (64-bit unsigned integer, little-endian) */
};

int main(int argc, char *argv[])
{
	char *outfile;
	struct lzma_encoder lzmaenc = {0};
	struct lzma_properties props = {
		.mf.dictsize = 65536,
	};
	struct lzma_encoder_destsize dstsize;
	uint8_t buf[4096];
	int inf, outf;

	int err;

	lzmaenc.mf.buffer = malloc(65536) + 1;
	lzmaenc.mf.buffer[-1] = 0;

	if (argc >= 3) {
		int len;

		inf = open(argv[2], O_RDONLY);

		len = lseek(inf, 0, SEEK_END);
		if (len >= 65535)
			len = 65535;

		printf("len: %d\n", len);

		lseek(inf, 0, SEEK_SET);
		read(inf, lzmaenc.mf.buffer, len);
		close(inf);
		lzmaenc.mf.iend = lzmaenc.mf.buffer + len;
	} else {
		memcpy(lzmaenc.mf.buffer, text, sizeof(text));
		lzmaenc.mf.iend = lzmaenc.mf.buffer + sizeof(text);
	}
	lzmaenc.op = buf;
	lzmaenc.oend = buf + sizeof(buf);
	lzmaenc.finish = true;

	lzmaenc.need_eopm = true;
	dstsize.capacity = 4096 - sizeof(lzma_header); //UINT32_MAX;
	lzmaenc.dstsize = &dstsize;


	lzma_default_properties(&props, 5);
	lzma_encoder_reset(&lzmaenc, &props);

	err = __lzma_encode(&lzmaenc);

	printf("%d\n", err);

	rc_encode(&lzmaenc.rc, &lzmaenc.op, lzmaenc.oend);

	if (err != -ERANGE) {
		memcpy(lzmaenc.op, dstsize.ending, dstsize.esz);
		lzmaenc.op += dstsize.esz;
	} else {
		encode_eopm(&lzmaenc);
		rc_flush(&lzmaenc.rc);

		rc_encode(&lzmaenc.rc, &lzmaenc.op, lzmaenc.oend);
	}

	printf("encoded length: %lu + %lu\n", lzmaenc.op - buf,
	       sizeof(lzma_header));

	if (argc < 2)
		outfile = "output.bin.lzma";
	else
		outfile = argv[1];

	outf = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	write(outf, lzma_header, sizeof(lzma_header));
	write(outf, buf, lzmaenc.op - buf);
	close(outf);

#if 0
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);
	encode_sequence(&lzmaenc, nliterals, back_res, len_res, &position);
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);
#endif
}

