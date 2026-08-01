/*
 * Windows notification-area item for UxPlay. See windows_tray.h.
 *
 * Structure follows renderers/macos_statusbar.m: the same state is kept, the
 * same rows are shown, and the same handlers are called back. Two things are
 * done differently because the platform leaves no choice.
 *
 * The icon and its window live on a thread of their own. On macOS the status
 * item can be driven from any thread by hopping to the AppKit main thread,
 * which is already running a run loop. UxPlay on Windows has no such thread:
 * main() runs the GLib loop, and that loop is torn down and rebuilt between
 * sessions, so a tray icon hosted in it would disappear whenever a client left.
 *
 * The menu is built at the moment it is opened rather than kept alive and
 * mutated. AppKit menu items can be retained and their titles changed; a Win32
 * popup menu is cheap to build and awkward to keep in step, and building it on
 * demand also picks up monitors that were attached since UxPlay started, which
 * the macOS side arranges explicitly through menuWillOpen:.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "windows_tray.h"

#define WM_TRAY_ICON    (WM_APP + 1)
#define WM_TRAY_REFRESH (WM_APP + 2)

#define TRAY_ICON_ID 1

/* Command ids. The ranged ones carry their index in the low bits. */
#define IDM_DISCONNECT     100
#define IDM_QUIT           101
#define IDM_CONSOLE        102
#define IDM_FULLSCREEN     103
#define IDM_VOLUME_BASE    200   /* + 0..10, tenths */
#define IDM_SEEK_ABS_BASE  300   /* + 0..9, tenths of the duration */
#define IDM_SEEK_REL_BASE  320   /* + 0..3, see seek_offsets[] */
#define IDM_DISPLAY_BASE   400   /* + monitor index */

#define UXPLAY_MAX_MONITORS 16

/* Relative jumps offered under "Seek", in seconds. */
static const double seek_offsets[] = { -60.0, -10.0, 10.0, 60.0 };
static const wchar_t *seek_offset_labels[] = {
    L"Back 1:00", L"Back 0:10", L"Forward 0:10", L"Forward 1:00"
};

static CRITICAL_SECTION lock;
static BOOL lock_ready = FALSE;

/* Guarded by lock. */
static statusbar_state_t current_state = STATUSBAR_IDLE;
static wchar_t client_name[128];
static wchar_t client_model[128];
static wchar_t track_text[128];
static double volume_fraction = 1.0;
static double progress_position = 0.0;
static double progress_duration = 0.0;
static int selected_display = -1;

static void (*volume_handler)(double) = NULL;
static void (*disconnect_handler)(void) = NULL;
static void (*seek_handler)(double) = NULL;
static void (*display_handler)(int index) = NULL;
static void (*quit_handler)(void) = NULL;
static bool (*fullscreen_on_connect_get)(void) = NULL;
static void (*fullscreen_on_connect_set)(bool enable) = NULL;

static HWND tray_window = NULL;
static HANDLE tray_thread = NULL;
static HICON icon_idle = NULL;
static HICON icon_active = NULL;
static UINT taskbar_created_message = 0;

/* ------------------------------------------------------------------ */

static void to_wide(const char *utf8, wchar_t *out, int out_chars) {
    out[0] = L'\0';
    if (utf8 && *utf8) {
        if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_chars) == 0) {
            out[0] = L'\0';
        }
    }
    out[out_chars - 1] = L'\0';
}

/* m:ss, or h:mm:ss once past an hour. */
static void clock_string(double seconds, wchar_t *out, int out_chars) {
    long total = (long) (seconds < 0.0 ? 0.0 : seconds);

    if (total >= 3600) {
        _snwprintf(out, out_chars, L"%ld:%02ld:%02ld",
                   total / 3600, (total % 3600) / 60, total % 60);
    } else {
        _snwprintf(out, out_chars, L"%ld:%02ld", total / 60, total % 60);
    }
    out[out_chars - 1] = L'\0';
}

/* Menu items get unreadable long before they get wide enough to hold a full
   track name, so clip with an ellipsis. */
static void elide(wchar_t *text, int limit) {
    if ((int) wcslen(text) > limit) {
        text[limit] = L'\x2026';
        text[limit + 1] = L'\0';
    }
}

