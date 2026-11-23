/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LM_WIN32_SCANOUT_H
#define LM_WIN32_SCANOUT_H
#include <stdint.h>
#include <stdbool.h>
typedef struct LmWin32Scanout LmWin32Scanout;
/* Main GL thread only. Owns independent imported allocation and
 * synchronization. Producer has completed its Vulkan fence and released
 * ownership in GENERAL.
 */
LmWin32Scanout *lm_win32_scanout_new(uint32_t resource, uint64_t size, uint32_t width,
                                     uint32_t height);
uint32_t lm_win32_scanout_texture(LmWin32Scanout *scanout);
bool lm_win32_scanout_begin(LmWin32Scanout *scanout);
bool lm_win32_scanout_end(LmWin32Scanout *scanout);
void lm_win32_scanout_free(LmWin32Scanout *scanout);
#endif
