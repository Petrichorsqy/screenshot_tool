// overlay.h — the region-selector overlay.
#pragma once
#include <windows.h>

// Creates and shows a full-virtual-screen dimmed overlay. Non-blocking.
// On completion the overlay posts WM_APP_REGION_SELECTED (lParam = const RECT*
// in virtual-screen pixels) or WM_APP_REGION_CANCELLED to hNotify, then
// destroys itself. Returns false if an overlay is already up.
bool StartRegionSelect(HINSTANCE hInst, HWND hNotify);

// Destroys the overlay if present. Called at shutdown so a mid-selection quit
// cannot leave a fullscreen window behind.
void AbortRegionSelect();
