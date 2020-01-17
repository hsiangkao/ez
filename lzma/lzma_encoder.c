/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ez/lzma/lzma_encoder.c
 *
 * Copyright (C) 2019 Gao Xiang <hsiangkao@aol.com>
 */
#include <stdlib.h>
#include <ez/bitops.h>
#include "lzma_common.h"
#include "mf.h"
#include "rc_encoder.h"

#define kNumBitModelTotalBits	11
#define kBitModelTotal		(1 << kNumBitModelTotalBits)
#define kProbInitValue		(kBitModelTotal >> 1)

#define kNumStates		12
#define LZMA_PB_MAX		4
#define LZMA_NUM_PB_STATES_MAX	(1 << LZMA_PB_MAX)

#define kLenNumLowBits		3
#define kLenNumLowSymbols	(1 << kLenNumLowBits)
#define kLenNumHighBits		8
#define kLenNumHighSymbols	(1 << kLenNumHighBits)

#define kNumLenToPosStates	4
#define kNumPosSlotBits		6

#define kStartPosModelIndex	4
#define kEndPosModelIndex	14
#define kNumFullDistances	(1 << (kEndPosModelIndex >> 1))

#define kNumAlignBits		4
#define kAlignTableSize		(1 << kNumAlignBits)
#define kAlignMask		(kAlignTableSize - 1)

#define kNumLenToPosStates	4
#define kMatchMinLen		MATCH_LEN_MIN

#define is_literal_state(state) ((state) < 7)

static unsigned int get_pos_slot2(unsigned int distance)
{
	const unsigned int zz = fls(distance);

	return (zz + zz) + ((distance >> (zz - 1)) & 1);
}

static unsigned int get_pos_slot(unsigned int distance)
{
	return distance <= 4 ?
		distance : get_pos_slot2(distance);
}

/* aka. GetLenToPosState in LZMA */
static inline unsigned int get_len_state(unsigned int len)
{
	if (len < kNumLenToPosStates - 1 + kMatchMinLen)
		return len - kMatchMinLen;

	return kNumLenToPosStates - 1;
}

#define get_len_state(len)	\
	(((len) < kNumLenToPosStates + 1) ? (len) - kMatchMinLen : kNumLenToPosStates - 1)

struct lzma_properties {
	uint32_t lc, lp, pb;

	uint32_t dict_size;
};

struct lzma_length_encoder {
	probability low[LZMA_NUM_PB_STATES_MAX << (kLenNumLowBits + 1)];
	probability high[kLenNumHighSymbols];
};

struct lzma_encoder {
	struct lzma_mf mf;
	struct lzma_rc_encoder rc;

	enum lzma_lzma_state state;

	/* the four most recent match distances */
	uint32_t reps[REPS];

	unsigned int pbMask;

	unsigned int lc, lp;

	/* the following names refer to lzma-specificatin.txt */
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
		struct lzma_match matches[MATCH_LEN_MAX];
		unsigned int matches_count;
	} fast;
};

#define change_pair(smalldist, bigdist) (((bigdist) >> 7) > (smalldist))

