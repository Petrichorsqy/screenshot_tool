// capture.cpp — the capture path.
//
// This is the only file in the program that can leak: every CreateDC/DeleteDC,
// CreateDIBSection/DeleteObject, OpenClipboard/CloseClipboard and COM Release
// pair lives here, so auditing for leaks means reading exactly one file.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <strsafe.h>
#include <shellapi.h>   // ShellExecuteW, for OpenShotsFolder

#include "capture.h"

// -----------------------------------------------------------------------------
// Minimal COM smart pointer. A hand-written Release() cascade leaks on every
// early-return path, and there are ten of them below.
// -----------------------------------------------------------------------------
template <class T>
class ComPtr {
public:
    ComPtr() : p_(nullptr) {}
    ~ComPtr() { if (p_) p_->Release(); }
    T**  operator&()       { return &p_; }
    T*   operator->() const { return p_; }
    T*   get() const       { return p_; }
private:
    T* p_;
    ComPtr(const ComPtr&);
    ComPtr& operator=(const ComPtr&);
};

// -----------------------------------------------------------------------------
bool GrabScreenRect(const RECT& rc, Bitmap32* out)
{
    ZeroMemory(out, sizeof(*out));

    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    // The DISPLAY DC is unambiguous about covering every monitor; GetDC(NULL)
    // is not. It must be released with DeleteDC, not ReleaseDC.
    HDC hScreen = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (!hScreen) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // negative => top-down, matching PNG row order
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC     hdc  = CreateCompatibleDC(hScreen);
    void*   bits = nullptr;
    HBITMAP hbm  = CreateDIBSection(hScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);

    if (hdc && hbm && bits) {
        // CreateDIBSection hands back bits but installs them nowhere: without
        // this SelectObject the BitBlt below draws into a 1x1 monochrome stub.
        HGDIOBJ old = SelectObject(hdc, hbm);
        if (old && old != HGDI_ERROR) {
            if (BitBlt(hdc, 0, 0, w, h, hScreen, rc.left, rc.top, SRCCOPY)) {
                out->hdc    = hdc;
                out->hbm    = hbm;
                out->hbmOld = old;
                out->bits   = bits;
                out->w = w;
                out->h = h;
                out->stride = w * 4;
                DeleteDC(hScreen);
                return true;
            }
            SelectObject(hdc, old);
        }
        DeleteObject(hbm);
    }
    if (hdc) DeleteDC(hdc);
    DeleteDC(hScreen);
    return false;
}

// -----------------------------------------------------------------------------
void FreeBitmap32(Bitmap32* bm)
{
    // Order matters. A DIB still selected into a DC cannot be freed, and
    // DeleteObject fails silently when you try.
    if (bm->hdc && bm->hbmOld) SelectObject(bm->hdc, bm->hbmOld);
    if (bm->hbm) DeleteObject(bm->hbm);
    if (bm->hdc) DeleteDC(bm->hdc);
    ZeroMemory(bm, sizeof(*bm));
}

// -----------------------------------------------------------------------------
HRESULT EncodePng(const Bitmap32& bm, const wchar_t* path)
{
    ComPtr<IWICImagingFactory>    factory;
    ComPtr<IWICBitmap>            bitmap;
    ComPtr<IWICStream>            stream;
    ComPtr<IWICBitmapEncoder>     encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         props;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));

    if (SUCCEEDED(hr))
        // 32bppBGR, NOT 32bppBGRA. A 32bpp BI_RGB DIB's fourth byte is always
        // zero, so encoding as BGRA writes a completely transparent PNG. The
        // 0xFF memset "fix" does not work: BitBlt overwrites those bytes.
        hr = factory->CreateBitmapFromMemory((UINT)bm.w, (UINT)bm.h,
                                             GUID_WICPixelFormat32bppBGR,
                                             (UINT)bm.stride,
                                             (UINT)(bm.stride * bm.h),
                                             (BYTE*)bm.bits, &bitmap);

    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.get());
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)bm.w, (UINT)bm.h);
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGR;
        hr = frame->SetPixelFormat(&fmt);
    }
    if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    return hr;
}

// -----------------------------------------------------------------------------
HRESULT SavePngWithCom(const Bitmap32& bm, const wchar_t* path)
{
    // COM is brought up only for the encode and torn down after. Leaving it
    // resident would keep the whole COM stack paged in for a program that
    // otherwise sits idle in the tray.
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrInit)) return hrInit;   // uninitialised => must NOT CoUninitialize
    const HRESULT hr = EncodePng(bm, path);
    CoUninitialize();
    return hr;
}

