// settings.h — compile-time configuration and shared message ids.
// There is deliberately no config file: three values, changed by recompiling.
#pragma once
#include <windows.h>

// ---- Global hotkey ----------------------------------------------------------
// Ctrl+Alt+Q. Avoids Win+Shift+S (system Snipping Tool) and Ctrl+Alt+A (QQ's
// screenshot key, near-universal on a Chinese desktop).
// MOD_NOREPEAT stops a held key from queueing a dozen captures.
#define SHOT_HOTKEY_MODS   (MOD_CONTROL | MOD_ALT | MOD_NOREPEAT)
#define SHOT_HOTKEY_VK     'Q'
#define SHOT_HOTKEY_ID     1

// ---- Overlay ----------------------------------------------------------------
#define SHOT_DIM_ALPHA     110   // LWA_ALPHA value for the dim mask, 0..255
#define SHOT_MIN_SEL       4     // drags smaller than this many px are a cancel

// ---- App-private messages ---------------------------------------------------
#define WM_APP_REGION_SELECTED   (WM_APP + 1)  // lParam = const RECT* (virtual px)
#define WM_APP_REGION_CANCELLED  (WM_APP + 2)  // lParam = 0

// ---- Pin border -------------------------------------------------------------
// The outline lives in a MARGIN around the image, never on top of it, so no
// screenshot pixel is ever covered. Without it a pin is invisible: it shows
// exactly the pixels that were already there, at exactly the same position.
#define SHOT_PIN_BORDER     2                        // persistent, px
// A saturated blue rather than a neutral. White only reads against a dark
// background — over a white page (a screenshot of a document is exactly that)
// a white outline has nothing to contrast with. A saturated colour reads
// against light and dark alike, which is the whole job of this outline.
#define SHOT_PIN_BORDER_RGB RGB(30, 144, 255)        // blue
// Creation flash: the margin turns bright AND a ring this thick is drawn over
// the image edge, taking the visible outline from 2px to 4px. A colour change
// alone is too subtle to catch the eye; doubling the thickness is not.
#define SHOT_PIN_FLASH_PX   2
#define SHOT_PIN_FLASH_MS   600
// Same hue as the persistent border, but much lighter, so the flash reads as a
// pulse of the pin's own colour rather than a swap to something foreign. The
// eye catches a brightness change plus the doubling in thickness; the hue
// staying put is what keeps it from looking like a glitch.
#define SHOT_PIN_FLASH_RGB  RGB(120, 200, 255)       // light blue
#define SHOT_PIN_FLASH_TIMER 1                       // the only timer a pin owns

// ---- Pinned images ----------------------------------------------------------
// Largest share of the screen a freshly pinned image may occupy, per dimension.
// A capture bigger than this is scaled down proportionally the moment it is
// pinned, so it does not land covering the desktop. Smaller captures are left
// at 1:1 — never scaled up, which would only make them blurry.
//
// This affects the DISPLAYED size only. The file written to disk and the copy
// on the clipboard keep the full original resolution; the DIB is never
// resampled. Drag a corner to resize after the fact.
#define SHOT_PIN_MAX_SCREEN_PCT 50

#define SHOT_PIN_CORNER    16    // corner resize hit zone, physical px
#define SHOT_PIN_MIN_PX    32    // min displayed width AND height
#define SHOT_PIN_MAX       64    // refuse to create beyond this many live pins
// GDI objects each pin permanently holds. MEASURED, not guessed: creating 10
// pins moved GR_GDIOBJECTS by exactly +20, so it is 2 — CreateCompatibleDC's DC
// plus CreateDIBSection's bitmap. The hbmOld that FreeBitmap32 restores is the
// system's own default bitmap for a fresh DC, not something we created.
//
// Only used by the leak sentinel in DoCapture, which is useless if this is
// wrong. Note the sentinel still reports once on the FIRST capture: loading WIC
// adds ~100 GDI objects that never go away. That is a known one-time warm-up,
// not a leak — what the sentinel is for is catching growth that REPEATS.
#define SHOT_PIN_GDI_COST  2
// USER objects per pin is NOT cleanly 1:1. Measured: 5 pins moved
// GR_USEROBJECTS by +4, and destroying them landed 1 BELOW the baseline,
// identically on every round. The system's USER handle accounting has internal
// slack that does not correspond to windows one-for-one.
//
// It is still worth watching, because what a leak looks like here is a RATCHET
// (58 -> 62 -> 63 -> 64 ...) rather than a constant offset. What matters is
// that it returns to the same value every round; it does.
#define SHOT_PIN_USER_SLACK 1

// ---- Tray -------------------------------------------------------------------
#define SHOT_TRAY_ID       1
#define SHOT_TRAY_CB       (WM_APP + 10)
#define IDM_CAPTURE        1001
#define IDM_EXIT           1002
#define IDM_CLOSE_ALL_PINS 1003
#define IDM_OPEN_SHOTS     1004

// ---- Pin context menu -------------------------------------------------------
// Deliberately a separate id range from the tray's. TrackPopupMenu sends
// WM_COMMAND to the PIN's WndProc so they cannot actually collide, but keeping
// the ranges disjoint means a mis-routed command can never silently trigger the
// wrong action.
#define IDM_PIN_COPY       2001
#define IDM_PIN_SAVEAS     2002
#define IDM_PIN_CLOSE      2003
#define IDM_PIN_OPEN_SHOTS 2004
