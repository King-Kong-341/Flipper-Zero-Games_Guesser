#include "guesser_engine.h"

#include <stdlib.h>
#include <string.h>

#define TQ_LEARNED_PRIOR 0.5f
/* Soft, never a hard filter (SPEC 6.2). 0.05 punished an honest mix-up such
 * as "is cheese animal or vegetable?" with a 20x penalty; 0.15 still lets the
 * hint lead the first questions. */
#define TQ_CATEGORY_MISS 0.15f
#define TQ_TINY          1e-6f

const char* tq_answer_label(Tq_Answer a) {
    switch(a) {
    case TqAnswerNo:
        return "No";
    case TqAnswerProbablyNot:
        return "Prob. not";
    case TqAnswerUnknown:
        return "Don't know";
    case TqAnswerProbably:
        return "Probably";
    case TqAnswerYes:
        return "Yes";
    default:
        return "?";
    }
}

const char* tq_category_label(uint8_t category) {
    switch(category) {
    case 0:
        return "Animal";
    case 1:
        return "Vegetable";
    case 2:
        return "Mineral";
    default:
        return "Other";
    }
}

static uint32_t tq_rand(Tq_Engine* eng) {
    /* xorshift32: deterministic in tests, seeded from furi_hal_random on device */
    uint32_t x = eng->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    eng->rng = x ? x : 0x1234567Bu;
    return eng->rng;
}

static uint32_t tq_asked_bytes(const Tq_Dataset* ds) {
    return (ds->question_count + 7u) / 8u;
}

bool tq_engine_is_asked(const Tq_Engine* eng, uint16_t qid) {
    if(qid >= eng->ds->question_count) return true;
    return (eng->asked[qid >> 3] >> (qid & 7u)) & 1u;
}

static void tq_mark_asked(Tq_Engine* eng, uint16_t qid) {
    eng->asked[qid >> 3] |= (uint8_t)(1u << (qid & 7u));
}

static uint32_t tq_learned_count(const Tq_Engine* eng) {
    return eng->learned ? eng->learned->count : 0;
}

Tq_Engine* tq_engine_alloc(const Tq_Dataset* ds, Tq_Learned* learned) {
    Tq_Engine* eng = malloc(sizeof(Tq_Engine));
    memset(eng, 0, sizeof(*eng));
    eng->ds = ds;
    eng->learned = learned;
    eng->n_total = ds->entity_count + tq_learned_count(eng);
    eng->belief = malloc(sizeof(float) * eng->n_total);
    eng->colbuf = malloc(ds->stride);
    if(tq_dataset_has_rows(ds)) {
        eng->rowbuf = malloc(ds->row_stride);
        eng->acc = malloc(sizeof(float) * 3u * ds->question_count);
    }
    eng->asked = malloc(tq_asked_bytes(ds));
    eng->question_limit = 20;
    eng->rng = 0x2545F491u;
    eng->category_hint = TQ_NO_CATEGORY;
    memset(eng->belief, 0, sizeof(float) * eng->n_total);
    memset(eng->asked, 0, tq_asked_bytes(ds));
    return eng;
}

void tq_engine_free(Tq_Engine* eng) {
    if(!eng) return;
    free(eng->belief);
    free(eng->colbuf);
    free(eng->rowbuf);
    free(eng->acc);
    free(eng->asked);
    free(eng);
}

void tq_engine_set_question_limit(Tq_Engine* eng, uint16_t limit) {
    if(limit < 5) limit = 5;
    if(limit > TQ_MAX_QUESTIONS_PER_GAME - TQ_MAX_GUESSES * TQ_EXTRA_QUESTIONS_PER_GUESS) {
        limit = TQ_MAX_QUESTIONS_PER_GAME - TQ_MAX_GUESSES * TQ_EXTRA_QUESTIONS_PER_GUESS;
    }
    eng->question_limit = limit;
}