static const wchar_t *state_text(statusbar_state_t state) {
    switch (state) {
    case STATUSBAR_MIRROR:
        return L"Mirroring";
    case STATUSBAR_AUDIO:
        return L"Streaming audio";
    case STATUSBAR_VIDEO:
        return L"Playing video";
    case STATUSBAR_IDLE:
    default:
        return L"Waiting for a client";
    }
}

/* ------------------------------------------------------------------ */

/* The AirPlay mark: a screen with a triangle beneath it, drawn rather than
   loaded, so the build carries no resource file. GDI does not antialias these
   primitives, which at tray size is no loss and keeps the one-pixel strokes
   crisp.

   Alpha is filled in afterwards: GDI writes nothing to the alpha byte of a
   32-bit DIB, so the bitmap is cleared to zero first and every pixel the
   drawing touched is then made opaque. Neither colour used here is black, so
   "some colour component is set" is a sound test for "was drawn". */
static HICON create_icon(COLORREF colour, int size) {
    BITMAPINFO bi;
    HDC screen_dc, dc;
    HBITMAP colour_bitmap, mask_bitmap, old_bitmap;
    HPEN pen, old_pen;
    HBRUSH brush, old_brush;
    unsigned char *bits = NULL;
    ICONINFO info;
    HICON icon;
    POINT triangle[3];
    int stroke = size / 16;
    int i;

    if (stroke < 1) {
        stroke = 1;
    }

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = size;      /* bottom-up; the shape is symmetric
                                          horizontally but not vertically, so
                                          the triangle is drawn at the top in
                                          DIB order and lands at the bottom. */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen_dc = GetDC(NULL);
    if (!screen_dc) {
        return NULL;
    }
    colour_bitmap = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS,
                                     (void **) &bits, NULL, 0);
    dc = CreateCompatibleDC(screen_dc);
    ReleaseDC(NULL, screen_dc);
    if (!colour_bitmap || !dc || !bits) {
        if (colour_bitmap) {
            DeleteObject(colour_bitmap);
        }
        if (dc) {
            DeleteDC(dc);
        }
        return NULL;
    }
    ZeroMemory(bits, (size_t) size * size * 4);

    old_bitmap = (HBITMAP) SelectObject(dc, colour_bitmap);
    pen = CreatePen(PS_SOLID, stroke, colour);
    old_pen = (HPEN) SelectObject(dc, pen);
    brush = CreateSolidBrush(colour);
    old_brush = (HBRUSH) SelectObject(dc, GetStockObject(NULL_BRUSH));

    /* The screen: an outline, so the mark reads as a rectangle rather than a
       blob at 16 pixels. */
    RoundRect(dc,
              stroke, stroke,
              size - stroke, size - (size * 6) / 16,
              size / 4, size / 4);

    /* The triangle below it, filled. */
    SelectObject(dc, brush);
    triangle[0].x = (size * 3) / 16;
    triangle[0].y = size - stroke;
    triangle[1].x = size - (size * 3) / 16;
    triangle[1].y = size - stroke;
    triangle[2].x = size / 2;
    triangle[2].y = size - (size * 7) / 16;
    Polygon(dc, triangle, 3);

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    DeleteObject(brush);
    SelectObject(dc, old_bitmap);
    GdiFlush();

    for (i = 0; i < size * size; i++) {
        unsigned char *pixel = bits + i * 4;

        if (pixel[0] || pixel[1] || pixel[2]) {
            pixel[3] = 0xff;
        }
    }

    /* A 32-bit colour bitmap carries its own transparency, but CreateIconIndirect
       still wants a mask; an all-zero one leaves the alpha channel in charge. */
    mask_bitmap = CreateBitmap(size, size, 1, 1, NULL);
    if (mask_bitmap) {
        HDC mask_dc = CreateCompatibleDC(NULL);
        HBITMAP old_mask = (HBITMAP) SelectObject(mask_dc, mask_bitmap);
        RECT all;

        all.left = 0;
        all.top = 0;
        all.right = size;
        all.bottom = size;
        FillRect(mask_dc, &all, (HBRUSH) GetStockObject(BLACK_BRUSH));
        SelectObject(mask_dc, old_mask);
        DeleteDC(mask_dc);
    }

    ZeroMemory(&info, sizeof(info));
    info.fIcon = TRUE;
    info.hbmMask = mask_bitmap;
    info.hbmColor = colour_bitmap;
    icon = CreateIconIndirect(&info);

    DeleteObject(colour_bitmap);
    if (mask_bitmap) {
        DeleteObject(mask_bitmap);
    }
    DeleteDC(dc);
    return icon;
}

