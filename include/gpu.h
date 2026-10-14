/*
 * TermmiK
 * Copyright (C) 2026 SfymmiK
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef GPU_H
#define GPU_H

#include "vt_parser.h"

// EGL/GLES2 renderer. gpu_init() returns 0 when a context was created and the
// GPU path is active; any failure leaves the CPU renderer in charge.
int  gpu_init(void);
void gpu_cleanup(void);
void gpu_resize(int width, int height);
void gpu_draw(VTState *state);
void gpu_present(void);
void gpu_pump(void);
void term_request_render(void);

extern int g_use_gpu;

#endif
