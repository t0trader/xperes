#pragma once

#include "stdafx.h"

#include "DialogCodec.h"
#include "ImageCodec.h"
#include "MenuCodec.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace peres::ui {

class CPreviewWnd : public CWindowImpl<CPreviewWnd> {
public:
    DECLARE_WND_CLASS(L"PeresPreviewWnd")

    BEGIN_MSG_MAP(CPreviewWnd)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
    END_MSG_MAP()

    ~CPreviewWnd();

    void ShowEmpty();
    void ShowMessage(std::wstring title, std::wstring subtitle, std::wstring body);
    void ShowIcon(std::vector<uint8_t> icoData, std::wstring title, std::wstring subtitle);
    void ShowBitmap(std::span<const uint8_t> dib, std::wstring title, std::wstring subtitle);
    void ShowImage(const DecodedImage& image, std::wstring title, std::wstring subtitle);
    void ShowMenu(const MenuResource& menu, std::wstring title, std::wstring subtitle);
    void ShowDialog(const DialogTemplate& dialog, std::wstring title, std::wstring subtitle);

private:
    enum class Kind { Empty, Message, Icon, Bitmap, Menu, Dialog };

    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);

    void ResetImages();
    void EnsureFonts(UINT dpi);
    HICON LoadIconFromMemory(std::span<const uint8_t> bytes) const;
    HBITMAP LoadBitmapFromDib(std::span<const uint8_t> dib, uint32_t& width, uint32_t& height,
                              bool& alpha) const;
    HBITMAP LoadBitmapFromImage(const DecodedImage& image, uint32_t& width, uint32_t& height) const;
    void DrawMenuPreview(HDC dc, const RECT& content);
    void DrawDialogPreview(HDC dc, const RECT& content);
    int Scale(int value) const;

    Kind m_kind = Kind::Empty;
    HICON m_icon = nullptr;
    HBITMAP m_bitmap = nullptr;
    uint32_t m_bitmapWidth = 0;
    uint32_t m_bitmapHeight = 0;
    bool m_bitmapAlpha = false;
    std::wstring m_title;
    std::wstring m_subtitle;
    std::wstring m_body;
    std::optional<MenuResource> m_menuPreview;
    std::optional<DialogTemplate> m_dialogPreview;

    HFONT m_titleFont = nullptr;
    HFONT m_subtitleFont = nullptr;
    HFONT m_bodyFont = nullptr;
    UINT m_dpi = 96;
};

}  // namespace peres::ui
