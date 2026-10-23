#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "helpers/guesser_engine.h"
#include "helpers/guesser_fx.h"
#include "helpers/guesser_storage.h"
#include "scenes/guesser_scene.h"
#include "views/guesser_view.h"

#define TAG "Guesser"

typedef enum {
    GuesserViewMain, /* the animated view: everything except the two below */
    GuesserViewTextInput,
    GuesserViewVarItemList,
} GuesserViewId;

typedef enum {
    GuesserEventWorkerDone = 3, /* the engine finished a turn */
    GuesserEventSettingsReset = 50, /* + setting index */
} GuesserCustomEvent;

/** What the worker thread should do next. */
typedef enum {
    GuesserJobStartGame,
    GuesserJobAnswer,
    GuesserJobUndo,
    GuesserJobRejectGuess,
} GuesserJob;

/** What it produced. */
typedef enum {
    GuesserOutcomeQuestion,
    GuesserOutcomeGuess,
    GuesserOutcomeOutOfIdeas,
    GuesserOutcomeError,
} GuesserOutcome;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    NotificationApp* notifications;
    Storage* storage;
    GuesserFxPlayer* fx;

    GuesserView* view;
    TextInput* text_input;
    VariableItemList* var_item_list;

    /* --- game state --- */
    GuesserStorage store; /* dataset file handle + reader bridge */
    Tq_Dataset dataset;
    Tq_Learned learned;
    Tq_Engine* engine;

    /* --- background loading: the loader opens the data while the intro
     * plays, then checks the whole file once without holding up the game */
    FuriThread* loader;
    FuriEventFlag* loaded; /* set when dataset, learned and engine are ready */
    volatile Tq_Error load_error;
    volatile bool dataset_ready; /* loaded without error */
    volatile bool data_bad; /* the background check found the file damaged */
    volatile bool closing; /* stops the background check */

    GuesserSettings settings;
    GuesserStats stats;

    FuriThread* worker;
    GuesserJob job;
    GuesserOutcome outcome;
    Tq_Answer pending_answer;
    uint32_t turn_ms; /* last turn duration, logged for the SPEC 6.6 budget */

    uint16_t question_id;
    char question_text[TQ_MAX_QTEXT + 1];
    uint32_t guess_index;
    char guess_name[TQ_MAX_NAME + 1];
    uint8_t guess_number;
    uint8_t menu_index;

    char learn_name[TQ_MAX_NAME + 1];
    uint8_t learn_category;
    char message[40];
    char message2[40];
    char message_header[24];
    bool message_fatal; /* the app closes after this message */
    bool game_won;
    bool result_saved; /* this game's result is in the stats already */
} GuesserApp;

/** A one-button message on the animated view; returns to the menu after OK. */
void guesser_show_message(GuesserApp* app, const char* header, const char* line1, const char* line2);
void guesser_show_error(GuesserApp* app, const char* line1, const char* line2);
void guesser_fx(GuesserApp* app, GuesserFx effect);
/**
 * Waits (if needed) until the loader is done; false if there is no usable
 * data. Only the first Play right after start can actually wait here.
 */
bool guesser_data_ok(GuesserApp* app);
/** The "data file missing/damaged" message; the app closes after OK. */
void guesser_show_data_error(GuesserApp* app);
