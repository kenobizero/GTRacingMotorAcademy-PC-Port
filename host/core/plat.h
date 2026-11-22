/* plat.h -- the host's platform header: <windows.h> on Windows, the POSIX stand-in (posixwin.h) on Android.

   The host was written for 32-bit Windows.  The Android port (armeabi-v7a, a 32-bit process like the Windows
   one, so guest and host pointers stay the same 32-bit words) keeps the same sources: the Win32 calls they make
   are given POSIX meanings in posixwin.h, and what has no meaning there (crash dumps, WinHTTP, GDI+, pixel
   formats) is #ifdef'd in its own file.

   GT_SEP / GT_SEP_S: the host's path separator ('\\' on Windows, '/' elsewhere). */
#ifndef GT_PLAT_H
#define GT_PLAT_H

#ifdef _WIN32
#include <windows.h>
#define GT_SEP '\\'
#define GT_SEP_S "\\"
#define GT_ANDROID 0
#else
#include "posixwin.h"
#define GT_SEP '/'
#define GT_SEP_S "/"
#ifdef __ANDROID__
#define GT_ANDROID 1
#else
#define GT_ANDROID 0
#endif
#endif

#endif
