// overlay.cpp — the region-selector state machine.
//
// This is its own translation unit not for size but for auditability: "the
// overlay must never survive as a zombie" is a property you verify by reading
// every return path of one WndProc. Every path funnels through FinishSelect().
//
// The selection is a genuine HOLE in the window region, not a painted
// rectangle. SetLayeredWindowAttributes applies one uniform alpha to the whole
// surface, so a bright rectangle cannot be painted into it — but a hole lets
// the untouched screen pixels show through at 100%, and costs zero bytes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "settings.h"
#include "overlay.h"

static HWND      g_overlay = nullptr;   // nullptr == no overlay alive
static HINSTANCE g_hInst   = nullptr;
static HWND      g_hNotify = nullptr;
static bool      g_posted  = false;     // makes "result posted" + "destroyed" atomic

static POINT g_anchor    = {};
static RECT  g_selRect   = {};          // normalized, virtual-screen pixels
static bool  g_dragging  = false;

static int g_vx, g_vy, g_vw, g_vh;      // cached virtual-screen metrics

static bool    g_classRegistered = false;
static HCURSOR g_cross = nullptr;

// -----------------------------------------------------------------------------
static void NormalizePoints(const POINT& a, const POINT& b, RECT* out)
{
    out->left   = (a.x < b.x) ? a.x : b.x;
    out->top    = (a.y < b.y) ? a.y : b.y;
    out->right  = (a.x > b.x) ? a.x : b.x;
    out->bottom = (a.y > b.y) ? a.y : b.y;
}

// Posts the outcome exactly once, then destroys. Every terminal path calls this.
static void FinishSelect(HWND hwnd, bool accepted)
{
    if (!g_posted) {
        g_posted = true;
        if (accepted)
            PostMessageW(g_hNotify, WM_APP_REGION_SELECTED, 0, (LPARAM)&g_selRect);
        else
            PostMessageW(g_hNotify, WM_APP_REGION_CANCELLED, 0, 0);
    }
    // DestroyWindow is synchronous, so the overlay no longer exists by the time
    // the message above is dispatched. The capture therefore cannot contain it
    // by construction, not by convention.
    DestroyWindow(hwnd);
}

// The window region is in CLIENT coordinates, and client (0,0) is virtual
// (g_vx, g_vy) -- so the hole must be translated by the virtual origin. Miss
// this and the hole is offset by a whole monitor, but only on a setup with a
// screen left of or above the primary.
static void UpdateRegion(HWND hwnd)
{
    HRGN rgn = CreateRectRgn(0, 0, g_vw, g_vh);
    if (!rgn) return;

    if (g_dragging) {
        HRGN hole = CreateRectRgn(g_selRect.left   - g_vx,
                                  g_selRect.top    - g_vy,
                                  g_selRect.right  - g_vx,
                                  g_selRect.bottom - g_vy);
        if (hole) {
            CombineRgn(rgn, rgn, hole, RGN_DIFF);
            DeleteObject(hole);
        }
    }

    // SetWindowRgn transfers ownership of the region to the system. Calling
    // DeleteObject(rgn) here would be a double free. If GR_GDIOBJECTS climbs
    // during a drag, this call is the culprit -- only re-set the region when
    // the rounded rectangle actually changed.
    SetWindowRgn(hwnd, rgn, TRUE);
}

