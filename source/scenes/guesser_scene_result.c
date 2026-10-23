#include "../guesser_app_i.h"

static void guesser_record_result(GuesserApp* app) {
    /* Back from the keyboard returns here: count each game once */
    if(app->result_saved) return;
    app->result_saved = true;
    GuesserStats* s = &app->stats;
    if(s->games < 0xFFFF) s->games++;
    if(app->game_won) {
        if(s->wins < 0xFFFF) s->wins++;
        if(app->guess_number == 0) {
            s->questions_to_win += app->engine->asked_count;
            if(s->win_games < 0xFFFF) s->win_games++;
            if(s->best_game == 0 || app->engine->asked_count < s->best_game) {
                s->best_game = app->engine->asked_count;
            }
        }
    }
    guesser_stats_save(app->storage, s);
}

void guesser_scene_result_on_enter(void* context) {
    GuesserApp* app = context;
    guesser_record_result(app);

    if(app->game_won) {
        uint8_t rate = app->stats.games ? (uint8_t)(100u * app->stats.wins / app->stats.games) : 0;
        guesser_view_show_win(app->view, app->engine->asked_count, app->guess_number, rate);
        guesser_fx(app, GuesserFxWin);
    } else {
        guesser_view_show_lose(app->view);
        guesser_fx(app, GuesserFxLose);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewMain);
}

bool guesser_scene_result_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, GuesserSceneStart);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == GuesserViewEventResultTeach) {
        if(app->learned.count >= TQ_LEARN_MAX_RECORDS) {
            guesser_show_error(app, "My memory is full.", "Reset taught things.");
        } else {
            app->learn_name[0] = '\0';
            /* the category of my best guess is the best guess for theirs */
            Tq_Candidate top[1];
            app->learn_category = (tq_engine_top(app->engine, top, 1) == 1) ?
                                      tq_engine_entity_category(app->engine, top[0].index) :
                                      TQ_CATEGORY_COUNT - 1;
            scene_manager_next_scene(app->scene_manager, GuesserSceneLearnName);
        }
        return true;
    }
    if(event.event == GuesserViewEventResultOk) {
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, GuesserSceneStart);
        return true;
    }
    return false;
}

void guesser_scene_result_on_exit(void* context) {
    UNUSED(context);
}
