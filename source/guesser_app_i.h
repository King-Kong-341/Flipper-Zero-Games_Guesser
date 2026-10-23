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

    GuesserView* view;
    TextInput* text_input;
    VariableItemList* var_item_list;

    /* --- game state --- */
    GuesserStorage store; /* dataset file handle + reader bridge */
    Tq_Dataset dataset;
    bool dataset_ready;
    Tq_Learned learned;
    Tq_Engine* engine;

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
    bool game_won;
    bool result_saved; /* this game's result is in the stats already */
} GuesserApp;

/** A one-button message on the animated view; returns to the menu after OK. */
void guesser_show_message(GuesserApp* app, const char* header, const char* line1, const char* line2);
void guesser_show_error(GuesserApp* app, const char* line1, const char* line2);
void guesser_notify(GuesserApp* app, const NotificationSequence* seq);
