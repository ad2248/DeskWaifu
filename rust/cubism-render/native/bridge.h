#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All functions return 0 on success, -1 on error. Error text is thread-local.
 * Opaque handles are checked against live registries. GL context/thread must
 * remain current from start through stop. stop refuses live dependent handles.
 *
 * Motion, expression and eye-blink primitives are exposed without application
 * policy: the caller chooses groups, priorities, fades and blink parameter IDs.
 * A motion/expression manager keeps a reference to every started motion until
 * stop/destroy, so destroying one earlier is an error. Managers retain their
 * model lease. All handles must be destroyed before cr_stop.
 */
const char* cr_error(void);
int cr_start(const char* standard_shader_directory);
int cr_stop(void);
int cr_model_create(const uint8_t* bytes, size_t size, void** out);
int cr_model_destroy(void* model);
int cr_model_update(void* model);
int cr_model_load_parameters(void* model);
int cr_model_save_parameters(void* model);
int cr_model_canvas(void* model, float* width, float* height);
int cr_parameter_count(void* model, int* count);
int cr_parameter_get(void* model, int index, const char** id, float* value, float* min, float* max, float* initial);
int cr_parameter_set(void* model, int index, float value);
int cr_renderer_create(void* model, int width, int height, void** out);
int cr_renderer_destroy(void* renderer);
int cr_renderer_bind_texture(void* renderer, int slot, uint32_t texture);
int cr_renderer_resize(void* renderer, int width, int height);
int cr_renderer_draw(void* renderer, const float* matrix16);
/* .motion3.json bytes and .exp3.json bytes are parsed by the Framework. */
int cr_motion_create(const uint8_t* bytes, size_t size, void** out);
int cr_motion_destroy(void* motion);
int cr_motion_set_fade_in(void* motion, float seconds);
int cr_motion_set_fade_out(void* motion, float seconds);
int cr_motion_manager_create(void* model, void** out);
int cr_motion_manager_destroy(void* manager);
/* Start returns an opaque entry token for cr_motion_manager_entry_is_finished. */
int cr_motion_manager_start(void* manager, void* motion, int priority, uint64_t* out_entry);
int cr_motion_manager_update(void* manager, float delta_seconds, int* out_updated);
int cr_motion_manager_stop(void* manager);
int cr_motion_manager_is_finished(void* manager, int* out_finished);
int cr_motion_manager_entry_is_finished(void* manager, uint64_t entry, int* out_finished);
int cr_expression_create(const uint8_t* bytes, size_t size, void** out);
int cr_expression_destroy(void* expression);
int cr_expression_manager_create(void* model, void** out);
int cr_expression_manager_destroy(void* manager);
int cr_expression_manager_start(void* manager, void* expression);
int cr_expression_manager_update(void* manager, float delta_seconds, int* out_updated);
/* Parameter IDs must exist on the given model; the caller supplies the list. */
int cr_eye_blink_create(void* model, const char* const* parameter_ids, int count, void** out);
int cr_eye_blink_destroy(void* eye_blink);
int cr_eye_blink_set_interval(void* eye_blink, float seconds);
int cr_eye_blink_set_settings(void* eye_blink, float closing, float closed, float opening);
int cr_eye_blink_update(void* eye_blink, void* model, float delta_seconds);
#ifdef __cplusplus
}
#endif
