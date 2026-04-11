/*
 * StickPoint — gamepad input layer
 *
 * Supports all four XInput slots.  Each slot has an independent mode state
 * machine and a configurable mode-switch button combo.
 *
 * Controller name and image detection uses the Windows HID device enumeration
 * API (SetupDi + HidD_GetProductString).  XInput-compatible devices always
 * have "IG_" in their device path, which we use to filter the HID device list.
 * Slots are mapped to HID devices by enumeration order (the same order XInput
 * assigns slot indices).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "gamepad.h"
#include "config.h"

/* ---- XInput runtime binding -------------------------------------------- */

typedef DWORD (WINAPI *PFN_GetStateEx)(DWORD, XINPUT_STATE_EX *);
typedef DWORD (WINAPI *PFN_GetState)  (DWORD, XINPUT_STATE *);

static HMODULE        s_dll        = NULL;
static PFN_GetStateEx s_GetStateEx = NULL;
static PFN_GetState   s_GetState   = NULL;

/* ---- Known VID/PID → image table --------------------------------------- */

static const struct {
    WORD          vid;
    WORD          pid;
    const wchar_t *image;
} s_known[] = {
    /* Xbox 360 */
    {0x045E, 0x028E, L"xbox-360.png"},
    {0x045E, 0x028F, L"xbox-360.png"},
    {0x045E, 0x0291, L"xbox-360.png"},
    /* Xbox One */
    {0x045E, 0x02D1, L"xbox-one.png"},
    {0x045E, 0x02DD, L"xbox-one.png"},
    {0x045E, 0x02EA, L"xbox-one.png"},
    {0x045E, 0x02FD, L"xbox-one.png"},
    {0x045E, 0x0719, L"xbox-one.png"},
    /* Xbox Elite Series 1 */
    {0x045E, 0x02E3, L"xbox-elite-1.png"},
    /* Xbox Elite Series 2 */
    {0x045E, 0x0B00, L"xbox-elite-2.png"},
    {0x045E, 0x0B05, L"xbox-elite-2.png"},
    {0x045E, 0x0B0A, L"xbox-elite-2.png"},
    /* 8BitDo Ultimate */
    {0x2DC8, 0x3012, L"8bitdo-ultimate.png"},
    {0x2DC8, 0x3016, L"8bitdo-ultimate.png"},
    /* Razer Wolverine */
    {0x1532, 0x0900, L"razer-volverine-v3.png"},
    {0x1532, 0x0A14, L"razer-volverine-v3.png"},
    {0x1532, 0x0A29, L"razer-volverine-v3.png"},
    /* GameSir */
    {0x3537, 0x1015, L"gamesir-t7.png"},
    /* Nacon */
    {0x146B, 0x0604, L"nacon.png"},
    {0x146B, 0x0605, L"nacon.png"},
    /* PowerA */
    {0x24C6, 0x543A, L"power-a.png"},
    {0x24C6, 0x5300, L"power-a.png"},
    {0x20D6, 0x2001, L"power-a.png"},
    {0x20D6, 0x2002, L"power-a.png"},
};

/* ---- Helpers ----------------------------------------------------------- */

/*
 * Build an absolute path for a controller image by locating the directory
 * of the running executable, then appending "assets\controllers\<name>".
 */
static void build_image_path(const wchar_t *filename, wchar_t *out, int out_count)
{
    GetModuleFileNameW(NULL, out, (DWORD)out_count);
    /* Strip the filename portion. */
    wchar_t *last_sep = wcsrchr(out, L'\\');
    if (!last_sep)
        last_sep = wcsrchr(out, L'/');
    if (last_sep)
        *(last_sep + 1) = L'\0';
    else
        out[0] = L'\0';

    /* Append the relative path. */
    wcsncat(out, L"assets\\controllers\\", out_count - (int)wcslen(out) - 1);
    wcsncat(out, filename, out_count - (int)wcslen(out) - 1);
}

/*
 * Parse "VID_xxxx&PID_xxxx" out of a HID device path string.
 * Returns true and fills *vid/*pid on success.
 */
static bool parse_vid_pid(const char *path, WORD *vid, WORD *pid)
{
    const char *p;
    unsigned v, pid_v;

    p = strstr(path, "VID_");
    if (!p) p = strstr(path, "vid_");
    if (!p) return false;
    if (sscanf(p + 4, "%4x", &v) != 1) return false;

    p = strstr(path, "PID_");
    if (!p) p = strstr(path, "pid_");
    if (!p) return false;
    if (sscanf(p + 4, "%4x", &pid_v) != 1) return false;

    *vid = (WORD)v;
    *pid = (WORD)pid_v;
    return true;
}

/* Get the INI file path next to the running exe (wide version). */
static void get_ini_path(wchar_t *out, int out_count)
{
    GetModuleFileNameW(NULL, out, (DWORD)out_count);
    wchar_t *sep = wcsrchr(out, L'\\');
    if (!sep) sep = wcsrchr(out, L'/');
    if (sep) *(sep + 1) = L'\0';
    wcsncat(out, L"StickPoint.ini", out_count - (int)wcslen(out) - 1);
}