// -----------------------------------------------------------------------------
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT pt;
        GetCursorPos(&pt);
        g_anchor = pt;
        g_selRect.left = g_selRect.right = pt.x;
        g_selRect.top = g_selRect.bottom = pt.y;
        g_dragging = true;
        SetCapture(hwnd);
        UpdateRegion(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            NormalizePoints(g_anchor, pt, &g_selRect);   // min/max: a drag up-and-left is legal
            UpdateRegion(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_LBUTTONUP: {
        if (!g_dragging) return 0;
        POINT pt;
        GetCursorPos(&pt);
        NormalizePoints(g_anchor, pt, &g_selRect);
        g_dragging = false;

        // Release-to-confirm, deliberately. While the hole exists a click
        // inside it lands on whatever window is underneath, so the overlay
        // must never be waiting for a confirmation click.
        const bool big = (g_selRect.right - g_selRect.left) >= SHOT_MIN_SEL &&
                         (g_selRect.bottom - g_selRect.top) >= SHOT_MIN_SEL;
        FinishSelect(hwnd, big);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) FinishSelect(hwnd, false);
        return 0;

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_CLOSE:
        FinishSelect(hwnd, false);
        return 0;

    case WM_SETCURSOR:
        // Must SetCursor AND return TRUE; returning FALSE lets DefWindowProc
        // put the arrow back.
        SetCursor(g_cross);
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Stock objects: zero GDI handles created, so nothing to leak.
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));

        if (g_dragging) {
            RECT r;
            r.left   = g_selRect.left   - g_vx;
            r.top    = g_selRect.top    - g_vy;
            r.right  = g_selRect.right  - g_vx;
            r.bottom = g_selRect.bottom - g_vy;

            // Drawn just OUTSIDE the hole -- anything inside is clipped away by
            // the region. A double line stays visible on busy backgrounds.
            RECT w = r; InflateRect(&w, 1, 1);
            FrameRect(hdc, &w, (HBRUSH)GetStockObject(WHITE_BRUSH));
            RECT b = r; InflateRect(&b, 2, 2);
            FrameRect(hdc, &b, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DPICHANGED:
        // Ignore. lParam's suggested rect is the NEW MONITOR'S WORK AREA;
        // applying it would shrink a full-virtual-screen overlay to one screen
        // in the middle of a capture.
        return 0;

    case WM_DESTROY:
        // Unconditional: pressing Esc mid-drag destroys the window while it
        // still holds capture, and leaving that behind makes the mouse behave
        // strangely system-wide.
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!g_posted) {          // reached via a path that did not post a result
            g_posted = true;
            PostMessageW(g_hNotify, WM_APP_REGION_CANCELLED, 0, 0);
        }
        g_overlay = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
bool StartRegionSelect(HINSTANCE hInst, HWND hNotify)
{
    if (g_overlay) return false;   // pressing the hotkey twice must not stack overlays

    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_vw <= 0 || g_vh <= 0) return false;

    if (!g_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = OverlayWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"ShotOverlayWnd";
        wc.hbrBackground = nullptr;   // WM_PAINT does all the painting
        if (!RegisterClassExW(&wc)) return false;
        g_cross = LoadCursorW(nullptr, IDC_CROSS);
        g_classRegistered = true;
    }

    g_hInst   = hInst;
    g_hNotify = hNotify;
    g_posted  = false;
    g_dragging = false;

    // WS_EX_TOOLWINDOW keeps it out of Alt+Tab and the taskbar. Deliberately
    // NOT WS_EX_NOACTIVATE -- we need the keyboard for Esc.
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                L"ShotOverlayWnd", L"", WS_POPUP,
                                g_vx, g_vy, g_vw, g_vh,
                                hNotify, nullptr, hInst, nullptr);
    if (!g_overlay) return false;

    SetLayeredWindowAttributes(g_overlay, 0, SHOT_DIM_ALPHA, LWA_ALPHA);
    UpdateRegion(g_overlay);

    // The overlay only receives WM_KEYDOWN while it is the foreground window.
    // Attaching to the current foreground thread's input queue makes the
    // SetForegroundWindow call succeed where it would otherwise be refused.
    HWND  fg      = GetForegroundWindow();
    DWORD fgTid   = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid   = GetCurrentThreadId();
    BOOL  attached = FALSE;
    if (fgTid && fgTid != myTid)
        attached = AttachThreadInput(fgTid, myTid, TRUE);

    ShowWindow(g_overlay, SW_SHOW);
    BringWindowToTop(g_overlay);
    SetForegroundWindow(g_overlay);
    SetFocus(g_overlay);

    if (attached) AttachThreadInput(fgTid, myTid, FALSE);   // always detach

    return true;
}

void AbortRegionSelect()
{
    if (g_overlay) DestroyWindow(g_overlay);   // WM_DESTROY clears g_overlay
}
