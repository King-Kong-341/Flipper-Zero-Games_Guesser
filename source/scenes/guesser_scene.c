#include "guesser_scene.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const guesser_scene_on_enter_handlers[])(void*) = {
#include "guesser_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const guesser_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "guesser_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const guesser_scene_on_exit_handlers[])(void* context) = {
#include "guesser_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers guesser_scene_handlers = {
    .on_enter_handlers = guesser_scene_on_enter_handlers,
    .on_event_handlers = guesser_scene_on_event_handlers,
    .on_exit_handlers = guesser_scene_on_exit_handlers,
    .scene_num = GuesserSceneCount,
};
