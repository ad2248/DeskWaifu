#include "LAppWayland.hpp"
#include "LAppWaylandRegion.hpp"
#include <GL/glew.h>
#include <vector>
#include <iostream>
#include <chrono>

bool ClearWaylandInputRegion(WaylandContext* wl) {
    if (!wl || !wl->compositor || !wl->surface) return false;
    wl_region* empty = wl_compositor_create_region(wl->compositor);
    if (!empty) return false;
    wl_surface_set_input_region(wl->surface, empty);
    wl_region_destroy(empty);
    wl_surface_commit(wl->surface);
    return true;
}

void ClipWaylandModelRegion(ModelRegion region, int width, int height) {
    if (region.x == 0 && region.y == 0 && region.width == width && region.height == height) return;

    // Clear only after Cubism has finished drawing: its mask/offscreen passes
    // manage their own scissor state and must not inherit the display clip.
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissor[4];
    GLfloat clearColor[4];
    GLboolean colorMask[4];
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    for (const auto& outside : OutsideModelRegion(region, width, height)) {
        if (outside.width <= 0 || outside.height <= 0) continue;
        const auto box = ModelRegionToOpenGL(outside, height);
        glScissor(box.x, box.y, box.width, box.height);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    if (!scissorEnabled) glDisable(GL_SCISSOR_TEST);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
}

void DrawWaylandEditFrame(const std::vector<ModelRegion>& handles, int width, int height) {
    const GLboolean enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissor[4]; GLfloat color[4]; GLboolean mask[4];
    glGetIntegerv(GL_SCISSOR_BOX, scissor); glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glEnable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(.1f, .75f, 1.f, 1.f);
    for (auto handle : handles) {
        auto box = IntersectModelRegions(handle, {0, 0, width, height});
        if (box.width <= 0 || box.height <= 0) continue;
        box = ModelRegionToOpenGL(box, height);
        glScissor(box.x, box.y, box.width, box.height); glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    if (!enabled) glDisable(GL_SCISSOR_TEST);
    glClearColor(color[0], color[1], color[2], color[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
}

void UpdateWaylandInputRegion(WaylandContext* wl, ModelRegion clip, const std::vector<ModelRegion>& handles) {
    if (!wl || !wl->compositor || !wl->surface) return;

    int width = wl->width;
    int height = wl->height;
    if (width <= 0 || height <= 0) return;

    // Use a downsampled buffer to avoid freezing the GPU pipeline with huge glReadPixels
    // Downsample factor:
    const int DOWNSAMPLE = 8;
    int rw = width / DOWNSAMPLE;
    int rh = height / DOWNSAMPLE;

    std::vector<unsigned char> pixels(rw * rh * 4);

    // Read a 1/8 downsampled screen! 
    // To do this we could use glBlitFramebuffer, but GLES2 doesn't have it natively.
    // Instead we just read the full frame buffer with a step size? No, glReadPixels reads a continuous block.
    // Reading 1920x1080 takes ~2ms. We can afford to read it all and downsample on CPU.
    
    std::vector<unsigned char> full_pixels(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, full_pixels.data());

    wl_region* region = wl_compositor_create_region(wl->compositor);
    if (!region) return;
    const auto addRegion = [&](int x, int y, int w, int h) {
        const auto box = IntersectModelRegions({x, y, w, h}, clip);
        if (box.width > 0 && box.height > 0)
            wl_region_add(region, box.x, box.y, box.width, box.height);
    };

    // Build region
    // We process it in blocks of 'BLOCK_SIZE' (e.g. 10x10 pixels) to reduce the number of rectangles
    const int BLOCK_SIZE = 10;
    
    for (int y = 0; y < height; y += BLOCK_SIZE) {
        int start_x = -1;
        for (int x = 0; x < width; x += BLOCK_SIZE) {
            
            bool opaque = false;
            // Just check the center pixel of the block for speed
            int test_x = x + BLOCK_SIZE/2;
            int test_y = y + BLOCK_SIZE/2;
            if (test_x < width && test_y < height) {
                // OpenGL reads bottom-to-top, Wayland is top-to-bottom
                int idx = (test_y * width + test_x) * 4;
                if (full_pixels[idx + 3] > 10) { // Alpha > 10
                    opaque = true;
                }
            }

            if (opaque) {
                if (start_x < 0) start_x = x;
            } else {
                if (start_x >= 0) {
                    // OpenGL Y is bottom-to-top, Wayland surface Y is top-to-bottom
                    int wayland_y = height - y - BLOCK_SIZE;
                    addRegion(start_x, wayland_y, x - start_x, BLOCK_SIZE);
                    start_x = -1;
                }
            }
        }
        if (start_x >= 0) {
            int wayland_y = height - y - BLOCK_SIZE;
            addRegion(start_x, wayland_y, width - start_x, BLOCK_SIZE);
        }
    }

    for (auto handle : handles) {
        auto box = IntersectModelRegions(handle, {0, 0, width, height});
        if (box.width > 0 && box.height > 0) wl_region_add(region, box.x, box.y, box.width, box.height);
    }
    wl_surface_set_input_region(wl->surface, region);
    wl_region_destroy(region);
}
