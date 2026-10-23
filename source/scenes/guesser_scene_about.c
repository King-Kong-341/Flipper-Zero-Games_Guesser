#include "../guesser_app_i.h"

/* "How to play": three animated pages, Left/Right to flip. */

void guesser_scene_about_on_enter(void* context) {
    GuesserApp* app = context;
    guesser_view_show_help(app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_about_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == GuesserViewEventBack) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void guesser_scene_about_on_exit(void* context) {
    UNUSED(context);
}
