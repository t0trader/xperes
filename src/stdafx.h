#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define _WIN32_IE 0x0A00

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include "resource.h"

#include <atlbase.h>

// WTL 9.10's atlgdi.h refers to the CDCHandle/CDC/CBitmap/... aliases inside
// template bodies before the typedefs at the bottom of the header.  Providing
// the forward declarations here keeps C++23 conformance mode happy.
namespace WTL {
template <bool t_bManaged> class CDCT;
template <bool t_bManaged> class CBitmapT;
template <bool t_bManaged> class CBrushT;
template <bool t_bManaged> class CPenT;
template <bool t_bManaged> class CFontT;
template <bool t_bManaged> class CRgnT;
typedef CDCT<false> CDCHandle;
typedef CDCT<true> CDC;
typedef CBitmapT<false> CBitmapHandle;
typedef CBitmapT<true> CBitmap;
typedef CBrushT<false> CBrushHandle;
typedef CBrushT<true> CBrush;
typedef CPenT<false> CPenHandle;
typedef CPenT<true> CPen;
typedef CFontT<false> CFontHandle;
typedef CFontT<true> CFont;
typedef CRgnT<false> CRgnHandle;
typedef CRgnT<true> CRgn;
}  // namespace WTL

#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlframe.h>
#include <atlctrls.h>
#include <atlctrlx.h>
#include <atldlgs.h>
#include <atlgdi.h>
#include <atlmisc.h>
#include <atlscrl.h>
#include <atlsplit.h>

#include <fmt/format.h>
