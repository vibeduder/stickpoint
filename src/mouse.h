#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * StickPoint — mouse simulation layer
 *
 * All functions inject synthetic mouse events via Win32 SendInput().
 * Coordinates are relative (MOUSEEVENTF_MOVE without MOUSEEVENTF_ABSOLUTE).
 */

typedef enum {
    MOUSE_BTN_LEFT   = 0,
    MOUSE_BTN_RIGHT  = 1,
    MOUSE_BTN_MIDDLE = 2,
} MouseButton;

/*
 * Accumulate fractional pixel movement and flush whole-pixel deltas.
 * Call every frame even when the stick is not moving (dx=0, dy=0).
 */
void mouse_move(float dx, float dy);

/* Press a mouse button down (use for held drags). */
void mouse_button_down(MouseButton btn);

/* Release a previously pressed mouse button. */
void mouse_button_up(MouseButton btn);

/* Atomic press+release (single click). */
void mouse_click(MouseButton btn);

/*
 * Atomic double-click: sends down/up/down/up in rapid succession.
 * Most applications respond to this as a double-click even without
 * relying on the system double-click timer.
 */
void mouse_double_click(MouseButton btn);

/*
 * Scroll the mouse wheel.
 *   delta > 0 → scroll up
 *   delta < 0 → scroll down
 * Each unit equals one WHEEL_DELTA tick (typically 120).
 */
void mouse_scroll(int delta);
