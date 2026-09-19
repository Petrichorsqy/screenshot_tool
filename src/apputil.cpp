#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <strsafe.h>

#include "apputil.h"

void TrimWorkingSet(void)
{
    // The -1,-1 pair is the documented "trim as hard as you can" case and must
    // be cast explicitly to SIZE_T or it sign-extends badly.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

void TraceGuiObjects(const wchar_t* tag)
{
    wchar_t buf[160];
    const DWORD gdi  = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (SUCCEEDED(StringCchPrintfW(buf, ARRAYSIZE(buf),
            L"[shot] %s GDI=%lu USER=%lu\n", tag, gdi, user)))
        OutputDebugStringW(buf);
}

void ReportError(const wchar_t* what, HRESULT hr)
{
    wchar_t buf[320];
    StringCchPrintfW(buf, ARRAYSIZE(buf),
        L"%s\n\nHRESULT = 0x%08lX", what, (unsigned long)hr);
    MessageBoxW(nullptr, buf, L"截图工具", MB_ICONERROR | MB_OK);
}