/* ---- Public API -------------------------------------------------------- */

bool gamepad_init(void)
{
    s_dll = LoadLibraryA("XInput1_4.dll");
    if (!s_dll)
        s_dll = LoadLibraryA("XInput1_3.dll");
    if (!s_dll)
        return false;

    s_GetStateEx = (PFN_GetStateEx)GetProcAddress(s_dll, (LPCSTR)(ULONG_PTR)100);
    s_GetState   = (PFN_GetState)  GetProcAddress(s_dll, "XInputGetState");

    return (s_GetStateEx != NULL || s_GetState != NULL);
}

void gamepad_shutdown(void)
{
    if (s_dll) {
        FreeLibrary(s_dll);
        s_dll        = NULL;
        s_GetStateEx = NULL;
        s_GetState   = NULL;
    }
}

void gamepad_poll_all(GamepadInfo infos[XUSER_MAX_COUNT])
{
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        GamepadInfo   *info  = &infos[i];
        GamepadState  *state = &info->state;
        XINPUT_GAMEPAD gp    = {0};
        DWORD          result;

        info->slot = (int)i;

        /* Carry forward previous button/trigger values for edge detection. */
        state->prev_buttons = state->buttons;
        state->prev_lt      = state->lt;
        state->prev_rt      = state->rt;

        if (s_GetStateEx) {
            XINPUT_STATE_EX xs = {0};
            result = s_GetStateEx(i, &xs);
            if (result != ERROR_SUCCESS) goto disconnected;
            gp = xs.Gamepad;
        } else if (s_GetState) {
            XINPUT_STATE xs = {0};
            result = s_GetState(i, &xs);
            if (result != ERROR_SUCCESS) goto disconnected;
            gp = xs.Gamepad;
        } else {
            goto disconnected;
        }

        state->buttons      = gp.wButtons;
        state->lx           = gp.sThumbLX;
        state->ly           = gp.sThumbLY;
        state->rx           = gp.sThumbRX;
        state->ry           = gp.sThumbRY;
        state->lt           = gp.bLeftTrigger;
        state->rt           = gp.bRightTrigger;
        state->timestamp_ms = GetTickCount();
        state->connected    = true;
        info->connected     = true;
        continue;

    disconnected:
        if (info->connected) {
            /* Reset button state on disconnect so edge macros don't fire stale. */
            state->buttons      = 0;
            state->prev_buttons = 0;
            state->lt = state->rt = state->prev_lt = state->prev_rt = 0;
        }
        state->connected = false;
        info->connected  = false;
    }
}

bool gamepad_update_mode_slot(GamepadState *state, GamepadSlotState *slot)
{
    AppMode prev_mode = slot->mode;

    bool hold_now  = (state->buttons      & slot->combo_hold_btn) != 0;
    bool hold_prev = (state->prev_buttons & slot->combo_hold_btn) != 0;
    bool press_just = ((state->buttons & ~state->prev_buttons) & slot->combo_press_btn) != 0;

    /* Leading edge of hold button. */
    if (hold_now && !hold_prev) {
        slot->guide_down_time   = state->timestamp_ms;
        slot->guide_active      = true;
        slot->guide_combo_armed = true;
    }

    /* Release of hold button. */
    if (!hold_now && hold_prev) {
        slot->guide_active      = false;
        slot->guide_combo_armed = false;
    }

    /* Combo: hold + press within timeout → toggle mode. */
    if (slot->guide_combo_armed && hold_now && press_just) {
        DWORD held = state->timestamp_ms - slot->guide_down_time;
        if (held < slot->combo_timeout_ms) {
            slot->mode = (slot->mode == MODE_NORMAL) ? MODE_MOUSE : MODE_NORMAL;
            slot->guide_combo_armed = false;
            /* Suppress the press button so it doesn't register as a click. */
            state->prev_buttons |= slot->combo_press_btn;
        }
    }

    /* Hold button alone for GUIDE_HOLD_EXIT_MS → force Normal mode. */
    if (slot->mode == MODE_MOUSE && slot->guide_active && slot->guide_combo_armed) {
        DWORD held = state->timestamp_ms - slot->guide_down_time;
        if (held >= (DWORD)GUIDE_HOLD_EXIT_MS) {
            slot->mode              = MODE_NORMAL;
            slot->guide_combo_armed = false;
        }
    }

    return (slot->mode != prev_mode);
}

