// main.cpp — the resident path.
//
// This file owns everything that lives for the whole process lifetime: one
// hidden top-level window, the tray icon, the global hotkey, and the message
// loop. It deliberately contains no DIB, WIC or clipboard code, so "what is
// resident" can be answered by reading this one file.
//
// The window is a never-shown top-level window rather than HWND_MESSAGE
// because message-only windows do not receive broadcasts — and we need
// WM_TASKBARCREATED to survive an Explorer restart.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <dwmapi.h>

#include "settings.h"
#include "capture.h"
#include "overlay.h"
#include "pin.h"
#include "apputil.h"

static HINSTANCE g_hInst = nullptr;
static HWND      g_hMsg  = nullptr;
static UINT      g_wmTaskbarCreated = 0;
static bool      g_trayAdded = false;

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// -----------------------------------------------------------------------------
// Capture orchestration. The order here IS the correctness; see the comments.
// -----------------------------------------------------------------------------
static void DoCapture(const RECT& rcVirtualPx)
{
    const DWORD gdiBefore  = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const int   pinsBefore = PinCount();

    // Wait for DWM to present a frame composed without the overlay. Without
    // this the BitBlt can return the previous frame and the capture comes back
    // with a grey wash — a race that looks like a deterministic bug.
    DwmFlush();

    Bitmap32 bm;
    if (!GrabScreenRect(rcVirtualPx, &bm)) {
        ReportError(L"抓取屏幕失败。", HRESULT_FROM_WIN32(GetLastError()));
        return;
    }

    // Save a PNG on every capture. This runs BEFORE PinCreate because the pin
    // takes ownership of bm — writing from the same bitmap means no copy, and
    // the alternative (saving from the pin afterwards) would need an accessor
    // and a lifetime rule for the sake of appearing a few milliseconds sooner.
    wchar_t path[MAX_PATH];
    if (BuildPngPath(path, ARRAYSIZE(path))) {
        const HRESULT hr = SavePngWithCom(bm, path);
        if (FAILED(hr)) ReportError(L"保存 PNG 失败。", hr);
    } else {
        ReportError(L"无法创建 shots 目录。", HRESULT_FROM_WIN32(GetLastError()));
    }

    // Clipboard next: it also reads bm, which PinCreate is about to consume.
    CopyBitmapToClipboard(g_hMsg, bm);

    if (!PinCreate(g_hInst, g_hMsg, &bm))
        ReportError(L"创建贴图失败。", HRESULT_FROM_WIN32(GetLastError()));

    // PinCreate consumed bm unconditionally, so this is a no-op on success and
    // the cleanup on failure — one path, no branch.
    FreeBitmap32(&bm);
    TrimWorkingSet();

    // The sentinel must account for the pins that were legitimately created, or
    // it fires on every successful capture and a real leak drowns in the noise.
    const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const int   newPins  = PinCount() - pinsBefore;
    if (gdiAfter != gdiBefore + (DWORD)(newPins * SHOT_PIN_GDI_COST))
        TraceGuiObjects(L"LEAK");
}

// -----------------------------------------------------------------------------
// Tray
// -----------------------------------------------------------------------------
static void RemoveTrayIcon(HWND hwnd)
{
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = SHOT_TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

static bool AddTrayIcon(HWND hwnd)
{
    HICON icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!icon) return false;

    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(nid);   // must be the W struct's size
    nid.hWnd             = hwnd;
    nid.uID              = SHOT_TRAY_ID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = SHOT_TRAY_CB;
    nid.hIcon            = icon;
    StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), L"截图工具 — Ctrl+Alt+Q");

    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return g_trayAdded;
}

static void ShowTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_CAPTURE, L"截图(&S)   Ctrl+Alt+Q");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN_SHOTS, L"打开截图文件夹(&O)");
    AppendMenuW(menu, MF_STRING, IDM_CLOSE_ALL_PINS, L"关闭全部贴图(&L)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出(&X)");

    POINT pt;
    GetCursorPos(&pt);

    // Both of these are required or the menu refuses to dismiss when the user
    // clicks elsewhere — a stuck menu that only clears on the next click.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

