#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DwRuntime DwRuntime;
typedef struct DwModel DwModel;

// Handles belong to the calling thread. Destroy every model before its runtime.
// Errors are copied from thread-local storage immediately after a failed call.
const char* dw_error(void);
int32_t dw_create(int32_t width, int32_t height, DwRuntime** out);
// Report an EGL device. Each nullable string is borrowed during the callback.
typedef void (*DwDeviceVisitor)(void* data, const char* render_node, const char* pci);
int32_t dw_devices(DwDeviceVisitor visitor, void* data);
void dw_active_device(const DwRuntime* runtime, DwDeviceVisitor visitor, void* data);
int32_t dw_create_on_device(int32_t width, int32_t height, int32_t by_pci,
                            const uint8_t* selector, size_t length, DwRuntime** out);
// Borrow a live wl_display and wl_egl_window for the runtime's lifetime.
int32_t dw_create_wayland(int32_t width, int32_t height, void* display, void* window, DwRuntime** out);
const char* dw_renderer(const DwRuntime* runtime);
int32_t dw_load(DwRuntime* runtime, const uint8_t* path, size_t length, DwModel** out);
int32_t dw_uses_masking(const DwModel* model);
int32_t dw_metadata_count(DwModel* model, int32_t expressions);
// The name is valid until the next model operation. The caller copies it immediately.
int32_t dw_metadata_entry(DwModel* model, int32_t expressions, int32_t index,
                          const char** name, int32_t* motions);
int32_t dw_expression(DwModel* model, const uint8_t* name, size_t length);
// Optional durations use -1 for disabled; all other values are finite nonnegative seconds.
int32_t dw_set_expression_timeout(DwModel* model, float timeout);
int32_t dw_toggle_expression(DwModel* model, const uint8_t* file, size_t length,
                             float fade, float duration, int32_t* active);
int32_t dw_expression_toggle_active(const DwModel* model, const uint8_t* file, size_t length,
                                   int32_t* active);
void dw_reset_expression_toggles(DwModel* model);
int32_t dw_motion(DwModel* model, const uint8_t* group, size_t length, int32_t index);
int32_t dw_switch_skin(DwModel* model);
typedef enum DwTapHit { DW_TAP_MISS, DW_TAP_HEAD, DW_TAP_BODY } DwTapHit;
// Coordinates are normalized in the top-down render buffer. The result is a DwTapHit.
int32_t dw_tap(DwModel* model, float x, float y, int32_t* hit);
// Active is 1 to set a persistent control or 0 to clear it.
int32_t dw_set_lip_sync(DwModel* model, int32_t active, float opening);
int32_t dw_set_look(DwModel* model, int32_t active, float x, float y);
// The output is tightly packed, top-down RGBA8 with premultiplied alpha.
int32_t dw_frame(DwModel* model, float dt, uint8_t* pixels, size_t length);
// Draw and blit without committing the Wayland surface.
int32_t dw_draw_window(DwModel* model, float dt);
// Read the staged back buffer, or a ceil(width/8) by ceil(height/8) sampled model FBO.
int32_t dw_read_window(DwRuntime* runtime, int32_t sampled, uint8_t* pixels, size_t length);
// Swap commits any pending input region along with the EGL buffer.
int32_t dw_present_window(DwRuntime* runtime);
void dw_model_destroy(DwModel* model);
void dw_destroy(DwRuntime* runtime);

#ifdef __cplusplus
}
#endif
