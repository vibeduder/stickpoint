#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

#include "gamepad.h"   /* AppMode */

/*
 * StickPoint — system tray icon and popup status window
 *
 * tray_init        Register the tray icon and create (hidden) the popup window.
 *                  hwnd_msg is the message-only window whose WndProc receives
 *                  the WM_TRAYICON callback and WM_COMMAND menu events.
 *
 * tray_update      Refresh the tray tooltip and popup labels to reflect the
 *                  current controller state.  Call once per polling frame.
 *
 * tray_shutdown    Remove the tray icon.  Call before process exit.
 *
 * tray_handle_message
 *                  Route tray-related messages inside the message-only window's
 *                  WndProc.  Returns true and sets *out if the message was
 *                  consumed; otherwise returns false.
 */

bool    tray_init(HINSTANCE hInst, HWND hwnd_msg);
void    tray_update(bool connected, AppMode mode);
void    tray_shutdown(void);
bool    tray_handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                            LRESULT *out);
