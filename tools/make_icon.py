#!/usr/bin/env python3
"""Generate resources/app.ico with a background "xPEx" watermark and the
foreground text lines "eXe" and "Res".

Only the Windows GDI (through ctypes) and the Python standard library are used,
so Pillow is not required.

Usage:
    python tools/make_icon.py [--out resources/app.ico]
"""

from __future__ import annotations

import argparse
import ctypes
import os
import struct
import sys
from ctypes import wintypes

if os.name != "nt":
    raise SystemExit("make_icon.py requires Windows GDI")

gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)

DIB_RGB_COLORS = 0
BI_RGB = 0
FW_BOLD = 700
DEFAULT_CHARSET = 1
OUT_TT_PRECIS = 4
CLIP_DEFAULT_PRECIS = 0
ANTIALIASED_QUALITY = 4
DEFAULT_PITCH = 0
FF_SWISS = 0x20
DT_CENTER = 0x0001
DT_VCENTER = 0x0004
DT_SINGLELINE = 0x0020
DT_NOPREFIX = 0x0800
TRANSPARENT = 1
SRCCOPY = 0x00CC0020

HBITMAP = wintypes.HANDLE
HFONT = wintypes.HANDLE
HGDIOBJ = wintypes.HANDLE


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class RGBQUAD(ctypes.Structure):
    _fields_ = [
        ("rgbBlue", wintypes.BYTE),
        ("rgbGreen", wintypes.BYTE),
        ("rgbRed", wintypes.BYTE),
        ("rgbReserved", wintypes.BYTE),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", RGBQUAD * 1)]


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", wintypes.LONG),
        ("top", wintypes.LONG),
        ("right", wintypes.LONG),
        ("bottom", wintypes.LONG),
    ]


gdi32.CreateDIBSection.restype = HBITMAP
gdi32.CreateDIBSection.argtypes = [
    wintypes.HDC,
    ctypes.POINTER(BITMAPINFO),
    wintypes.DWORD,
    ctypes.POINTER(ctypes.c_void_p),
    wintypes.HANDLE,
    wintypes.DWORD,
]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.SelectObject.restype = HGDIOBJ
gdi32.SelectObject.argtypes = [wintypes.HDC, HGDIOBJ]
gdi32.DeleteObject.argtypes = [HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.CreateFontW.restype = HFONT
gdi32.CreateFontW.argtypes = [
    wintypes.LONG, wintypes.LONG, wintypes.LONG, wintypes.LONG,
    wintypes.LONG, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
    wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
    wintypes.DWORD, wintypes.LPCWSTR,
]
gdi32.SetTextColor.restype = wintypes.DWORD
gdi32.SetTextColor.argtypes = [wintypes.HDC, wintypes.DWORD]
gdi32.SetBkMode.argtypes = [wintypes.HDC, wintypes.LONG]
gdi32.GdiFlush.argtypes = []
user32.DrawTextW.argtypes = [
    wintypes.HDC, wintypes.LPCWSTR, ctypes.c_int, ctypes.POINTER(RECT), wintypes.UINT,
]
user32.GetDC.restype = wintypes.HDC
user32.GetDC.argtypes = [wintypes.HWND]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]


def lerp(a: int, b: int, t: float) -> int:
    return int(a + (b - a) * t + 0.5)


def rgb(bgr: int) -> tuple[int, int, int]:
    return bgr & 0xFF, (bgr >> 8) & 0xFF, (bgr >> 16) & 0xFF


