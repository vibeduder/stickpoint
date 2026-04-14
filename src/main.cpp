/*
 * StickPoint — main entry point
 *
 * Runs as a background process (no console window, WinMain entry).
 * Creates a message-only window so it can receive WM_QUIT cleanly.
 * Polls all four XInput slots at POLL_INTERVAL_MS and, for each slot
 * in mouse mode, translates stick/button input into injected mouse events.
 *
 * TrayApplication::updateGamepads() is called unconditionally every frame (including
 * when controllers are disconnected) so the UI always reflects the true state.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbt.h>

#include <array>
#include <cmath>
#include <string>

#include "config.h"
#include "gamepad.h"
#include "mouse.h"
#include "tray.h"

using stickpoint::MouseButton;
using stickpoint::MouseInjector;

namespace {

constexpr DWORD kDetectRetryIntervalMs = 100;
constexpr DWORD kDetectRetryWindowMs = 1500;

struct SlotMouseState {
    DWORD a_release_time = 0;
    bool a_pending_dc = false;
    bool a_held = false;
    float scroll_acc = 0.0f;
    bool lt_held = false;
    bool rt_held = false;
};

class StickPointApp {
public:
    int run(HINSTANCE instance)
    {
        instance_ = instance;
        SetProcessDPIAware();

        if (!createMessageWindow()) {
            MessageBoxA(nullptr, "Failed to create message window.",
                        "StickPoint", MB_ICONERROR | MB_OK);
            return 1;
        }

        if (!gamepads_.init()) {
            MessageBoxA(nullptr,
                        "Could not load XInput.\n"
                        "Make sure XInput1_4.dll is present (Windows 8+).",
                        "StickPoint", MB_ICONERROR | MB_OK);
            return 1;
        }

        if (!tray_.init(instance_, messageWindow_)) {
            MessageBoxA(nullptr, "Failed to create tray icon.",
                        "StickPoint", MB_ICONERROR | MB_OK);
            gamepads_.shutdown();
            return 1;
        }

        gamepads_.registerRawInput(messageWindow_);
        initializeGamepadInfos();
        return runLoop();
    }

private:
    static constexpr const char* kWindowClassName = "StickPointWnd";

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) {
            const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lp);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        StickPointApp* self = reinterpret_cast<StickPointApp*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (self) {
            const LRESULT result = self->wndProc(hwnd, msg, wp, lp);
            if (msg == WM_NCDESTROY) {
                SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
            }
            return result;
        }

        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    bool createMessageWindow()
    {
        WNDCLASSEXA windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WndProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        if (!RegisterClassExA(&windowClass)) {
            return false;
        }

        messageWindow_ = CreateWindowExA(0, kWindowClassName, "StickPoint",
                                         0, 0, 0, 0, 0,
                                         HWND_MESSAGE, nullptr, instance_, this);
        return messageWindow_ != nullptr;
    }

    void initializeGamepadInfos()
    {
        GamepadInfos& infos = tray_.getInfos();
        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            infos[slot].slot_state.combo_hold_btn = XINPUT_GAMEPAD_GUIDE;
            infos[slot].slot_state.combo_press_btn = XINPUT_GAMEPAD_A;
            infos[slot].slot_state.combo_timeout_ms = COMBO_TIMEOUT_MS;
            infos[slot].slot_state.mode = MODE_NORMAL;
        }
        GamepadConfigStore::load(infos);

        gamepads_.pollAll(infos);
        gamepads_.detectControllers(infos);
        connectedMask_ = connectedMask(infos);
        scheduleControllerRedetectIfNeeded(GetTickCount(), infos);
    }

    int runLoop()
    {
        DWORD previousTick = GetTickCount();

        while (running_) {
            if (!pumpMessages()) {
                break;
            }

            const DWORD now = GetTickCount();
            const float dt = clampedDeltaSeconds(now, previousTick);
            previousTick = now;

            GamepadInfos& infos = tray_.getInfos();
            gamepads_.pollAll(infos);
            maybeRedetectControllers(now, infos);
            gamepads_.reconcileActiveSlot(infos);
            updateMouseMode(dt, infos);
            tray_.updateGamepads(infos);
            Sleep(POLL_INTERVAL_MS);
        }

        tray_.shutdown();
        gamepads_.shutdown();
        return 0;
    }

    bool pumpMessages()
    {
        MSG msg = {};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running_ = false;
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return running_;
    }

    void maybeRedetectControllers(DWORD now, GamepadInfos& infos)
    {
        const DWORD currentConnectedMask = connectedMask(infos);
        if (detectPending_ || currentConnectedMask != connectedMask_) {
            detectPending_ = false;
            gamepads_.detectControllers(infos);
            connectedMask_ = currentConnectedMask;
            scheduleControllerRedetectIfNeeded(now, infos);
            return;
        }

        if (detectRetryUntil_ != 0 && now >= detectRetryAt_) {
            gamepads_.detectControllers(infos);
            if (hasUnresolvedControllerInfo(infos) && now < detectRetryUntil_) {
                detectRetryAt_ = now + kDetectRetryIntervalMs;
            } else {
                detectRetryUntil_ = 0;
                detectRetryAt_ = 0;
            }
        }
    }

    void scheduleControllerRedetectIfNeeded(DWORD now, const GamepadInfos& infos)
    {
        if (connectedMask_ != 0 && hasUnresolvedControllerInfo(infos)) {
            detectRetryUntil_ = now + kDetectRetryWindowMs;
            detectRetryAt_ = now + kDetectRetryIntervalMs;
            return;
        }

        detectRetryUntil_ = 0;
        detectRetryAt_ = 0;
    }

    void updateMouseMode(float dt, GamepadInfos& infos)
    {
        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (!infos[slot].connected) {
                continue;
            }

            const AppMode modeBefore = infos[slot].slot_state.mode;
            const bool changed = infos[slot].slot_state.updateMode(infos[slot].state);
            if (!changed && modeBefore == MODE_MOUSE) {
                processMouseMode(infos[slot].state, dt, mouseState_[slot]);
            }
        }
    }

    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        LRESULT result = 0;
        if (tray_.handleMessage(hwnd, msg, wp, lp, &result)) {
            return result;
        }

        switch (msg) {
        case WM_DESTROY:
            running_ = false;
            PostQuitMessage(0);
            return 0;

        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                detectPending_ = true;
            }
            return 0;

        case WM_INPUT:
            gamepads_.handleRawInput(reinterpret_cast<HRAWINPUT>(lp));
            return DefWindowProcA(hwnd, msg, wp, lp);

        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
    }

    static float normalizeAxis(SHORT raw)
    {
        const float value = static_cast<float>(raw);
        const float sign = (value >= 0.0f) ? 1.0f : -1.0f;
        float magnitude = std::fabs(value);

        if (magnitude < static_cast<float>(STICK_DEADZONE)) {
            return 0.0f;
        }

        magnitude = (magnitude - static_cast<float>(STICK_DEADZONE)) /
                    (32767.0f - static_cast<float>(STICK_DEADZONE));
        if (magnitude > 1.0f) {
            magnitude = 1.0f;
        }
        magnitude = std::pow(magnitude, MOUSE_ACCEL_EXPONENT);
        return sign * magnitude;
    }

    void processMouseMode(const GamepadState& state, float dt, SlotMouseState& mouseState)
    {
        const float normalizedX = normalizeAxis(state.lx);
        const float normalizedY = -normalizeAxis(state.ly);

        if (normalizedX != 0.0f || normalizedY != 0.0f) {
            mouseInjector_.move(normalizedX * MOUSE_MAX_SPEED * dt,
                                normalizedY * MOUSE_MAX_SPEED * dt);
        }

        if (state.justPressed(XINPUT_GAMEPAD_A)) {
            if (mouseState.a_pending_dc) {
                mouseInjector_.doubleClick(MouseButton::Left);
                mouseState.a_pending_dc = false;
                mouseState.a_held = false;
            } else {
                mouseInjector_.buttonDown(MouseButton::Left);
                mouseState.a_held = true;
            }
        }
        if (state.justReleased(XINPUT_GAMEPAD_A) && mouseState.a_held) {
            mouseInjector_.buttonUp(MouseButton::Left);
            mouseState.a_held = false;
            mouseState.a_release_time = state.timestamp_ms;
            mouseState.a_pending_dc = true;
        }
        if (mouseState.a_pending_dc &&
            (state.timestamp_ms - mouseState.a_release_time) >= static_cast<DWORD>(DOUBLE_CLICK_MS)) {
            mouseState.a_pending_dc = false;
        }

        if (state.justPressed(XINPUT_GAMEPAD_B)) {
            mouseInjector_.buttonDown(MouseButton::Right);
        }
        if (state.justReleased(XINPUT_GAMEPAD_B)) {
            mouseInjector_.buttonUp(MouseButton::Right);
        }

        if (state.justPressed(XINPUT_GAMEPAD_Y)) {
            mouseInjector_.click(MouseButton::Middle);
        }

        const bool leftTriggerActive = state.lt > TRIGGER_THRESHOLD;
        if (leftTriggerActive && !mouseState.lt_held) {
            mouseInjector_.buttonDown(MouseButton::Left);
            mouseState.lt_held = true;
        } else if (!leftTriggerActive && mouseState.lt_held) {
            mouseInjector_.buttonUp(MouseButton::Left);
            mouseState.lt_held = false;
        }

        const bool rightTriggerActive = state.rt > TRIGGER_THRESHOLD;
        if (rightTriggerActive && !mouseState.rt_held) {
            mouseInjector_.buttonDown(MouseButton::Right);
            mouseState.rt_held = true;
        } else if (!rightTriggerActive && mouseState.rt_held) {
            mouseInjector_.buttonUp(MouseButton::Right);
            mouseState.rt_held = false;
        }

        if ((state.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0) {
            mouseState.scroll_acc += SCROLL_SPEED * dt;
        }
        if ((state.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0) {
            mouseState.scroll_acc -= SCROLL_SPEED * dt;
        }

        const float normalizedRY = normalizeAxis(state.ry);
        if (normalizedRY != 0.0f) {
            mouseState.scroll_acc += normalizedRY * SCROLL_SPEED * dt;
        }

        if (std::fabs(mouseState.scroll_acc) >= 1.0f) {
            const int ticks = static_cast<int>(mouseState.scroll_acc);
            mouseState.scroll_acc -= static_cast<float>(ticks);
            mouseInjector_.scroll(ticks);
        }
    }

    static DWORD connectedMask(const GamepadInfos& infos)
    {
        DWORD mask = 0;
        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (infos[slot].connected) {
                mask |= (1u << slot);
            }
        }
        return mask;
    }

    static bool hasUnresolvedControllerInfo(const GamepadInfos& infos)
    {
        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (!infos[slot].connected) {
                continue;
            }

            if (infos[slot].name == ("Controller " + std::to_string(slot + 1))) {
                return true;
            }
        }
        return false;
    }

    static float clampedDeltaSeconds(DWORD now, DWORD previousTick)
    {
        float dt = static_cast<float>(now - previousTick) / 1000.0f;
        if (dt > 0.05f) {
            dt = 0.05f;
        }
        return dt;
    }

    HINSTANCE instance_ = nullptr;
    HWND messageWindow_ = nullptr;
    bool running_ = true;
    DWORD connectedMask_ = 0;
    bool detectPending_ = false;
    DWORD detectRetryUntil_ = 0;
    DWORD detectRetryAt_ = 0;
    std::array<SlotMouseState, XUSER_MAX_COUNT> mouseState_ = {};
    MouseInjector mouseInjector_ = {};
    TrayApplication tray_ = {};
    GamepadManager gamepads_ = {};
};

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    StickPointApp app;
    return app.run(hInstance);
}