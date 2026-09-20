// pin.cpp — pinned images.
//
// A pin owns exactly ONE DIB (the original-resolution capture) and never
// rescales or copies it. Display size lives in curW/curH and is applied at
// paint time, so resizing costs zero extra memory.
//
// This file is also where the two subtle lifecycle hazards live:
//   - a pin must never be reachable from the list after its window is gone
//   - a pin must not be freed while a modal dialog is about to dereference it
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN drops these from windows.h.
#include <commdlg.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include "settings.h"
#include "capture.h"
#include "pin.h"
#include "apputil.h"

enum { PIN_DRAG_NONE = 0, PIN_DRAG_MOVE,
       PIN_DRAG_RESIZE_TL, PIN_DRAG_RESIZE_TR,
       PIN_DRAG_RESIZE_BL, PIN_DRAG_RESIZE_BR };

struct Pin {
    Pin*     next;          // intrusive singly-linked list link; null-terminated
    HWND     hwnd;
    Bitmap32 bm;            // THE one DIB. Never resampled, never copied.
    int      curW, curH;    // displayed IMAGE size; aspect-locked to bm.w:bm.h
                            // (the window is this plus 2*SHOT_PIN_BORDER)
    bool     flashing;      // creation flash still running
    int      dragMode;
    POINT    dragStartPt;   // cursor at button-down, SCREEN coords
    RECT     dragStartRect; // window rect at button-down, SCREEN coords
};

static Pin*  g_pins     = nullptr;   // head
static int   g_pinCount = 0;
static int   g_modalDepth = 0;
static bool  g_classRegistered = false;
static HCURSOR g_curMove = nullptr, g_curNWSE = nullptr, g_curNESW = nullptr;
// Created once at class registration, never per paint. WM_PAINT therefore still
// allocates zero GDI objects — the same auditability property overlay.cpp earns
// from stock brushes.
static HBRUSH  g_brushBorder = nullptr, g_brushFlash = nullptr;

// Pointer-to-pointer unlink: no prev bookkeeping, no special case for the head.
static void UnlinkPin(Pin* target)
{
    Pin** pp = &g_pins;
    while (*pp && *pp != target) pp = &(*pp)->next;
    if (*pp) *pp = target->next;
}

// -----------------------------------------------------------------------------
// Drag
// -----------------------------------------------------------------------------
static int MaxI(int a, int b) { return a > b ? a : b; }
static int MinI(int a, int b) { return a < b ? a : b; }
static int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Corner hit-zone size. The clamp z <= min(w,h)/3 is what makes the four corner
// squares pairwise disjoint AND leaves a non-empty centre band: the left zone
// is x<z and the right is x>=w-z, which overlap only if w<2z, and 2z<=2w/3<w.
// So small pins get smaller zones rather than zones that swallow the whole pin
// and make it impossible to move.
static int CornerZone(const Pin* p)
{
    int z = SHOT_PIN_CORNER;
    const int m = (p->curW < p->curH) ? p->curW : p->curH;
    if (z * 3 > m) z = m / 3;
    return z;
}

static bool LeftEdge(int mode)  { return mode == PIN_DRAG_RESIZE_TL || mode == PIN_DRAG_RESIZE_BL; }
static bool TopEdge(int mode)   { return mode == PIN_DRAG_RESIZE_TL || mode == PIN_DRAG_RESIZE_TR; }

static void BeginDrag(HWND hwnd, Pin* p, LPARAM lp)
{
    // Client coordinates from lParam: the window is stationary at this instant,
    // so they are unambiguous and need no GetCursorPos/ScreenToClient pair.
    // Shifted into IMAGE space, because the margin is not part of the picture.
    const int mx = GET_X_LPARAM(lp) - SHOT_PIN_BORDER;
    const int my = GET_Y_LPARAM(lp) - SHOT_PIN_BORDER;
    const int z  = CornerZone(p);
    const bool L = mx < z, R = mx >= p->curW - z;
    const bool T = my < z, B = my >= p->curH - z;

    p->dragMode = (L && T) ? PIN_DRAG_RESIZE_TL
                : (R && T) ? PIN_DRAG_RESIZE_TR
                : (L && B) ? PIN_DRAG_RESIZE_BL
                : (R && B) ? PIN_DRAG_RESIZE_BR
                :            PIN_DRAG_MOVE;

    GetCursorPos(&p->dragStartPt);       // SCREEN coords
    GetWindowRect(hwnd, &p->dragStartRect);
    SetCapture(hwnd);
}

