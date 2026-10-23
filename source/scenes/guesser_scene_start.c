#include "../guesser_app_i.h"

/* The main menu. Play starts straight away -- no category question. */

void guesser_scene_start_on_enter(void* context) {
    GuesserApp* app = context;
    guesser_view_show_menu(app->view, app->menu_index);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_start_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == GuesserViewEventBack) {
        scene_manager_stop(app->scene_manager);
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    if(event.event < GuesserViewEventMenu ||
       event.event >= GuesserViewEventMenu + GuesserMenuCount)
        return false;

    app->menu_index = (uint8_t)(event.event - GuesserViewEventMenu);
    switch(app->menu_index) {
    case GuesserMenuPlay:
        app->guess_number = 0;
        app->game_won = false;
        app->result_saved = false;
        app->job = GuesserJobStartGame;
        scene_manager_next_scene(app->scene_manager, GuesserSceneThinking);
        break;
    case GuesserMenuHelp:
        scene_manager_next_scene(app->scene_manager, GuesserSceneAbout);
        break;
    case GuesserMenuStats:
        scene_manager_next_scene(app->scene_manager, GuesserSceneStats);
        break;
    default:
        scene_manager_next_scene(app->scene_manager, GuesserSceneSettings);
        break;
    }
    return true;
}

void guesser_scene_start_on_exit(void* context) {
    UNUSED(context);
}
