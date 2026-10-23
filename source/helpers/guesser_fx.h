/**
 * Sound, vibration and LED effects, played through the notification service
 * so they can be started from any thread and never block the game.
 */
#pragma once

#include <notification/notification.h>

#include "guesser_storage.h"

typedef enum {
    GuesserFxClick, /* moving the selection */
    GuesserFxSelect, /* an answer or menu item chosen */
    GuesserFxUndo,
    GuesserFxIntro,
    GuesserFxGuess,
    GuesserFxWin,
    GuesserFxLose,
    GuesserFxSaved,
    GuesserFxError,
    GuesserFxTestSound,
    GuesserFxTestVibro,
    GuesserFxTestLed,
    GuesserFxCount,
} GuesserFx;

typedef struct GuesserFxPlayer GuesserFxPlayer;

GuesserFxPlayer* guesser_fx_alloc(NotificationApp* notifications);
void guesser_fx_free(GuesserFxPlayer* fx);
/** Call after every settings change: sound, volume, clicks, vibration, LED. */
void guesser_fx_configure(GuesserFxPlayer* fx, const GuesserSettings* settings);
void guesser_fx_play(GuesserFxPlayer* fx, GuesserFx effect);
