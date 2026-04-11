/*
 * StickPoint — gamepad input layer
 *
 * Supports all four XInput slots.  Each slot has an independent mode state
 * machine and a configurable mode-switch button combo.
 *
 * Controller name and image detection uses the Windows HID device enumeration
 * API (SetupDi + HidD_GetProductString). XInput-compatible devices always
 * have "IG_" in their device path, which we use to filter the HID device list.
 * Multiple HID interfaces may belong to the same physical controller, so we
 * dedupe by container/device identity first, then map the resulting list to
 * connected XInput slots in enumeration order.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "gamepad.h"
#include "config.h"

/* ---- XInput runtime binding -------------------------------------------- */

typedef DWORD (WINAPI *PFN_GetStateEx)(DWORD, XINPUT_STATE_EX *);
typedef DWORD (WINAPI *PFN_GetState)  (DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *PFN_GetCapabilitiesEx)(DWORD, DWORD, DWORD, void *);

typedef struct {
    XINPUT_CAPABILITIES capabilities;
    WORD                vendor_id;
    WORD                product_id;
    WORD                product_version;
    WORD                reserved;
    DWORD               unk2;
} XINPUT_CAPABILITIES_EX;

static HMODULE        s_dll        = NULL;
static PFN_GetStateEx s_GetStateEx = NULL;
static PFN_GetState   s_GetState   = NULL;
static PFN_GetCapabilitiesEx s_GetCapabilitiesEx = NULL;

#define DEVICE_INSTANCE_ID_CHARS 256
#define HID_PATH_CHARS           512

typedef struct {
    char path[HID_PATH_CHARS];
    char name[256];
    char instance_id[DEVICE_INSTANCE_ID_CHARS];
    GUID container_id;
    bool has_container_id;
    WORD vid;
    WORD pid;
} HidEntry;

typedef struct {
    bool  valid;
    DWORD timestamp_ms;
    char  path[HID_PATH_CHARS];
} RecentRawInput;

static HidEntry       s_hid_entries[XUSER_MAX_COUNT * 4];
static int            s_hid_entry_count = 0;
static int            s_slot_binding[XUSER_MAX_COUNT] = {-1, -1, -1, -1};
static RecentRawInput s_recent_raw = {0};

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
    {0x045E, 0x0B12, L"xbox-one.png"},
    /* Xbox Elite Series 1 */
    {0x045E, 0x02E3, L"xbox-elite-1.png"},
    {0x045E, 0x02FF, L"xbox-elite-1.png"},
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

static const wchar_t *lookup_controller_image(WORD vid, WORD pid)
{
    for (int i = 0; i < (int)(sizeof(s_known) / sizeof(s_known[0])); i++) {
        if (s_known[i].vid == vid && s_known[i].pid == pid)
            return s_known[i].image;
    }
    return L"xbox-360.png";
}

static int score_product_name(const char *name)
{
    size_t len;
    bool digits_only = true;

    if (!name || name[0] == '\0')
        return 0;

    if (strncmp(name, "Controller (VID ", 16) == 0)
        return 1;

    len = strlen(name);
    if (len <= 1)
        return 1;

    for (size_t i = 0; i < len; i++) {
        if (name[i] < '0' || name[i] > '9') {
            digits_only = false;
            break;
        }
    }

    if (digits_only)
        return 1;

    return 2;
}

static void sanitize_product_name(char *name)
{
    size_t start = 0;
    size_t end;

    if (!name || name[0] == '\0')
        return;

    end = strlen(name);
    while (start < end && (name[start] == ' ' || name[start] == '\t'))
        start++;
    while (end > start && (name[end - 1] == ' ' || name[end - 1] == '\t'))
        end--;

    if (end - start < 2 || name[start] != '(' || name[end - 1] != ')')
        return;

    start++;
    end--;
    while (start < end && (name[start] == ' ' || name[start] == '\t'))
        start++;
    while (end > start && (name[end - 1] == ' ' || name[end - 1] == '\t'))
        end--;

    if (start >= end)
        return;

    memmove(name, name + start, end - start);
    name[end - start] = '\0';
}

