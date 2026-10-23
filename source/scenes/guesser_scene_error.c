#include "../guesser_app_i.h"

/* One-button message: errors and short confirmations. */

void guesser_scene_error_on_enter(void* context) {
    GuesserApp* app = context;
    guesser_view_show_dialog(
        app->view,
        app->message_header[0] ? app->message_header : "Oops",
        app->message,
        app->message2,
        "OK",
        NULL);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_error_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeBack ||
       (event.type == SceneManagerEventTypeCustom && event.event == GuesserViewEventDialogLeft)) {
        if(app->dataset_ready) {
            /* recoverable: back to the menu */
            scene_manager_search_and_switch_to_another_scene(app->scene_manager, GuesserSceneStart);
        } else {
            /* nothing to play with: leave cleanly */
            scene_manager_stop(app->scene_manager);
            view_dispatcher_stop(app->view_dispatcher);
        }
        return true;
    }
    return false;
}

void guesser_scene_error_on_exit(void* context) {
    UNUSED(context);
}