void tq_engine_seed(Tq_Engine* eng, uint32_t seed) {
    eng->rng = seed ? seed : 0x2545F491u;
}

/** Rebuilds the prior belief. Learned entities join at a flat weight. */
static bool tq_engine_load_prior(Tq_Engine* eng, uint8_t category_hint) {
    const Tq_Dataset* ds = eng->ds;
    uint32_t n = ds->entity_count;
    if(!tq_dataset_read_priors(ds, eng->belief)) {
        eng->io_error = true;
        return false;
    }
    for(uint32_t i = n; i < eng->n_total; i++)
        eng->belief[i] = TQ_LEARNED_PRIOR;

    if(category_hint < TQ_CATEGORY_COUNT) {
        for(uint32_t i = 0; i < eng->n_total; i++) {
            uint8_t cat = tq_engine_entity_category(eng, i);
            if(cat != category_hint) eng->belief[i] *= TQ_CATEGORY_MISS;
        }
    }
    for(uint8_t r = 0; r < eng->rejected_count; r++) {
        if(eng->rejected[r] < eng->n_total) eng->belief[eng->rejected[r]] = 0.0f;
    }

    float sum = 0.0f;
    for(uint32_t i = 0; i < eng->n_total; i++)
        sum += eng->belief[i];
    if(sum <= 0.0f) {
        for(uint32_t i = 0; i < eng->n_total; i++)
            eng->belief[i] = 1.0f / (float)eng->n_total;
        return true;
    }
    float inv = 1.0f / sum;
    for(uint32_t i = 0; i < eng->n_total; i++)
        eng->belief[i] *= inv;
    return true;
}

bool tq_engine_reset(Tq_Engine* eng, uint8_t category_hint) {
    uint32_t want_total = eng->ds->entity_count + tq_learned_count(eng);
    if(want_total != eng->n_total) { /* a word was taught since the last game */
        free(eng->belief);
        eng->n_total = want_total;
        eng->belief = malloc(sizeof(float) * eng->n_total);
    }
    memset(eng->asked, 0, tq_asked_bytes(eng->ds));
    memset(eng->family_used, 0, sizeof(eng->family_used));
    eng->asked_count = 0;
    eng->rejected_count = 0;
    eng->io_error = false;
    eng->category_hint = category_hint;
    return tq_engine_load_prior(eng, category_hint);
}

/**
 * P(yes | entity i, question qid). Dataset entities read their nibble from the
 * column we hold; learned entities use their own answer if they gave one,
 * else the mean over dataset entities of their category -- the category prior
 * for this question, precomputed in the data file.
 */
static inline float
    tq_cell(const Tq_Engine* eng, const uint8_t* col, uint32_t i, uint16_t qid) {
    const Tq_Dataset* ds = eng->ds;
    if(i < ds->entity_count) return tq_pyes(ds, col, i);
    uint16_t rec = (uint16_t)(i - ds->entity_count);
    float p;
    if(eng->learned && tq_learned_pyes(eng->learned, rec, qid, ds->lut, &p)) return p;
    uint8_t cat = eng->learned ? eng->learned->records[rec]->category : (TQ_CATEGORY_COUNT - 1);
    return tq_dataset_catmean(ds, qid, cat);
}

static bool tq_apply(Tq_Engine* eng, uint16_t qid, Tq_Answer answer) {
    float w = tq_answer_weight(answer);
    if(answer == TqAnswerUnknown) { /* no information -- skip the multiply */
        tq_mark_asked(eng, qid);
        return true;
    }
    if(!tq_dataset_read_column(eng->ds, qid, eng->colbuf)) {
        eng->io_error = true;
        return false;
    }
    const float k = 1.0f - 2.0f * TQ_EPS;
    float sum = 0.0f;
    for(uint32_t i = 0; i < eng->n_total; i++) {
        float p = tq_cell(eng, eng->colbuf, i, qid);
        float l = TQ_EPS + k * (w * p + (1.0f - w) * (1.0f - p));
        eng->belief[i] *= l;
        sum += eng->belief[i];
    }
    if(sum > 0.0f) {
        float inv = 1.0f / sum;
        for(uint32_t i = 0; i < eng->n_total; i++)
            eng->belief[i] *= inv;
    }
    tq_mark_asked(eng, qid);
    return true;
}

