#include "../guesser_app_i.h"

typedef enum {
    SettingIndexQuestions,
    SettingIndexDefaultAnswer,
    SettingIndexSound,
    SettingIndexVolume,
    SettingIndexClicks,
    SettingIndexVibro,
    SettingIndexLed,
    SettingIndexResetLearned,
    SettingIndexResetStats,
} SettingIndex;

static const uint8_t kQuestionLimits[] = {15, 20, 25};
static const char* const kQuestionLabels[] = {"15", "20", "25"};
static const char* const kOnOff[] = {"Off", "On"};
static const char* const kDefaultAnswer[] = {"?", "Yes"};
static const char* const kVolume[] =
    {"0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"};

/* every change is saved at once and the effects follow it */
static void guesser_settings_apply(GuesserApp* app) {
    guesser_settings_save(app->storage, &app->settings);
    guesser_fx_configure(app->fx, &app->settings);
}

static void guesser_settings_questions_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kQuestionLabels[index]);
    app->settings.question_limit = kQuestionLimits[index];
    guesser_settings_apply(app);
}

static void guesser_settings_default_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kDefaultAnswer[index]);
    app->settings.default_answer_yes = index;
    guesser_settings_apply(app);
}

static void guesser_settings_sound_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kOnOff[index]);
    app->settings.sound = index;
    guesser_settings_apply(app);
    guesser_fx(app, GuesserFxTestSound);
}

static void guesser_settings_volume_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kVolume[index]);
    app->settings.volume = index;
    guesser_settings_apply(app);
    guesser_fx(app, GuesserFxTestSound); /* hear the new volume */
}

static void guesser_settings_clicks_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kOnOff[index]);
    app->settings.clicks = index;
    guesser_settings_apply(app);
    guesser_fx(app, GuesserFxSelect);
}

static void guesser_settings_vibro_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kOnOff[index]);
    app->settings.vibro = index;
    guesser_settings_apply(app);
    guesser_fx(app, GuesserFxTestVibro);
}

static void guesser_settings_led_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kOnOff[index]);
    app->settings.led = index;
    guesser_settings_apply(app);
    guesser_fx(app, GuesserFxTestLed);
}

static VariableItem* guesser_settings_add(
    VariableItemList* list,
    const char* label,
    uint8_t count,
    VariableItemChangeCallback callback,
    GuesserApp* app,
    uint8_t index,
    const char* const* texts) {
    VariableItem* item = variable_item_list_add(list, label, count, callback, app);
    variable_item_set_current_value_index(item, index);
    variable_item_set_current_value_text(item, texts[index]);
    return item;
}

static void guesser_settings_enter_callback(void* context, uint32_t index) {
    GuesserApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void guesser_scene_settings_on_enter(void* context) {
    GuesserApp* app = context;
    VariableItemList* list = app->var_item_list;
    variable_item_list_reset(list);

    uint8_t limit_index = 1;
    for(uint8_t i = 0; i < COUNT_OF(kQuestionLimits); i++) {
        if(kQuestionLimits[i] == app->settings.question_limit) limit_index = i;
    }

    const GuesserSettings* set = &app->settings;
    guesser_settings_add(
        list,
        "Questions",
        COUNT_OF(kQuestionLimits),
        guesser_settings_questions_changed,
        app,
        limit_index,
        kQuestionLabels);
    guesser_settings_add(
        list,
        "Answer starts on",
        2,
        guesser_settings_default_changed,
        app,
        set->default_answer_yes ? 1 : 0,
        kDefaultAnswer);
    guesser_settings_add(
        list, "Sound", 2, guesser_settings_sound_changed, app, set->sound ? 1 : 0, kOnOff);
    guesser_settings_add(
        list,
        "Volume",
        COUNT_OF(kVolume),
        guesser_settings_volume_changed,
        app,
        set->volume <= 10 ? set->volume : 7,
        kVolume);
    guesser_settings_add(
        list, "Key sounds", 2, guesser_settings_clicks_changed, app, set->clicks ? 1 : 0, kOnOff);
    guesser_settings_add(
        list, "Vibration", 2, guesser_settings_vibro_changed, app, set->vibro ? 1 : 0, kOnOff);
    guesser_settings_add(
        list, "LED", 2, guesser_settings_led_changed, app, set->led ? 1 : 0, kOnOff);

    variable_item_list_add(list, "Forget taught things", 0, NULL, app);
    variable_item_list_add(list, "Reset stats", 0, NULL, app);

    variable_item_list_set_enter_callback(list, guesser_settings_enter_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewVarItemList);
}

bool guesser_scene_settings_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == SettingIndexResetLearned) {
        guesser_data_ok(app); /* the taught things are loaded with the data */
        tq_learned_clear(&app->learned);
        guesser_learned_delete_file(app->storage);
        guesser_show_message(app, "Done", "Taught things", "are forgotten.");
        return true;
    }
    if(event.event == SettingIndexResetStats) {
        memset(&app->stats, 0, sizeof(app->stats));
        app->stats.magic = GUESSER_STATS_MAGIC;
        app->stats.version = GUESSER_STATS_VERSION;
        guesser_stats_save(app->storage, &app->stats);
        guesser_show_message(app, "Done", "Stats are reset.", "");
        return true;
    }
    return false;
}

void guesser_scene_settings_on_exit(void* context) {
    GuesserApp* app = context;
    guesser_settings_save(app->storage, &app->settings);
    variable_item_list_reset(app->var_item_list);
}
