#include "../guesser_app_i.h"

void guesser_scene_guess_on_enter(void* context) {
    GuesserApp* app = context;
    guesser_view_show_guess(app->view, app->guess_name, app->guess_number);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
    guesser_fx(app, GuesserFxGuess);
}

bool guesser_scene_guess_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeBack) return true;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == GuesserViewEventGuessYes) {
        app->game_won = true;
        scene_manager_next_scene(app->scene_manager, GuesserSceneResult);
        return true;
    }
    if(event.event != GuesserViewEventGuessNo) return false;

    /* Wrong: zero that entity, take five more questions, try again (SPEC 6.5). */
    app->guess_number++;
    if(app->guess_number >= TQ_MAX_GUESSES) {
        app->game_won = false;
        scene_manager_next_scene(app->scene_manager, GuesserSceneResult);
        return true;
    }
    app->job = GuesserJobRejectGuess;
    scene_manager_next_scene(app->scene_manager, GuesserSceneThinking);
    return true;
}

void guesser_scene_guess_on_exit(void* context) {
    UNUSED(context);
}
