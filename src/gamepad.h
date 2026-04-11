#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdbool.h>

/*
 * StickPoint — gamepad input layer (XInput wrapper)
 *
 * Supports all four XInput slots simultaneously.
 * The Guide/Home button is accessed via XInputGetStateEx (ordinal 100).
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
    XINPUT_GAMEPAD Gamepad;
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
    WORD  buttons;
    WORD  prev_buttons;
    DWORD packet_number;
    DWORD prev_packet_number;

    SHORT lx, ly;
    SHORT rx, ry;

    BYTE  lt,      rt;
    BYTE  prev_lt, prev_rt;

    DWORD timestamp_ms;
    bool  connected;
} GamepadState;

/* -------------------------------------------------------------------------
 * Convenience edge macros
 * ------------------------------------------------------------------------- */
#define GP_JUST_PRESSED(s, btn)  ( ((s)->buttons & ~(s)->prev_buttons) & (btn) )
#define GP_JUST_RELEASED(s, btn) ( (~(s)->buttons & (s)->prev_buttons) & (btn) )
#define GP_HELD(s, btn)          ( ((s)->buttons  &  (s)->prev_buttons) & (btn) )

/* -------------------------------------------------------------------------
 * Per-slot mode state machine + configurable combo
 * ------------------------------------------------------------------------- */
typedef struct {
    AppMode mode;
    DWORD   guide_down_time;
    bool    guide_active;
    bool    guide_combo_armed;
    WORD    combo_hold_btn;    /* button to hold  (default: XINPUT_GAMEPAD_GUIDE) */
    WORD    combo_press_btn;   /* button to press (default: XINPUT_GAMEPAD_A)     */
    DWORD   combo_timeout_ms;  /* window in ms    (default: COMBO_TIMEOUT_MS)     */
} GamepadSlotState;

/* -------------------------------------------------------------------------
 * All information about one controller (used by the UI)
 * ------------------------------------------------------------------------- */
typedef struct {
    int              slot;           /* 0–3 XInput slot index                */
    bool             connected;
    GamepadState     state;          /* raw per-frame snapshot               */
    GamepadSlotState slot_state;     /* mode FSM + configurable combo        */
    char             name[256];      /* product string or "Controller N"     */
    wchar_t          image[MAX_PATH];/* absolute path to PNG                 */
} GamepadInfo;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Load XInput DLL (XInput1_4 → XInput1_3 fallback). Returns false on failure. */
bool gamepad_init(void);

/* Register Raw Input gamepad/joystick notifications on the given window. */
bool gamepad_register_raw_input(HWND hwnd);

/* Poll all four XInput slots. Fills infos[0..XUSER_MAX_COUNT-1].
   Only .connected and .state are modified; name/image are preserved. */
void gamepad_poll_all(GamepadInfo infos[XUSER_MAX_COUNT]);

/* Per-slot mode state machine.
   Returns true if the mode changed this frame (skip mouse events that frame). */
bool gamepad_update_mode_slot(GamepadState *state, GamepadSlotState *slot);

/* Enumerate HID devices to fill name + image path for each connected slot.
   Call at startup and on WM_DEVICECHANGE. */
void gamepad_detect_controllers(GamepadInfo infos[XUSER_MAX_COUNT]);

/* Track recent Raw Input activity so HID devices can be reconciled to XInput slots. */
void gamepad_handle_raw_input(HRAWINPUT raw_input);
void gamepad_reconcile_active_slot(GamepadInfo infos[XUSER_MAX_COUNT]);

/* Load/save per-slot combo config from/to StickPoint.ini next to the exe. */
void gamepad_load_config(GamepadInfo infos[XUSER_MAX_COUNT]);
void gamepad_save_config(const GamepadInfo infos[XUSER_MAX_COUNT]);

/* Release the XInput DLL. */
void gamepad_shutdown(void);
