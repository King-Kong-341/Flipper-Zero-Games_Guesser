/**
 * Bayesian belief engine (SPEC 6). Pure C11, no Furi, no transcendentals,
 * no double. Same object file runs on the Flipper and under tests/.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guesser_dataset.h"
#include "guesser_learned.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TQ_MAX_QUESTIONS_PER_GAME    40
#define TQ_MAX_GUESSES               3
#define TQ_EXTRA_QUESTIONS_PER_GUESS 5
/* Column mode (early game, belief still spread out): score a shortlist of
 * questions by streaming whole columns. The shortlist shrinks as N grows so a
 * turn never reads much more than TQ_COLUMN_BYTES from the SD card. */
#ifndef TQ_CANDIDATES
#define TQ_CANDIDATES 80
#endif
#ifndef TQ_COLUMN_BYTES
#define TQ_COLUMN_BYTES (32u * 1024u)
#endif
#ifndef TQ_MIN_CANDIDATES
#define TQ_MIN_CANDIDATES 8
#endif

/* Row mode (v2 files): once the TQ_ROW_ENTITIES front-runners hold at least
 * TQ_ROW_MASS of the belief, read just their rows and score EVERY unasked
 * question. The rest of the belief is spread over the category means. */
#ifndef TQ_ROW_ENTITIES
#define TQ_ROW_ENTITIES 112
#endif
#ifndef TQ_ROW_MASS
#define TQ_ROW_MASS 0.35f
#endif

/* Question score: 1 = expected information gain (entropy, via a libm-free
 * log2), 0 = the original Gini purity. TQ_TIE_BITS is how close to the best
 * score a question must be to win on the "vary the families" tie-break. */
#ifndef TQ_ENTROPY
#define TQ_ENTROPY 1
#endif
#ifndef TQ_TIE_BITS
#define TQ_TIE_BITS 0.01f
#endif

/* Topic weighting: a question's information gain counts in full only for the
 * belief held by suspects it is on topic for; the rest counts TQ_OFF_TOPIC.
 * Stops "Is it male?" while the suspects are all pieces of furniture. */
#ifndef TQ_OFF_TOPIC
#define TQ_OFF_TOPIC 0.3f
#endif
#define TQ_FAMILY_COUNT 16

/* SPEC 6.3: noise floor. The single most important tuning knob. */
#ifndef TQ_EPS
#define TQ_EPS 0.05f
#endif

/* SPEC 6.5 guess trigger. Overridable so tools/eval.py can sweep them. */
/* SPEC 6.5 suggests 0.85 / 6.0. Measured: that runs most games to the
 * 20-question limit. 0.50 / 4.0 wins as often (98% honest) but guesses after
 * ~15 questions instead of ~19, which is a better game to play. */
#ifndef TQ_GUESS_CONFIDENCE
#define TQ_GUESS_CONFIDENCE 0.50f
#endif
#ifndef TQ_GUESS_RATIO
#define TQ_GUESS_RATIO 4.0f
#endif

#define TQ_NO_CATEGORY 0xFFu

/* Ordered as drawn on screen, left to right (SPEC 8.2). w = index * 0.25. */
typedef enum {
    TqAnswerNo = 0,
    TqAnswerProbablyNot = 1,
    TqAnswerUnknown = 2,
    TqAnswerProbably = 3,
    TqAnswerYes = 4,
    TqAnswerCount = 5,
} Tq_Answer;

typedef struct {
    uint32_t index; /* < N: dataset entity. >= N: learned overlay record. */
    float belief;
} Tq_Candidate;

typedef struct {
    const Tq_Dataset* ds;
    Tq_Learned* learned; /* may be NULL */

    float* belief; /* n_total entries, normalised to sum 1 */
    uint8_t* colbuf; /* ds->stride scratch bytes */
    uint8_t* rowbuf; /* ds->row_stride scratch bytes, NULL without rows */
    float* acc; /* 3 * Q accumulators for row mode, NULL without rows */
    uint32_t top_idx[TQ_ROW_ENTITIES];
    float top_b[TQ_ROW_ENTITIES];
    uint8_t* asked; /* bitmap, ceil(Q/8) bytes */
    uint32_t n_total;

    uint16_t asked_order[TQ_MAX_QUESTIONS_PER_GAME];
    uint8_t answers[TQ_MAX_QUESTIONS_PER_GAME];
    uint16_t asked_count;

    uint32_t rejected[TQ_MAX_GUESSES];
    uint8_t rejected_count;

    uint8_t family_used[TQ_FAMILY_COUNT];
    uint8_t category_hint;
    uint16_t question_limit;
    uint32_t rng;
    uint32_t row_turns; /* turns scored in row mode, for the debug log */
    bool io_error; /* sticky: set when the SD card stops answering */
} Tq_Engine;

static inline float tq_answer_weight(Tq_Answer a) {
    return (float)((int)a) * 0.25f;
}

const char* tq_answer_label(Tq_Answer a);
const char* tq_category_label(uint8_t category);

Tq_Engine* tq_engine_alloc(const Tq_Dataset* ds, Tq_Learned* learned);
void tq_engine_free(Tq_Engine* eng);

void tq_engine_set_question_limit(Tq_Engine* eng, uint16_t limit);
void tq_engine_seed(Tq_Engine* eng, uint32_t seed);

/** Starts a game. category_hint is 0..3 or TQ_NO_CATEGORY (SPEC 6.2). */
bool tq_engine_reset(Tq_Engine* eng, uint8_t category_hint);

/** Applies one answer and renormalises (SPEC 6.3). */
bool tq_engine_answer(Tq_Engine* eng, uint16_t qid, Tq_Answer answer);

/** Undo by replay, not by snapshot (SPEC 8.2). */
bool tq_engine_undo(Tq_Engine* eng);

/** Picks the next question: every question in row mode, a shortlist otherwise. */
bool tq_engine_select_question(Tq_Engine* eng, uint16_t* out_qid);

/** Top-k by belief, skipping rejected entities. Returns how many were written. */
uint32_t tq_engine_top(const Tq_Engine* eng, Tq_Candidate* out, uint32_t max);

bool tq_engine_should_guess(const Tq_Engine* eng);
bool tq_engine_out_of_questions(const Tq_Engine* eng);
uint16_t tq_engine_question_budget(const Tq_Engine* eng);

/** Zeroes a rejected guess and renormalises (SPEC 6.5). */
void tq_engine_reject(Tq_Engine* eng, uint32_t entity_index);

bool tq_engine_entity_name(const Tq_Engine* eng, uint32_t index, char* out, size_t out_size);
uint8_t tq_engine_entity_category(const Tq_Engine* eng, uint32_t index);
bool tq_engine_is_asked(const Tq_Engine* eng, uint16_t qid);

/** Nibble index for an answer, for writing learned records. */
uint8_t tq_answer_to_nibble(const Tq_Dataset* ds, Tq_Answer answer);

#ifdef __cplusplus
}
#endif
