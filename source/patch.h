/**
 * @file patch.h
 * @brief Game-specific hooks, memory patches, and per-frame logic callbacks.
 */
#pragma once
#include "imports.h"

/**
 * @brief Initializes game-specific memory patches, trampolines, and binary hooks.
 */
void patch_init(void);

/**
 * @brief Per-frame callback invoked from the main loop for custom game logic.
 */
void patch_frame(void);

/**
 * @brief Adds synthetic touches for physical buttons / left stick after input_poll().
 */
void patch_input(void);

/**
 * @brief Draws loader overlays (camera sensitivity bar) on the screen framebuffer before the swap.
 *        Uses only scissor + clear; gles_present saves and restores the GL state around it.
 */
void patch_overlay(void);