static void create_icons(void) {
    int size = GetSystemMetrics(SM_CXSMICON);

    if (size <= 0) {
        size = 16;
    }
    /* Blue while a client is connected, matching the macOS item; a mid grey
       when idle, which is the one neutral that stays visible on both a light
       and a dark taskbar. */
    if (!icon_active) {
        icon_active = create_icon(RGB(0, 122, 255), size);
    }
    if (!icon_idle) {
        icon_idle = create_icon(RGB(128, 128, 128), size);
    }
}

/* ------------------------------------------------------------------ */

static void fill_notify_data(NOTIFYICONDATAW *nid) {
    wchar_t who[128];
    statusbar_state_t state;

    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize = sizeof(NOTIFYICONDATAW);
    nid->hWnd = tray_window;
    nid->uID = TRAY_ICON_ID;
    nid->uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid->uCallbackMessage = WM_TRAY_ICON;

    EnterCriticalSection(&lock);
    state = current_state;
    wcscpy(who, client_name[0] ? client_name : L"UxPlay");
    LeaveCriticalSection(&lock);

    nid->hIcon = (state == STATUSBAR_IDLE) ? icon_idle : icon_active;
    _snwprintf(nid->szTip, ARRAYSIZE(nid->szTip) - 1, L"%ls \x2014 %ls",
               who, state_text(state));
    nid->szTip[ARRAYSIZE(nid->szTip) - 1] = L'\0';
}

static void refresh_icon(void) {
    NOTIFYICONDATAW nid;

    if (!tray_window) {
        return;
    }
    fill_notify_data(&nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ------------------------------------------------------------------ */

typedef struct {
    RECT rect[UXPLAY_MAX_MONITORS];
    BOOL primary[UXPLAY_MAX_MONITORS];
    int count;
} monitor_list_t;

static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC dc, LPRECT rect,
                                     LPARAM data) {
    monitor_list_t *list = (monitor_list_t *) data;
    MONITORINFO info;

    (void) dc;
    (void) rect;
    if (list->count >= UXPLAY_MAX_MONITORS) {
        return FALSE;
    }
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }
    list->rect[list->count] = info.rcMonitor;
    list->primary[list->count] = (info.dwFlags & MONITORINFOF_PRIMARY) ? TRUE : FALSE;
    list->count++;
    return TRUE;
}

static void list_monitors(monitor_list_t *list) {
    list->count = 0;
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM) list);
}

/* ------------------------------------------------------------------ */

/* uxplay is a console program, so a console window comes with it. Run from the
   tray that window is just clutter, but it is also where -d puts its output, so
   it is offered as something to switch off rather than taken away.
 *
 * Only when this process is the console's sole owner. Started from a terminal,
 * GetConsoleWindow() returns the terminal the user is sitting in, and hiding it
 * would take their shell with it. */
static BOOL own_the_console(void) {
    DWORD pids[4];

    if (!GetConsoleWindow()) {
        return FALSE;
    }
    return GetConsoleProcessList(pids, ARRAYSIZE(pids)) == 1;
}

static void toggle_console(void) {
    HWND console = GetConsoleWindow();

    if (!console || !own_the_console()) {
        return;
    }
    ShowWindow(console, IsWindowVisible(console) ? SW_HIDE : SW_SHOW);
}

void statusbar_setup_console(bool debug_log) {
    HWND console = GetConsoleWindow();

    if (debug_log || !console || !own_the_console()) {
        return;
    }
    ShowWindow(console, SW_HIDE);
}

