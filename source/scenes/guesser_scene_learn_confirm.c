#include "../guesser_app_i.h"

/** Stores this game's answers against the new word, so it is findable next time. */
static bool guesser_learn_save(GuesserApp* app) {
    Tq_Engine* eng = app->engine;
    uint16_t qids[TQ_LEARN_MAX_ANSWERS];
    uint8_t nibs[TQ_LEARN_MAX_ANSWERS];
    uint16_t count = 0;

    for(uint16_t i = 0; i < eng->asked_count && count < TQ_LEARN_MAX_ANSWERS; i++) {
        Tq_Answer a = (Tq_Answer)eng->answers[i];
        if(a == TqAnswerUnknown) continue; /* carries no information */
        qids[count] = eng->asked_order[i];
        nibs[count] = tq_answer_to_nibble(&app->dataset, a);
        count++;
    }

    if(!tq_learned_add(&app->learned, app->learn_name, app->learn_category, qids, nibs, count)) {
        return false;
    }
    return guesser_learned_save_file(app->storage, &app->learned);
}

void guesser_scene_learn_confirm_on_enter(void* context) {
    GuesserApp* app = context;
    snprintf(app->message, sizeof(app->message), "\"%s\"", app->learn_name);
    guesser_view_show_dialog(
        app->view, "Remember it?", app->message, "I'll know it next time.", "No", "Save");
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_learn_confirm_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, GuesserSceneStart);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == GuesserViewEventDialogRight) {
        if(!guesser_learn_save(app)) {
            guesser_show_error(app, "Could not save.", "Check the SD card.");
            return true;
        }
        guesser_notify(app, &sequence_success);
        guesser_show_message(app, "Thanks!", "I learned", app->learn_name);
        return true;
    }
    if(event.event == GuesserViewEventDialogLeft) {
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, GuesserSceneStart);
        return true;
    }
    return false;
}

void guesser_scene_learn_confirm_on_exit(void* context) {
    UNUSED(context);
}
