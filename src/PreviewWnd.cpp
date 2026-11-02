#include "PreviewWnd.h"

#include "BitmapCodec.h"
#include "TextUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>

namespace peres::ui {

namespace {

constexpr COLORREF kBackground = RGB(248, 249, 251);
constexpr COLORREF kTitleColor = RGB(24, 32, 48);
constexpr COLORREF kSubtitleColor = RGB(92, 101, 116);
constexpr COLORREF kBodyColor = RGB(118, 126, 140);
constexpr COLORREF kAccentColor = RGB(37, 99, 235);

void DrawTextLine(HDC dc, HFONT font, COLORREF color, const RECT& rect, const std::wstring& text,
                  UINT format) {
    ::SelectObject(dc, font);
    ::SetTextColor(dc, color);
    ::SetBkMode(dc, TRANSPARENT);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), const_cast<RECT*>(&rect),
                format | DT_NOPREFIX);
}

int MeasureMenuText(HDC dc, HFONT font, const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    ::SelectObject(dc, font);
    SIZE size{};
    ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

bool MenuItemDisabled(const MenuItem& item, MenuTemplateFormat format) {
    if (format == MenuTemplateFormat::Classic) {
        return (item.classicFlags & 0x0003) != 0;
    }
    return (item.state & 0x00000003) != 0;
}

bool MenuItemChecked(const MenuItem& item, MenuTemplateFormat format) {
    if (format == MenuTemplateFormat::Classic) {
        return (item.classicFlags & 0x0008) != 0;
    }
    return (item.state & 0x00000008) != 0;
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

void DrawFrame(HDC dc, const RECT& rect, COLORREF border) {
    HBRUSH brush = ::CreateSolidBrush(border);
    ::FrameRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

}  // namespace

CPreviewWnd::~CPreviewWnd() {
    ResetImages();
    if (m_titleFont != nullptr) {
        ::DeleteObject(m_titleFont);
    }
    if (m_subtitleFont != nullptr) {
        ::DeleteObject(m_subtitleFont);
    }
    if (m_bodyFont != nullptr) {
        ::DeleteObject(m_bodyFont);
    }
}

int CPreviewWnd::Scale(int value) const {
    return ::MulDiv(value, static_cast<int>(m_dpi), 96);
}

void CPreviewWnd::ResetImages() {
    if (m_icon != nullptr) {
        ::DestroyIcon(m_icon);
        m_icon = nullptr;
    }
    if (m_bitmap != nullptr) {
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    m_bitmapWidth = 0;
    m_bitmapHeight = 0;
    m_bitmapAlpha = false;
    m_menuPreview.reset();
    m_dialogPreview.reset();
}

void CPreviewWnd::EnsureFonts(UINT dpi) {
    if (dpi == 0) {
        dpi = 96;
    }
    if (m_titleFont != nullptr && m_dpi == dpi) {
        return;
    }
    m_dpi = dpi;
    if (m_titleFont != nullptr) {
        ::DeleteObject(m_titleFont);
        m_titleFont = nullptr;
    }
    if (m_subtitleFont != nullptr) {
        ::DeleteObject(m_subtitleFont);
        m_subtitleFont = nullptr;
    }
    if (m_bodyFont != nullptr) {
        ::DeleteObject(m_bodyFont);
        m_bodyFont = nullptr;
    }

    const auto makeFont = [this](int pointSize, int weight) {
        return ::CreateFontW(-::MulDiv(pointSize, static_cast<int>(m_dpi), 72), 0, 0, 0, weight,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    m_titleFont = makeFont(12, FW_SEMIBOLD);
    m_subtitleFont = makeFont(9, FW_NORMAL);
    m_bodyFont = makeFont(10, FW_NORMAL);
}

HICON CPreviewWnd::LoadIconFromMemory(std::span<const uint8_t> bytes) const {
    if (bytes.empty()) {
        return nullptr;
    }
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath) == 0) {
        return nullptr;
    }
    wchar_t tempFile[MAX_PATH] = {};
    if (::GetTempFileNameW(tempPath, L"pex", 0, tempFile) == 0) {
        return nullptr;
    }
    {
        std::ofstream file(tempFile, std::ios::binary | std::ios::trunc);
        if (!file) {
            ::DeleteFileW(tempFile);
            return nullptr;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            ::DeleteFileW(tempFile);
            return nullptr;
        }
    }

    const int size = Scale(96);
    HICON icon = static_cast<HICON>(
        ::LoadImageW(nullptr, tempFile, IMAGE_ICON, size, size, LR_LOADFROMFILE));
    if (icon == nullptr) {
        icon = static_cast<HICON>(
            ::LoadImageW(nullptr, tempFile, IMAGE_ICON, 0, 0, LR_LOADFROMFILE));
    }
    ::DeleteFileW(tempFile);
    return icon;
}

HBITMAP CPreviewWnd::LoadBitmapFromDib(std::span<const uint8_t> dib, uint32_t& width,
                                       uint32_t& height, bool& alpha) const {
    auto info = InspectBitmapDib(dib);
    if (!info) {
        return nullptr;
    }
    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) {
        return nullptr;
    }
    void* bits = nullptr;
    BITMAPINFO* bitmapInfo = reinterpret_cast<BITMAPINFO*>(const_cast<uint8_t*>(dib.data()));
    HBITMAP bitmap = ::CreateDIBSection(screen, bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
        return nullptr;
    }
    const uint64_t rowBytes = (static_cast<uint64_t>(info->width) * info->bitCount + 31ull) / 32ull * 4ull;
    const uint64_t expectedBytes = rowBytes * info->height;
    const size_t available = dib.size() - info->bitsOffset;
    if (expectedBytes > available) {
        ::DeleteObject(bitmap);
        return nullptr;
    }
    std::memcpy(bits, dib.data() + info->bitsOffset, static_cast<size_t>(expectedBytes));
    width = info->width;
    height = info->height;
    // RT_BITMAP has no reliable alpha convention; draw it opaque.
    alpha = false;
    return bitmap;
}

HBITMAP CPreviewWnd::LoadBitmapFromImage(const DecodedImage& image, uint32_t& width,
                                         uint32_t& height) const {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(image.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(image.height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(image.pixels.size());

    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) {
        return nullptr;
    }
    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
        return nullptr;
    }
    std::memcpy(bits, image.pixels.data(), image.pixels.size());
    width = image.width;
    height = image.height;
    return bitmap;
}

void CPreviewWnd::ShowEmpty() {
    ResetImages();
    m_kind = Kind::Empty;
    m_title.clear();
    m_subtitle.clear();
    m_body.clear();
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void CPreviewWnd::ShowMessage(std::wstring title, std::wstring subtitle, std::wstring body) {
    ResetImages();
    m_kind = Kind::Message;
    m_title = std::move(title);
    m_subtitle = std::move(subtitle);
    m_body = std::move(body);
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void CPreviewWnd::ShowIcon(std::vector<uint8_t> icoData, std::wstring title,
                           std::wstring subtitle) {
    ResetImages();
    m_icon = LoadIconFromMemory(icoData);
    m_title = std::move(title);
    m_subtitle = std::move(subtitle);
    m_body.clear();
    if (m_icon == nullptr) {
        m_kind = Kind::Message;
        m_body = L"无法创建图标预览。";
    } else {
        m_kind = Kind::Icon;
    }
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void CPreviewWnd::ShowBitmap(std::span<const uint8_t> dib, std::wstring title,
                             std::wstring subtitle) {
    ResetImages();
    m_bitmap = LoadBitmapFromDib(dib, m_bitmapWidth, m_bitmapHeight, m_bitmapAlpha);
    m_title = std::move(title);
    m_subtitle = std::move(subtitle);
    m_body.clear();
    if (m_bitmap == nullptr) {
        m_kind = Kind::Message;
        m_body = L"无法创建位图预览。";
    } else {
        m_kind = Kind::Bitmap;
    }
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void CPreviewWnd::ShowImage(const DecodedImage& image, std::wstring title,
                            std::wstring subtitle) {
    ResetImages();
    m_bitmap = LoadBitmapFromImage(image, m_bitmapWidth, m_bitmapHeight);
    m_bitmapAlpha = true;
    m_title = std::move(title);
    m_subtitle = std::move(subtitle);
    m_body.clear();
    if (m_bitmap == nullptr) {
        m_kind = Kind::Message;
        m_body = L"无法创建图像预览。";
    } else {
        m_kind = Kind::Bitmap;
    }
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void CPreviewWnd::ShowMenu(const MenuResource& menu, std::wstring title,
                           std::wstring subtitle) {
    ResetImages();
    m_kind = Kind::Menu;
    m_title = std::move(title);
    m_subtitle = std::move(subtitle);
    m_menuPreview = menu;
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void CPreviewWnd::ShowDialog(const DialogTemplate& dialog, std::wstring title,
                             std::wstring subtitle) {
    ResetImages();
    m_kind = Kind::Dialog;
    m_title = std::move(title);
    m_subtitle = std::move(subtitle);
    m_dialogPreview = dialog;
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void CPreviewWnd::DrawMenuPreview(HDC dc, const RECT& content) {
    if (!m_menuPreview) {
        return;
    }
    const MenuResource& menu = *m_menuPreview;
    FillSolid(dc, content, kBackground);
    if (content.bottom <= content.top || content.right <= content.left) {
        return;
    }

    const int padding = Scale(8);
    const int barHeight = std::min(Scale(28), std::max(0, static_cast<int>(content.bottom - content.top)));
    RECT bar{content.left, content.top, content.right, content.top + barHeight};
    FillSolid(dc, bar, RGB(240, 243, 248));
    DrawFrame(dc, bar, RGB(196, 203, 214));

    struct TopItem {
        const MenuItem* item = nullptr;
        RECT itemRect{};
    };
    std::vector<TopItem> tops;
    int x = bar.left + Scale(5);
    bool highlighted = false;
    for (const MenuItem& item : menu.items) {
        if (item.separator) {
            continue;
        }
        const int textWidth = MeasureMenuText(dc, m_bodyFont, item.text);
        const int width = std::min(Scale(220), std::max(Scale(52), textWidth + Scale(22)));
        if (x + width > bar.right) {
            break;
        }
        RECT itemRect{x, bar.top + Scale(2), x + width, bar.bottom - Scale(2)};
        if (item.popup && !highlighted) {
            FillSolid(dc, itemRect, RGB(224, 236, 255));
            highlighted = true;
        }
        DrawTextLine(dc, m_bodyFont, RGB(30, 36, 48), itemRect, item.text,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        tops.push_back(TopItem{&item, itemRect});
        x += width + Scale(3);
    }

    const int itemHeight = Scale(24);
    const int separatorHeight = Scale(10);
    const int indentStep = Scale(18);

    const std::function<int(const std::vector<MenuItem>&, int)> measureWidth =
        [&](const std::vector<MenuItem>& items, int depth) -> int {
        int widest = Scale(120);
        for (const MenuItem& item : items) {
            if (item.separator) {
                widest = std::max(widest, Scale(80));
                continue;
            }
            int width = Scale(26) + depth * indentStep +
                        MeasureMenuText(dc, m_bodyFont, item.text);
            if (item.popup) {
                width += Scale(20);
            } else if (item.id != 0) {
                width += MeasureMenuText(dc, m_bodyFont, std::to_wstring(item.id)) + Scale(22);
            }
            widest = std::max(widest, width);
            if (item.popup && !item.children.empty()) {
                widest = std::max(widest, measureWidth(item.children, depth + 1));
            }
        }
        return widest;
    };

    const std::function<int(const std::vector<MenuItem>&)> measureHeight =
        [&](const std::vector<MenuItem>& items) -> int {
        int height = 0;
        for (const MenuItem& item : items) {
            height += item.separator ? separatorHeight : itemHeight;
            if (item.popup && !item.children.empty()) {
                height += measureHeight(item.children);
            }
        }
        return height;
    };

    int panelY = bar.bottom + Scale(5);
    int lastRight = content.left;
    for (const TopItem& top : tops) {
        if (!top.item->popup) {
            continue;
        }
        int width = measureWidth(top.item->children, 0) + Scale(12);
        width = std::min(width, std::max(Scale(120), static_cast<int>(content.right - content.left) -
                                                    Scale(6)));
        const int height = measureHeight(top.item->children) + Scale(10);
        int panelX = top.itemRect.left;
        panelX = std::max(panelX, lastRight + Scale(2));
        if (panelX + width > content.right - Scale(2)) {
            panelX = content.right - Scale(2) - width;
        }
        if (panelX < content.left + Scale(2)) {
            panelX = content.left + Scale(2);
        }
        lastRight = panelX + width;

        RECT shadow{panelX + Scale(2), panelY + Scale(2), panelX + width + Scale(2),
                    panelY + height + Scale(2)};
        FillSolid(dc, shadow, RGB(210, 214, 222));
        RECT panel{panelX, panelY, panelX + width, panelY + height};
        FillSolid(dc, panel, RGB(255, 255, 255));
        DrawFrame(dc, panel, RGB(158, 166, 180));

        int saved = ::SaveDC(dc);
        ::IntersectClipRect(dc, panel.left, panel.top, panel.right, panel.bottom);

        const std::function<void(const std::vector<MenuItem>&, int, int&)> drawItems =
            [&](const std::vector<MenuItem>& items, int depth, int& y) {
            for (const MenuItem& item : items) {
                if (y >= panel.bottom) {
                    return;
                }
                if (item.separator) {
                    const int lineY = y + separatorHeight / 2;
                    HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(210, 214, 220));
                    HGDIOBJ old = ::SelectObject(dc, pen);
                    ::MoveToEx(dc, panel.left + Scale(8), lineY, nullptr);
                    ::LineTo(dc, panel.right - Scale(8), lineY);
                    ::SelectObject(dc, old);
                    ::DeleteObject(pen);
                    y += separatorHeight;
                    continue;
                }

                const bool disabled = MenuItemDisabled(item, menu.format);
                const bool checked = MenuItemChecked(item, menu.format);
                const COLORREF color = disabled ? RGB(156, 161, 170) : RGB(28, 34, 44);
                const int checkWidth = checked ? Scale(16) : 0;
                const int textLeft = panel.left + Scale(10) + depth * indentStep + checkWidth;
                const std::wstring idText =
                    (!item.popup && item.id != 0) ? std::to_wstring(item.id) : L"";
                const int idWidth = idText.empty() ? 0 : MeasureMenuText(dc, m_bodyFont, idText);
                const int rightReserve = item.popup ? Scale(20) : (idWidth + Scale(12));

                RECT textRect{textLeft, y, panel.right - Scale(8) - rightReserve, y + itemHeight};
                if (checked) {
                    HPEN pen = ::CreatePen(PS_SOLID, 1, disabled ? RGB(156, 161, 170)
                                                                 : RGB(22, 128, 72));
                    HGDIOBJ old = ::SelectObject(dc, pen);
                    const int bx = panel.left + Scale(9) + depth * indentStep;
                    const int by = y + itemHeight / 2;
                    ::MoveToEx(dc, bx, by, nullptr);
                    ::LineTo(dc, bx + Scale(4), by + Scale(5));
                    ::LineTo(dc, bx + Scale(12), by - Scale(5));
                    ::SelectObject(dc, old);
                    ::DeleteObject(pen);
                }
                DrawTextLine(dc, m_bodyFont, color, textRect, item.text,
                             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                if (!idText.empty()) {
                    RECT idRect{panel.right - Scale(8) - idWidth, y, panel.right - Scale(8),
                                y + itemHeight};
                    DrawTextLine(dc, m_bodyFont, RGB(128, 134, 145), idRect, idText,
                                 DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                }
                if (item.popup && !item.children.empty()) {
                    RECT arrowRect{panel.right - Scale(18), y, panel.right - Scale(5),
                                   y + itemHeight};
                    DrawTextLine(dc, m_bodyFont, color, arrowRect, L"›",
                                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                }
                y += itemHeight;
                if (item.popup && !item.children.empty()) {
                    drawItems(item.children, depth + 1, y);
                }
            }
        };

        int y = panel.top + Scale(5);
        drawItems(top.item->children, 0, y);
        ::RestoreDC(dc, saved);
        panelY += height + Scale(8);
    }

    if (tops.empty() || !highlighted) {
        RECT hint{content.left + padding, bar.bottom + Scale(12), content.right - padding,
                  content.bottom - padding};
        DrawTextLine(dc, m_bodyFont, kBodyColor, hint,
                     L"该菜单没有可显示的弹出子菜单。", DT_LEFT | DT_WORDBREAK);
    }
}

void CPreviewWnd::DrawDialogPreview(HDC dc, const RECT& content) {
    if (!m_dialogPreview) {
        return;
    }
    const DialogTemplate& dialog = *m_dialogPreview;
    FillSolid(dc, content, kBackground);
    if (content.bottom <= content.top || content.right <= content.left) {
        return;
    }

    const int padding = Scale(10);
    const int dialogWidth = std::max<int>(20, dialog.cx);
    const int dialogHeight = std::max<int>(20, dialog.cy);
    const bool hasCaption = (dialog.style & 0x00C00000u) != 0;
    const int captionHeight = hasCaption ? Scale(26) : 0;
    const int border = Scale(1);
    const int availableWidth =
        std::max(1, static_cast<int>(content.right - content.left) - padding * 2);
    const int availableHeight =
        std::max(1, static_cast<int>(content.bottom - content.top) - padding * 2 - captionHeight);

    double scale = std::min(static_cast<double>(availableWidth) / dialogWidth,
                            static_cast<double>(availableHeight) / dialogHeight);
    scale = std::min(scale, 4.0);
    if (scale < 0.2) {
        scale = 0.2;
    }

    const int frameWidth = static_cast<int>(std::lround(dialogWidth * scale)) + border * 2;
    const int frameHeight = static_cast<int>(std::lround(dialogHeight * scale)) + border * 2 +
                            captionHeight;
    const int frameX = content.left + (content.right - content.left - frameWidth) / 2;
    const int frameY = content.top + (content.bottom - content.top - frameHeight) / 2;

    RECT shadow{frameX + Scale(3), frameY + Scale(3), frameX + frameWidth + Scale(3),
                frameY + frameHeight + Scale(3)};
    FillSolid(dc, shadow, RGB(208, 212, 220));
    RECT frame{frameX, frameY, frameX + frameWidth, frameY + frameHeight};
    FillSolid(dc, frame, RGB(204, 208, 214));
    DrawFrame(dc, frame, RGB(130, 136, 146));

    RECT client{frameX + border, frameY + border, frameX + frameWidth - border,
                frameY + frameHeight - border};
    if (hasCaption) {
        RECT caption{client.left, client.top, client.right, client.top + captionHeight};
        FillSolid(dc, caption, RGB(36, 74, 138));
        std::wstring captionText = dialog.title.ordinal
                                       ? L"#" + std::to_wstring(dialog.title.ordinalValue)
                                       : dialog.title.text;
        RECT textRect{caption.left + Scale(8), caption.top, caption.right - Scale(28),
                      caption.bottom};
        DrawTextLine(dc, m_subtitleFont, RGB(255, 255, 255), textRect, captionText,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if ((dialog.style & 0x00080000u) != 0) {
            RECT closeRect{caption.right - Scale(22), caption.top + Scale(5),
                           caption.right - Scale(8), caption.bottom - Scale(5)};
            DrawTextLine(dc, m_bodyFont, RGB(255, 255, 255), closeRect, L"×",
                         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        client.top = caption.bottom;
    }
    FillSolid(dc, client, RGB(240, 241, 243));

    int saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);

    const auto mapX = [&](int value) {
        return client.left + static_cast<int>(std::lround(value * scale));
    };
    const auto mapY = [&](int value) {
        return client.top + static_cast<int>(std::lround(value * scale));
    };

    for (const DialogControl& control : dialog.controls) {
        RECT rect{mapX(control.x), mapY(control.y), mapX(control.x + control.cx),
                  mapY(control.y + control.cy)};
        if (rect.right <= client.left || rect.bottom <= client.top ||
            rect.left >= client.right || rect.top >= client.bottom) {
            continue;
        }

        const bool disabled = (control.style & 0x08000000u) != 0;
        const COLORREF textColor = disabled ? RGB(150, 155, 164) : RGB(30, 36, 46);
        std::wstring text = control.title.ordinal ? L"" : control.title.text;
        const int atom = control.windowClass.ordinal
                             ? static_cast<int>(control.windowClass.ordinalValue)
                             : -1;
        std::wstring className;
        if (!control.windowClass.ordinal) {
            className = ToLowerAscii(control.windowClass.text);
        }

        const auto isClass = [&](int expectedAtom, std::wstring_view expectedName) {
            return atom == expectedAtom || className == expectedName;
        };

        if (isClass(0x0080, L"button")) {
            const int type = static_cast<int>(control.style & 0x0Fu);
            if (type == 7) {
                DrawFrame(dc, rect, RGB(130, 136, 146));
                RECT labelRect{rect.left + Scale(6), rect.top - Scale(7),
                               rect.right - Scale(6), rect.top + Scale(8)};
                FillSolid(dc, labelRect, RGB(240, 241, 243));
                DrawTextLine(dc, m_bodyFont, textColor, labelRect, text,
                             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            } else if (type == 2 || type == 3 || type == 5 || type == 6) {
                const int box = std::min(
                    Scale(14),
                    std::max(1, static_cast<int>(rect.bottom - rect.top) - Scale(4)));
                RECT check{rect.left + Scale(3), rect.top + (rect.bottom - rect.top - box) / 2,
                           rect.left + Scale(3) + box, rect.top + (rect.bottom - rect.top + box) / 2};
                FillSolid(dc, check, RGB(255, 255, 255));
                DrawFrame(dc, check, RGB(120, 126, 138));
                RECT label{check.right + Scale(5), rect.top, rect.right, rect.bottom};
                DrawTextLine(dc, m_bodyFont, textColor, label, text,
                             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            } else if (type == 4 || type == 9) {
                const int box = std::min(
                    Scale(14),
                    std::max(1, static_cast<int>(rect.bottom - rect.top) - Scale(4)));
                const int left = rect.left + Scale(3);
                const int top = rect.top + (rect.bottom - rect.top - box) / 2;
                HBRUSH hollow = static_cast<HBRUSH>(::GetStockObject(HOLLOW_BRUSH));
                HGDIOBJ oldBrush = ::SelectObject(dc, hollow);
                HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(120, 126, 138));
                HGDIOBJ oldPen = ::SelectObject(dc, pen);
                ::Ellipse(dc, left, top, left + box, top + box);
                ::SelectObject(dc, oldPen);
                ::SelectObject(dc, oldBrush);
                ::DeleteObject(pen);
                RECT label{left + box + Scale(5), rect.top, rect.right, rect.bottom};
                DrawTextLine(dc, m_bodyFont, textColor, label, text,
                             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            } else {
                FillSolid(dc, rect, RGB(250, 250, 252));
                DrawFrame(dc, rect, type == 1 ? RGB(70, 88, 120) : RGB(140, 146, 158));
                RECT label{rect.left + Scale(4), rect.top, rect.right - Scale(4), rect.bottom};
                DrawTextLine(dc, m_bodyFont, textColor, label, text,
                             DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        } else if (isClass(0x0081, L"edit")) {
            FillSolid(dc, rect, RGB(255, 255, 255));
            DrawFrame(dc, rect, RGB(130, 136, 148));
            RECT label{rect.left + Scale(4), rect.top, rect.right - Scale(4), rect.bottom};
            DrawTextLine(dc, m_bodyFont, textColor, label, text,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else if (isClass(0x0082, L"static")) {
            const UINT alignment = (control.style & 0x00000001u)
                                       ? DT_CENTER
                                       : ((control.style & 0x00000002u) ? DT_RIGHT : DT_LEFT);
            DrawTextLine(dc, m_bodyFont, textColor, rect, text,
                         alignment | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else if (isClass(0x0083, L"listbox")) {
            FillSolid(dc, rect, RGB(255, 255, 255));
            DrawFrame(dc, rect, RGB(130, 136, 148));
            RECT label{rect.left + Scale(4), rect.top, rect.right - Scale(4), rect.bottom};
            DrawTextLine(dc, m_bodyFont, textColor, label, text,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else if (isClass(0x0084, L"scrollbar")) {
            FillSolid(dc, rect, RGB(225, 228, 234));
            DrawFrame(dc, rect, RGB(150, 156, 166));
        } else if (isClass(0x0085, L"combobox")) {
            FillSolid(dc, rect, RGB(255, 255, 255));
            DrawFrame(dc, rect, RGB(130, 136, 148));
            RECT label{rect.left + Scale(4), rect.top, rect.right - Scale(20), rect.bottom};
            DrawTextLine(dc, m_bodyFont, textColor, label, text,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            RECT arrowRect{rect.right - Scale(18), rect.top + Scale(1), rect.right - Scale(2),
                           rect.bottom - Scale(1)};
            FillSolid(dc, arrowRect, RGB(238, 240, 244));
            DrawTextLine(dc, m_bodyFont, RGB(70, 76, 88), arrowRect, L"▾",
                         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            FillSolid(dc, rect, RGB(232, 236, 242));
            DrawFrame(dc, rect, RGB(140, 146, 158));
            RECT label{rect.left + Scale(4), rect.top, rect.right - Scale(4), rect.bottom};
            DrawTextLine(dc, m_bodyFont, textColor, label, text,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
    ::RestoreDC(dc, saved);
}

LRESULT CPreviewWnd::OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) {
    return 1;
}

LRESULT CPreviewWnd::OnSize(UINT, WPARAM, LPARAM, BOOL&) {
    ::InvalidateRect(m_hWnd, nullptr, FALSE);
    return 0;
}

LRESULT CPreviewWnd::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
    CPaintDC dc(m_hWnd);
    RECT client{};
    ::GetClientRect(m_hWnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return 0;
    }

    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP buffer = ::CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ oldBuffer = ::SelectObject(mem, buffer);
    HBRUSH background = ::CreateSolidBrush(kBackground);
    ::FillRect(mem, &client, background);
    ::DeleteObject(background);

    EnsureFonts(::GetDpiForWindow(m_hWnd));
    const int padding = Scale(20);

    if (m_kind == Kind::Empty) {
        RECT area = client;
        area.top = std::max<int>(0, height / 2 - Scale(52));
        area.bottom = std::min<int>(height, area.top + Scale(100));
        DrawTextLine(mem, m_titleFont, kTitleColor, area, L"eXe Res",
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        area.top += Scale(44);
        DrawTextLine(mem, m_bodyFont, kBodyColor, area,
                     L"打开 EXE / DLL 文件以浏览资源", DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        area.top += Scale(26);
        DrawTextLine(mem, m_subtitleFont, kSubtitleColor, area,
                     L"支持 32/64 位 PE · 可替换图标 / 位图 / 清单", DT_CENTER | DT_SINGLELINE |
                                                                      DT_VCENTER);
        ::BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, oldBuffer);
        ::DeleteObject(buffer);
        ::DeleteDC(mem);
        return 0;
    }

    RECT titleRect{padding, padding, width - padding, height};
    titleRect.bottom = titleRect.top + Scale(28);
    DrawTextLine(mem, m_titleFont, kTitleColor, titleRect, m_title,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT subtitleRect{padding, titleRect.bottom + Scale(2), width - padding, height};
    subtitleRect.bottom = subtitleRect.top + Scale(22);
    DrawTextLine(mem, m_subtitleFont, kSubtitleColor, subtitleRect, m_subtitle,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    const int separatorY = subtitleRect.bottom + Scale(10);
    RECT accent{padding, separatorY, padding + Scale(28), separatorY + Scale(2)};
    HBRUSH accentBrush = ::CreateSolidBrush(kAccentColor);
    ::FillRect(mem, &accent, accentBrush);
    ::DeleteObject(accentBrush);

    RECT content{padding, separatorY + Scale(14), width - padding, height - padding};
    if (content.bottom <= content.top || content.right <= content.left) {
        ::BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, oldBuffer);
        ::DeleteObject(buffer);
        ::DeleteDC(mem);
        return 0;
    }

    if (m_kind == Kind::Menu) {
        DrawMenuPreview(mem, content);
    } else if (m_kind == Kind::Dialog) {
        DrawDialogPreview(mem, content);
    } else if (m_kind == Kind::Icon && m_icon != nullptr) {
        int iconSize = std::min(Scale(128), std::min(static_cast<int>(content.right - content.left),
                                                    static_cast<int>(content.bottom - content.top)));
        iconSize = std::max(iconSize, Scale(32));
        const int x = (content.left + content.right - iconSize) / 2;
        const int y = (content.top + content.bottom - iconSize) / 2;
        ::DrawIconEx(mem, x, y, m_icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    } else if (m_kind == Kind::Bitmap && m_bitmap != nullptr) {
        const double availableWidth = static_cast<double>(content.right - content.left);
        const double availableHeight = static_cast<double>(content.bottom - content.top);
        const double fit = std::min<double>(
            std::min(availableWidth / std::max<uint32_t>(m_bitmapWidth, 1),
                     availableHeight / std::max<uint32_t>(m_bitmapHeight, 1)),
            4.0);
        const int drawWidth = std::max<int>(1, static_cast<int>(m_bitmapWidth * fit));
        const int drawHeight = std::max<int>(1, static_cast<int>(m_bitmapHeight * fit));
        const int x = (content.left + content.right - drawWidth) / 2;
        const int y = (content.top + content.bottom - drawHeight) / 2;

        HDC source = ::CreateCompatibleDC(mem);
        HGDIOBJ oldBitmap = ::SelectObject(source, m_bitmap);
        ::SetStretchBltMode(mem, HALFTONE);
        ::SetBrushOrgEx(mem, 0, 0, nullptr);
        if (m_bitmapAlpha) {
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            ::AlphaBlend(mem, x, y, drawWidth, drawHeight, source, 0, 0, m_bitmapWidth,
                         m_bitmapHeight, blend);
        } else {
            ::StretchBlt(mem, x, y, drawWidth, drawHeight, source, 0, 0, m_bitmapWidth,
                         m_bitmapHeight, SRCCOPY);
        }
        ::SelectObject(source, oldBitmap);
        ::DeleteDC(source);
    } else {
        RECT message = content;
        DrawTextLine(mem, m_bodyFont, kBodyColor, message,
                     m_body.empty() ? L"无图形预览，资源内容显示在下方。" : m_body,
                     DT_CENTER | DT_WORDBREAK | DT_VCENTER);
    }

    ::BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, oldBuffer);
    ::DeleteObject(buffer);
    ::DeleteDC(mem);
    return 0;
}

}  // namespace peres::ui
