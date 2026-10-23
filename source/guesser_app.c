#include "guesser_app_i.h"

#include <furi_hal_random.h>

static bool guesser_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    GuesserApp* app = context;
    if(event == GuesserViewEventClick) {
        /* the selection moved: just the key sound, no scene cares */
        guesser_fx(app, GuesserFxClick);
        return true;
    }
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool guesser_back_event_callback(void* context) {
    furi_assert(context);
    GuesserApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

/* The animated view reports everything as a custom event. */
static void guesser_view_event_callback(uint32_t event, void* context) {
    GuesserApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

void guesser_fx(GuesserApp* app, GuesserFx effect) {
    guesser_fx_play(app->fx, effect);
}

#define GUESSER_LOADED_FLAG 1u

bool guesser_data_ok(GuesserApp* app) {
    furi_event_flag_wait(
        app->loaded, GUESSER_LOADED_FLAG, FuriFlagWaitAny | FuriFlagNoClear, FuriWaitForever);
    return app->dataset_ready && !app->data_bad;
}

void guesser_show_data_error(GuesserApp* app) {
    const char* what = app->data_bad                   ? "Data file damaged." :
                       (app->load_error == TqErrIo) ? "Data file missing." :
                                                        "Data file unusable.";
    guesser_fx(app, GuesserFxError);
    guesser_show_message(app, "Oops", what, "Please reinstall the app.");
    app->message_fatal = true;
}

/* Runs alongside the intro and the menu: opens the knowledge base (a few
 * small tables, fast), lets the game start, then checks the whole 4 MB file
 * once -- the very first start only -- while the player is already playing. */
static int32_t guesser_loader_thread(void* context) {
    GuesserApp* app = context;
    Tq_Error err = guesser_storage_open_dataset(&app->store, app->storage, &app->dataset);
    if(err == TqOk) {
        guesser_learned_load_file(app->storage, &app->learned, app->dataset.crc32);
        app->engine = tq_engine_alloc(&app->dataset, &app->learned);
        tq_engine_seed(app->engine, furi_hal_random_get());
        FURI_LOG_I(
            TAG,
            "%s: %lu entities x %lu questions",
            app->dataset.dataset_id,
            app->dataset.entity_count,
            app->dataset.question_count);
    }
    app->load_error = err;
    app->dataset_ready = (err == TqOk);
    furi_event_flag_set(app->loaded, GUESSER_LOADED_FLAG);

    if(err == TqOk) {
        furi_thread_set_current_priority(FuriThreadPriorityLow);
        if(!guesser_storage_verify_dataset(app->storage, &app->dataset, &app->closing)) {
            app->data_bad = true;
        }
    }
    return 0;
}

void guesser_show_message(GuesserApp* app, const char* header, const char* line1, const char* line2) {
    strncpy(app->message_header, header, sizeof(app->message_header) - 1);
    app->message_header[sizeof(app->message_header) - 1] = '\0';
    strncpy(app->message, line1 ? line1 : "", sizeof(app->message) - 1);
    app->message[sizeof(app->message) - 1] = '\0';
    strncpy(app->message2, line2 ? line2 : "", sizeof(app->message2) - 1);
    app->message2[sizeof(app->message2) - 1] = '\0';
    app->message_fatal = false;
    scene_manager_next_scene(app->scene_manager, GuesserSceneError);
}

void guesser_show_error(GuesserApp* app, const char* line1, const char* line2) {
    guesser_show_message(app, "Oops", line1, line2);
}

static GuesserApp* guesser_app_alloc(void) {
    GuesserApp* app = malloc(sizeof(GuesserApp));
    memset(app, 0, sizeof(GuesserApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->loaded = furi_event_flag_alloc();

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&guesser_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, guesser_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, guesser_back_event_callback);

    app->view = guesser_view_alloc();
    guesser_view_set_callback(app->view, guesser_view_event_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, GuesserViewMain, guesser_view_get_view(app->view));
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GuesserViewTextInput, text_input_get_view(app->text_input));
    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        GuesserViewVarItemList,
        variable_item_list_get_view(app->var_item_list));

    guesser_settings_load(app->storage, &app->settings);
    guesser_stats_load(app->storage, &app->stats);
    app->fx = guesser_fx_alloc(app->notifications);
    guesser_fx_configure(app->fx, &app->settings);
    return app;
}

static void guesser_app_free(GuesserApp* app) {
    furi_assert(app);

    /* stop the background check before the data goes away */
    app->closing = true;
    furi_thread_join(app->loader);
    furi_thread_free(app->loader);
    guesser_fx_free(app->fx);

    if(app->engine) tq_engine_free(app->engine);
    tq_learned_clear(&app->learned);
    guesser_storage_close_dataset(&app->store, app->dataset_ready ? &app->dataset : NULL);

    view_dispatcher_remove_view(app->view_dispatcher, GuesserViewMain);
    view_dispatcher_remove_view(app->view_dispatcher, GuesserViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, GuesserViewVarItemList);
    guesser_view_free(app->view);
    text_input_free(app->text_input);
    variable_item_list_free(app->var_item_list);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_event_flag_free(app->loaded);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t guesser_app(void* p) {
    UNUSED(p);
    GuesserApp* app = guesser_app_alloc();

    /* The intro plays straight away; the knowledge base loads behind it
     * and keeps loading while the menu is up. */
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, GuesserSceneIntro);

    /* 3 KB: opening peaks at ~1.4 KB (tq_dataset_open + block loads) */
    app->loader = furi_thread_alloc_ex("GuesserLoader", 3072, guesser_loader_thread, app);
    furi_thread_start(app->loader);

    view_dispatcher_run(app->view_dispatcher);
    guesser_app_free(app);
    return 0;
}