// -----------------------------------------------------------------------------
// Hotkey
// -----------------------------------------------------------------------------
static bool RegisterShotHotkey(HWND hwnd)
{
    if (RegisterHotKey(hwnd, SHOT_HOTKEY_ID, SHOT_HOTKEY_MODS, SHOT_HOTKEY_VK))
        return true;

    // A silent registration failure is indistinguishable from "the app is
    // broken", so never assume success.
    const DWORD err = GetLastError();
    wchar_t buf[320];
    StringCchPrintfW(buf, ARRAYSIZE(buf),
        L"注册全局热键 Ctrl+Alt+Q 失败（错误码 %lu）。\n\n"
        L"该热键通常已被其它程序占用。\n"
        L"请关闭冲突程序，或修改 src\\settings.h 中的 SHOT_HOTKEY_VK 后重新编译。",
        err);
    MessageBoxW(nullptr, buf, L"截图工具", MB_ICONERROR | MB_OK);
    return false;
}

// -----------------------------------------------------------------------------
// Window procedure — the whole resident behaviour
// -----------------------------------------------------------------------------
static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_wmTaskbarCreated != 0 && msg == g_wmTaskbarCreated) {
        // Explorer restarted and the tray was rebuilt from scratch. Without
        // this the icon disappears permanently.
        g_trayAdded = false;
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg) {
    case SHOT_TRAY_CB:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hwnd);
            break;
        case WM_LBUTTONUP:
            PostMessageW(hwnd, WM_COMMAND, IDM_CAPTURE, 0);
            break;
        }
        return 0;

    case WM_HOTKEY:
        if (wp == SHOT_HOTKEY_ID)
            StartRegionSelect(g_hInst, g_hMsg);
        return 0;

    case WM_APP_REGION_SELECTED:
        // The overlay is already destroyed at this point, and lParam points at
        // overlay.cpp's file-scope RECT, which stays valid.
        if (lp) DoCapture(*(const RECT*)lp);
        return 0;

    case WM_APP_REGION_CANCELLED:
        // A cancelled selection never reaches DoCapture, so without this the
        // pages the overlay paged in would stay resident indefinitely.
        AbortRegionSelect();
        TrimWorkingSet();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_CAPTURE:
            StartRegionSelect(g_hInst, g_hMsg);
            return 0;
        case IDM_OPEN_SHOTS:
            if (!OpenShotsFolder())
                ReportError(L"无法打开截图文件夹。", HRESULT_FROM_WIN32(GetLastError()));
            return 0;
        case IDM_CLOSE_ALL_PINS:
            // Refused while a pin's modal save dialog is up: that dialog
            // dispatches messages thread-wide, so destroying pins here would
            // free one out from under the code about to dereference it.
            if (!PinModalActive()) CloseAllPins();
            return 0;
        case IDM_EXIT:
            // Same interlock: quitting mid-dialog would free the pin the dialog
            // owner belongs to. The user just clicks 退出 again afterwards.
            if (PinModalActive()) return 0;
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        // Order matters: free every pin DIB while the process is still sane,
        // then the overlay, then the resident bits.
        CloseAllPins();
        AbortRegionSelect();       // a quit mid-selection must not leave a fullscreen window
        UnregisterHotKey(hwnd, SHOT_HOTKEY_ID);
        RemoveTrayIcon(hwnd);      // must happen here, not after the loop
        g_hMsg = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // MUST be the first statement in the program. Called after any window
    // exists it returns FALSE and fails *silently*, and the symptom is a
    // process that believes a 4K@150% display is 2560px wide: the overlay
    // fails to cover the screen and captures come back scaled and blurry.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Deliberately no CoInitializeEx here. COM is only needed by the WIC
    // encoder on the capture path, and initialising it in the resident process
    // would page in the whole COM stack (ole32, combase, RPCRT4, MSCTF...) for
    // a program that sits idle in the tray all day. The capture path does its
    // own CoInitializeEx/CoUninitialize pair.
    g_hInst = hInst;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MsgWndProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = L"ShotMsgWnd";
    wc.hbrBackground = nullptr;   // never painted; creating a brush would be waste
    if (!RegisterClassExW(&wc))
        return 1;

    g_hMsg = CreateWindowExW(WS_EX_TOOLWINDOW, L"ShotMsgWnd", L"", 0,
                             0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
    if (!g_hMsg)
        return 1;

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Registering the hotkey before adding the tray icon gives a free
    // single-instance guard: a second launch fails here and exits.
    if (!RegisterShotHotkey(g_hMsg)) {
        DestroyWindow(g_hMsg);
        return 1;
    }

    if (!AddTrayIcon(g_hMsg))
        OutputDebugStringW(L"[shot] WARNING: NIM_ADD failed - no tray icon\n");

    TrimWorkingSet();              // drop the pages startup touched
    TraceGuiObjects(L"baseline");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
