#include "../guesser_app_i.h"

void guesser_scene_stats_on_enter(void* context) {
    GuesserApp* app = context;
    const GuesserStats* s = &app->stats;
    uint16_t average = s->win_games ? (uint16_t)(s->questions_to_win / s->win_games) : 0;
    guesser_view_show_stats(app->view, s->games, s->wins, s->best_game, average, app->learned.count);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_stats_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == GuesserViewEventBack) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void guesser_scene_stats_on_exit(void* context) {
    UNUSED(context);
}
