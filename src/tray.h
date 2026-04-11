#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

#include "gamepad.h"   /* GamepadInfo, XUSER_MAX_COUNT */

/*
 * StickPoint — system tray icon and main status window
 *
 * tray_init        Register the tray icon and create (hidden) the main window.
 *                  hwnd_msg is the message-only window that receives WM_TRAYICON
 *                  and WM_COMMAND menu events.
 *
 * tray_update_gamepads
 *                  Refresh the main window contents to reflect the current state
 *                  of all four XInput slots.  Call once per polling frame.
 *                  infos must point to an array of XUSER_MAX_COUNT entries.
 *
 * tray_get_infos   Return a pointer to the internal cached GamepadInfo array so
 *                  the options dialog can mutate slot_state and save config.
 *
 * tray_shutdown    Remove the tray icon.  Call before process exit.
 *
 * tray_handle_message
 *                  Route tray-related messages inside the message-only window's
 *                  WndProc.  Returns true and sets *out if the message was
 *                  consumed; otherwise returns false.
 */

bool          tray_init(HINSTANCE hInst, HWND hwnd_msg);
void          tray_update_gamepads(const GamepadInfo infos[XUSER_MAX_COUNT]);
GamepadInfo  *tray_get_infos(void);
void          tray_shutdown(void);
bool          tray_handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  LRESULT *out);