def render(size: int) -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """Render one icon and return (ARG-less BGRA rows, palette) metadata."""
    hdc_screen = user32.GetDC(None)
    hdc = gdi32.CreateCompatibleDC(hdc_screen)
    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = size
    bmi.bmiHeader.biHeight = -size  # top-down: first row is the top row
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = BI_RGB
    bits = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(hdc, ctypes.byref(bmi), DIB_RGB_COLORS,
                                  ctypes.byref(bits), None, 0)
    if not hbmp or not bits:
        raise OSError("CreateDIBSection failed")
    gdi32.SelectObject(hdc, hbmp)

    # Draw a dark blue gradient with rounded corners directly into the DIB bits.
    # Text is drawn into the same DIB afterwards, otherwise GDI output and the
    # Python-side pixel buffer would be two different surfaces.
    radius = max(2, int(size * 0.16))
    pixel_count = size * size
    base = bits.value
    top = (24, 32, 52)       # B, G, R
    bottom = (150, 72, 28)   # B, G, R (blue-ish in BGR)
    for y in range(size):
        t = y / max(1, size - 1)
        color = (
            lerp(top[0], bottom[0], t),
            lerp(top[1], bottom[1], t),
            lerp(top[2], bottom[2], t),
        )
        for x in range(size):
            # Rounded rectangle coverage. The corner test is exact enough.
            inside = True
            cx = min(x, size - 1 - x)
            cy = min(y, size - 1 - y)
            if cx < radius and cy < radius:
                dx = radius - cx
                dy = radius - cy
                inside = dx * dx + dy * dy <= radius * radius
            idx = (y * size + x) * 4
            if inside:
                # Slight vertical highlight towards the top.
                v = 1.0 - 0.10 * (y / max(1, size - 1))
                c = (
                    min(255, int(color[0] * v)),
                    min(255, int(color[1] * v)),
                    min(255, int(color[2] * v)),
                    255,
                )
            else:
                c = (0, 0, 0, 0)
            ctypes.memset(base + idx, 0, 4)
            ctypes.c_ubyte.from_address(base + idx + 0).value = c[0]
            ctypes.c_ubyte.from_address(base + idx + 1).value = c[1]
            ctypes.c_ubyte.from_address(base + idx + 2).value = c[2]
            ctypes.c_ubyte.from_address(base + idx + 3).value = c[3]

    # Draw the text with GDI. Alpha is restored to opaque inside the shape later.
    font_height = max(5, int(size * 0.31))
    font = gdi32.CreateFontW(
        font_height, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI",
    )
    if not font:
        font = gdi32.CreateFontW(
            font_height, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial",
        )
    if not font:
        raise OSError("CreateFontW failed")
    gdi32.SelectObject(hdc, font)
    gdi32.SetBkMode(hdc, TRANSPARENT)

    margin = max(1, int(size * 0.06))
    top_rect = RECT(margin, margin, size - margin, int(size * 0.55))
    bot_rect = RECT(margin, int(size * 0.47), size - margin, size - margin)

    # Large, low-contrast "xPEx" watermark painted behind the two foreground
    # lines. It is skipped on 16/24 px icons where it would only turn into noise.
    if size >= 32:
        watermark_height = max(8, int(size * 0.40))
        watermark_font = gdi32.CreateFontW(
            watermark_height, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI",
        )
        if watermark_font:
            previous_font = gdi32.SelectObject(hdc, watermark_font)
            watermark_rect = RECT(margin, int(size * 0.20), size - margin,
                                  int(size * 0.80))
            gdi32.SetTextColor(hdc, 0x00A88C78)  # muted slate blue
            user32.DrawTextW(hdc, "xPEx", -1, ctypes.byref(watermark_rect),
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX)
            gdi32.SelectObject(hdc, previous_font)
            gdi32.DeleteObject(watermark_font)

    gdi32.SelectObject(hdc, font)
    gdi32.SetTextColor(hdc, 0x00FFFFFF)  # COLORREF: white
    user32.DrawTextW(hdc, "eXe", -1, ctypes.byref(top_rect),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX)
    gdi32.SetTextColor(hdc, 0x007DD3FC)  # warm gold (BGR)
    user32.DrawTextW(hdc, "Res", -1, ctypes.byref(bot_rect),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX)
    gdi32.GdiFlush()

    pixels = ctypes.string_at(bits, pixel_count * 4)

    gdi32.SelectObject(hdc, hbmp)
    gdi32.DeleteObject(font)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc)
    user32.ReleaseDC(None, hdc_screen)

    # Restore alpha: inside the rounded rectangle -> opaque, outside -> transparent.
    data = bytearray(pixels)
    for y in range(size):
        for x in range(size):
            idx = (y * size + x) * 4
            cx = min(x, size - 1 - x)
            cy = min(y, size - 1 - y)
            inside = True
            if cx < radius and cy < radius:
                dx = radius - cx
                dy = radius - cy
                inside = dx * dx + dy * dy <= radius * radius
            data[idx + 3] = 255 if inside else 0
            if not inside:
                data[idx + 0] = 0
                data[idx + 1] = 0
                data[idx + 2] = 0
    return bytes(data), []


def ico_image_bytes(size: int, pixels_bgra: bytes) -> bytes:
    """Build the BMP payload used inside an .ico file (32bpp XOR + AND mask)."""
    # ICO BMP rows are bottom-up.
    rows = []
    stride = size * 4
    for y in range(size - 1, -1, -1):
        rows.append(pixels_bgra[y * stride:(y + 1) * stride])
    xor_bits = b"".join(rows)
    and_stride = ((size + 31) // 32) * 4
    and_bits = b"\x00" * (and_stride * size)

    header = struct.pack(
        "<IiiHHIIiiII",
        40,                # biSize
        size,              # biWidth
        size * 2,          # biHeight (XOR + AND)
        1,                 # biPlanes
        32,                # biBitCount
        BI_RGB,            # biCompression
        len(xor_bits) + len(and_bits),
        0, 0, 0, 0,
    )
    return header + xor_bits + and_bits


def write_ico(path: str, sizes: list[int]) -> None:
    images = []
    for size in sizes:
        pixels, _ = render(size)
        images.append((size, ico_image_bytes(size, pixels)))

    total_entries = 6 + 16 * len(images)
    offset = (total_entries + 3) & ~3
    dir_entries = []
    payloads = []
    for size, data in images:
        dir_entries.append(
            struct.pack(
                "<BBBBHHII",
                0 if size >= 256 else size,
                0 if size >= 256 else size,
                0,    # colors
                0,    # reserved
                1,    # planes
                32,   # bit count
                len(data),
                offset,
            )
        )
        payloads.append((offset, data))
        offset += len(data)
        offset = (offset + 3) & ~3

    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    out += b"".join(dir_entries)
    out += b"\x00" * (((len(out) + 3) & ~3) - len(out))
    for off, data in payloads:
        out += b"\x00" * (off - len(out))
        out += data
    with open(path, "wb") as f:
        f.write(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=os.path.join("resources", "app.ico"))
    args = parser.parse_args()
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    sizes = [16, 32, 64]
    write_ico(args.out, sizes)
    print(f"generated {args.out} ({os.path.getsize(args.out)} bytes, {len(sizes)} images)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
