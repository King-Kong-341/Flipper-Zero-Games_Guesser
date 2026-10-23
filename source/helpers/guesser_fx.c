#include "guesser_fx.h"

#include <furi.h>
#include <notification/notification_messages.h>

/*
 * Every effect is a short list of steps; a step plays a note (or a rest),
 * may set the LED colour and switch the vibration motor, then waits.
 * The messages are built once; which of them a sequence uses depends on the
 * settings, so only the pointer lists are rebuilt when those change. The
 * notification service may still be reading a list while it is rebuilt:
 * every pointer stays valid and the last slot is always NULL.
 */

#define FX_LED_KEEP 0xFF /* step leaves the LED as it is */

typedef struct {
    uint16_t freq; /* Hz, 0 = rest */
    uint8_t vol; /* % of the volume setting */
    uint16_t ms;
    uint8_t r, g, b; /* FX_LED_KEEP in r = unchanged */
    int8_t vib; /* 1 on, 0 off, -1 unchanged */
} FxStep;

#define N(f, v, ms)                  {f, v, ms, FX_LED_KEEP, 0, 0, -1}
#define NL(f, v, ms, r, g, b)        {f, v, ms, r, g, b, -1}
#define NLV(f, v, ms, r, g, b, vib_) {f, v, ms, r, g, b, vib_}

static const FxStep fx_click[] = {N(2093, 25, 6)};
static const FxStep fx_select[] = {NL(1047, 60, 25, 0, 0, 90), NL(1568, 60, 35, 0, 0, 30)};
static const FxStep fx_undo[] = {N(1568, 50, 25), N(1047, 50, 35)};
static const FxStep fx_intro[] = {
    NL(784, 45, 70, 50, 0, 110),
    NL(1047, 45, 70, 80, 0, 170),
    NL(1319, 50, 70, 120, 0, 255),
    NL(1568, 55, 90, 90, 0, 190),
    NL(2093, 40, 260, 30, 0, 70),
};
static const FxStep fx_guess[] = {
    NLV(659, 60, 70, 0, 0, 255, 1),
    NLV(0, 0, 50, 0, 0, 90, 0),
    NL(659, 60, 70, 0, 0, 255),
    NL(0, 0, 50, 0, 0, 90),
    NLV(1047, 70, 240, 0, 40, 255, 1),
    NLV(0, 0, 10, 0, 40, 255, 0),
};
static const FxStep fx_win[] = {
    NLV(1047, 60, 90, 0, 255, 0, 1),
    NLV(1319, 60, 90, 0, 120, 0, 0),
    NL(1568, 60, 90, 0, 255, 0),
    NLV(2093, 70, 140, 0, 120, 0, 1),
    NLV(0, 0, 40, 0, 255, 0, 0),
    NL(1568, 60, 90, 0, 120, 0),
    NL(2093, 80, 320, 0, 255, 40),
};
static const FxStep fx_lose[] = {
    NLV(392, 60, 180, 255, 0, 0, 1),
    NLV(370, 60, 180, 140, 0, 0, 0),
    NL(349, 60, 180, 255, 0, 0),
    NLV(330, 60, 420, 90, 0, 0, 1),
    NLV(0, 0, 10, 90, 0, 0, 0),
};
static const FxStep fx_saved[] = {NL(1319, 60, 70, 0, 255, 60), NL(1760, 60, 140, 0, 120, 30)};
static const FxStep fx_error[] = {NLV(220, 70, 250, 255, 0, 0, 1), NLV(0, 0, 40, 255, 0, 0, 0)};
static const FxStep fx_test_sound[] = {N(1047, 70, 120)};
static const FxStep fx_test_vibro[] = {NLV(0, 0, 150, FX_LED_KEEP, 0, 0, 1)};
static const FxStep fx_test_led[] = {NL(0, 0, 300, 0, 140, 255)};

typedef struct {
    const FxStep* steps;
    uint8_t count;
    bool is_click; /* follows the "key sounds" switch */
} FxDef;

#define FX_DEF(a, click) {a, COUNT_OF(a), click}

static const FxDef fx_defs[GuesserFxCount] = {
    [GuesserFxClick] = FX_DEF(fx_click, true),
    [GuesserFxSelect] = FX_DEF(fx_select, true),
    [GuesserFxUndo] = FX_DEF(fx_undo, true),
    [GuesserFxIntro] = FX_DEF(fx_intro, false),
    [GuesserFxGuess] = FX_DEF(fx_guess, false),
    [GuesserFxWin] = FX_DEF(fx_win, false),
    [GuesserFxLose] = FX_DEF(fx_lose, false),
    [GuesserFxSaved] = FX_DEF(fx_saved, false),
    [GuesserFxError] = FX_DEF(fx_error, false),
    [GuesserFxTestSound] = FX_DEF(fx_test_sound, false),
    [GuesserFxTestVibro] = FX_DEF(fx_test_vibro, false),
    [GuesserFxTestLed] = FX_DEF(fx_test_led, false),
};

/* own messages per step: note, delay, red, green, blue */
#define FX_MSG_PER_STEP 5
/* pointers per step: those 5 + vibro; plus volume, vibro force, 5 closing */
#define FX_PTR_PER_STEP 6
#define FX_PTR_EXTRA    8