bool tq_engine_answer(Tq_Engine* eng, uint16_t qid, Tq_Answer answer) {
    if(qid >= eng->ds->question_count) return false;
    if(eng->asked_count >= TQ_MAX_QUESTIONS_PER_GAME) return false;
    if(!tq_apply(eng, qid, answer)) return false;
    eng->asked_order[eng->asked_count] = qid;
    eng->answers[eng->asked_count] = (uint8_t)answer;
    eng->asked_count++;
    uint8_t fam = tq_dataset_family(eng->ds, qid);
    if(fam < TQ_FAMILY_COUNT && eng->family_used[fam] < 255) eng->family_used[fam]++;
    return true;
}

bool tq_engine_undo(Tq_Engine* eng) {
    if(eng->asked_count == 0) return false;
    uint16_t keep = (uint16_t)(eng->asked_count - 1);
    uint16_t order[TQ_MAX_QUESTIONS_PER_GAME];
    uint8_t ans[TQ_MAX_QUESTIONS_PER_GAME];
    memcpy(order, eng->asked_order, sizeof(uint16_t) * keep);
    memcpy(ans, eng->answers, keep);

    memset(eng->asked, 0, tq_asked_bytes(eng->ds));
    memset(eng->family_used, 0, sizeof(eng->family_used));
    eng->asked_count = 0;
    if(!tq_engine_load_prior(eng, eng->category_hint)) return false;
    for(uint16_t k = 0; k < keep; k++) {
        if(!tq_engine_answer(eng, order[k], (Tq_Answer)ans[k])) return false;
    }
    return true;
}

/* --- question selection (SPEC 6.4) ------------------------------------- */

static bool tq_cand_push(uint16_t* cand, uint8_t* count, uint8_t* seen, uint16_t qid) {
    if(*count >= TQ_CANDIDATES) return false;
    if((seen[qid >> 3] >> (qid & 7u)) & 1u) return true;
    seen[qid >> 3] |= (uint8_t)(1u << (qid & 7u));
    cand[(*count)++] = qid;
    return true;
}

static uint8_t tq_build_shortlist(Tq_Engine* eng, uint16_t* cand) {
    const Tq_Dataset* ds = eng->ds;
    uint32_t q = ds->question_count;
    uint8_t seen[(TQ_MAX_QUESTIONS + 7) / 8];
    memset(seen, 0, (q + 7u) / 8u);
    uint8_t count = 0;

    /* 28 from the static ranking, best first */
    for(uint32_t k = 0; k < q && count < 28; k++) {
        uint16_t qid = ds->qrank[k];
        if(tq_engine_is_asked(eng, qid)) continue;
        tq_cand_push(cand, &count, seen, qid);
    }
    /* 8 from families this game has not touched yet -- keeps the run varied */
    uint8_t target = (uint8_t)(count + 8);
    if(target > TQ_CANDIDATES) target = TQ_CANDIDATES;
    for(uint32_t k = 0; k < q && count < target; k++) {
        uint16_t qid = ds->qrank[k];
        if(tq_engine_is_asked(eng, qid)) continue;
        uint8_t fam = tq_dataset_family(ds, qid);
        if(fam < TQ_FAMILY_COUNT && eng->family_used[fam] != 0) continue;
        tq_cand_push(cand, &count, seen, qid);
    }
    /* 4 at random so repeat games differ */
    uint32_t guard = 0;
    while(count < TQ_CANDIDATES && guard++ < 64) {
        uint16_t qid = (uint16_t)(tq_rand(eng) % q);
        if(tq_engine_is_asked(eng, qid)) continue;
        tq_cand_push(cand, &count, seen, qid);
    }
    return count;
}

