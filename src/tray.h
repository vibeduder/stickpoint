#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>

#include "gamepad.h"   /* GamepadInfo, XUSER_MAX_COUNT */

/*
 * StickPoint — system tray icon and main status window
 */

class TrayApplication {
public:
    TrayApplication();
    ~TrayApplication();

    TrayApplication(const TrayApplication&) = delete;
    TrayApplication& operator=(const TrayApplication&) = delete;
    TrayApplication(TrayApplication&&) = delete;
    TrayApplication& operator=(TrayApplication&&) = delete;

    bool init(HINSTANCE instance, HWND messageWindow);
        void updateGamepads(const GamepadInfos& infos);
        GamepadInfos& getInfos();
    void shutdown();
    bool handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
