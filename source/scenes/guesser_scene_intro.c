#include "../guesser_app_i.h"

/* The animated logo; the view sends IntroDone after ~2 s or on any key. */

void guesser_scene_intro_on_enter(void* context) {
    GuesserApp* app = context;
    /* the intro has been playing since app start (while loading) */
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_intro_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == GuesserViewEventIntroDone) {
        /* replace the intro, so Back in the menu leaves the app */
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, GuesserSceneStart);
        return true;
    }
    if(event.type == SceneManagerEventTypeBack) return true; /* skip with any key instead */
    return false;
}

void guesser_scene_intro_on_exit(void* context) {
    UNUSED(context);
}
