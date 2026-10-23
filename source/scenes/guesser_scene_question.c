#include "../guesser_app_i.h"

void guesser_scene_question_on_enter(void* context) {
    GuesserApp* app = context;
    guesser_view_show_question(
        app->view,
        app->question_text,
        app->engine->asked_count,
        tq_engine_question_budget(app->engine),
        app->settings.default_answer_yes ? TqAnswerYes : TqAnswerUnknown);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_question_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeBack) return true; /* the view handles Back */
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event >= GuesserViewEventAnswer &&
       event.event < GuesserViewEventAnswer + TqAnswerCount) {
        app->pending_answer = (Tq_Answer)(event.event - GuesserViewEventAnswer);
        guesser_notify(app, &sequence_blink_blue_10);
        app->job = GuesserJobAnswer;
        scene_manager_next_scene(app->scene_manager, GuesserSceneThinking);
        return true;
    }
    if(event.event == GuesserViewEventUndo) {
        if(app->engine->asked_count == 0) {
            scene_manager_next_scene(app->scene_manager, GuesserSceneQuitConfirm);
        } else {
            app->job = GuesserJobUndo;
            scene_manager_next_scene(app->scene_manager, GuesserSceneThinking);
        }
        return true;
    }
    if(event.event == GuesserViewEventQuit) {
        scene_manager_next_scene(app->scene_manager, GuesserSceneQuitConfirm);
        return true;
    }
    return false;
}

void guesser_scene_question_on_exit(void* context) {
    UNUSED(context);
}
