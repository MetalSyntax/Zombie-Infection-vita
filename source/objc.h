/**
 * @file objc.h
 * @brief Objective-C selector dispatch runtime and on-screen keyboard IME bridge.
 */
#pragma once
#include <stdint.h>

void objc_init(void);
void uikit_init(void);
void ime_poll(void);
void ime_open(int type, void (*done)(const char *utf8));
int ime_active(void);
