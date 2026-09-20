// pin.h — pinned images: floating always-on-top windows showing a captured region.
#pragma once
#include <windows.h>
#include "capture.h"

// Creates a pin centred on the desktop, scaled down if the capture is too big
// to sit comfortably on screen.
//
// TAKES OWNERSHIP of *bm: on return *bm is ALWAYS zeroed, whether this
// succeeded or failed, so the caller may unconditionally FreeBitmap32(bm).
// Never copies pixels — the Bitmap32 becomes the pin's single DIB.
bool PinCreate(HINSTANCE hInst, HWND hOwner, Bitmap32* bm);

// Destroys every live pin. Safe to call when there are none, and safe to call
// repeatedly. Refuses while a pin-owned modal dialog is up.
void CloseAllPins(void);

// Live pin count.
int PinCount(void);

// True while a modal dialog owned by a pin is running. Any path that would
// destroy a pin (tray quit, close-all, the pin's own close gesture) MUST refuse
// while this is true: GetSaveFileNameW runs a modal loop that dispatches
// messages to every window on the thread, so the user can otherwise free a Pin
// out from under the code that is about to dereference it.
bool PinModalActive(void);
