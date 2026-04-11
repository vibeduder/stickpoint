#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdbool.h>

/*
 * StickPoint — gamepad input layer (XInput wrapper)
 *
 * The Guide/Home button is not exposed by the public XInput headers.
 * We access it via XInputGetStateEx (ordinal 100) from XInput1_4.dll,
 * which extends XINPUT_STATE with a Guide-button flag.
 */

/* Guide/Home button bit — not present in public <xinput.h>. */
#ifndef XINPUT_GAMEPAD_GUIDE
#define XINPUT_GAMEPAD_GUIDE    0x0400
#endif

/*
 * Extended state structure returned by XInputGetStateEx (ordinal 100).
 * Identical layout to XINPUT_STATE; the Guide bit appears in wButtons.
 */
typedef struct {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;         /* wButtons includes XINPUT_GAMEPAD_GUIDE */
} XINPUT_STATE_EX;

/* -------------------------------------------------------------------------
 * Application mode
 * ------------------------------------------------------------------------- */
typedef enum {
    MODE_NORMAL = 0,    /* Gamepad acts normally; no mouse injection        */
    MODE_MOUSE  = 1,    /* Left stick moves cursor; buttons trigger clicks  */
} AppMode;

/* -------------------------------------------------------------------------
 * Per-frame gamepad snapshot
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Current and previous button bitmasks for edge detection. */
    WORD  buttons;
    WORD  prev_buttons;

    /* Thumb-stick axes (-32768 … 32767). */
    SHORT lx, ly;
    SHORT rx, ry;

    /* Triggers (0 … 255). */
    BYTE  lt,      rt;
    BYTE  prev_lt, prev_rt;

    /* GetTickCount() value at the time this snapshot was taken. */
    DWORD timestamp_ms;

    bool  connected;
} GamepadState;

/* -------------------------------------------------------------------------
 * Convenience edge macros (operate on a GamepadState *s)
 * ------------------------------------------------------------------------- */
#define GP_JUST_PRESSED(s, btn)  ( ((s)->buttons & ~(s)->prev_buttons) & (btn) )
#define GP_JUST_RELEASED(s, btn) ( (~(s)->buttons & (s)->prev_buttons) & (btn) )
#define GP_HELD(s, btn)          ( ((s)->buttons  &  (s)->prev_buttons) & (btn) )

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/*
 * Load XInput at runtime (tries XInput1_4.dll first, then XInput1_3.dll).
 * Returns false if no XInput DLL is found.
 */
bool gamepad_init(void);

/*
 * Poll controller slot 0.
 * Fills *state and returns true if a controller is connected; otherwise
 * sets state->connected = false and returns false.
 */
bool gamepad_poll(GamepadState *state);

/* Release the XInput DLL handle. */
void gamepad_shutdown(void);

/* Returns the current AppMode. */
AppMode gamepad_get_mode(void);

/*
 * Examine the current snapshot and drive the mode state machine:
 *   - Guide + A within COMBO_TIMEOUT_MS  → toggle MODE_NORMAL ↔ MODE_MOUSE
 *   - Guide held alone ≥ GUIDE_HOLD_EXIT_MS → force MODE_NORMAL
 *
 * Returns true if the mode changed this frame (caller should skip
 * processing button events that frame to avoid spurious clicks).
 */
bool gamepad_update_mode(GamepadState *state);
