/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/virtio/lm-win32-scanout.h"
#include "ui/egl-helpers.h"
#include <virglrenderer.h>

struct LmWin32Scanout {
    uint32_t resource;
    GLuint memory;
    GLuint texture;
    GLuint semaphore;
    bool failed;
    HANDLE memory_handle;
    HANDLE semaphore_handle;
};

LmWin32Scanout *lm_win32_scanout_new(uint32_t resource, uint64_t size, uint32_t width,
                                     uint32_t height) {
    if (!epoxy_has_gl_extension("GL_EXT_memory_object_win32") ||
        !epoxy_has_gl_extension("GL_EXT_semaphore_win32")) {
        return NULL;
    }
    HANDLE handle = NULL;
    uint64_t actual_size = 0;
    uint32_t memory_type = 0;
    uint8_t device_uuid[16], driver_uuid[16], gl_driver[16];
    if (virgl_renderer_resource_export_opaque_win32_v1(resource, (void **)&handle, &actual_size,
                                                       device_uuid, driver_uuid, &memory_type)) {
        return NULL;
    }
    GLint devices = 0;
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &devices);
    glGetUnsignedBytevEXT(GL_DRIVER_UUID_EXT, gl_driver);
    bool matched = false;
    if (devices > 0 && devices <= 16 && !memcmp(driver_uuid, gl_driver, 16)) {
        for (GLint i = 0; i < devices; i++) {
            uint8_t uuid[16];
            glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, i, uuid);
            matched |= !memcmp(uuid, device_uuid, 16);
        }
    }
    if (!matched || size != actual_size || (uint64_t)width * height * 4 > size) {
        CloseHandle(handle);
        return NULL;
    }
    LmWin32Scanout *s = g_new0(LmWin32Scanout, 1);
    s->resource = resource;
    glCreateMemoryObjectsEXT(1, &s->memory);
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(s->memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glImportMemoryWin32HandleEXT(s->memory, size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    s->memory_handle = handle;
    glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, width, height, s->memory, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    handle = NULL;
    if (glGetError() != GL_NO_ERROR ||
        virgl_renderer_resource_win32_sync_v1(resource, 0, (void **)&handle)) {
        lm_win32_scanout_free(s);
        return NULL;
    }
    glGenSemaphoresEXT(1, &s->semaphore);
    glImportSemaphoreWin32HandleEXT(s->semaphore, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    s->semaphore_handle = handle;
    if (glGetError() != GL_NO_ERROR) {
        lm_win32_scanout_free(s);
        return NULL;
    }
    return s;
}
uint32_t lm_win32_scanout_texture(LmWin32Scanout *s) { return s->texture; }

bool lm_win32_scanout_begin(LmWin32Scanout *s) {
    if (s->failed || virgl_renderer_resource_win32_sync_v1(s->resource, 1, NULL)) {
        s->failed = true;
        return false;
    }
    GLenum layout = GL_LAYOUT_GENERAL_EXT;
    glWaitSemaphoreEXT(s->semaphore, 0, NULL, 1, &s->texture, &layout);
    s->failed = glGetError() != GL_NO_ERROR;
    return !s->failed;
}
bool lm_win32_scanout_end(LmWin32Scanout *s) {
    GLenum layout = GL_LAYOUT_GENERAL_EXT;
    glSignalSemaphoreEXT(s->semaphore, 0, NULL, 1, &s->texture, &layout);
    s->failed =
        glGetError() != GL_NO_ERROR || virgl_renderer_resource_win32_sync_v1(s->resource, 2, NULL);
    return !s->failed;
}
void lm_win32_scanout_free(LmWin32Scanout *s) {
    if (!s || s->failed) {
        return;
    }
    if (s->texture) {
        glDeleteTextures(1, &s->texture);
    }
    if (s->memory) {
        glDeleteMemoryObjectsEXT(1, &s->memory);
    }
    if (s->semaphore) {
        glDeleteSemaphoresEXT(1, &s->semaphore);
    }
    if (s->memory_handle)
        CloseHandle(s->memory_handle);
    if (s->semaphore_handle)
        CloseHandle(s->semaphore_handle);
    g_free(s);
}