// -----------------------------------------------------------------------------
bool BuildShotsDir(wchar_t* out, size_t cch)
{
    wchar_t exePath[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
    if (n == 0 || n >= ARRAYSIZE(exePath)) return false;

    // Strip the file name, keeping the trailing backslash. Done by hand rather
    // than with wcsrchr to keep the CRT's wide-string machinery out of the link.
    size_t len = 0;
    while (len < ARRAYSIZE(exePath) && exePath[len] != L'\0') ++len;
    size_t cut = len;
    while (cut > 0 && exePath[cut - 1] != L'\\') --cut;
    if (cut == 0) return false;
    exePath[cut] = L'\0';

    if (FAILED(StringCchPrintfW(out, cch, L"%sshots\\", exePath)))
        return false;

    if (!CreateDirectoryW(out, nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return false;
    }
    return true;
}

bool OpenShotsFolder(void)
{
    wchar_t dir[MAX_PATH];
    if (!BuildShotsDir(dir, ARRAYSIZE(dir))) return false;

    // Trim the trailing separator before handing it to the shell: a path ending
    // in '\' is ambiguous about whether it names a folder or a drive root.
    size_t len = 0;
    while (len < ARRAYSIZE(dir) && dir[len] != L'\0') ++len;
    if (len > 0 && dir[len - 1] == L'\\') dir[len - 1] = L'\0';

    const HINSTANCE r = ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr,
                                      SW_SHOWNORMAL);
    return (INT_PTR)r > 32;   // <= 32 is a failure code, per the docs
}

// -----------------------------------------------------------------------------
bool BuildPngPath(wchar_t* out, size_t cch)
{
    wchar_t dir[MAX_PATH];
    if (!BuildShotsDir(dir, ARRAYSIZE(dir)))
        return false;

    SYSTEMTIME st;
    GetLocalTime(&st);   // local, not UTC: the user's mental model is wall-clock

    for (int i = 1; i <= 99; ++i) {
        wchar_t name[64];
        HRESULT hr;
        if (i == 1) {
            hr = StringCchPrintfW(name, ARRAYSIZE(name),
                    L"Screenshot_%04u%02u%02u_%02u%02u%02u.png",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        } else {
            hr = StringCchPrintfW(name, ARRAYSIZE(name),
                    L"Screenshot_%04u%02u%02u_%02u%02u%02u_%d.png",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, i);
        }
        if (FAILED(hr)) return false;
        if (FAILED(StringCchPrintfW(out, cch, L"%s%s", dir, name)))
            return false;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES)
            return true;   // name is free
    }
    return false;          // 99 captures inside one second is not a real scenario
}

// -----------------------------------------------------------------------------
bool CopyBitmapToClipboard(HWND owner, const Bitmap32& bm)
{
    if (bm.w <= 0 || bm.h <= 0 || !bm.bits) return false;

    // 24bpp, not 32bpp: a 32bpp CF_DIB carries a fourth byte that every reader
    // interprets as alpha, and ours is always zero, so it would paste as fully
    // transparent. 24bpp has no alpha semantics and Paint/Word/Chrome/WeChat/
    // Photoshop all accept it. The system synthesises CF_BITMAP on demand, so
    // there is no need to set that format too.
    const int    dstStride = ((bm.w * 3) + 3) & ~3;
    const SIZE_T pixBytes  = (SIZE_T)dstStride * (SIZE_T)bm.h;
    const SIZE_T total     = sizeof(BITMAPINFOHEADER) + pixBytes;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!hMem) return false;

    BYTE* dst = (BYTE*)GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        return false;
    }

    BITMAPINFOHEADER* hdr = (BITMAPINFOHEADER*)dst;
    hdr->biSize          = sizeof(BITMAPINFOHEADER);
    hdr->biWidth         = bm.w;
    hdr->biHeight        = bm.h;   // POSITIVE = bottom-up, the opposite of our DIB
    hdr->biPlanes        = 1;
    hdr->biBitCount      = 24;
    hdr->biCompression   = BI_RGB;
    hdr->biSizeImage     = (DWORD)pixBytes;
    hdr->biXPelsPerMeter = 0;
    hdr->biYPelsPerMeter = 0;
    hdr->biClrUsed       = 0;
    hdr->biClrImportant  = 0;

    BYTE* rows = dst + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < bm.h; ++y) {
        // Source is top-down, destination bottom-up: walk the rows backwards.
        const BYTE* src = (const BYTE*)bm.bits + (SIZE_T)y * (SIZE_T)bm.stride;
        BYTE*       out = rows + (SIZE_T)(bm.h - 1 - y) * (SIZE_T)dstStride;
        for (int x = 0; x < bm.w; ++x) {
            out[x * 3 + 0] = src[x * 4 + 0];   // B
            out[x * 3 + 1] = src[x * 4 + 1];   // G
            out[x * 3 + 2] = src[x * 4 + 2];   // R  (4th byte dropped)
        }
    }
    GlobalUnlock(hMem);

    // Another process may hold the clipboard — clipboard managers and IME
    // helpers are watching right after a screenshot, so this fails for real.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(owner)) {
            bool set = false;
            if (EmptyClipboard())
                set = SetClipboardData(CF_DIB, hMem) != nullptr;
            CloseClipboard();       // paired with the OpenClipboard above
            if (set) return true;   // ownership moved to the clipboard: do NOT free
            break;                  // we held it and the set failed; retry is pointless
        }
        Sleep(20);
    }

    GlobalFree(hMem);               // only ever freed when the set did not happen
    return false;
}
