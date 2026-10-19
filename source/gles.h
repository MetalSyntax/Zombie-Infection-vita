/**
 * @file gles.h
 * @brief OpenGL ES 1.1 / 2.0 graphics pipeline over vitaGL with SoftFP calling convention.
 */
#pragma once
#include <stdint.h>

/**
 * @brief Initializes the OpenGL ES pipeline and off-screen virtual FBO.
 */
void gles_init(void);

/**
 * @brief Presents current frame: blits and scales iOS FBO to PS Vita 960x544 screen.
 * @param rotate180 1 when the game renders for device orientation 1 (rotated 180 degrees vs 3).
 */
void gles_present(int rotate180);

/**
 * @brief Returns the total number of draw calls dispatched this session.
 */
uint32_t gles_get_draw_calls(void);

/**
 * @brief Marks the currently bound renderbuffer as the CAEAGLLayer drawable
 *        ([EAGLContext renderbufferStorage:fromDrawable:]).
 */
void gles_bind_drawable(void);