/* log2 without libm (SPEC 1: no transcendentals from -lm): exponent bits plus
 * a quadratic for the mantissa, max error ~0.005 -- plenty for ranking. */
static inline float tq_log2(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    float e = (float)(int32_t)((bits >> 23) & 0xFFu) - 128.0f;
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m;
    memcpy(&m, &bits, sizeof(m));
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

static inline float tq_xlog(float x) {
    return x > TQ_TINY ? x * tq_log2(x) : 0.0f;
}

/**
 * How good a yes/no split is. ylog/nlog are sum(y log y) and sum(n log n)
 * over the candidates. Entropy: minus the expected entropy left after the
 * answer (offset to stay positive). Gini: the expected collision probability.
 */
static inline float tq_split_score(float s_yes, float ya, float nb) {
    float s_no = 1.0f - s_yes;
    if(s_yes < TQ_TINY) s_yes = TQ_TINY;
    if(s_no < TQ_TINY) s_no = TQ_TINY;
#if TQ_ENTROPY
    return 64.0f + ya + nb - tq_xlog(s_yes) - tq_xlog(s_no);
#else
    return ya / s_yes + nb / s_no;
#endif
}

/**
 * Belief per topic domain (dmass, sums to the total belief) for the topic
 * weighting. Learned words count as "any topic".
 */
static void tq_domain_mass(const Tq_Engine* eng, float* dmass) {
    const Tq_Dataset* ds = eng->ds;
    for(uint8_t d = 0; d < TQ_DOMAIN_COUNT; d++)
        dmass[d] = 0.0f;
    for(uint32_t i = 0; i < eng->n_total; i++) {
        float b = eng->belief[i];
        if(b <= 0.0f) continue;
        dmass[i < ds->entity_count ? tq_domain(ds, i) : TQ_DOMAIN_ANY] += b;
    }
}

/** Share of the belief for which question j is on topic. */
static float tq_on_topic(const Tq_Engine* eng, const float* dmass, uint32_t j) {
    uint16_t mask = tq_qdomain(eng->ds, j);
    float rel = dmass[TQ_DOMAIN_ANY];
    for(uint8_t d = 0; d < TQ_DOMAIN_ANY; d++)
        if((mask >> d) & 1u) rel += dmass[d];
    return rel;
}

/**
 * Entropy score -> topic-weighted score. h0 is the entropy before the
 * question, so (score - 64 + h0) is the expected information gain in bits.
 * The result stays positive (callers use negative for "skip").
 */
static inline float tq_weigh(float score, float h0, float on_topic) {
#if TQ_ENTROPY
    float gain = score - 64.0f + h0;
    if(gain < 0.0f) gain = 0.0f;
    return 1.0f + gain * (TQ_OFF_TOPIC + (1.0f - TQ_OFF_TOPIC) * on_topic);
#else
    (void)h0;
    (void)on_topic;
    return score;
#endif
}

/* Tie-break: within a hair of the best, prefer the least-used family, then the
 * better static rank. score[c] belongs to question qids[c]; negative = skip. */
static uint16_t
    tq_pick_best(const Tq_Engine* eng, const uint16_t* qids, const float* score, uint32_t n) {
    float best = -1.0f;
    for(uint32_t c = 0; c < n; c++)
        if(score[c] > best) best = score[c];
    uint16_t chosen = 0xFFFFu;
    uint8_t best_fam_use = 255;
    uint16_t best_rank = 0xFFFFu;
#if TQ_ENTROPY
    float cutoff = best - TQ_TIE_BITS;
#else
    float cutoff = best * 0.98f;
#endif
    for(uint32_t c = 0; c < n; c++) {
        if(score[c] < 0.0f || score[c] < cutoff) continue;
        uint16_t qid = qids ? qids[c] : (uint16_t)c;
        uint8_t fam = tq_dataset_family(eng->ds, qid);
        uint8_t use = (fam < TQ_FAMILY_COUNT) ? eng->family_used[fam] : 0;
        uint16_t rank = eng->ds->qrank_pos[qid];
        if(use < best_fam_use || (use == best_fam_use && rank < best_rank)) {
            best_fam_use = use;
            best_rank = rank;
            chosen = qid;
        }
    }
    return chosen;
}

/** Column mode: stream each shortlisted column and score it exactly. */
/* How many whole columns a turn may stream: keeps the SD traffic per turn
 * roughly constant as the dataset grows. */
static uint32_t tq_column_budget(const Tq_Engine* eng) {
    uint32_t limit = TQ_COLUMN_BYTES / (eng->ds->stride ? eng->ds->stride : 1u);
    if(limit < TQ_MIN_CANDIDATES) limit = TQ_MIN_CANDIDATES;
    if(limit > TQ_CANDIDATES) limit = TQ_CANDIDATES;
    return limit;
}

/** Streams each candidate's column and scores it exactly over every entity. */
static bool tq_score_columns(Tq_Engine* eng, const uint16_t* cand, uint8_t n_cand, uint16_t* out_qid) {
    float purity[TQ_CANDIDATES];
    float dmass[TQ_DOMAIN_COUNT];
    tq_domain_mass(eng, dmass);
    float h0 = 0.0f;
    for(uint32_t i = 0; i < eng->n_total; i++)
        h0 -= tq_xlog(eng->belief[i]);
    for(uint8_t c = 0; c < n_cand; c++) {
        if(!tq_dataset_read_column(eng->ds, cand[c], eng->colbuf)) {
            eng->io_error = true;
            return false;
        }
        float s_yes = 0.0f, a = 0.0f, b = 0.0f;
        for(uint32_t i = 0; i < eng->n_total; i++) {
            float bi = eng->belief[i];
            if(bi <= 0.0f) continue;
            float p = tq_cell(eng, eng->colbuf, i, cand[c]);
            float y = bi * p;
            float nno = bi - y;
            s_yes += y;
#if TQ_ENTROPY
            a += tq_xlog(y);
            b += tq_xlog(nno);
#else
            a += y * y;
            b += nno * nno;
#endif
        }
        purity[c] = tq_weigh(tq_split_score(s_yes, a, b), h0, tq_on_topic(eng, dmass, cand[c]));
    }
    *out_qid = tq_pick_best(eng, cand, purity, n_cand);
    return true;
}

/** Column mode for v1 files: a static shortlist, scored exactly. */
static bool tq_select_by_columns(Tq_Engine* eng, uint16_t* out_qid) {
    uint16_t cand[TQ_CANDIDATES];
    uint8_t n_cand = tq_build_shortlist(eng, cand);
    if(n_cand == 0) return false;
    uint32_t limit = tq_column_budget(eng);
    if(n_cand > limit) n_cand = (uint8_t)limit;
    return tq_score_columns(eng, cand, n_cand, out_qid);
}

/**
 * Collects the TQ_ROW_ENTITIES dataset entities with the highest belief into
 * eng->top_idx (ascending index, so the row reads walk forward through the
 * file) and returns their total belief. tail[c] receives the belief of every
 * other dataset entity of category c.
 */
static float
    tq_collect_top(Tq_Engine* eng, uint32_t* out_count, float* tail, uint32_t* tail_n) {
    const Tq_Dataset* ds = eng->ds;
    uint32_t count = 0;
    for(uint8_t c = 0; c < TQ_CATEGORY_COUNT; c++) {
        tail[c] = 0.0f;
        tail_n[c] = 0;
    }
    for(uint32_t i = 0; i < ds->entity_count; i++) {
        float b = eng->belief[i];
        if(b <= 0.0f) continue;
        uint8_t cat = tq_cat(ds, i);
        tail[cat] += b;
        tail_n[cat]++;
        if(count == TQ_ROW_ENTITIES && b <= eng->top_b[count - 1]) continue;
        uint32_t pos = (count < TQ_ROW_ENTITIES) ? count++ : count - 1;
        while(pos > 0 && eng->top_b[pos - 1] < b) {
            eng->top_b[pos] = eng->top_b[pos - 1];
            eng->top_idx[pos] = eng->top_idx[pos - 1];
            pos--;
        }
        eng->top_b[pos] = b;
        eng->top_idx[pos] = i;
    }
    float mass = 0.0f;
    for(uint32_t k = 0; k < count; k++) {
        mass += eng->top_b[k];
        uint8_t cat = tq_cat(ds, eng->top_idx[k]);
        tail[cat] -= eng->top_b[k];
        tail_n[cat]--;
    }
    for(uint8_t c = 0; c < TQ_CATEGORY_COUNT; c++)
        if(tail[c] < 0.0f) tail[c] = 0.0f;
    /* by index, so the row reads walk forward through the file; count <= 96 */
    for(uint32_t k = 1; k < count; k++) {
        uint32_t idx = eng->top_idx[k];
        float b = eng->top_b[k];
        uint32_t pos = k;
        while(pos > 0 && eng->top_idx[pos - 1] > idx) {
            eng->top_idx[pos] = eng->top_idx[pos - 1];
            eng->top_b[pos] = eng->top_b[pos - 1];
            pos--;
        }
        eng->top_idx[pos] = idx;
        eng->top_b[pos] = b;
    }
    *out_count = count;
    return mass;
}

static inline void tq_acc_add(float* acc, uint32_t j, float b, float p) {
    float y = b * p;
    float n = b - y;
    acc[3u * j] += y;
#if TQ_ENTROPY
    acc[3u * j + 1u] += tq_xlog(y);
    acc[3u * j + 2u] += tq_xlog(n);
#else
    acc[3u * j + 1u] += y * y;
    acc[3u * j + 2u] += n * n;
#endif
}

/**
 * Row mode: score every unasked question against the front-runners. Sets
 * *handled to false, without touching the SD card, while the belief is still
 * too spread out for the front-runners to speak for everyone.
 */
static bool tq_select_by_rows(Tq_Engine* eng, uint16_t* out_qid, bool* handled) {
    const Tq_Dataset* ds = eng->ds;
    uint32_t q = ds->question_count;
    *handled = false;

    uint32_t count = 0;
    float tail[TQ_CATEGORY_COUNT];
    uint32_t tail_n[TQ_CATEGORY_COUNT];
    float mass = tq_collect_top(eng, &count, tail, tail_n);
    for(uint32_t i = ds->entity_count; i < eng->n_total; i++)
        mass += eng->belief[i];
    if(count == 0) return true;
    *handled = true;
    /* Belief still spread out: the front-runners cannot speak for everyone,
     * but they are a good guide to which questions are worth a closer look. */
    bool spread = mass < TQ_ROW_MASS;

    float* acc = eng->acc;
    memset(acc, 0, sizeof(float) * 3u * q);
    for(uint32_t k = 0; k < count; k++) {
        if(!tq_dataset_read_row(ds, eng->top_idx[k], eng->rowbuf)) {
            eng->io_error = true;
            return false;
        }
        float b = eng->top_b[k];
        for(uint32_t j = 0; j < q; j++)
            tq_acc_add(acc, j, b, tq_row_pyes(ds, eng->rowbuf, j));
    }
    /* learned words: few, and already in RAM */
    for(uint32_t i = ds->entity_count; i < eng->n_total; i++) {
        float b = eng->belief[i];
        if(b <= 0.0f) continue;
        for(uint32_t j = 0; j < q; j++)
            tq_acc_add(acc, j, b, tq_cell(eng, NULL, i, (uint16_t)j));
    }
    /* The long tail: many tiny beliefs, treated as spread evenly over the
     * tail_n entities of each category with the category's mean answer. */
#if TQ_ENTROPY
    float log_n[TQ_CATEGORY_COUNT];
    for(uint8_t c = 0; c < TQ_CATEGORY_COUNT; c++)
        log_n[c] = tail_n[c] ? tq_log2((float)tail_n[c]) : 0.0f;
#endif
    for(uint32_t j = 0; j < q; j++) {
        for(uint8_t c = 0; c < TQ_CATEGORY_COUNT; c++) {
            if(tail[c] <= 0.0f) continue;
            float y = tail[c] * tq_dataset_catmean(ds, j, c);
            acc[3u * j] += y;
#if TQ_ENTROPY
            float n = tail[c] - y;
            acc[3u * j + 1u] += tq_xlog(y) - y * log_n[c];
            acc[3u * j + 2u] += tq_xlog(n) - n * log_n[c];
#endif
            /* Gini: their squares are negligible, so only s_yes moves */
        }
    }

    /* Entropy before the question, under the same tail approximation the
     * scores use, so the difference is the information gain. */
    float h0 = 0.0f;
    for(uint32_t k = 0; k < count; k++)
        h0 -= tq_xlog(eng->top_b[k]);
    for(uint32_t i = ds->entity_count; i < eng->n_total; i++)
        h0 -= tq_xlog(eng->belief[i]);
#if TQ_ENTROPY
    for(uint8_t c = 0; c < TQ_CATEGORY_COUNT; c++)
        if(tail[c] > 0.0f) h0 -= tq_xlog(tail[c]) - tail[c] * log_n[c];
#endif
    float dmass[TQ_DOMAIN_COUNT];
    tq_domain_mass(eng, dmass);

    /* Score per question, written over slot j (j <= 3j, so that slot is
     * already consumed); -1 marks the questions already asked. */
    for(uint32_t j = 0; j < q; j++) {
        float score = -1.0f;
        if(!tq_engine_is_asked(eng, (uint16_t)j))
            score = tq_weigh(
                tq_split_score(acc[3u * j], acc[3u * j + 1u], acc[3u * j + 2u]),
                h0,
                tq_on_topic(eng, dmass, j));
        acc[j] = score;
    }
    if(spread) {
        /* Adaptive shortlist: the questions that look best for the current
         * answers so far, then scored exactly over all entities. */
        uint16_t cand[TQ_CANDIDATES];
        uint32_t limit = tq_column_budget(eng);
        uint8_t n_cand = 0;
        while(n_cand < limit) {
            float best = -1.0f;
            uint32_t bj = q;
            for(uint32_t j = 0; j < q; j++) {
                if(acc[j] > best) {
                    best = acc[j];
                    bj = j;
                }
            }
            if(bj == q) break;
            cand[n_cand++] = (uint16_t)bj;
            acc[bj] = -1.0f;
        }
        if(n_cand == 0) return false;
        return tq_score_columns(eng, cand, n_cand, out_qid);
    }
    uint16_t chosen = tq_pick_best(eng, NULL, acc, q);
    if(chosen == 0xFFFFu) return false;
    eng->row_turns++;
    *out_qid = chosen;
    return true;
}

bool tq_engine_select_question(Tq_Engine* eng, uint16_t* out_qid) {
    /* Opening move with no hint: the precomputed best question for the prior. */
    if(eng->asked_count == 0 && eng->rejected_count == 0 &&
       eng->category_hint == TQ_NO_CATEGORY && eng->n_total == eng->ds->entity_count &&
       !tq_engine_is_asked(eng, eng->ds->qrank[0])) {
        *out_qid = eng->ds->qrank[0];
        return true;
    }
    if(eng->acc) {
        bool handled = false;
        bool ok = tq_select_by_rows(eng, out_qid, &handled);
        if(handled || !ok) return ok;
    }
    if(!tq_select_by_columns(eng, out_qid)) return false;
    return *out_qid != 0xFFFFu;
}

/* --- guessing (SPEC 6.5) ----------------------------------------------- */

uint32_t tq_engine_top(const Tq_Engine* eng, Tq_Candidate* out, uint32_t max) {
    if(max == 0) return 0;
    uint32_t filled = 0;
    for(uint32_t i = 0; i < eng->n_total; i++) {
        float b = eng->belief[i];
        bool skip = false;
        for(uint8_t r = 0; r < eng->rejected_count; r++) {
            if(eng->rejected[r] == i) skip = true;
        }
        if(skip) continue;
        if(filled < max) {
            uint32_t pos = filled++;
            while(pos > 0 && out[pos - 1].belief < b) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos].index = i;
            out[pos].belief = b;
        } else if(b > out[max - 1].belief) {
            uint32_t pos = max - 1;
            while(pos > 0 && out[pos - 1].belief < b) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos].index = i;
            out[pos].belief = b;
        }
    }
    return filled;
}

