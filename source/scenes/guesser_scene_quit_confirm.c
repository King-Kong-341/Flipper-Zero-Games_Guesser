#include "../guesser_app_i.h"

void guesser_scene_quit_confirm_on_enter(void* context) {
    GuesserApp* app = context;
    guesser_view_show_dialog(
        app->view, "Give up?", "This game will not", "be counted.", "Keep going", "Quit");
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_quit_confirm_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == GuesserViewEventDialogRight) {
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, GuesserSceneStart);
        return true;
    }
    if(event.event == GuesserViewEventDialogLeft) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void guesser_scene_quit_confirm_on_exit(void* context) {
    UNUSED(context);
}