static HMENU build_volume_menu(double fraction) {
    HMENU menu = CreatePopupMenu();
    int nearest = (int) floor(fraction * 10.0 + 0.5);
    int i;

    if (nearest < 0) {
        nearest = 0;
    }
    if (nearest > 10) {
        nearest = 10;
    }
    for (i = 10; i >= 0; i--) {
        wchar_t label[32];

        _snwprintf(label, ARRAYSIZE(label), L"%d%%", i * 10);
        label[ARRAYSIZE(label) - 1] = L'\0';
        AppendMenuW(menu, MF_STRING, IDM_VOLUME_BASE + i, label);
    }
    /* The list runs loudest-first, so the item for a given tenth is at
       10 - tenth. */
    CheckMenuRadioItem(menu, IDM_VOLUME_BASE, IDM_VOLUME_BASE + 10,
                       IDM_VOLUME_BASE + nearest, MF_BYCOMMAND);
    return menu;
}

static HMENU build_seek_menu(double duration) {
    HMENU menu = CreatePopupMenu();
    int i;

    AppendMenuW(menu, MF_STRING, IDM_SEEK_ABS_BASE, L"Start");
    for (i = 1; i <= 9; i++) {
        wchar_t label[64];
        wchar_t at[32];

        clock_string(duration * i / 10.0, at, ARRAYSIZE(at));
        _snwprintf(label, ARRAYSIZE(label), L"%d%%\t%ls", i * 10, at);
        label[ARRAYSIZE(label) - 1] = L'\0';
        AppendMenuW(menu, MF_STRING, IDM_SEEK_ABS_BASE + i, label);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < (int) ARRAYSIZE(seek_offsets); i++) {
        AppendMenuW(menu, MF_STRING, IDM_SEEK_REL_BASE + i, seek_offset_labels[i]);
    }
    return menu;
}

static HMENU build_display_menu(const monitor_list_t *list, int selected) {
    HMENU menu = CreatePopupMenu();
    int i;

    for (i = 0; i < list->count; i++) {
        wchar_t label[64];

        _snwprintf(label, ARRAYSIZE(label), L"%d. %ld x %ld%ls",
                   i + 1,
                   (long) (list->rect[i].right - list->rect[i].left),
                   (long) (list->rect[i].bottom - list->rect[i].top),
                   list->primary[i] ? L"  (primary)" : L"");
        label[ARRAYSIZE(label) - 1] = L'\0';
        AppendMenuW(menu, MF_STRING, IDM_DISPLAY_BASE + i, label);
    }
    if (selected >= 0 && selected < list->count) {
        CheckMenuRadioItem(menu, IDM_DISPLAY_BASE,
                           IDM_DISPLAY_BASE + list->count - 1,
                           IDM_DISPLAY_BASE + selected, MF_BYCOMMAND);
    }
    return menu;
}