uint16_t tq_engine_question_budget(const Tq_Engine* eng) {
    return (uint16_t)(eng->question_limit + eng->rejected_count * TQ_EXTRA_QUESTIONS_PER_GUESS);
}

bool tq_engine_out_of_questions(const Tq_Engine* eng) {
    return eng->asked_count >= tq_engine_question_budget(eng) ||
           eng->asked_count >= TQ_MAX_QUESTIONS_PER_GAME;
}

bool tq_engine_should_guess(const Tq_Engine* eng) {
    if(tq_engine_out_of_questions(eng)) return true;
    Tq_Candidate top[2];
    uint32_t got = tq_engine_top(eng, top, 2);
    if(got == 0) return true;
    if(got == 1) return true;
    return top[0].belief >= TQ_GUESS_CONFIDENCE && top[0].belief >= TQ_GUESS_RATIO * top[1].belief;
}

void tq_engine_reject(Tq_Engine* eng, uint32_t entity_index) {
    if(entity_index >= eng->n_total) return;
    if(eng->rejected_count < TQ_MAX_GUESSES) eng->rejected[eng->rejected_count++] = entity_index;
    eng->belief[entity_index] = 0.0f;
    float sum = 0.0f;
    for(uint32_t i = 0; i < eng->n_total; i++)
        sum += eng->belief[i];
    if(sum > 0.0f) {
        float inv = 1.0f / sum;
        for(uint32_t i = 0; i < eng->n_total; i++)
            eng->belief[i] *= inv;
    } else {
        for(uint32_t i = 0; i < eng->n_total; i++)
            eng->belief[i] = 1.0f / (float)eng->n_total;
    }
}

