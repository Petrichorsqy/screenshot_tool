// capture.h — the capture path. Everything here runs to completion and then
// releases; nothing is cached or kept warm.
#pragma once
#include <windows.h>

// A top-down 32bpp BI_RGB DIB plus the memory DC it is selected into.
// Owns exactly three GDI objects: hdc, hbm and the previous bitmap hbmOld.
struct Bitmap32 {
    HDC     hdc    = nullptr;   // CreateCompatibleDC(screen DC)
    HBITMAP hbm    = nullptr;   // CreateDIBSection, biHeight negative (top-down)
    HGDIOBJ hbmOld = nullptr;   // result of SelectObject(hdc, hbm)
    void*   bits   = nullptr;   // raw DIB bits; stride = w * 4
    int     w = 0, h = 0, stride = 0;
};

// BitBlt from the DISPLAY DC into a fresh Bitmap32 exactly rc's size.
// rc is in virtual-screen PHYSICAL pixels and may have negative left/top.
bool GrabScreenRect(const RECT& rcVirtualPx, Bitmap32* out);

// Restores, deletes and zeroes. Safe on a zeroed struct.
void FreeBitmap32(Bitmap32* bm);

// WIC PNG encode straight from bm.bits. Requires COM to be initialised.
HRESULT EncodePng(const Bitmap32& bm, const wchar_t* path);

// CoInitializeEx(APARTMENTTHREADED) / EncodePng / CoUninitialize. COM is
// deliberately not held resident by the process; this pair is the only place
// it exists. Lives here rather than in pin.cpp to keep every COM/DIB/WIC
// pairing inside this one auditable file.
HRESULT SavePngWithCom(const Bitmap32& bm, const wchar_t* path);

// Builds <exe dir>\shots\Screenshot_yyyyMMdd_HHmmss.png, creating the directory
// if needed and disambiguating with _2, _3... if the name is taken.
bool BuildPngPath(wchar_t* out, size_t cch);

// <exe dir>\shots\ , created if absent. Includes the trailing separator.
bool BuildShotsDir(wchar_t* out, size_t cch);

// Opens the shots folder in Explorer, creating it first if it does not exist.
bool OpenShotsFolder(void);

// CF_DIB (24bpp, bottom-up, positive biHeight) with an OpenClipboard retry.
bool CopyBitmapToClipboard(HWND owner, const Bitmap32& bm);