void gamepad_detect_controllers(GamepadInfo infos[XUSER_MAX_COUNT])
{
    /* Default name/image for all slots. */
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        _snprintf_s(infos[i].name, sizeof(infos[i].name), _TRUNCATE,
                    "Controller %d", i + 1);
        build_image_path(L"xbox-360.png", infos[i].image, MAX_PATH);
    }

    /* Enumerate HID devices. */
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL,
                                              DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE)
        return;

    /* Collect XInput-compatible HID devices (those with "IG_" in path). */
    typedef struct {
        char path[512];
        char name[256];
        WORD vid;
        WORD pid;
    } HidEntry;

    HidEntry entries[XUSER_MAX_COUNT * 2];
    int entry_count = 0;

    SP_DEVICE_INTERFACE_DATA iface = {0};
    iface.cbSize = sizeof(iface);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, idx, &iface);
         idx++)
    {
        if (entry_count >= (int)(sizeof(entries) / sizeof(entries[0])))
            break;

        /* Get device path length. */
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &iface, NULL, 0, &needed, NULL);
        if (needed == 0 || needed > 512 + offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath))
            continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)LocalAlloc(LMEM_ZEROINIT, needed);
        if (!detail)
            continue;
        detail->cbSize = sizeof(*detail);

        if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &iface, detail, needed, NULL, NULL)) {
            LocalFree(detail);
            continue;
        }

        /* XInput HID devices always contain "IG_" in the path. */
        if (!strstr(detail->DevicePath, "IG_") && !strstr(detail->DevicePath, "ig_")) {
            LocalFree(detail);
            continue;
        }

        HidEntry *e = &entries[entry_count];
        strncpy_s(e->path, sizeof(e->path), detail->DevicePath, _TRUNCATE);
        LocalFree(detail);

        /* Parse VID/PID from path. */
        if (!parse_vid_pid(e->path, &e->vid, &e->pid))
            continue;

        /* Get product name from the device. */
        HANDLE h = CreateFileA(e->path, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            /* Product name unavailable — use a placeholder. */
            _snprintf_s(e->name, sizeof(e->name), _TRUNCATE,
                        "Controller (VID %04X PID %04X)", e->vid, e->pid);
        } else {
            wchar_t wname[256] = {0};
            if (HidD_GetProductString(h, wname, sizeof(wname))) {
                WideCharToMultiByte(CP_UTF8, 0, wname, -1,
                                    e->name, sizeof(e->name), NULL, NULL);
            } else {
                _snprintf_s(e->name, sizeof(e->name), _TRUNCATE,
                            "Controller (VID %04X PID %04X)", e->vid, e->pid);
            }
            CloseHandle(h);
        }

        entry_count++;
    }

    SetupDiDestroyDeviceInfoList(dev_info);

    /* Map entries to XInput slots in enumeration order. */
    int mapped = 0;
    for (int s = 0; s < XUSER_MAX_COUNT && mapped < entry_count; s++) {
        if (!infos[s].connected)
            continue;

        HidEntry *e = &entries[mapped++];

        /* Copy product name. */
        strncpy_s(infos[s].name, sizeof(infos[s].name), e->name, _TRUNCATE);

        /* Look up image by VID/PID. */
        const wchar_t *img = L"xbox-360.png";
        for (int k = 0; k < (int)(sizeof(s_known) / sizeof(s_known[0])); k++) {
            if (s_known[k].vid == e->vid && s_known[k].pid == e->pid) {
                img = s_known[k].image;
                break;
            }
        }
        build_image_path(img, infos[s].image, MAX_PATH);
    }
}

void gamepad_load_config(GamepadInfo infos[XUSER_MAX_COUNT])
{
    wchar_t ini[MAX_PATH];
    get_ini_path(ini, MAX_PATH);

    /* Convert to ANSI for GetPrivateProfileInt (simpler than wide version). */
    char ini_a[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, ini, -1, ini_a, sizeof(ini_a), NULL, NULL);

    char section[16];
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        GamepadSlotState *s = &infos[i].slot_state;
        _snprintf_s(section, sizeof(section), _TRUNCATE, "Slot%d", i);

        UINT hold = (UINT)GetPrivateProfileIntA(section, "ComboHoldBtn",
                                                 (int)s->combo_hold_btn, ini_a);
        UINT press = (UINT)GetPrivateProfileIntA(section, "ComboPressBtn",
                                                  (int)s->combo_press_btn, ini_a);
        UINT timeout = (UINT)GetPrivateProfileIntA(section, "ComboTimeoutMs",
                                                    (int)s->combo_timeout_ms, ini_a);
        s->combo_hold_btn    = (WORD)hold;
        s->combo_press_btn   = (WORD)press;
        s->combo_timeout_ms  = timeout;
    }
}

void gamepad_save_config(const GamepadInfo infos[XUSER_MAX_COUNT])
{
    wchar_t ini[MAX_PATH];
    get_ini_path(ini, MAX_PATH);

    char ini_a[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, ini, -1, ini_a, sizeof(ini_a), NULL, NULL);

    char section[16];
    char val[16];
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        const GamepadSlotState *s = &infos[i].slot_state;
        _snprintf_s(section, sizeof(section), _TRUNCATE, "Slot%d", i);

        _snprintf_s(val, sizeof(val), _TRUNCATE, "%u", (unsigned)s->combo_hold_btn);
        WritePrivateProfileStringA(section, "ComboHoldBtn", val, ini_a);

        _snprintf_s(val, sizeof(val), _TRUNCATE, "%u", (unsigned)s->combo_press_btn);
        WritePrivateProfileStringA(section, "ComboPressBtn", val, ini_a);

        _snprintf_s(val, sizeof(val), _TRUNCATE, "%u", (unsigned)s->combo_timeout_ms);
        WritePrivateProfileStringA(section, "ComboTimeoutMs", val, ini_a);
    }
}
