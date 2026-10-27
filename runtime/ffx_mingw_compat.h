#pragma once

// FidelityFX 1.1.4's Windows message helper calls the MSVC secure-CRT
// spelling. MinGW/Clang does not expose that spelling consistently to C++.
// Include the standard declaration first, then provide the source-compatible
// alias. Keeping this outside third_party avoids modifying the FSR submodule.
#if defined(_WIN32) && !defined(_MSC_VER)
#include <wchar.h>
#include <new>
#include <math.h>
#ifndef swprintf_s
#define swprintf_s swprintf
#endif
#endif