static void show_menu(void) {
    HMENU menu;
    HMENU volume_menu;
    HMENU seek_menu = NULL;
    HMENU display_menu = NULL;
    monitor_list_t monitors;
    POINT cursor;
    int chosen;
    statusbar_state_t state;
    wchar_t name[128], track[128];
    double volume, position, duration;
    int display;
    BOOL connected;

    EnterCriticalSection(&lock);
    state = current_state;
    wcscpy(name, client_name);
    wcscpy(track, track_text);
    volume = volume_fraction;
    position = progress_position;
    duration = progress_duration;
    display = selected_display;
    LeaveCriticalSection(&lock);

    connected = (state != STATUSBAR_IDLE);
    list_monitors(&monitors);
    if (display >= monitors.count) {
        display = -1;
    }

    menu = CreatePopupMenu();
    if (connected && name[0]) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, name);
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, state_text(state));
    if (connected && track[0]) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, track);
    }

    /* Only HLS reports a duration; mirroring has no timeline and the rows stay
       away, exactly as the macOS progress row hides itself. */
    if (duration > 0.0) {
        wchar_t at[32], total[32], label[80];

        clock_string(position, at, ARRAYSIZE(at));
        clock_string(duration, total, ARRAYSIZE(total));
        _snwprintf(label, ARRAYSIZE(label), L"%ls / %ls", at, total);
        label[ARRAYSIZE(label) - 1] = L'\0';
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, label);
        seek_menu = build_seek_menu(duration);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR) seek_menu, L"Seek");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    volume_menu = build_volume_menu(volume);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR) volume_menu, L"Volume");

    /* One display is no choice at all. */
    if (monitors.count > 1) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        display_menu = build_display_menu(&monitors, display);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR) display_menu, L"Display");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (fullscreen_on_connect_get) {
        AppendMenuW(menu, MF_STRING |
                    (fullscreen_on_connect_get() ? MF_CHECKED : MF_UNCHECKED),
                    IDM_FULLSCREEN, L"Fullscreen on connect");
    }
    if (own_the_console()) {
        HWND console = GetConsoleWindow();

        AppendMenuW(menu, MF_STRING |
                    (IsWindowVisible(console) ? MF_CHECKED : MF_UNCHECKED),
                    IDM_CONSOLE, L"Console window");
    }
    AppendMenuW(menu, MF_STRING | (connected ? MF_ENABLED : MF_GRAYED),
                IDM_DISCONNECT, L"Disconnect");
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit UxPlay");

    GetCursorPos(&cursor);
    /* Without the foreground window the menu never receives the click that
       dismisses it and stays on screen; the trailing message is the documented
       companion to that. */
    SetForegroundWindow(tray_window);
    chosen = (int) TrackPopupMenu(menu,
                                  TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                  cursor.x, cursor.y, 0, tray_window, NULL);
    PostMessageW(tray_window, WM_NULL, 0, 0);

    DestroyMenu(menu);

    if (chosen == IDM_QUIT) {
        if (quit_handler) {
            quit_handler();
        }
    } else if (chosen == IDM_DISCONNECT) {
        if (disconnect_handler) {
            disconnect_handler();
        }
    } else if (chosen == IDM_CONSOLE) {
        toggle_console();
    } else if (chosen == IDM_FULLSCREEN) {
        if (fullscreen_on_connect_get && fullscreen_on_connect_set) {
            fullscreen_on_connect_set(!fullscreen_on_connect_get());
        }
    } else if (chosen >= IDM_VOLUME_BASE && chosen <= IDM_VOLUME_BASE + 10) {
        double fraction = (chosen - IDM_VOLUME_BASE) / 10.0;

        EnterCriticalSection(&lock);
        volume_fraction = fraction;
        LeaveCriticalSection(&lock);
        if (volume_handler) {
            volume_handler(fraction);
        }
    } else if (chosen >= IDM_SEEK_ABS_BASE && chosen <= IDM_SEEK_ABS_BASE + 9) {
        if (seek_handler) {
            seek_handler(duration * (chosen - IDM_SEEK_ABS_BASE) / 10.0);
        }
    } else if (chosen >= IDM_SEEK_REL_BASE &&
               chosen < IDM_SEEK_REL_BASE + (int) ARRAYSIZE(seek_offsets)) {
        double target = position + seek_offsets[chosen - IDM_SEEK_REL_BASE];

        if (target < 0.0) {
            target = 0.0;
        }
        if (target > duration) {
            target = duration;
        }
        if (seek_handler) {
            seek_handler(target);
        }
    } else if (chosen >= IDM_DISPLAY_BASE &&
               chosen < IDM_DISPLAY_BASE + monitors.count) {
        int index = chosen - IDM_DISPLAY_BASE;

        EnterCriticalSection(&lock);
        selected_display = index;
        LeaveCriticalSection(&lock);
        if (display_handler) {
            display_handler(index);
        }
    }
}

