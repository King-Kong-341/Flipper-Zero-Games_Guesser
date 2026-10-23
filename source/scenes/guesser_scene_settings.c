#include "../guesser_app_i.h"

typedef enum {
    SettingIndexQuestions,
    SettingIndexSound,
    SettingIndexVibro,
    SettingIndexDefaultAnswer,
    SettingIndexResetLearned,
    SettingIndexResetStats,
} SettingIndex;

static const uint8_t kQuestionLimits[] = {15, 20, 25};
static const char* const kQuestionLabels[] = {"15", "20", "25"};
static const char* const kOnOff[] = {"Off", "On"};
static const char* const kDefaultAnswer[] = {"?", "Yes"};

static void guesser_settings_questions_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kQuestionLabels[index]);
    app->settings.question_limit = kQuestionLimits[index];
    guesser_settings_save(app->storage, &app->settings);
}

static void guesser_settings_sound_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kOnOff[index]);
    app->settings.sound = index;
    guesser_settings_save(app->storage, &app->settings);
}

static void guesser_settings_vibro_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kOnOff[index]);
    app->settings.vibro = index;
    guesser_settings_save(app->storage, &app->settings);
}

static void guesser_settings_default_changed(VariableItem* item) {
    GuesserApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kDefaultAnswer[index]);
    app->settings.default_answer_yes = index;
    guesser_settings_save(app->storage, &app->settings);
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

    VariableItem* item;
    item = variable_item_list_add(
        list, "Questions", COUNT_OF(kQuestionLimits), guesser_settings_questions_changed, app);
    variable_item_set_current_value_index(item, limit_index);
    variable_item_set_current_value_text(item, kQuestionLabels[limit_index]);

    item = variable_item_list_add(list, "Sound", 2, guesser_settings_sound_changed, app);
    variable_item_set_current_value_index(item, app->settings.sound ? 1 : 0);
    variable_item_set_current_value_text(item, kOnOff[app->settings.sound ? 1 : 0]);

    item = variable_item_list_add(list, "Vibration", 2, guesser_settings_vibro_changed, app);
    variable_item_set_current_value_index(item, app->settings.vibro ? 1 : 0);
    variable_item_set_current_value_text(item, kOnOff[app->settings.vibro ? 1 : 0]);

    item = variable_item_list_add(list, "Answer starts on", 2, guesser_settings_default_changed, app);
    variable_item_set_current_value_index(item, app->settings.default_answer_yes ? 1 : 0);
    variable_item_set_current_value_text(
        item, kDefaultAnswer[app->settings.default_answer_yes ? 1 : 0]);

    variable_item_list_add(list, "Forget taught things", 0, NULL, app);
    variable_item_list_add(list, "Reset stats", 0, NULL, app);

    variable_item_list_set_enter_callback(list, guesser_settings_enter_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, GuesserViewVarItemList);
}

bool guesser_scene_settings_on_event(void* context, SceneManagerEvent event) {
    GuesserApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == SettingIndexResetLearned) {
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
