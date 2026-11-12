#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All functions return 0 on success, -1 on error. Error text is thread-local.
 * Opaque handles are checked against live registries. GL context/thread must
 * remain current from start through stop. stop refuses live dependent handles.
 */
const char* cr_error(void);
int cr_start(const char* standard_shader_directory);
int cr_stop(void);
int cr_model_create(const uint8_t* bytes, size_t size, void** out);
int cr_model_destroy(void* model);
int cr_model_update(void* model);
int cr_model_canvas(void* model, float* width, float* height);
int cr_parameter_count(void* model, int* count);
int cr_parameter_get(void* model, int index, const char** id, float* value, float* min, float* max, float* initial);
int cr_parameter_set(void* model, int index, float value);
int cr_renderer_create(void* model, int width, int height, void** out);
int cr_renderer_destroy(void* renderer);
int cr_renderer_bind_texture(void* renderer, int slot, uint32_t texture);
int cr_renderer_resize(void* renderer, int width, int height);
int cr_renderer_draw(void* renderer, const float* matrix16);
#ifdef __cplusplus
}
#endif