static void read_product_name(const char *path, WORD vid, WORD pid,
                              char *out, size_t out_size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf_s(out, out_size, _TRUNCATE,
                    "Controller (VID %04X PID %04X)", vid, pid);
        return;
    }

    wchar_t wname[256] = {0};
    if (HidD_GetProductString(h, wname, sizeof(wname)) && wname[0] != L'\0') {
        WideCharToMultiByte(CP_UTF8, 0, wname, -1,
                            out, (int)out_size, NULL, NULL);
        sanitize_product_name(out);
    } else {
        _snprintf_s(out, out_size, _TRUNCATE,
                    "Controller (VID %04X PID %04X)", vid, pid);
    }

    CloseHandle(h);
}

static bool get_container_id(HDEVINFO dev_info, SP_DEVINFO_DATA *dev_data,
                             GUID *out)
{
    DEVPROPTYPE prop_type = 0;
    GUID container_id = {0};

    if (!SetupDiGetDevicePropertyW(dev_info, dev_data,
                                   &DEVPKEY_Device_ContainerId,
                                   &prop_type,
                                   (PBYTE)&container_id,
                                   (DWORD)sizeof(container_id),
                                   NULL,
                                   0)) {
        return false;
    }

    if (prop_type != DEVPROP_TYPE_GUID)
        return false;

    *out = container_id;
    return true;
}

static void get_device_instance_id(HDEVINFO dev_info, SP_DEVINFO_DATA *dev_data,
                                   char *out, size_t out_size)
{
    if (!SetupDiGetDeviceInstanceIdA(dev_info, dev_data, out, (DWORD)out_size, NULL))
        out[0] = '\0';
}

static bool hid_entries_match(const HidEntry *a, const HidEntry *b)
{
    if (a->has_container_id && b->has_container_id &&
        IsEqualGUID(&a->container_id, &b->container_id)) {
        return true;
    }

    if (a->instance_id[0] != '\0' && b->instance_id[0] != '\0' &&
        strcmp(a->instance_id, b->instance_id) == 0) {
        return true;
    }

    if (a->path[0] != '\0' && b->path[0] != '\0' &&
        strcmp(a->path, b->path) == 0) {
        return true;
    }

    return false;
}

static int find_cached_entry_by_path(const char *path)
{
    for (int i = 0; i < s_hid_entry_count; i++) {
        if (strcmp(s_hid_entries[i].path, path) == 0)
            return i;
    }
    return -1;
}

static void reset_default_controller_info(GamepadInfo infos[XUSER_MAX_COUNT])
{
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        _snprintf_s(infos[i].name, sizeof(infos[i].name), _TRUNCATE,
                    "Controller %d", i + 1);
        build_image_path(L"xbox-360.png", infos[i].image, MAX_PATH);
    }
}