bool tq_engine_entity_name(const Tq_Engine* eng, uint32_t index, char* out, size_t out_size) {
    if(index < eng->ds->entity_count) {
        return tq_dataset_entity_name(eng->ds, index, out, out_size);
    }
    uint16_t rec = (uint16_t)(index - eng->ds->entity_count);
    if(!eng->learned || rec >= eng->learned->count) {
        if(out_size) out[0] = '\0';
        return false;
    }
    size_t len = strlen(eng->learned->records[rec]->name);
    if(len >= out_size) len = out_size - 1;
    memcpy(out, eng->learned->records[rec]->name, len);
    out[len] = '\0';
    return true;
}

uint8_t tq_engine_entity_category(const Tq_Engine* eng, uint32_t index) {
    if(index < eng->ds->entity_count) return tq_dataset_category(eng->ds, index);
    uint16_t rec = (uint16_t)(index - eng->ds->entity_count);
    if(!eng->learned || rec >= eng->learned->count) return TQ_CATEGORY_COUNT - 1;
    return eng->learned->records[rec]->category;
}

uint8_t tq_answer_to_nibble(const Tq_Dataset* ds, Tq_Answer answer) {
    float w = tq_answer_weight(answer);
    uint8_t best = 0;
    float best_d = 9.9f;
    for(uint8_t i = 0; i < TQ_LUT_SIZE; i++) {
        float d = ds->lut[i] - w;
        if(d < 0.0f) d = -d;
        if(d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}
