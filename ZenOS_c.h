#ifndef ZenOS_C_H
#define ZenOS_C_H
/**
 * @file    ZenOS_c.h
 * @brief   Thin C wrapper for ZenOS — use from .c files only
 *
 * Provides declarations for the few ZenOS functions callable from C.
 * For C++ code, include ZenOS.hpp directly.
 *
 * Usage in .c files:
 *     #include "ZenOS_c.h"
 *     void SysTick_Handler(void) { os_tick(); }
 */

#ifdef __cplusplus
extern "C" {
#endif

void os_tick(void);
void os_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ZenOS_C_H */