static bool get_slot_vid_pid(int slot, WORD *vid, WORD *pid)
{
    XINPUT_CAPABILITIES_EX caps = {0};

    if (!s_GetCapabilitiesEx)
        return false;

    if (s_GetCapabilitiesEx(1, (DWORD)slot, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS)
        return false;

    if (caps.vendor_id == 0 && caps.product_id == 0)
        return false;

    if (caps.product_id == 0 && (caps.capabilities.Flags & XINPUT_CAPS_WIRELESS)) {
        caps.vendor_id = 0x045E;
        caps.product_id = 0x028E;
    }

    *vid = caps.vendor_id;
    *pid = caps.product_id;
    return true;
}

static void apply_cached_bindings(GamepadInfo infos[XUSER_MAX_COUNT])
{
    bool used[sizeof(s_hid_entries) / sizeof(s_hid_entries[0])] = {0};

    reset_default_controller_info(infos);

    for (int slot = 0; slot < XUSER_MAX_COUNT; slot++) {
        if (!infos[slot].connected)
            continue;

        WORD slot_vid = 0;
        WORD slot_pid = 0;
        int idx = -1;

        if (get_slot_vid_pid(slot, &slot_vid, &slot_pid)) {
            for (int i = 0; i < s_hid_entry_count; i++) {
                if (used[i])
                    continue;
                if (s_hid_entries[i].vid == slot_vid && s_hid_entries[i].pid == slot_pid) {
                    idx = i;
                    break;
                }
            }
        }

        if (idx < 0) {
            int bound_idx = s_slot_binding[slot];
            if (bound_idx >= 0 && bound_idx < s_hid_entry_count && !used[bound_idx])
                idx = bound_idx;
        }

        if (idx < 0)
            continue;

        s_slot_binding[slot] = idx;

        strncpy_s(infos[slot].name, sizeof(infos[slot].name),
                  s_hid_entries[idx].name, _TRUNCATE);
        build_image_path(lookup_controller_image(s_hid_entries[idx].vid,
                                                 s_hid_entries[idx].pid),
                         infos[slot].image,
                         MAX_PATH);
        used[idx] = true;
    }

    int next_unbound = 0;
    for (int slot = 0; slot < XUSER_MAX_COUNT; slot++) {
        if (!infos[slot].connected)
            continue;

        int idx = s_slot_binding[slot];
        if (idx >= 0 && idx < s_hid_entry_count && used[idx])
            continue;

        while (next_unbound < s_hid_entry_count && used[next_unbound])
            next_unbound++;
        if (next_unbound >= s_hid_entry_count)
            break;

        s_slot_binding[slot] = next_unbound;
        strncpy_s(infos[slot].name, sizeof(infos[slot].name),
                  s_hid_entries[next_unbound].name, _TRUNCATE);
        build_image_path(lookup_controller_image(s_hid_entries[next_unbound].vid,
                                                 s_hid_entries[next_unbound].pid),
                         infos[slot].image,
                         MAX_PATH);
        used[next_unbound] = true;
    }
}

static void rebuild_cached_entries(const HidEntry *entries, int entry_count)
{
    int new_binding[XUSER_MAX_COUNT] = {-1, -1, -1, -1};

    for (int slot = 0; slot < XUSER_MAX_COUNT; slot++) {
        int old_idx = s_slot_binding[slot];
        if (old_idx < 0 || old_idx >= s_hid_entry_count)
            continue;

        for (int i = 0; i < entry_count; i++) {
            if (hid_entries_match(&s_hid_entries[old_idx], &entries[i])) {
                new_binding[slot] = i;
                break;
            }
        }
    }

    memset(s_hid_entries, 0, sizeof(s_hid_entries));
    if (entry_count > 0)
        memcpy(s_hid_entries, entries, (size_t)entry_count * sizeof(entries[0]));
    s_hid_entry_count = entry_count;
    memcpy(s_slot_binding, new_binding, sizeof(s_slot_binding));
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
    s_GetCapabilitiesEx = (PFN_GetCapabilitiesEx)GetProcAddress(s_dll, (LPCSTR)(ULONG_PTR)108);

    return (s_GetStateEx != NULL || s_GetState != NULL);
}

bool gamepad_register_raw_input(HWND hwnd)
{
    RAWINPUTDEVICE devices[2] = {
        {0x01, 0x04, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd},
        {0x01, 0x05, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd},
    };

    return RegisterRawInputDevices(devices,
                                   (UINT)(sizeof(devices) / sizeof(devices[0])),
                                   sizeof(devices[0])) == TRUE;
}

void gamepad_shutdown(void)
{
    if (s_dll) {
        FreeLibrary(s_dll);
        s_dll        = NULL;
        s_GetStateEx = NULL;
        s_GetState   = NULL;
        s_GetCapabilitiesEx = NULL;
    }
}

void gamepad_poll_all(GamepadInfo infos[XUSER_MAX_COUNT])
{
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        GamepadInfo   *info  = &infos[i];
        GamepadState  *state = &info->state;
        XINPUT_GAMEPAD gp    = {0};
        DWORD          packet_number = 0;
        DWORD          result;

        info->slot = (int)i;

        /* Carry forward previous button/trigger values for edge detection. */
        state->prev_buttons = state->buttons;
        state->prev_packet_number = state->packet_number;
        state->prev_lt      = state->lt;
        state->prev_rt      = state->rt;

        if (s_GetStateEx) {
            XINPUT_STATE_EX xs = {0};
            result = s_GetStateEx(i, &xs);
            if (result != ERROR_SUCCESS) goto disconnected;
            gp = xs.Gamepad;
            packet_number = xs.dwPacketNumber;
        } else if (s_GetState) {
            XINPUT_STATE xs = {0};
            result = s_GetState(i, &xs);
            if (result != ERROR_SUCCESS) goto disconnected;
            gp = xs.Gamepad;
            packet_number = xs.dwPacketNumber;
        } else {
            goto disconnected;
        }

        state->buttons      = gp.wButtons;
        state->packet_number = packet_number;
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
            state->packet_number = 0;
            state->prev_packet_number = 0;
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
    reset_default_controller_info(infos);

    /* Enumerate HID devices. */
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL,
                                              DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE)
        return;

    /* Collect XInput-compatible HID devices (those with "IG_" in path). */
    HidEntry entries[XUSER_MAX_COUNT * 4];
    int entry_count = 0;

    SP_DEVICE_INTERFACE_DATA iface = {0};
    iface.cbSize = sizeof(iface);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, idx, &iface);
         idx++)
    {
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

        SP_DEVINFO_DATA dev_data = {0};
        dev_data.cbSize = sizeof(dev_data);

        if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &iface, detail, needed, NULL, &dev_data)) {
            LocalFree(detail);
            continue;
        }

        /* XInput HID devices always contain "IG_" in the path. */
        if (!strstr(detail->DevicePath, "IG_") && !strstr(detail->DevicePath, "ig_")) {
            LocalFree(detail);
            continue;
        }

        /* Parse VID/PID from path. */
        WORD vid = 0;
        WORD pid = 0;
        if (!parse_vid_pid(detail->DevicePath, &vid, &pid)) {
            LocalFree(detail);
            continue;
        }

        GUID container_id = {0};
        bool has_container_id = get_container_id(dev_info, &dev_data, &container_id);

        char instance_id[DEVICE_INSTANCE_ID_CHARS] = {0};
        get_device_instance_id(dev_info, &dev_data, instance_id, sizeof(instance_id));

        char product_name[256] = {0};
        read_product_name(detail->DevicePath, vid, pid, product_name, sizeof(product_name));

        int existing = -1;
        for (int i = 0; i < entry_count; i++) {
            bool same_container = has_container_id && entries[i].has_container_id &&
                                  IsEqualGUID(&entries[i].container_id, &container_id);
            bool same_instance = instance_id[0] != '\0' && entries[i].instance_id[0] != '\0' &&
                                 strcmp(entries[i].instance_id, instance_id) == 0;
            if (same_container || same_instance) {
                existing = i;
                break;
            }
        }

        if (existing >= 0) {
            HidEntry *e = &entries[existing];
            if (score_product_name(product_name) > score_product_name(e->name))
                strncpy_s(e->name, sizeof(e->name), product_name, _TRUNCATE);
            if (!e->has_container_id && has_container_id) {
                e->container_id = container_id;
                e->has_container_id = true;
            }
            if (e->instance_id[0] == '\0' && instance_id[0] != '\0')
                strncpy_s(e->instance_id, sizeof(e->instance_id), instance_id, _TRUNCATE);
            LocalFree(detail);
            continue;
        }

        if (entry_count >= (int)(sizeof(entries) / sizeof(entries[0]))) {
            LocalFree(detail);
            break;
        }

        HidEntry *e = &entries[entry_count++];
        memset(e, 0, sizeof(*e));
        strncpy_s(e->path, sizeof(e->path), detail->DevicePath, _TRUNCATE);
        strncpy_s(e->name, sizeof(e->name), product_name, _TRUNCATE);
        strncpy_s(e->instance_id, sizeof(e->instance_id), instance_id, _TRUNCATE);
        e->has_container_id = has_container_id;
        if (has_container_id)
            e->container_id = container_id;
        e->vid = vid;
        e->pid = pid;
        LocalFree(detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);

    rebuild_cached_entries(entries, entry_count);
    apply_cached_bindings(infos);
}

void gamepad_handle_raw_input(HRAWINPUT raw_input)
{
    RAWINPUTHEADER header = {0};
    UINT size = sizeof(header);

    if (GetRawInputData(raw_input, RID_HEADER, &header, &size,
                        sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        return;
    }

    if (header.dwType != RIM_TYPEHID || header.hDevice == NULL)
        return;

    char path[HID_PATH_CHARS] = {0};
    size = (UINT)sizeof(path);
    if (GetRawInputDeviceInfoA(header.hDevice, RIDI_DEVICENAME, path, &size) == (UINT)-1)
        return;

    if (!strstr(path, "IG_") && !strstr(path, "ig_"))
        return;

    strncpy_s(s_recent_raw.path, sizeof(s_recent_raw.path), path, _TRUNCATE);
    s_recent_raw.timestamp_ms = GetTickCount();
    s_recent_raw.valid = true;
}

void gamepad_reconcile_active_slot(GamepadInfo infos[XUSER_MAX_COUNT])
{
    (void)infos;

    /* Slot-to-device mapping now comes from XInputGetCapabilitiesEx per slot. */
    s_recent_raw.valid = false;
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