/* ------------------------------------------------------------------ */

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    if (message == taskbar_created_message && taskbar_created_message) {
        /* Explorer restarted and took every icon with it. */
        NOTIFYICONDATAW nid;

        fill_notify_data(&nid);
        Shell_NotifyIconW(NIM_ADD, &nid);
        return 0;
    }

    switch (message) {
    case WM_TRAY_ICON:
        switch (LOWORD(lparam)) {
        case WM_RBUTTONUP:
        case WM_LBUTTONUP:
        case WM_CONTEXTMENU:
            show_menu();
            break;
        default:
            break;
        }
        return 0;
    case WM_TRAY_REFRESH:
        refresh_icon();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static DWORD WINAPI tray_main(LPVOID param) {
    WNDCLASSEXW wc;
    HINSTANCE instance = GetModuleHandleW(NULL);
    NOTIFYICONDATAW nid;
    MSG message;

    (void) param;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"UxPlayTrayWindow";
    RegisterClassExW(&wc);

    /* Never shown: it exists to own the icon and receive its messages. */
    tray_window = CreateWindowExW(0, L"UxPlayTrayWindow", L"UxPlay",
                                  WS_OVERLAPPED, 0, 0, 0, 0,
                                  NULL, NULL, instance, NULL);
    if (!tray_window) {
        return 1;
    }

    create_icons();
    taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");

    fill_notify_data(&nid);
    Shell_NotifyIconW(NIM_ADD, &nid);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = tray_window;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);

    if (icon_idle) {
        DestroyIcon(icon_idle);
        icon_idle = NULL;
    }
    if (icon_active) {
        DestroyIcon(icon_active);
        icon_active = NULL;
    }
    tray_window = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */

void statusbar_init(void) {
    DWORD id;

    if (!lock_ready) {
        InitializeCriticalSection(&lock);
        lock_ready = TRUE;
    }
    if (tray_thread) {
        return;
    }
    tray_thread = CreateThread(NULL, 0, tray_main, NULL, 0, &id);
}

static void post_refresh(void) {
    if (tray_window) {
        PostMessageW(tray_window, WM_TRAY_REFRESH, 0, 0);
    }
}

void statusbar_set_client(const char *name, const char *model) {
    if (!lock_ready) {
        return;
    }
    EnterCriticalSection(&lock);
    to_wide(name, client_name, ARRAYSIZE(client_name));
    to_wide(model, client_model, ARRAYSIZE(client_model));
    LeaveCriticalSection(&lock);
    post_refresh();
}

void statusbar_set_state(statusbar_state_t state) {
    if (!lock_ready) {
        return;
    }
    EnterCriticalSection(&lock);
    current_state = state;
    LeaveCriticalSection(&lock);
    post_refresh();
}

void statusbar_set_metadata(const char *title, const char *artist) {
    wchar_t t[128], a[128];

    if (!lock_ready) {
        return;
    }
    to_wide(title, t, ARRAYSIZE(t));
    to_wide(artist, a, ARRAYSIZE(a));

    EnterCriticalSection(&lock);
    if (t[0] && a[0]) {
        _snwprintf(track_text, ARRAYSIZE(track_text), L"\x266A %ls \x2014 %ls", a, t);
    } else if (t[0]) {
        _snwprintf(track_text, ARRAYSIZE(track_text), L"\x266A %ls", t);
    } else if (a[0]) {
        _snwprintf(track_text, ARRAYSIZE(track_text), L"\x266A %ls", a);
    } else {
        track_text[0] = L'\0';
    }
    track_text[ARRAYSIZE(track_text) - 1] = L'\0';
    elide(track_text, 46);
    LeaveCriticalSection(&lock);
}

void statusbar_set_volume(double fraction) {
    if (!lock_ready) {
        return;
    }
    EnterCriticalSection(&lock);
    volume_fraction = fraction;
    LeaveCriticalSection(&lock);
}

/* Called about once a second while an HLS stream runs. The menu reads this
   when it is opened, so there is nothing to redraw here and no message to
   post. */
void statusbar_set_progress(double position, double duration) {
    if (!lock_ready) {
        return;
    }
    EnterCriticalSection(&lock);
    progress_position = position;
    progress_duration = duration;
    LeaveCriticalSection(&lock);
}

void statusbar_set_volume_handler(void (*handler)(double fraction)) {
    volume_handler = handler;
}

void statusbar_set_disconnect_handler(void (*handler)(void)) {
    disconnect_handler = handler;
}

void statusbar_set_seek_handler(void (*handler)(double position)) {
    seek_handler = handler;
}

void statusbar_set_display_handler(void (*handler)(int index)) {
    display_handler = handler;
}

void statusbar_set_quit_handler(void (*handler)(void)) {
    quit_handler = handler;
}

void statusbar_set_fullscreen_on_connect_hooks(bool (*get)(void),
                                               void (*set)(bool enable)) {
    fullscreen_on_connect_get = get;
    fullscreen_on_connect_set = set;
}

void statusbar_destroy(void) {
    HANDLE thread = tray_thread;

    if (!thread) {
        return;
    }
    tray_thread = NULL;
    if (tray_window) {
        PostMessageW(tray_window, WM_CLOSE, 0, 0);
    }
    /* Bounded: cleanup() runs on the way out and must not hang if the pump is
       stuck inside a menu the user left open. */
    WaitForSingleObject(thread, 2000);
    CloseHandle(thread);
}
