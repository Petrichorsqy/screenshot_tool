// apputil.h — process-level utilities shared by the resident and capture paths.
// These have no capture semantics; they live here so main.cpp and pin.cpp can
// both use them without duplicating definitions.
#pragma once
#include <windows.h>

// Hand the working set back to the OS.
void TrimWorkingSet(void);

// Log GDI/USER handle counts under `tag`. Read with DebugView or the VS
// Output window.
void TraceGuiObjects(const wchar_t* tag);

// Modal error box with an HRESULT rendered in hex.
void ReportError(const wchar_t* what, HRESULT hr);
