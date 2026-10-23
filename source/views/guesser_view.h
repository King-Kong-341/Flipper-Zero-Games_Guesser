/**
 * The one custom view Guesser draws everything with (except the keyboard and
 * the settings list): intro, menu, thinking, question, guess reveal, results,
 * stats, help and dialogs -- all with a 25 fps animation timer.
 */
#pragma once

#include <gui/view.h>

#include "../helpers/guesser_engine.h"

typedef struct GuesserView GuesserView;

/* Events the view sends; numbered above the app's own custom events. */
typedef enum {
    GuesserViewEventIntroDone = 100,
    GuesserViewEventMenu = 101, /* + menu index 0..3 */
    GuesserViewEventAnswer = 110, /* + Tq_Answer 0..4 */
    GuesserViewEventUndo = 120,
    GuesserViewEventQuit,
    GuesserViewEventGuessYes,
    GuesserViewEventGuessNo,
    GuesserViewEventResultOk,
    GuesserViewEventResultTeach,
    GuesserViewEventDialogLeft,
    GuesserViewEventDialogRight,
    GuesserViewEventBack,
    GuesserViewEventClick, /* the selection moved (key sound only) */
} GuesserViewEvent;

typedef enum {
    GuesserMenuPlay,
    GuesserMenuHelp,
    GuesserMenuStats,
    GuesserMenuSettings,
    GuesserMenuCount,
} GuesserMenuItem;

typedef void (*GuesserViewCallback)(uint32_t event, void* context);

GuesserView* guesser_view_alloc(void);
void guesser_view_free(GuesserView* view);
View* guesser_view_get_view(GuesserView* view);
void guesser_view_set_callback(GuesserView* view, GuesserViewCallback callback, void* context);

void guesser_view_show_intro(GuesserView* view);
/**
 * The app is working on the next turn: the screen stays, keys are ignored,
 * and only if it takes longer than ~0.5 s the thinking animation takes over.
 * Any show_* call ends it.
 */
void guesser_view_set_busy(GuesserView* view);
void guesser_view_show_menu(GuesserView* view, uint8_t selected);
void guesser_view_show_thinking(GuesserView* view);
void guesser_view_show_question(
    GuesserView* view,
    const char* text,
    uint16_t asked,
    uint16_t total,
    Tq_Answer default_answer);
void guesser_view_show_guess(GuesserView* view, const char* name, uint8_t guess_number);
void guesser_view_show_win(GuesserView* view, uint16_t questions, uint8_t guess_number, uint8_t win_rate);
void guesser_view_show_lose(GuesserView* view);
void guesser_view_show_stats(
    GuesserView* view,
    uint16_t games,
    uint16_t wins,
    uint16_t best,
    uint16_t average,
    uint16_t learned);
void guesser_view_show_help(GuesserView* view);
/** right may be NULL for a single "OK" button. */
void guesser_view_show_dialog(
    GuesserView* view,
    const char* title,
    const char* line1,
    const char* line2,
    const char* left,
    const char* right);