static unsigned int lzma_get_optimum_fast(struct lzma_encoder *lzma,
					  uint32_t *back_res, uint32_t *len_res)
{
	struct lzma_mf *const mf = &lzma->mf;
	const uint32_t nice_len = mf->nice_len;

	struct lzma_match matches[MATCH_LEN_MAX + 1];
	unsigned int matches_count, i, nlits;
	unsigned int longest_match_length, longest_match_back;
	unsigned int best_replen, best_rep;
	const uint8_t *ip, *ilimit;

	if (!mf->lookahead) {
		matches_count = lzma_mf_find(mf, lzma->fast.matches);
	} else {
		matches_count = lzma->fast.matches_count;
	}

	ip = mf->buffer + mf->cur - mf->lookahead;

	/* no valid match found by matchfinder */
	if (!matches_count ||
	/* not enough input left to encode a match */
	   mf->iend - ip <= 2)
		goto out_literal;

	ilimit = (mf->iend <= ip + MATCH_LEN_MAX ?
		  mf->iend : ip + MATCH_LEN_MAX);

	/* look for all valid repeat matches */
	for (i = 0; i < REPS; ++i) {
		const uint8_t *const repp = ip - lzma->reps[i];
		uint32_t len;

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
	longest_match_back = lzma->fast.matches[matches_count - 1].len;
	if (longest_match_length >= nice_len) {
		*back_res = longest_match_back;
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

	nlits = 0;
	while ((lzma->fast.matches_count =
		lzma_mf_find(mf, lzma->fast.matches))) {
		const struct lzma_match *const victim =
			&lzma->fast.matches[lzma->fast.matches_count - 1];

		if (victim->len + nlits + 1 < longest_match_length)
			break;

		if (victim->len + nlits + 1 == longest_match_length &&
		    !change_pair(victim->dist + nlits, longest_match_back))
			break;

		if (victim->len + nlits == longest_match_length &&
		    victim->dist + nlits >= longest_match_back)
			break;
		++nlits;
	}
	if (nlits) {
		*len_res = 0;
	} else {
		*back_res = REPS + longest_match_back;
		*len_res = longest_match_length;
		lzma_mf_skip(mf, longest_match_length - 2);
	}
	return nlits;
out_literal:
	*len_res = 0;
	return 1;
}

static int do_checkpoint(struct lzma_encoder *lzma)
{
	/* end marker is mandatory for this stream */
//	if (lzma->need_eopm) {

//	}
	return 0;
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
		offset &= ~(match_byte ^symbol);
	} while (symbol < 0x10000);
}

static int literal(struct lzma_encoder *lzma, uint32_t position)
{
	static const unsigned char kLiteralNextStates[] =
		{0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5};
	struct lzma_mf *mf = &lzma->mf;
	const uint8_t *ptr = &mf->buffer[mf->cur - mf->lookahead];

	probability *probs = lzma->literal +
		3 * ((((position << 8) + ptr[-1]) & lzma->pbMask) << lzma->lc);

	if (is_literal_state(lzma->state)) {
		/*
		 * Previous LZMA-symbol was a literal. Encode a normal
		 * literal without a match byte.
		 */
		rc_bittree(&lzma->rc, probs, 8, *ptr);
	} else {
		/*
		 * Previous LZMA-symbol was a match. Use the last byte of
		 * the match as a "match byte". That is, compare the bits
		 * of the current literal and the match byte.
		 */
		const uint8_t match_byte = ptr[-1 + lzma->reps[0]];

		literal_matched(&lzma->rc, probs, match_byte, *ptr);
	}

	lzma->state = kLiteralNextStates[lzma->state];
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
	rc_bittree(rc, probs + 1, kLenNumLowBits, sym);
}

/* Match */
static void match(struct lzma_encoder *lzma, const uint32_t pos_state,
		  const uint32_t distance, const uint32_t len)
{
	const uint32_t posSlot = get_pos_slot(distance);
	const uint32_t lenState = get_len_state(len);

	lzma->state = (is_literal_state(lzma->state) ? 7 : 10);
	length(&lzma->rc, &lzma->lenEnc, pos_state, len);

	/* - unsigned posSlot = PosSlotDecoder[lenState].Decode(&RangeDec); */
	rc_bittree(&lzma->rc, lzma->posSlotEncoder[lenState],
		   kNumPosSlotBits, posSlot);

	if (posSlot >= kStartPosModelIndex) {
		const uint32_t footer_bits = (posSlot >> 1) - 1;
		const uint32_t base = (2 | (posSlot & 1)) << footer_bits;
		const uint32_t dist_reduced = distance - base;

		if (posSlot < kNumFullDistances) {
			/*
			 * Careful here: base - dist_slot - 1 can be -1, but
			 * rc_bittree_reverse starts at probs[1], not probs[0].
			 */
			rc_bittree_reverse(&lzma->rc,
					   lzma->posEncoders + base,
					   footer_bits, distance);
		} else {
			const uint32_t dist_reduced = distance - base;

			rc_direct(&lzma->rc, dist_reduced >> kNumAlignBits,
				  footer_bits - kNumAlignBits);
			rc_bittree_reverse(&lzma->rc, lzma->posAlignEncoder,
					   kNumAlignBits,
					   dist_reduced & kAlignMask);
		}
	}
	lzma->reps[3] = lzma->reps[2];
	lzma->reps[2] = lzma->reps[1];
	lzma->reps[1] = lzma->reps[0];
	lzma->reps[0] = distance;
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
		lzma->state = is_literal_state(lzma->state) ? 9 : 11;
	} else {
		length(&lzma->rc, &lzma->repLenEnc, pos_state, len);
		lzma->state = is_literal_state(lzma->state) ? 8 : 11;
	}
}

static int encode_symbol(struct lzma_encoder *lzma, uint32_t back,
			 uint32_t len, uint32_t *position)
{
	int err = do_checkpoint(lzma);

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
			rc_bit(&lzma->rc, &lzma->isMatch[state][pos_state], 0);

			if (back < REPS) {
				/* repeated match */
				rc_bit(&lzma->rc, &lzma->isRep[state], 1);
				rep_match(lzma, pos_state, back, len);
			} else {
				/* normal match */
				rc_bit(&lzma->rc, &lzma->isRep[state], 0);
				match(lzma, pos_state, back - REPS, len);
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
	return encode_symbol(lzma, back, len, position);
}

static int __lzma_encode(struct lzma_encoder *lzma)
{
	uint32_t pos32 = lzma->mf.cur - lzma->mf.lookahead;
	int err;

	do {
		unsigned int nlits;
		uint32_t back, len;
		int err;



		err = encode_sequence(lzma, nlits, back, len, &pos32);
	} while (err);
	return err;
}

static int lzma_length_encoder_reset(struct lzma_length_encoder *lc)
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

	lzma_mf_reset(&lzma->mf, props->dict_size);
	rc_reset(&lzma->rc);

	/* refer to "The main loop of decoder" of lzma specification */
	lzma->state = 0;
	lzma->reps[0] = lzma->reps[1] = lzma->reps[2] =
		lzma->reps[3] = 2;

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

	lzma_length_encoder_reset(&lzma->lenEnc);
	lzma_length_encoder_reset(&lzma->repLenEnc);
	return 0;
}

#include <stdlib.h>
#include <stdio.h>

int main(void)
{
	struct lzma_encoder lzmaenc = {0};
	struct lzma_properties props = {
		.dict_size = 65536,
	};
	unsigned int back_res = 0, len_res = 0;
	unsigned int nliterals;

	unsigned int position = 0;

	lzmaenc.mf.buffer = malloc(65536);
	lzmaenc.mf.iend = lzmaenc.mf.buffer + 65536;

	memcpy(lzmaenc.mf.buffer, "abcde", sizeof("abcde"));

	lzma_encoder_reset(&lzmaenc, &props);
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);
	encode_sequence(&lzmaenc, nliterals, back_res, len_res, &position);
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);
	nliterals = lzma_get_optimum_fast(&lzmaenc, &back_res, &len_res);
	printf("nlits %d (%d %d)\n", nliterals, back_res, len_res);

}