static void DoResize(HWND hwnd, Pin* p, const POINT& pt)
{
    const int bd = SHOT_PIN_BORDER;
    const int a  = p->bm.w;   // the ORIGINAL aspect, never the current size
    const int b  = p->bm.h;
    if (a <= 0 || b <= 0) return;

    const bool left = LeftEdge(p->dragMode);
    const bool top  = TopEdge(p->dragMode);

    // The corner OPPOSITE the grabbed one is the fixed anchor. dragStartRect is
    // the button-down snapshot, so the anchored edge lands on exactly the same
    // pixel on every event instead of creeping.
    const int ax = left ? p->dragStartRect.right  : p->dragStartRect.left;
    const int ay = top  ? p->dragStartRect.bottom : p->dragStartRect.top;

    const __int64 dx = (__int64)pt.x - ax;   // cursor relative to the anchor
    const __int64 dy = (__int64)pt.y - ay;
    const __int64 sx = left ? -1 : 1;        // which side the dragged corner is on
    const __int64 sy = top  ? -1 : 1;

    // Project the cursor onto the aspect diagonal: the w minimising
    // |(sx*w, sy*w*b/a) - (dx,dy)|^2, cleared of fractions.
    //
    // This replaces a "whichever axis moved more wins" rule, which was
    // DISCONTINUOUS. When the horizontal and vertical candidates sit on
    // opposite sides of the current width, the winner flipping made the size
    // jump by the entire gap between them — which on a diagonal drag is
    // exactly what happens. The projection is continuous everywhere, so a jump
    // is not merely unlikely, it is unrepresentable.
    const __int64 aa  = (__int64)a * a;
    const __int64 bb  = (__int64)b * b;
    const __int64 num = sx * dx * aa + sy * dy * b * a;
    const __int64 den = aa + bb;
    const int raw = (int)(num / den);

    // Bounds.
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    // The min width whose DERIVED height also clears the floor. Plain
    // SHOT_PIN_MIN_PX would let a 4000x200 panorama become 32x1.
    const int minW = MaxI(SHOT_PIN_MIN_PX, MulDiv(SHOT_PIN_MIN_PX, a, b));
    // The max width whose derived height exactly fills the desktop. Past it the
    // pin's corner zones are off-screen and it can never be grabbed again.
    const int maxW = MinI(vw, MulDiv(vh, a, b));

    const int w = ClampI(raw, minW, maxW);
    // Height derived LAST, so the min-size constraint stays a single-variable
    // problem. Clamping each axis independently would break the aspect ratio
    // exactly when the user is least able to tell.
    const int h = MulDiv(w, b, a);

    const int winW = w + 2 * bd;
    const int winH = h + 2 * bd;
    const int x = left ? ax - winW : ax;
    const int y = top  ? ay - winH : ay;

    SetWindowPos(hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
    p->curW = w;
    p->curH = h;

    // Repaint SYNCHRONOUSLY, before returning to the message loop.
    //
    // InvalidateRect only marks the window dirty; the actual painting happens
    // later, when the message queue drains. But SetWindowPos has already
    // applied the new size, so there is a window of time in which the window is
    // presented at its new size while still showing the previous frame — the
    // outline sits at its old position for a frame and then jumps. UpdateWindow
    // paints now, so no such frame can ever be composed.
    InvalidateRect(hwnd, nullptr, FALSE);   // FALSE: the blit covers every pixel
    UpdateWindow(hwnd);
}

static void DoDrag(HWND hwnd, Pin* p)
{
    // GetCursorPos, NOT the WM_MOUSEMOVE lParam. With capture held, lParam is
    // in CLIENT coordinates — and in move mode the window is moving because of
    // this very message, so the frame would chase itself and the pin would
    // jitter. Screen coordinates make the delta frame-invariant.
    POINT pt;
    GetCursorPos(&pt);

    if (p->dragMode == PIN_DRAG_MOVE) {
        // Every geometry is a pure function of the button-down snapshot and the
        // current cursor. Never feed the result of SetWindowPos back in as the
        // next input: SetWindowPos quantizes to integers, so an incremental
        // scheme creeps a pixel per event and the window visibly walks.
        // SWP_NOREDRAW is load-bearing, not an optimisation. What the window
        // draws depends only on the bitmap and the display size, never on where
        // it sits — so a move changes nothing about its contents and a repaint
        // is pure waste. Without the flag SetWindowPos invalidates the window
        // on every single mouse-move, and dragging a large pin becomes a stream
        // of full "fill the border, re-blit the whole image" cycles. That is
        // what makes the white outline appear to flicker while moving.
        SetWindowPos(hwnd, nullptr,
                     p->dragStartRect.left + (pt.x - p->dragStartPt.x),
                     p->dragStartRect.top  + (pt.y - p->dragStartPt.y),
                     0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    } else {
        DoResize(hwnd, p, pt);
    }
}

// -----------------------------------------------------------------------------
// Context menu
// -----------------------------------------------------------------------------
static void ShowPinMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_PIN_COPY,       L"复制到剪贴板(&C)");
    AppendMenuW(menu, MF_STRING, IDM_PIN_SAVEAS,     L"另存为 PNG(&S)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Navigation, so it sits apart from the per-image actions above. Every
    // capture is already written to shots\ automatically, so "where did it go"
    // is the natural next question while looking at the pin.
    AppendMenuW(menu, MF_STRING, IDM_PIN_OPEN_SHOTS, L"打开截图文件夹(&O)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_PIN_CLOSE,      L"关闭(&X)");

    POINT pt;
    GetCursorPos(&pt);

    // Both required or the menu refuses to dismiss on an outside click —
    // especially for a WS_EX_TOPMOST window, where the foreground window when
    // the user clicks another topmost window is what drives dismissal.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

static void PinSaveAs(HWND hwnd, Pin* p)
{
    // Seed the dialog with a ready-made path: zero new code buys a sensible
    // directory, a timestamped name and collision handling. shots\ is created
    // lazily here, only when the user actually saves.
    wchar_t path[MAX_PATH];
    if (!BuildPngPath(path, ARRAYSIZE(path)))
        path[0] = L'\0';

    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hwnd;    // the pin, not the hidden window, so the dialog
                                // cannot appear behind a topmost window
    ofn.lpstrFilter  = L"PNG 图片 (*.png)\0*.png\0\0";   // trailing double null
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = ARRAYSIZE(path);
    ofn.lpstrDefExt  = L"png";  // omit this and typing a bare name saves a file
                                // with no extension, silently
    ofn.lpstrTitle   = L"另存为 PNG";
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    ++g_modalDepth;
    const BOOL ok = GetSaveFileNameW(&ofn);
    --g_modalDepth;
    if (!ok) return;

    // Second line of defence. GetSaveFileNameW runs a modal loop that
    // dispatches to EVERY window on the thread, so the pin can be destroyed
    // while the dialog is up even with the interlock — cheap to check.
    if (!IsWindow(hwnd)) return;

    // Saves at the ORIGINAL bm.w x bm.h, never the displayed size. The pin is a
    // view; the data is always the original. This falls out of "never rescale
    // the DIB" for free — as long as we pass p->bm and not a synthesised size.
    const HRESULT hr = SavePngWithCom(p->bm, path);
    if (FAILED(hr)) ReportError(L"保存 PNG 失败。", hr);
}

// -----------------------------------------------------------------------------
static LRESULT CALLBACK PinWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Pin* p = (Pin*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (p && p->bm.hdc) {
            const int bd = SHOT_PIN_BORDER;

            // Fill ONLY the four margin strips, never the whole client.
            //
            // Filling the whole client and then painting the image over it
            // looks equivalent and is not: a resize drag repaints on every
            // mouse-move, and the intermediate state — whole window painted in
            // the border colour, image not yet drawn — is visible as a flash of
            // the border colour. Leaving the image area untouched means there is
            // nothing to flash; the previous frame stays until the blit replaces
            // it.
            const int W = p->curW + 2 * bd;
            const int H = p->curH + 2 * bd;
            HBRUSH br = p->flashing ? g_brushFlash : g_brushBorder;
            RECT   s;

            s.left = 0;     s.top = 0;      s.right = W;     s.bottom = bd;
            FillRect(hdc, &s, br);                                          // top
            s.top = H - bd; s.bottom = H;
            FillRect(hdc, &s, br);                                          // bottom
            s.left = 0;     s.top = bd;     s.right = bd;    s.bottom = H - bd;
            FillRect(hdc, &s, br);                                          // left
            s.left = W - bd; s.right = W;
            FillRect(hdc, &s, br);                                          // right

            if (p->curW == p->bm.w && p->curH == p->bm.h) {
                // 1:1 is the common case and a freshly created pin is always
                // 1:1, so this branch is what the user sees first. Routing it
                // through StretchBlt would mean a brand-new pin is already a
                // filtered version of the capture.
                BitBlt(hdc, bd, bd, p->bm.w, p->bm.h, p->bm.hdc, 0, 0, SRCCOPY);
            } else {
                // COLORONCOLOR (nearest-neighbour), deliberately not HALFTONE:
                // screenshots are aliased text and 1px rules, and HALFTONE's
                // averaging turns those to mush.
                SetStretchBltMode(hdc, COLORONCOLOR);
                StretchBlt(hdc, bd, bd, p->curW, p->curH,
                           p->bm.hdc, 0, 0, p->bm.w, p->bm.h, SRCCOPY);
            }

            if (p->flashing) {
                // Rings drawn OVER the image edge take the visible outline from
                // 2px to 4px. They only borrow those pixels: the timer reverts
                // them 600ms later, and any resize repaints from bm anyway.
                for (int i = 0; i < SHOT_PIN_FLASH_PX; ++i) {
                    RECT ring;
                    ring.left   = bd + i;
                    ring.top    = bd + i;
                    ring.right  = bd + p->curW - i;
                    ring.bottom = bd + p->curH - i;
                    FrameRect(hdc, &ring, g_brushFlash);
                }
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == SHOT_PIN_FLASH_TIMER && p) {
            KillTimer(hwnd, SHOT_PIN_FLASH_TIMER);
            p->flashing = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        // The blit covers the entire client area; erasing first is pure flicker.
        return TRUE;

    case WM_LBUTTONDOWN:
        if (p) BeginDrag(hwnd, p, lp);
        return 0;

    case WM_MOUSEMOVE:
        // The MK_LBUTTON test is belt-and-braces against a lost button-up.
        if (p && p->dragMode != PIN_DRAG_NONE && (wp & MK_LBUTTON))
            DoDrag(hwnd, p);
        return 0;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        if (p) p->dragMode = PIN_DRAG_NONE;
        return 0;

    case WM_CAPTURECHANGED:
        // Capture was lost involuntarily (a UAC prompt, another app, Alt+Tab).
        // Without this, dragMode stays set and the pin follows the bare cursor
        // forever with no button held — the classic "my window is possessed".
        if (p) p->dragMode = PIN_DRAG_NONE;
        return 0;

    case WM_LBUTTONDBLCLK:
        // A double-click arrives as DOWN, UP, DBLCLK, UP. The first DOWN ran
        // the full move path and the first UP cleared dragMode harmlessly —
        // which is exactly why this handler must not depend on drag state, and
        // why WM_LBUTTONUP here must never grow commit/cancel semantics.
        if (!PinModalActive()) DestroyWindow(hwnd);
        return 0;

    case WM_RBUTTONUP:
        // TrackPopupMenu takes capture internally, so any left-drag still in
        // flight must be ended first or the two fight over it. (Reachable:
        // press left, press right, release left last.)
        if (GetCapture() == hwnd) ReleaseCapture();
        if (p) p->dragMode = PIN_DRAG_NONE;
        ShowPinMenu(hwnd);
        return 0;

    case WM_SETCURSOR: {
        if (!p) break;
        // Client coordinates, taken from the same geometry the hit test uses.
        POINT pt;
        GetCursorPos(&pt);
        RECT r;
        GetWindowRect(hwnd, &r);
        const int mx = pt.x - r.left - SHOT_PIN_BORDER;   // image-relative
        const int my = pt.y - r.top  - SHOT_PIN_BORDER;
        const int z  = CornerZone(p);
        const bool L = mx < z, R = mx >= p->curW - z;
        const bool T = my < z, B = my >= p->curH - z;

        HCURSOR cur = g_curMove;
        if ((L && T) || (R && B))      cur = g_curNWSE;
        else if ((R && T) || (L && B)) cur = g_curNESW;

        // Must SetCursor AND return TRUE; returning FALSE lets DefWindowProc
        // put the arrow back.
        SetCursor(cur);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_PIN_COPY:
            if (p) CopyBitmapToClipboard(hwnd, p->bm);
            return 0;
        case IDM_PIN_SAVEAS:
            if (p) PinSaveAs(hwnd, p);
            return 0;
        case IDM_PIN_OPEN_SHOTS:
            if (!OpenShotsFolder())
                ReportError(L"无法打开截图文件夹。", HRESULT_FROM_WIN32(GetLastError()));
            return 0;
        case IDM_PIN_CLOSE:
            if (!PinModalActive()) DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_DPICHANGED:
        // Ignore, exactly as the overlay does. lParam's suggested rect is the
        // new monitor's work area; applying it would resize a pin when it is
        // dragged across a monitor boundary. The DIB is in physical pixels and
        // the process is per-monitor-DPI-aware, so nothing here needs scaling.
        return 0;

    case WM_DESTROY:
        if (GetCapture() == hwnd) ReleaseCapture();
        KillTimer(hwnd, SHOT_PIN_FLASH_TIMER);   // harmless if never set
        if (p) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // no later paint can see it
            UnlinkPin(p);                               // list surgery BEFORE freeing
            FreeBitmap32(&p->bm);
            HeapFree(GetProcessHeap(), 0, p);
            g_pinCount--;
        }
        // Only trim when nothing is left on screen. Trimming while other pins
        // are still visible pages their DIBs out and the compositor pages them
        // straight back in — twenty syscalls and a pile of soft faults for
        // nothing.
        if (g_pinCount == 0) TrimWorkingSet();
        TraceGuiObjects(L"pin-destroyed");
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
bool PinCreate(HINSTANCE hInst, HWND hOwner, Bitmap32* bm)
{
    // Consume *bm unconditionally on EVERY path, including all failures below.
    // That is what lets DoCapture have a single cleanup path with no branch on
    // success, which is the difference between a leak you can spot by reading
    // and one you cannot.
    if (!bm || !bm->hdc || bm->w <= 0 || bm->h <= 0) {
        FreeBitmap32(bm);
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (g_pinCount >= SHOT_PIN_MAX) {
        // Fail legibly rather than letting StretchBlt start failing silently
        // and pins render as black rectangles.
        FreeBitmap32(bm);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return false;
    }

    if (!g_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        // CS_DBLCLKS is what makes WM_LBUTTONDBLCLK arrive at all. It is a
        // CLASS attribute, and first registration wins — which is exactly why
        // this must never be registered a second time without it.
        wc.style         = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = PinWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"ShotPinWnd";
        wc.hbrBackground = nullptr;   // WM_PAINT covers everything
        if (!RegisterClassExW(&wc)) {
            FreeBitmap32(bm);
            return false;
        }
        g_classRegistered = true;

        // Stock cursors, loaded once. SetCursor with a shared stock handle
        // creates nothing, so WM_SETCURSOR stays a zero-GDI-handle message.
        g_curMove = LoadCursorW(nullptr, IDC_SIZEALL);
        g_curNWSE = LoadCursorW(nullptr, IDC_SIZENWSE);
        g_curNESW = LoadCursorW(nullptr, IDC_SIZENESW);

        g_brushBorder = CreateSolidBrush(SHOT_PIN_BORDER_RGB);
        g_brushFlash  = CreateSolidBrush(SHOT_PIN_FLASH_RGB);
    }

    Pin* p = (Pin*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Pin));
    if (!p) {
        FreeBitmap32(bm);
        return false;
    }

    p->bm = *bm;                  // take the GDI objects
    ZeroMemory(bm, sizeof(*bm));  // caller's struct is now empty
    p->curW = p->bm.w;
    p->curH = p->bm.h;
    p->dragMode = PIN_DRAG_NONE;

    // Scale the DISPLAYED size down if the capture would otherwise land
    // covering the desktop. Only ever down: a capture already smaller than the
    // cap is left at 1:1, because upscaling it would only make it blurry.
    //
    // p->bm keeps the original dimensions regardless, so the PNG on disk and
    // the clipboard copy are full resolution no matter what this computes.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw > 0 && vh > 0) {
        const int maxW = MulDiv(vw, SHOT_PIN_MAX_SCREEN_PCT, 100);
        const int maxH = MulDiv(vh, SHOT_PIN_MAX_SCREEN_PCT, 100);
        if (p->curW > maxW || p->curH > maxH) {
            // Width satisfying both limits, then derive the height from it —
            // same discipline as DoResize, and for the same reason: the ratio
            // must come from the ORIGINAL, not from a previous result.
            p->curW = MinI(maxW, MulDiv(maxH, p->bm.w, p->bm.h));
            p->curH = MulDiv(p->curW, p->bm.h, p->bm.w);
        }
    }

    // WS_POPUP with no border/caption means the client area IS the window rect,
    // so client and window coordinates coincide and every hit test is direct.
    // WS_EX_TOOLWINDOW keeps pins out of Alt+Tab. No WS_EX_LAYERED (per-pixel
    // alpha would double the memory for a feature we do not want).
    const int bd   = SHOT_PIN_BORDER;
    const int winW = p->curW + 2 * bd;
    const int winH = p->curH + 2 * bd;

    // Centred on the desktop rather than placed at the captured position. At
    // the captured position the pin sits exactly on top of whatever it depicts
    // — hiding the very thing the user wants to compare it against.
    const int x = (vw > 0) ? vx + (vw - winW) / 2 : vx;
    const int y = (vh > 0) ? vy + (vh - winH) / 2 : vy;

    p->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                              L"ShotPinWnd", L"", WS_POPUP,
                              x, y, winW, winH,
                              hOwner, nullptr, hInst, nullptr);
    if (!p->hwnd) {
        FreeBitmap32(&p->bm);
        HeapFree(GetProcessHeap(), 0, p);
        return false;
    }

    SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);

    p->next = g_pins;
    g_pins  = p;
    g_pinCount++;

    // Flash the outline so the arrival is noticed. A pin showing pixel-perfect
    // screen content is easy to miss even when it is small and central — the
    // outline is the only thing that says "this is a window, not the desktop".
    p->flashing = true;
    SetTimer(p->hwnd, SHOT_PIN_FLASH_TIMER, SHOT_PIN_FLASH_MS, nullptr);

    // SW_SHOWNA: show without activating, so capturing does not yank focus out
    // of whatever the user was doing.
    ShowWindow(p->hwnd, SW_SHOWNA);
    return true;
}

// -----------------------------------------------------------------------------
void CloseAllPins(void)
{
    if (PinModalActive()) return;

    // DestroyWindow synchronously fires WM_DESTROY, which unlinks the node.
    // That is not a hazard here, it IS the mechanism: no iteration state ever
    // exists, so there is nothing to invalidate.
    for (int guard = 0; g_pins && guard < SHOT_PIN_MAX * 4; ++guard) {
        const HWND h = g_pins->hwnd;
        DestroyWindow(h);
        // DestroyWindow fails silently and sends no WM_DESTROY if the window
        // belongs to another thread. Impossible in this single-threaded design,
        // but an infinite loop is a much worse failure than a truncated sweep,
        // and this costs nothing.
        if (g_pins && g_pins->hwnd == h) return;
    }
    TrimWorkingSet();
}

int  PinCount(void)       { return g_pinCount; }
bool PinModalActive(void) { return g_modalDepth > 0; }