typedef struct {
    NotificationMessage* msg;
    const NotificationMessage** seq;
    size_t seq_size;
    bool silent; /* nothing to play with the current settings */
} FxSeq;

struct GuesserFxPlayer {
    NotificationApp* notifications;
    NotificationMessage volume; /* forced speaker volume, from the settings */
    FxSeq seqs[GuesserFxCount];
};

static void fx_build_messages(FxSeq* s, const FxDef* def) {
    for(uint8_t i = 0; i < def->count; i++) {
        const FxStep* st = &def->steps[i];
        NotificationMessage* m = &s->msg[i * FX_MSG_PER_STEP];
        m[0].type = NotificationMessageTypeSoundOn;
        m[0].data.sound.frequency = (float)st->freq;
        m[0].data.sound.volume = (float)st->vol / 100.0f;
        m[1].type = NotificationMessageTypeDelay;
        m[1].data.delay.length = st->ms;
        m[2].type = NotificationMessageTypeLedRed;
        m[2].data.led.value = st->r;
        m[3].type = NotificationMessageTypeLedGreen;
        m[3].data.led.value = st->g;
        m[4].type = NotificationMessageTypeLedBlue;
        m[4].data.led.value = st->b;
    }
}

GuesserFxPlayer* guesser_fx_alloc(NotificationApp* notifications) {
    GuesserFxPlayer* fx = malloc(sizeof(GuesserFxPlayer));
    memset(fx, 0, sizeof(*fx));
    fx->notifications = notifications;
    fx->volume.type = NotificationMessageTypeForceSpeakerVolumeSetting;
    fx->volume.data.forced_settings.speaker_volume = 0.7f;
    for(uint8_t e = 0; e < GuesserFxCount; e++) {
        const FxDef* def = &fx_defs[e];
        FxSeq* s = &fx->seqs[e];
        s->msg = malloc(sizeof(NotificationMessage) * def->count * FX_MSG_PER_STEP);
        memset(s->msg, 0, sizeof(NotificationMessage) * def->count * FX_MSG_PER_STEP);
        s->seq_size = (size_t)def->count * FX_PTR_PER_STEP + FX_PTR_EXTRA + 1;
        s->seq = malloc(sizeof(NotificationMessage*) * s->seq_size);
        memset(s->seq, 0, sizeof(NotificationMessage*) * s->seq_size);
        s->silent = true;
        fx_build_messages(s, def);
    }
    return fx;
}

void guesser_fx_free(GuesserFxPlayer* fx) {
    if(!fx) return;
    /* the service may still be reading a sequence: wait for it to finish */
    notification_message_block(fx->notifications, &sequence_empty);
    for(uint8_t e = 0; e < GuesserFxCount; e++) {
        free(fx->seqs[e].msg);
        free(fx->seqs[e].seq);
    }
    free(fx);
}

void guesser_fx_configure(GuesserFxPlayer* fx, const GuesserSettings* set) {
    bool sound = set->sound && set->volume > 0;
    fx->volume.data.forced_settings.speaker_volume = (float)set->volume / 10.0f;

    for(uint8_t e = 0; e < GuesserFxCount; e++) {
        const FxDef* def = &fx_defs[e];
        FxSeq* s = &fx->seqs[e];
        bool snd = sound && (!def->is_click || set->clicks);
        bool vib = set->vibro && !def->is_click;
        bool led = set->led;

        bool any_note = false, any_led = false, any_vib = false;
        for(uint8_t i = 0; i < def->count; i++) {
            if(def->steps[i].freq) any_note = true;
            if(def->steps[i].r != FX_LED_KEEP) any_led = true;
            if(def->steps[i].vib > 0) any_vib = true;
        }
        snd = snd && any_note;
        led = led && any_led;
        vib = vib && any_vib;

        size_t n = 0;
        const NotificationMessage** q = s->seq;
        if(snd) q[n++] = &fx->volume;
        if(vib) q[n++] = &message_force_vibro_setting_on;
        for(uint8_t i = 0; i < def->count; i++) {
            const FxStep* st = &def->steps[i];
            const NotificationMessage* m = &s->msg[i * FX_MSG_PER_STEP];
            if(led && st->r != FX_LED_KEEP) {
                q[n++] = &m[2];
                q[n++] = &m[3];
                q[n++] = &m[4];
            }
            if(vib && st->vib >= 0) q[n++] = st->vib ? &message_vibro_on : &message_vibro_off;
            if(snd) q[n++] = st->freq ? &m[0] : &message_sound_off;
            q[n++] = &m[1];
        }
        if(snd) q[n++] = &message_sound_off;
        if(vib) q[n++] = &message_vibro_off;
        if(led) {
            q[n++] = &message_red_0;
            q[n++] = &message_green_0;
            q[n++] = &message_blue_0;
        }
        furi_check(n < s->seq_size);
        /* then clear the tail: the last slot is never written, so it stays NULL */
        for(size_t k = n; k < s->seq_size; k++)
            q[k] = NULL;
        s->silent = !(snd || vib || led);
    }
}

void guesser_fx_play(GuesserFxPlayer* fx, GuesserFx effect) {
    if(!fx || effect >= GuesserFxCount) return;
    FxSeq* s = &fx->seqs[effect];
    if(s->silent) return;
    notification_message(fx->notifications, (const NotificationSequence*)s->seq);
}
