#include "MainFrame.h"

#include "BitmapCodec.h"
#include "Config.h"
#include "DialogCodec.h"
#include "IconCodec.h"
#include "ImageCodec.h"
#include "MenuCodec.h"
#include "ResourceTypes.h"
#include "StringTableCodec.h"
#include "TextUtil.h"
#include "VersionCodec.h"
#include "UpXTool.h"
#include "XmlFormat.h"

#include <fmt/format.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <span>

namespace peres::ui {

namespace {

constexpr wchar_t kPeFilter[] =
    L"PE 文件 (*.exe;*.dll;*.ocx;*.sys)\0*.exe;*.dll;*.ocx;*.sys\0所有文件 (*.*)\0*.*\0\0";
constexpr wchar_t kIcoFilter[] =
    L"图标文件 (*.ico)\0*.ico\0所有文件 (*.*)\0*.*\0\0";
constexpr wchar_t kBmpFilter[] =
    L"位图文件 (*.bmp)\0*.bmp\0所有文件 (*.*)\0*.*\0\0";
constexpr wchar_t kImageFilter[] =
    L"图像文件 (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.ico)\0"
    L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.ico\0所有文件 (*.*)\0*.*\0\0";
constexpr wchar_t kManifestFilter[] =
    L"清单文件 (*.xml;*.manifest;*.txt)\0*.xml;*.manifest;*.txt\0所有文件 (*.*)\0*.*\0\0";
constexpr wchar_t kAllFilter[] = L"所有文件 (*.*)\0*.*\0\0";

std::wstring FormatWin32Error(DWORD code) {
    wchar_t buffer[512] = {};
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                          nullptr, code, 0, buffer,
                                          static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring message;
    if (length > 0) {
        message.assign(buffer, length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
    }
    if (message.empty()) {
        message = L"未知错误";
    }
    return Utf8ToWide(fmt::format("{} (0x{:08X})", WideToUtf8(message), code));
}

std::wstring SanitizeComponent(std::wstring_view raw) {
    std::wstring result;
    result.reserve(std::min<size_t>(raw.size(), 80));
    for (const wchar_t ch : raw) {
        if (result.size() >= 80) {
            break;
        }
        const bool invalid = ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
                             ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' ||
                             ch == L'|' || ch < 0x20;
        result.push_back(invalid ? L'_' : ch);
    }
    while (!result.empty() && (std::iswspace(result.back()) != 0 || result.back() == L'.')) {
        result.pop_back();
    }
    if (result.empty()) {
        result = L"resource";
    }
    return result;
}

std::wstring BuildHexDump(std::span<const uint8_t> data) {
    constexpr size_t kLimit = 256 * 1024;
    const size_t count = std::min(data.size(), kLimit);
    std::string output;
    output.reserve(count * 5);
    for (size_t offset = 0; offset < count; offset += 16) {
        output += fmt::format("{:08X}  ", offset);
        for (size_t i = 0; i < 16; ++i) {
            if (offset + i < count) {
                output += fmt::format("{:02X} ", data[offset + i]);
            } else {
                output += "   ";
            }
            if (i == 7) {
                output.push_back(' ');
            }
        }
        output += "  ";
        for (size_t i = 0; i < 16 && offset + i < count; ++i) {
            const uint8_t value = data[offset + i];
            output.push_back(value >= 0x20 && value < 0x7F ? static_cast<char>(value) : '.');
        }
        output.push_back('\n');
    }
    if (data.size() > kLimit) {
        output += fmt::format("\n仅显示前 {} 字节，共 {} 字节。\n", kLimit, data.size());
    }
    return Utf8ToWide(output);
}

std::wstring DecodeStringTable(std::span<const uint8_t> data, uint16_t blockId) {
    std::wstring output;
    size_t offset = 0;
    for (uint32_t index = 0; index < 16 && offset + 2 <= data.size(); ++index) {
        const uint16_t length = static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
        offset += 2;
        if (length == 0) {
            continue;
        }
        if (offset + static_cast<size_t>(length) * 2 > data.size()) {
            output += L"（字符串表数据损坏）\r\n";
            break;
        }
        std::wstring text;
        text.reserve(length);
        for (uint16_t i = 0; i < length; ++i) {
            const uint16_t ch = static_cast<uint16_t>(
                data[offset + i * 2] | (data[offset + i * 2 + 1] << 8));
            text.push_back(static_cast<wchar_t>(ch));
        }
        offset += static_cast<size_t>(length) * 2;
        const uint64_t stringId =
            blockId > 0 ? static_cast<uint64_t>(blockId - 1) * 16ull + index : index;
        output += L"#" + std::to_wstring(stringId) + L": " + text + L"\r\n";
    }
    if (output.empty()) {
        output = L"（字符串表为空）";
    }
    return output;
}

std::wstring GroupDetails(const std::vector<GroupIconEntry>& entries, size_t dataSize) {
    std::string text = fmt::format("图标组：{} 个图像，资源数据 {}。\r\n\r\n",
                                   entries.size(), WideToUtf8(HumanSize(dataSize)));
    for (const auto& entry : entries) {
        const uint32_t width = entry.width == 0 ? 256u : entry.width;
        const uint32_t height = entry.height == 0 ? 256u : entry.height;
        text += fmt::format("#{:<6} {:>4} × {:<4} {:>2} bpp  {}\r\n", entry.id, width, height,
                            entry.bitCount, WideToUtf8(HumanSize(entry.bytesInRes)));
    }
    return Utf8ToWide(text);
}

std::wstring IconDetails(const IconImageInfo& info, size_t dataSize) {
    return Utf8ToWide(fmt::format("图标图像\r\n\r\n尺寸：{} × {}\r\n位深：{} bpp\r\n格式：{}\r\n数据大小：{}\r\n",
                                  info.width, info.height, info.bitCount,
                                  info.png ? "PNG" : "DIB", WideToUtf8(HumanSize(dataSize))));
}

std::wstring BitmapInfoDetails(const BitmapDibInfo& info, size_t dataSize) {
    const std::string compression =
        info.compression == 0 ? std::string("BI_RGB") : std::to_string(info.compression);
    return Utf8ToWide(fmt::format(
        "位图（DIB）\r\n\r\n尺寸：{} × {}\r\n位深：{} bpp\r\n压缩：{}\r\n数据大小：{}\r\n",
        info.width, info.height, info.bitCount, compression,
        WideToUtf8(HumanSize(dataSize))));
}

std::wstring ImageDetails(const DecodedImage& image, size_t dataSize) {
    return Utf8ToWide(fmt::format(
        "图像资源\r\n\r\n格式：{}\r\n尺寸：{} × {}\r\n数据大小：{}\r\n\r\n"
        "该资源保存的是原始图像文件，可直接导入 PNG / JPEG / GIF / BMP 等文件覆盖。\r\n",
        WideToUtf8(ImageFormatName(image.format)), image.width, image.height,
        WideToUtf8(HumanSize(dataSize))));
}

bool LooksLikePe(std::span<const uint8_t> data) {
    return data.size() >= 0x40 && data[0] == 'M' && data[1] == 'Z';
}

std::wstring PeSubsystemLabel(uint32_t subsystem) {
    switch (subsystem) {
        case 1:
            return L"Native";
        case 2:
            return L"Windows GUI";
        case 3:
            return L"Windows CUI";
        case 5:
            return L"OS/2 CUI";
        case 7:
            return L"POSIX CUI";
        case 9:
            return L"Windows CE GUI";
        case 10:
            return L"EFI Application";
        case 11:
            return L"EFI Boot Service Driver";
        case 12:
            return L"EFI Runtime Driver";
        case 13:
            return L"EFI ROM";
        case 14:
            return L"Xbox";
        case 16:
            return L"Windows Boot Application";
        default:
            return L"Unknown";
    }
}

std::wstring PeSummary(const PeInfo& info) {
    std::wstring text = info.ArchitectureLabel() + L" · " + info.FileKind() + L" · " +
                        info.MachineLabel() + L" · " + std::to_wstring(info.sectionCount) +
                        L" 节";
    if (info.hasResources) {
        text += L" · 含资源";
    }
    if (info.hasSignature) {
        text += L" · 已签名";
    }
    if (info.upxPacked) {
        text += L" · UPX";
    }
    return text;
}

std::wstring PeDetails(const PeImage& image) {
    const PeInfo& info = image.Info();
    std::wstring text = L"内嵌 PE 文件\r\n\r\n";
    text += L"格式: " + info.ArchitectureLabel() + L" (" +
            std::to_wstring(info.is64 ? 64 : 32) + L" 位)\r\n";
    text += L"类型: " + info.FileKind() + L"\r\n";
    text += L"架构: " + info.MachineLabel() + L" (machine 0x" +
            Utf8ToWide(fmt::format("{:04X}", info.machine)) + L")\r\n";
    text += L"节数量: " + std::to_wstring(info.sectionCount) + L"\r\n";
    text += L"文件大小: " + HumanSize(info.fileSize) + L"\r\n";
    text += L"映像基址: 0x" + Utf8ToWide(fmt::format("{:X}", info.imageBase)) + L"\r\n";
    text += L"子系统: " + PeSubsystemLabel(info.subsystem) + L" (" +
            std::to_wstring(info.subsystem) + L")\r\n";
    text += L"包含资源: " + std::wstring(info.hasResources ? L"是" : L"否") + L"\r\n";
    text += L"数字签名: " + std::wstring(info.hasSignature ? L"是" : L"否") + L"\r\n";
    text += L"UPX: " + std::wstring(info.upxPacked ? L"是" : L"否") + L"\r\n";
    text += L"建议导出扩展名: ." + SuggestedPeExtension(info) + L"\r\n";

    const auto& sections = image.Sections();
    if (!sections.empty()) {
        text += L"\r\n节:";
        for (const auto& section : sections) {
            text += L" " + Utf8ToWide(section.name) + L"(" + HumanSize(section.rawSize) + L")";
        }
        text += L"\r\n";
    }
    return text;
}

const wchar_t* FilterForExtension(std::wstring_view extension) {
    if (IEqualsAscii(extension, L"ico")) {
        return kIcoFilter;
    }
    if (IEqualsAscii(extension, L"bmp")) {
        return kBmpFilter;
    }
    if (IEqualsAscii(extension, L"png") || IEqualsAscii(extension, L"jpg") ||
        IEqualsAscii(extension, L"jpeg") || IEqualsAscii(extension, L"gif") ||
        IEqualsAscii(extension, L"tif") || IEqualsAscii(extension, L"tiff")) {
        return kImageFilter;
    }
    if (IEqualsAscii(extension, L"xml") || IEqualsAscii(extension, L"manifest")) {
        return kManifestFilter;
    }
    if (IEqualsAscii(extension, L"exe") || IEqualsAscii(extension, L"dll") ||
        IEqualsAscii(extension, L"ocx") || IEqualsAscii(extension, L"sys")) {
        return kPeFilter;
    }
    return kAllFilter;
}

std::wstring TypeStem(const ResourceId& type) {
    if (type.named) {
        return SanitizeComponent(type.name);
    }
    switch (type.id) {
        case 2:
            return L"bitmap";
        case 3:
            return L"icon";
        case 6:
            return L"string_table";
        case 10:
            return L"rcdata";
        case 14:
            return L"icon_group";
        case 16:
            return L"version";
        case 24:
            return L"manifest";
        default:
            return L"resource_" + std::to_wstring(type.id);
    }
}

std::wstring NameStem(const ResourceId& name) {
    if (name.named && !name.name.empty()) {
        return SanitizeComponent(name.name);
    }
    return L"id" + std::to_wstring(name.id);
}

struct WaitCursorGuard {
    WaitCursorGuard() : old_(::SetCursor(::LoadCursorW(nullptr, IDC_WAIT))) {}
    ~WaitCursorGuard() { ::SetCursor(old_); }
    HCURSOR old_ = nullptr;
};

uint16_t StringTableBlockId(const ResourceEntry& entry) {
    return entry.name.named ? 0 : entry.name.id;
}

}  // namespace

CMainFrame::~CMainFrame() {
    if (m_uiFont != nullptr) {
        ::DeleteObject(m_uiFont);
    }
    if (m_monoFont != nullptr) {
        ::DeleteObject(m_monoFont);
    }
    if (m_editBrush != nullptr) {
        ::DeleteObject(m_editBrush);
    }
}

int CMainFrame::Scale(int value) const {
    UINT dpi = 96;
    if (m_hWnd != nullptr) {
        dpi = ::GetDpiForWindow(m_hWnd);
        if (dpi == 0) {
            dpi = 96;
        }
    }
    return ::MulDiv(value, static_cast<int>(dpi), 96);
}

LRESULT CMainFrame::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
    HICON appIcon =
        ::LoadIconW(_Module.GetResourceInstance(), MAKEINTRESOURCEW(IDI_APPICON));
    if (appIcon != nullptr) {
        ::SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        ::SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        m_uiFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    UINT dpi = ::GetDpiForWindow(m_hWnd);
    if (dpi == 0) {
        dpi = 96;
    }
    m_monoFont = ::CreateFontW(-::MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
                              FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_MODERN, L"Consolas");
    m_editBrush = ::CreateSolidBrush(RGB(255, 255, 255));

    if (!m_splitter.Create(m_hWnd, CWindow::rcDefault, nullptr,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)) {
        return -1;
    }
    if (!m_tree.Create(m_splitter.m_hWnd, CWindow::rcDefault, nullptr,
                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                           TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                       WS_EX_CLIENTEDGE,
                       reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_RES_TREE)))) {
        return -1;
    }
    if (!m_rightSplitter.Create(m_splitter.m_hWnd, CWindow::rcDefault, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)) {
        return -1;
    }
    m_rightSplitter.SetOrientation(false, false);
    if (!m_preview.Create(m_rightSplitter.m_hWnd, CWindow::rcDefault, nullptr,
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)) {
        return -1;
    }
    if (!m_edit.Create(m_rightSplitter.m_hWnd, CWindow::rcDefault, nullptr,
                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                           ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                       WS_EX_CLIENTEDGE,
                       reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_RES_EDIT)))) {
        return -1;
    }
    ::SendMessageW(m_edit.m_hWnd, EM_SETREADONLY, TRUE, 0);
    ::SendMessageW(m_edit.m_hWnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                   MAKELPARAM(Scale(8), Scale(8)));

    if (m_uiFont != nullptr) {
        ::SendMessageW(m_tree.m_hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_uiFont), TRUE);
    }
    if (m_monoFont != nullptr) {
        ::SendMessageW(m_edit.m_hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_monoFont), TRUE);
    }

    m_splitter.SetSplitterPanes(m_tree.m_hWnd, m_rightSplitter.m_hWnd);
    m_rightSplitter.SetSplitterPanes(m_preview.m_hWnd, m_edit.m_hWnd);
    m_hWndClient = m_splitter.m_hWnd;

    if (!CreateSimpleStatusBar(L"就绪")) {
        return -1;
    }
    m_status.Attach(m_hWndStatusBar);
    UpdateStatusParts();

    BuildMenu();
    BuildAccelerators();
    ::DragAcceptFiles(m_hWnd, TRUE);

    RECT workArea{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        const int maxWidth = std::max(Scale(780), static_cast<int>(workArea.right - workArea.left) -
                                                    Scale(32));
        const int maxHeight = std::max(Scale(540), static_cast<int>(workArea.bottom - workArea.top) -
                                                     Scale(32));
        SetWindowPos(nullptr, 0, 0, std::min(Scale(1240), maxWidth),
                     std::min(Scale(840), maxHeight), SWP_NOMOVE | SWP_NOZORDER);
    } else {
        SetWindowPos(nullptr, 0, 0, Scale(1240), Scale(840), SWP_NOMOVE | SWP_NOZORDER);
    }
    CenterWindow();
    UpdateLayout();

    UpdateTitle();
    UpdateStatusSummary();
    UpdateCommandStates();
    UpdatePreview();
    SetStatus(L"就绪 · 打开 EXE / DLL 文件以浏览资源");
    return 0;
}

LRESULT CMainFrame::OnDestroy(UINT, WPARAM, LPARAM, BOOL& bHandled) {
    if (m_accel != nullptr) {
        m_hAccel = nullptr;
        ::DestroyAcceleratorTable(m_accel);
        m_accel = nullptr;
    }
    if (m_menu != nullptr) {
        ::SetMenu(m_hWnd, nullptr);
        ::DestroyMenu(m_menu);
        m_menu = nullptr;
    }
    bHandled = FALSE;
    return 0;
}

LRESULT CMainFrame::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
    if (!QuerySaveChanges()) {
        return 0;
    }
    DestroyWindow();
    return 0;
}

LRESULT CMainFrame::OnSize(UINT, WPARAM wParam, LPARAM, BOOL&) {
    if (wParam != SIZE_MINIMIZED) {
        UpdateLayout();
        if (!m_layoutInitialized && m_splitter.IsWindow()) {
            RECT rc{};
            ::GetClientRect(m_splitter.m_hWnd, &rc);
            if (rc.right > 0) {
                m_splitter.SetSplitterPosPct(27, false);
                m_rightSplitter.SetSplitterPosPct(38, false);
                m_layoutInitialized = true;
            }
        }
        UpdateStatusParts();
    }
    return 0;
}

LRESULT CMainFrame::OnGetMinMaxInfo(UINT, WPARAM, LPARAM lParam, BOOL& bHandled) {
    auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
    info->ptMinTrackSize.x = Scale(780);
    info->ptMinTrackSize.y = Scale(540);
    bHandled = TRUE;
    return 0;
}

LRESULT CMainFrame::OnCtlColorStatic(UINT, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    if (reinterpret_cast<HWND>(lParam) == m_edit.m_hWnd) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetTextColor(dc, RGB(32, 36, 44));
        ::SetBkColor(dc, RGB(255, 255, 255));
        bHandled = TRUE;
        return reinterpret_cast<LRESULT>(m_editBrush);
    }
    bHandled = FALSE;
    return 0;
}

LRESULT CMainFrame::OnDropFiles(UINT, WPARAM wParam, LPARAM, BOOL&) {
    HDROP drop = reinterpret_cast<HDROP>(wParam);
    wchar_t path[MAX_PATH * 2] = {};
    if (::DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))) > 0) {
        OpenFile(std::filesystem::path(path));
    }
    ::DragFinish(drop);
    return 0;
}

void CMainFrame::BuildMenu() {
    if (m_menu != nullptr) {
        return;
    }
    HMENU main = ::CreateMenu();
    HMENU file = ::CreatePopupMenu();
    HMENU resource = ::CreatePopupMenu();
    HMENU tools = ::CreatePopupMenu();
    HMENU help = ::CreatePopupMenu();
    if (main == nullptr || file == nullptr || resource == nullptr || tools == nullptr ||
        help == nullptr) {
        if (main) ::DestroyMenu(main);
        if (file) ::DestroyMenu(file);
        if (resource) ::DestroyMenu(resource);
        if (tools) ::DestroyMenu(tools);
        if (help) ::DestroyMenu(help);
        return;
    }

    ::AppendMenuW(file, MF_STRING, ID_CMD_FILE_OPEN, L"打开(&O)...\tCtrl+O");
    ::AppendMenuW(file, MF_STRING, ID_CMD_FILE_SAVE, L"保存(&S)\tCtrl+S");
    ::AppendMenuW(file, MF_STRING, ID_CMD_FILE_SAVEAS, L"另存为(&A)...\tCtrl+Shift+S");
    ::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(file, MF_STRING, ID_CMD_FILE_EXIT, L"退出(&X)");

    ::AppendMenuW(resource, MF_STRING, ID_CMD_RES_EXPORT, L"导出资源(&E)...\tCtrl+E");
    ::AppendMenuW(resource, MF_STRING, ID_CMD_RES_REPLACE, L"替换资源(&R)...\tCtrl+R");
    ::AppendMenuW(resource, MF_STRING, ID_CMD_RES_DELETE, L"删除资源(&D)\tDel");
    ::AppendMenuW(resource, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(resource, MF_STRING, ID_CMD_RES_APPLY, L"应用编辑(&P)\tCtrl+Enter");
    ::AppendMenuW(resource, MF_STRING, ID_CMD_RES_DISCARD, L"放弃编辑(&L)");

    ::AppendMenuW(tools, MF_STRING, ID_CMD_TOOL_UNPACK, L"UPX 脱壳(&U)...");
    ::AppendMenuW(tools, MF_STRING, ID_CMD_TOOL_CONFIG, L"编辑配置(&C)...");

    ::AppendMenuW(help, MF_STRING, ID_CMD_HELP_ABOUT, L"关于 eXe Res(&A)\tF1");

    ::AppendMenuW(main, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件(&F)");
    ::AppendMenuW(main, MF_POPUP, reinterpret_cast<UINT_PTR>(resource), L"资源(&R)");
    ::AppendMenuW(main, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"工具(&T)");
    ::AppendMenuW(main, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助(&H)");

    m_menu = main;
    ::SetMenu(m_hWnd, m_menu);
}

void CMainFrame::BuildAccelerators() {
    ACCEL accels[] = {
        {FVIRTKEY | FCONTROL, 'O', ID_CMD_FILE_OPEN},
        {FVIRTKEY | FCONTROL, 'S', ID_CMD_FILE_SAVE},
        {FVIRTKEY | FCONTROL | FSHIFT, 'S', ID_CMD_FILE_SAVEAS},
        {FVIRTKEY | FCONTROL, 'E', ID_CMD_RES_EXPORT},
        {FVIRTKEY | FCONTROL, 'R', ID_CMD_RES_REPLACE},
        {FVIRTKEY, VK_DELETE, ID_CMD_RES_DELETE},
        {FVIRTKEY | FCONTROL, VK_RETURN, ID_CMD_RES_APPLY},
        {FVIRTKEY, VK_F1, ID_CMD_HELP_ABOUT},
    };
    m_accel = ::CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));
    m_hAccel = m_accel;
}

void CMainFrame::UpdateStatusParts() {
    if (!::IsWindow(m_hWndStatusBar)) {
        return;
    }
    RECT rc{};
    ::GetClientRect(m_hWndStatusBar, &rc);
    const int width = rc.right;
    if (width <= 0) {
        return;
    }
    int parts[2] = {std::max<int>(Scale(220), width - Scale(520)), -1};
    ::SendMessageW(m_hWndStatusBar, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
}

LRESULT CMainFrame::OnResourceApply(WORD, WORD, HWND, BOOL&) {
    if (!m_editDirty) {
        SetStatus(L"没有待应用的编辑");
        return 0;
    }
    ApplyCurrentEditor();
    return 0;
}

LRESULT CMainFrame::OnResourceDiscard(WORD, WORD, HWND, BOOL&) {
    DiscardCurrentEditor();
    return 0;
}

void CMainFrame::SetStatus(const std::wstring& text) {
    if (::IsWindow(m_hWndStatusBar)) {
        ::SendMessageW(m_hWndStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void CMainFrame::UpdateStatusSummary() {
    if (!::IsWindow(m_hWndStatusBar)) {
        return;
    }
    std::string summary;
    if (!m_pe) {
        summary = "未打开 PE 文件";
    } else {
        const auto& info = m_pe->Info();
        summary = fmt::format("{} · {} · {} · {} 节 · {} 项资源",
                              WideToUtf8(info.ArchitectureLabel()),
                              WideToUtf8(info.FileKind()),
                              WideToUtf8(info.MachineLabel()),
                              info.sectionCount,
                              m_model.Entries().size());
        if (info.upxPacked) {
            summary += " · UPX 加壳";
        }
        if (info.hasSignature) {
            summary += " · 已签名";
        }
        if (m_model.IsDirty()) {
            summary += " · 已修改";
        } else if (m_editDirty) {
            summary += " · 编辑中";
        }
    }
    const std::wstring text = Utf8ToWide(summary);
    ::SendMessageW(m_hWndStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(text.c_str()));
}

void CMainFrame::UpdateTitle() {
    std::wstring title = L"eXe Res";
    if (m_hasFile) {
        title = m_path.filename().wstring() + L" - eXe Res";
    }
    if (m_model.IsDirty() || m_editDirty) {
        title += L" *";
    }
    ::SetWindowTextW(m_hWnd, title.c_str());
}

void CMainFrame::UpdateCommandStates() {
    if (m_menu == nullptr) {
        return;
    }
    const bool hasFile = m_hasFile && m_pe.has_value();
    const bool dirty = m_model.IsDirty();
    const bool hasEntry = SelectedEntry() != nullptr;

    const auto enable = [this](UINT id, bool enabled) {
        ::EnableMenuItem(m_menu, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    enable(ID_CMD_FILE_SAVE, hasFile && (dirty || m_editDirty));
    enable(ID_CMD_FILE_SAVEAS, hasFile);
    enable(ID_CMD_RES_EXPORT, hasEntry);
    enable(ID_CMD_RES_REPLACE, hasEntry);
    enable(ID_CMD_RES_DELETE, hasEntry);
    enable(ID_CMD_RES_APPLY, m_editDirty);
    enable(ID_CMD_RES_DISCARD, m_editDirty);
    enable(ID_CMD_TOOL_UNPACK, hasFile);
    ::DrawMenuBar(m_hWnd);
}

void CMainFrame::RebuildTree(std::optional<ResourceKey> selectedKey) {
    if (!::IsWindow(m_tree.m_hWnd)) {
        return;
    }
    m_rebuilding = true;
    m_tree.SetRedraw(FALSE);
    m_tree.DeleteAllItems();
    m_nodes.clear();
    m_leafItems.clear();

    HTREEITEM typeItem = nullptr;
    HTREEITEM nameItem = nullptr;
    ResourceId currentType;
    ResourceId currentName;
    bool hasCurrent = false;
    std::vector<HTREEITEM> typeItems;

    const auto insertNode = [&](HTREEITEM parent, const std::wstring& text, TreeLevel level,
                                const ResourceId& type, const ResourceId& name,
                                uint16_t language) -> HTREEITEM {
        m_nodes.emplace_back(NodeData{level, type, name, language});
        NodeData* node = &m_nodes.back();
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<LPWSTR>(text.c_str());
        insert.item.lParam = reinterpret_cast<LPARAM>(node);
        return reinterpret_cast<HTREEITEM>(
            ::SendMessageW(m_tree.m_hWnd, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
    };

    for (const auto& entry : m_model.Entries()) {
        if (!hasCurrent || !(entry.type == currentType)) {
            currentType = entry.type;
            currentName = {};
            hasCurrent = true;
            nameItem = nullptr;
            typeItem = insertNode(nullptr, ResourceTypeLabel(entry.type), TreeLevel::Type,
                                  entry.type, {}, 0);
            if (typeItem != nullptr) {
                typeItems.push_back(typeItem);
            }
        }
        if (nameItem == nullptr || !(entry.name == currentName)) {
            currentName = entry.name;
            nameItem = insertNode(typeItem, QuoteResourceName(entry.name), TreeLevel::Name,
                                  entry.type, entry.name, 0);
        }
        const std::wstring leafText =
            LanguageLabel(entry.language) + L"  ·  " + HumanSize(entry.data.size());
        const HTREEITEM leaf =
            insertNode(nameItem, leafText, TreeLevel::Language, entry.type, entry.name,
                       entry.language);
        if (leaf != nullptr) {
            m_leafItems.emplace(entry.Key(), leaf);
        }
    }

    for (const HTREEITEM item : typeItems) {
        m_tree.Expand(item, TVE_EXPAND);
    }
    m_tree.SetRedraw(TRUE);
    ::InvalidateRect(m_tree.m_hWnd, nullptr, FALSE);
    m_rebuilding = false;

    if (selectedKey) {
        SelectLeaf(*selectedKey);
    }
    UpdatePreview();
    UpdateCommandStates();
}

void CMainFrame::SelectLeaf(const ResourceKey& key) {
    const auto it = m_leafItems.find(key);
    if (it == m_leafItems.end()) {
        return;
    }
    HTREEITEM item = it->second;
    HTREEITEM parent = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(m_tree.m_hWnd, TVM_GETNEXTITEM, TVGN_PARENT,
                       reinterpret_cast<LPARAM>(item)));
    while (parent != nullptr) {
        m_tree.Expand(parent, TVE_EXPAND);
        parent = reinterpret_cast<HTREEITEM>(
            ::SendMessageW(m_tree.m_hWnd, TVM_GETNEXTITEM, TVGN_PARENT,
                           reinterpret_cast<LPARAM>(parent)));
    }
    m_tree.SelectItem(item);
    m_tree.EnsureVisible(item);
}

CMainFrame::NodeData* CMainFrame::SelectedNode() const {
    if (!::IsWindow(m_tree.m_hWnd)) {
        return nullptr;
    }
    HTREEITEM item = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(m_tree.m_hWnd, TVM_GETNEXTITEM, TVGN_CARET, 0));
    if (item == nullptr) {
        return nullptr;
    }
    TVITEMW info{};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    if (!::SendMessageW(m_tree.m_hWnd, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&info))) {
        return nullptr;
    }
    return reinterpret_cast<NodeData*>(info.lParam);
}

const ResourceEntry* CMainFrame::SelectedEntry() const {
    const NodeData* node = SelectedNode();
    if (node == nullptr || node->level != TreeLevel::Language) {
        return nullptr;
    }
    return m_model.Find(node->type, node->name, node->language);
}

std::optional<ResourceKey> CMainFrame::MakeKey(const NodeData& node) const {
    if (node.level != TreeLevel::Language) {
        return std::nullopt;
    }
    return ResourceKey{node.type, node.name, node.language};
}

std::optional<ResourceKey> CMainFrame::SelectedLeafKey() const {
    const NodeData* node = SelectedNode();
    if (node == nullptr) {
        return std::nullopt;
    }
    return MakeKey(*node);
}

void CMainFrame::SetEditText(const std::wstring& text, bool editable,
                             std::optional<ResourceKey> key) {
    if (!::IsWindow(m_edit.m_hWnd)) {
        return;
    }
    m_editApplying = true;
    m_editDirty = false;
    m_editEditable = editable;
    m_editKey = std::move(key);
    ::SendMessageW(m_edit.m_hWnd, EM_SETREADONLY, editable ? FALSE : TRUE, 0);
    ::SendMessageW(m_edit.m_hWnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
    ::SendMessageW(m_edit.m_hWnd, EM_SETSEL, 0, 0);
    m_editApplying = false;
    UpdateCommandStates();
}

std::wstring CMainFrame::GetEditText() const {
    if (!::IsWindow(m_edit.m_hWnd)) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(m_edit.m_hWnd);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(m_edit.m_hWnd, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(copied, 0)));
    return text;
}

bool CMainFrame::ApplyCurrentEditor() {
    if (!m_editDirty) {
        return true;
    }
    if (!m_editKey) {
        ShowError(L"应用编辑", L"当前编辑内容没有对应的资源。");
        return false;
    }
    const ResourceKey key = *m_editKey;
    const ResourceEntry* entry = m_model.Find(key.type, key.name, key.language);
    if (entry == nullptr) {
        ShowError(L"应用编辑", L"资源条目已不存在。");
        m_editDirty = false;
        return false;
    }
    const std::wstring display = ResourceDisplayName(*entry);
    const std::wstring editorText = GetEditText();
    std::vector<uint8_t> bytes;

    if (IsResourceType(entry->type, ResourceType::Manifest)) {
        auto formatted = FormatXmlText(editorText);
        if (!formatted) {
            ShowError(L"XML 格式错误", formatted.error());
            return false;
        }
        auto encoded = EncodeTextPreservingEncoding(entry->data, *formatted);
        if (!encoded) {
            ShowError(L"应用编辑", encoded.error());
            return false;
        }
        bytes = std::move(*encoded);
    } else if (IsResourceType(entry->type, ResourceType::String)) {
        auto built = BuildStringTable(editorText, StringTableBlockId(*entry));
        if (!built) {
            ShowError(L"字符串表格式错误", built.error());
            return false;
        }
        bytes = std::move(*built);
    } else if (IsResourceType(entry->type, ResourceType::Menu)) {
        auto menu = ParseMenuText(editorText);
        if (!menu) {
            ShowError(L"菜单文本错误", menu.error());
            return false;
        }
        auto built = BuildMenuResource(*menu);
        if (!built) {
            ShowError(L"菜单编译失败", built.error());
            return false;
        }
        bytes = std::move(*built);
    } else if (IsResourceType(entry->type, ResourceType::Dialog)) {
        auto dialog = ParseDialogText(editorText);
        if (!dialog) {
            ShowError(L"对话框文本错误", dialog.error());
            return false;
        }
        auto built = BuildDialogResource(*dialog);
        if (!built) {
            ShowError(L"对话框编译失败", built.error());
            return false;
        }
        bytes = std::move(*built);
    } else {
        ShowInfo(L"应用编辑", L"该资源类型不支持文本编辑。");
        return false;
    }

    m_editDirty = false;
    m_model.SetData(key.type, key.name, key.language, entry->codePage, std::move(bytes));
    AfterModelChanged(key, L"编辑已应用，保存后写入 PE：" + display);
    return true;
}

void CMainFrame::DiscardCurrentEditor() {
    if (!m_editDirty) {
        SetStatus(L"没有待放弃的编辑");
        return;
    }
    m_editDirty = false;
    UpdatePreview();
    UpdateTitle();
    UpdateStatusSummary();
    UpdateCommandStates();
    SetStatus(L"已放弃当前编辑");
}

void CMainFrame::UpdatePreview() {
    const NodeData* node = SelectedNode();
    if (node == nullptr) {
        SetEditText(L"");
        if (!m_hasFile) {
            m_preview.ShowEmpty();
        } else if (m_model.Entries().empty()) {
            m_preview.ShowMessage(L"该 PE 文件没有资源", L"",
                                  L"文件已打开，但没有可浏览的资源。");
        } else {
            m_preview.ShowMessage(L"资源浏览器", L"",
                                  L"请从左侧树中选择一个资源条目。");
        }
        return;
    }

    if (node->level != TreeLevel::Language) {
        const std::wstring title = node->level == TreeLevel::Type
                                       ? ResourceTypeLabel(node->type)
                                       : QuoteResourceName(node->name);
        m_preview.ShowMessage(title, L"资源目录", L"请选择具体的语言条目以查看资源内容。");
        SetEditText(L"");
        return;
    }

    const ResourceEntry* entry = m_model.Find(node->type, node->name, node->language);
    if (entry == nullptr) {
        m_preview.ShowMessage(L"资源不存在", L"", L"资源条目已被删除。");
        SetEditText(L"");
        return;
    }

    const std::wstring title =
        ResourceTypeLabel(entry->type) + L"  ·  " + QuoteResourceName(entry->name);
    std::wstring subtitle =
        LanguageLabel(entry->language) + L"  ·  " + HumanSize(entry->data.size());

    if (IsResourceType(entry->type, ResourceType::GroupIcon)) {
        auto group = ParseGroupIcon(entry->data);
        if (!group) {
            m_preview.ShowMessage(title, subtitle, group.error());
            SetEditText(GroupDetails({}, entry->data.size()));
            return;
        }
        subtitle += Utf8ToWide(fmt::format("  ·  {} 个图像", group->size()));
        auto ico = BuildIcoFromGroup(m_model, *entry);
        if (ico) {
            m_preview.ShowIcon(std::move(*ico), title, subtitle);
        } else {
            m_preview.ShowMessage(title, subtitle, ico.error());
        }
        SetEditText(GroupDetails(*group, entry->data.size()));
        return;
    }

    if (IsResourceType(entry->type, ResourceType::Icon)) {
        auto info = InspectIconData(entry->data);
        if (!info) {
            m_preview.ShowMessage(title, subtitle, info.error());
            SetEditText(BuildHexDump(entry->data));
            return;
        }
        subtitle += Utf8ToWide(fmt::format("  ·  {} × {} {}", info->width, info->height,
                                           info->bitCount));
        auto ico = BuildIcoFromResource(entry->data);
        if (ico) {
            m_preview.ShowIcon(std::move(*ico), title, subtitle);
        } else {
            m_preview.ShowMessage(title, subtitle, ico.error());
        }
        SetEditText(IconDetails(*info, entry->data.size()));
        return;
    }

    if (IsResourceType(entry->type, ResourceType::Bitmap)) {
        const ImageFileFormat embedded = DetectImageFormat(entry->data);
        if (embedded != ImageFileFormat::Unknown) {
            auto image = DecodeImage(entry->data);
            if (image) {
                subtitle += Utf8ToWide(fmt::format("  ·  {} × {} · {}",
                                                   image->width, image->height,
                                                   WideToUtf8(ImageFormatName(image->format))));
                m_preview.ShowImage(*image, title, subtitle);
                SetEditText(ImageDetails(*image, entry->data.size()));
            } else {
                m_preview.ShowMessage(title, subtitle, image.error());
                SetEditText(BuildHexDump(entry->data));
            }
            return;
        }
        auto info = InspectBitmapDib(entry->data);
        if (!info) {
            m_preview.ShowMessage(title, subtitle, info.error());
            SetEditText(BuildHexDump(entry->data));
            return;
        }
        subtitle += Utf8ToWide(fmt::format("  ·  {} × {} · {} bpp", info->width, info->height,
                                           info->bitCount));
        m_preview.ShowBitmap(entry->data, title, subtitle);
        SetEditText(BitmapInfoDetails(*info, entry->data.size()));
        return;
    }

    if (IsResourceType(entry->type, ResourceType::String)) {
        auto text = FormatStringTable(entry->data, StringTableBlockId(*entry));
        if (!text) {
            m_preview.ShowMessage(title, subtitle, text.error());
            SetEditText(BuildHexDump(entry->data));
            return;
        }
        m_preview.ShowMessage(
            title, subtitle,
            L"字符串表已按 UTF-16 解析，可在下方编辑（Ctrl+Enter 应用）。");
        SetEditText(*text, true, entry->Key());
        return;
    }

    if (IsResourceType(entry->type, ResourceType::Menu)) {
        auto menu = ParseMenuResource(entry->data);
        if (!menu) {
            m_preview.ShowMessage(title, subtitle, menu.error());
            SetEditText(BuildHexDump(entry->data));
            return;
        }
        const std::wstring text = FormatMenuText(*menu);
        m_preview.ShowMenu(*menu, title, subtitle);
        SetEditText(text, true, entry->Key());
        return;
    }

    if (IsResourceType(entry->type, ResourceType::Dialog)) {
        auto dialog = ParseDialogResource(entry->data);
        if (!dialog) {
            m_preview.ShowMessage(title, subtitle, dialog.error());
            SetEditText(BuildHexDump(entry->data));
            return;
        }
        const std::wstring text = FormatDialogText(*dialog);
        m_preview.ShowDialog(*dialog, title, subtitle);
        SetEditText(text, true, entry->Key());
        return;
    }

    if (IsResourceType(entry->type, ResourceType::Version)) {
        auto version = ParseVersionResource(entry->data);
        if (!version) {
            m_preview.ShowMessage(title, subtitle, version.error());
            SetEditText(BuildHexDump(entry->data));
            return;
        }
        std::wstring body = L"版本信息已解析，完整内容显示在下方。";
        if (version->fixed.present) {
            body = L"FileVersion " +
                   FormatVersionDwords(version->fixed.fileVersionMs,
                                       version->fixed.fileVersionLs) +
                   L"   ·   ProductVersion " +
                   FormatVersionDwords(version->fixed.productVersionMs,
                                       version->fixed.productVersionLs);
        }
        m_preview.ShowMessage(title, subtitle, std::move(body));
        SetEditText(FormatVersionText(*version));
        return;
    }

    if (LooksLikePe(entry->data)) {
        auto nested = PeImage::LoadFromBytes(entry->data);
        if (nested) {
            const PeInfo& info = nested->Info();
            std::wstring body = L"内嵌 PE 文件：" + PeSummary(info) +
                                L"\r\n建议导出扩展名：." + SuggestedPeExtension(info);
            m_preview.ShowMessage(title, subtitle, std::move(body));
            SetEditText(PeDetails(*nested));
            return;
        }
    }

    const ImageFileFormat detected = DetectImageFormat(entry->data);
    if (detected != ImageFileFormat::Unknown) {
        auto image = DecodeImage(entry->data);
        if (image) {
            subtitle += Utf8ToWide(fmt::format("  ·  {} × {} · {}", image->width, image->height,
                                               WideToUtf8(ImageFormatName(image->format))));
            m_preview.ShowImage(*image, title, subtitle);
            SetEditText(ImageDetails(*image, entry->data.size()));
            return;
        }
    }

    if (auto text = TryDecodeText(entry->data, 2u * 1024u * 1024u)) {
        std::wstring display = *text;
        bool formattedXml = false;
        const bool manifest = IsResourceType(entry->type, ResourceType::Manifest);
        if (manifest || LooksLikeXml(display)) {
            if (auto formatted = FormatXmlText(display)) {
                display = std::move(*formatted);
                formattedXml = true;
            }
        }
        if (manifest) {
            m_preview.ShowMessage(
                title, subtitle,
                formattedXml ? L"XML 清单已按节点格式化，可在下方编辑（Ctrl+Enter 应用）。"
                             : L"XML 清单解码失败，仅显示原始文本。");
            SetEditText(display, true, entry->Key());
        } else {
            m_preview.ShowMessage(title, subtitle, L"文本内容显示在下方。");
            SetEditText(display);
        }
        return;
    }

    m_preview.ShowMessage(title, subtitle, L"无图形预览，二进制内容显示在下方。");
    SetEditText(BuildHexDump(entry->data));
}

void CMainFrame::AfterModelChanged(std::optional<ResourceKey> selectedKey, std::wstring status) {
    RebuildTree(selectedKey);
    UpdateTitle();
    UpdateStatusSummary();
    UpdateCommandStates();
    SetStatus(status);
}

bool CMainFrame::OpenFile(const std::filesystem::path& path) {
    if (!QuerySaveChanges()) {
        return false;
    }
    auto loaded = PeImage::LoadFromFile(path);
    if (!loaded) {
        ShowError(L"无法打开文件", loaded.error());
        return false;
    }

    m_pe = std::move(*loaded);
    m_model.Reset(m_pe->Resources());
    m_model.ClearDirty();
    m_path = path;
    m_hasFile = true;
    m_lastDir = path.parent_path();

    RebuildTree(std::nullopt);
    UpdateTitle();
    UpdateStatusSummary();
    UpdateCommandStates();
    UpdatePreview();

    std::wstring message = L"已打开：" + path.filename().wstring();
    if (m_pe->Info().upxPacked) {
        message += L" · 检测到 UPX 特征，可用“工具 → UPX 脱壳”";
    }
    if (m_pe->Info().hasSignature) {
        message += L" · 文件已签名";
    }
    SetStatus(message);
    return true;
}

bool CMainFrame::QuerySaveChanges() {
    if (m_editDirty) {
        const int editAnswer = ::MessageBoxW(
            m_hWnd, L"当前资源编辑尚未应用，是否应用该编辑？", L"eXe Res",
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (editAnswer == IDYES) {
            if (!ApplyCurrentEditor()) {
                return false;
            }
        } else if (editAnswer == IDNO) {
            m_editDirty = false;
        } else {
            return false;
        }
    }
    if (!m_hasFile || !m_model.IsDirty()) {
        return true;
    }
    const int answer = ::MessageBoxW(m_hWnd, L"资源已修改，是否保存？", L"eXe Res",
                                     MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDYES) {
        return SaveCurrent();
    }
    return answer == IDNO;
}

bool CMainFrame::SaveCurrent() {
    if (!m_hasFile) {
        ShowInfo(L"保存", L"当前没有打开文件。");
        return false;
    }
    return SaveTo(m_path, false);
}

bool CMainFrame::SaveTo(const std::filesystem::path& path, bool saveAs) {
    if (!m_pe) {
        return false;
    }
    if (m_editDirty && !ApplyCurrentEditor()) {
        return false;
    }

    const bool modifying = m_model.IsDirty();
    const std::optional<ResourceKey> selected = SelectedLeafKey();
    const bool signatureWasPresent = m_pe->Info().hasSignature;

    std::vector<uint8_t> bytes;
    auto verified = std::optional<PeImage>{};
    if (modifying) {
        if (m_pe->Info().upxPacked) {
            if (!Confirm(L"保存", L"文件带有 UPX 加壳特征，直接修改资源可能破坏文件。\r\n"
                                   L"建议先使用 UPX 脱壳。仍要继续保存吗？")) {
                return false;
            }
        }
        if (signatureWasPresent) {
            if (!Confirm(L"保存", L"修改资源后原有数字签名将失效，保存时会移除签名目录。\r\n"
                                   L"继续保存吗？")) {
                return false;
            }
        }

        auto compiled = m_pe->CompileResources(m_model);
        if (!compiled) {
            ShowError(L"保存失败", compiled.error());
            return false;
        }
        // Validate the rebuilt image before touching the original file.
        auto parsed = PeImage::LoadFromBytes(std::move(*compiled));
        if (!parsed) {
            ShowError(L"内部错误", L"重建后的 PE 无法解析，已取消保存：\r\n" + parsed.error());
            return false;
        }
        verified.emplace(std::move(*parsed));
        bytes = verified->OriginalBytes();
    } else {
        bytes = m_pe->OriginalBytes();
    }

    std::wstring writeError;
    if (!WriteFileAtomic(path, bytes, writeError)) {
        ShowError(L"保存失败", writeError);
        return false;
    }

    m_path = path;
    m_hasFile = true;
    m_lastDir = path.parent_path();
    if (modifying) {
        m_pe = std::move(*verified);
        m_model.Reset(m_pe->Resources());
        m_model.ClearDirty();
        RebuildTree(selected);
    } else {
        m_model.ClearDirty();
    }
    UpdateTitle();
    UpdateStatusSummary();
    UpdateCommandStates();
    UpdatePreview();

    std::wstring status = std::wstring(saveAs ? L"已另存为：" : L"已保存：") + path.wstring();
    if (modifying && signatureWasPresent) {
        status += L"（数字签名已移除）";
    }
    SetStatus(status);
    return true;
}

std::optional<std::filesystem::path> CMainFrame::AskOpenFile() {
    std::wstring initialDir = LastDirectory().wstring();
    CFileDialog dialog(TRUE, nullptr, nullptr,
                       OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY, kPeFilter, m_hWnd);
    dialog.m_ofn.lpstrInitialDir = initialDir.c_str();
    if (dialog.DoModal(m_hWnd) != IDOK) {
        return std::nullopt;
    }
    std::filesystem::path path(dialog.m_szFileName);
    m_lastDir = path.parent_path();
    return path;
}

std::optional<std::filesystem::path> CMainFrame::AskSavePath(const wchar_t* extension,
                                                             const std::wstring& defaultName,
                                                             const wchar_t* filter) {
    std::wstring initialDir = LastDirectory().wstring();
    CFileDialog dialog(FALSE, extension, defaultName.c_str(),
                       OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY, filter, m_hWnd);
    dialog.m_ofn.lpstrInitialDir = initialDir.c_str();
    if (dialog.DoModal(m_hWnd) != IDOK) {
        return std::nullopt;
    }
    std::filesystem::path path(dialog.m_szFileName);
    m_lastDir = path.parent_path();
    return path;
}

std::optional<std::filesystem::path> CMainFrame::AskImageReplacePath(const ResourceEntry& entry) {
    const wchar_t* filter = kAllFilter;
    if (IsResourceType(entry.type, ResourceType::GroupIcon)) {
        filter = kIcoFilter;
    } else if (IsResourceType(entry.type, ResourceType::Bitmap)) {
        filter = kImageFilter;
    } else if (IsResourceType(entry.type, ResourceType::Manifest)) {
        filter = kManifestFilter;
    } else if (DetectImageFormat(entry.data) != ImageFileFormat::Unknown) {
        filter = kImageFilter;
    }

    std::wstring initialDir = LastDirectory().wstring();
    CFileDialog dialog(TRUE, nullptr, nullptr,
                       OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY, filter, m_hWnd);
    dialog.m_ofn.lpstrInitialDir = initialDir.c_str();
    if (dialog.DoModal(m_hWnd) != IDOK) {
        return std::nullopt;
    }
    std::filesystem::path path(dialog.m_szFileName);
    m_lastDir = path.parent_path();
    return path;
}

std::optional<std::filesystem::path> CMainFrame::AskExportPath(const std::wstring& extension,
                                                              const std::wstring& defaultName,
                                                              const wchar_t* filter) {
    return AskSavePath(extension.empty() ? nullptr : extension.c_str(), defaultName, filter);
}

std::filesystem::path CMainFrame::LastDirectory() const {
    if (!m_lastDir.empty()) {
        return m_lastDir;
    }
    if (m_hasFile) {
        return m_path.parent_path();
    }
    return ExecutableDirectory();
}

bool CMainFrame::ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes,
                               std::wstring& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = L"无法打开文件：" + path.wstring();
        return false;
    }
    const std::streamoff size = static_cast<std::streamoff>(file.tellg());
    if (size < 0) {
        error = L"无法读取文件大小：" + path.wstring();
        return false;
    }
    if (size > 1024ll * 1024 * 1024) {
        error = L"文件超过 1 GB，暂不支持：" + path.wstring();
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 &&
        !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        error = L"读取文件失败：" + path.wstring();
        return false;
    }
    return true;
}

bool CMainFrame::WriteFileAtomic(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                                 std::wstring& error) {
    std::filesystem::path temp = path;
    temp += L".peres_tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = L"无法创建临时文件：" + temp.wstring();
            return false;
        }
        if (!bytes.empty()) {
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        }
        if (!file) {
            error = L"写入临时文件失败：" + temp.wstring();
            file.close();
            std::filesystem::remove(temp);
            return false;
        }
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"替换目标文件失败：" + path.wstring() + L"\r\n" + FormatWin32Error(::GetLastError());
        std::filesystem::remove(temp);
        return false;
    }
    return true;
}

void CMainFrame::ShowError(const std::wstring& title, const std::wstring& message) {
    ::MessageBoxW(m_hWnd, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

void CMainFrame::ShowInfo(const std::wstring& title, const std::wstring& message) {
    ::MessageBoxW(m_hWnd, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

bool CMainFrame::Confirm(const std::wstring& title, const std::wstring& message) {
    return ::MessageBoxW(m_hWnd, message.c_str(), title.c_str(),
                         MB_YESNO | MB_ICONQUESTION) == IDYES;
}

std::wstring CMainFrame::ResourceDisplayName(const ResourceEntry& entry) const {
    return ResourceTypeLabel(entry.type) + L" " + QuoteResourceName(entry.name) + L" [" +
           LanguageLabel(entry.language) + L"]";
}

std::wstring CMainFrame::ExportExtension(const ResourceEntry& entry,
                                         ImageFileFormat& format) const {
    format = ImageFileFormat::Unknown;
    if (IsResourceType(entry.type, ResourceType::GroupIcon)) {
        format = ImageFileFormat::Ico;
        return L"ico";
    }
    if (IsResourceType(entry.type, ResourceType::Icon)) {
        format = ImageFileFormat::Ico;
        return L"ico";
    }
    if (IsResourceType(entry.type, ResourceType::Manifest)) {
        return L"xml";
    }
    if (LooksLikePe(entry.data)) {
        auto pe = PeImage::LoadFromBytes(entry.data);
        if (pe) {
            return SuggestedPeExtension(pe->Info());
        }
    }
    const ImageFileFormat detected = DetectImageFormat(entry.data);
    if (detected != ImageFileFormat::Unknown) {
        format = detected;
        return ImageFormatExtension(detected);
    }
    if (IsResourceType(entry.type, ResourceType::Bitmap)) {
        return L"bmp";
    }
    return L"bin";
}

std::wstring CMainFrame::BuildExportName(const ResourceEntry& entry,
                                         const std::wstring& extension) const {
    return TypeStem(entry.type) + L"_" + NameStem(entry.name) + L"_" +
           std::to_wstring(entry.language) + L"." + extension;
}

LRESULT CMainFrame::OnTreeSelChanging(int, LPNMHDR, BOOL&) {
    if (m_rebuilding || m_editApplying || !m_editDirty) {
        return FALSE;
    }
    ShowInfo(L"编辑未应用",
             L"当前资源编辑尚未应用。\r\n"
             L"请按 Ctrl+Enter 应用编辑，或使用“资源 → 放弃编辑”后再切换资源。");
    return TRUE;
}

LRESULT CMainFrame::OnTreeSelChanged(int, LPNMHDR, BOOL&) {
    if (m_rebuilding) {
        return 0;
    }
    UpdatePreview();
    UpdateCommandStates();
    return 0;
}

LRESULT CMainFrame::OnEditChanged(WORD, WORD, HWND control, BOOL&) {
    if (m_editApplying || !m_editEditable || control != m_edit.m_hWnd) {
        return 0;
    }
    if (!m_editDirty) {
        m_editDirty = true;
        UpdateTitle();
        UpdateStatusSummary();
        UpdateCommandStates();
        SetStatus(L"编辑中，按 Ctrl+Enter 应用修改，或使用“资源 → 放弃编辑”");
    }
    return 0;
}

LRESULT CMainFrame::OnTreeRightClick(int, LPNMHDR, BOOL&) {
    const DWORD position = ::GetMessagePos();
    POINT screenPoint{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    TVHITTESTINFO hit{};
    hit.pt = screenPoint;
    ::ScreenToClient(m_tree.m_hWnd, &hit.pt);
    HTREEITEM item = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(m_tree.m_hWnd, TVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)));
    if (item == nullptr) {
        return 0;
    }
    m_tree.SelectItem(item);
    ShowTreeContextMenu(screenPoint);
    return 0;
}

void CMainFrame::ShowTreeContextMenu(POINT screenPoint) {
    CMenu menu;
    if (!menu.CreatePopupMenu()) {
        return;
    }
    const bool hasEntry = SelectedEntry() != nullptr;
    const auto flag = [hasEntry]() { return hasEntry ? MF_STRING : (MF_STRING | MF_GRAYED); };
    menu.AppendMenu(flag(), ID_CMD_RES_EXPORT, L"导出资源(&E)...");
    menu.AppendMenu(flag(), ID_CMD_RES_REPLACE, L"替换资源(&R)...");
    menu.AppendMenu(flag(), ID_CMD_RES_DELETE, L"删除资源(&D)");
    if (m_editDirty) {
        menu.AppendMenu(MF_SEPARATOR);
        menu.AppendMenu(MF_STRING, ID_CMD_RES_APPLY, L"应用编辑(&P)\tCtrl+Enter");
        menu.AppendMenu(MF_STRING, ID_CMD_RES_DISCARD, L"放弃编辑(&L)");
    }
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_CMD_FILE_SAVE, L"保存(&S)\tCtrl+S");
    const UINT command = static_cast<UINT>(
        menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, m_hWnd));
    if (command != 0) {
        ::SendMessageW(m_hWnd, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
}

LRESULT CMainFrame::OnFileOpen(WORD, WORD, HWND, BOOL&) {
    auto path = AskOpenFile();
    if (!path) {
        return 0;
    }
    OpenFile(*path);
    return 0;
}

LRESULT CMainFrame::OnFileSave(WORD, WORD, HWND, BOOL&) {
    if (!m_hasFile) {
        ShowInfo(L"保存", L"当前没有打开文件。");
        return 0;
    }
    if (!m_model.IsDirty() && !m_editDirty) {
        SetStatus(L"没有需要保存的修改");
        return 0;
    }
    SaveCurrent();
    return 0;
}

LRESULT CMainFrame::OnFileSaveAs(WORD, WORD, HWND, BOOL&) {
    if (!m_hasFile) {
        ShowInfo(L"另存为", L"当前没有打开文件。");
        return 0;
    }
    if (m_editDirty && !ApplyCurrentEditor()) {
        return 0;
    }
    std::wstring extension = m_path.extension().wstring();
    if (!extension.empty() && extension.front() == L'.') {
        extension.erase(extension.begin());
    }
    if (extension.empty()) {
        extension = L"exe";
    }
    const std::wstring defaultName = m_path.filename().wstring();
    auto path = AskSavePath(extension.c_str(), defaultName, kPeFilter);
    if (!path) {
        return 0;
    }
    SaveTo(*path, true);
    return 0;
}

LRESULT CMainFrame::OnFileExit(WORD, WORD, HWND, BOOL&) {
    SendMessageW(WM_CLOSE, 0, 0);
    return 0;
}

LRESULT CMainFrame::OnResourceExport(WORD, WORD, HWND, BOOL&) {
    if (m_editDirty) {
        ShowInfo(L"导出资源", L"请先应用或放弃当前编辑。");
        return 0;
    }
    const ResourceEntry* entry = SelectedEntry();
    if (entry == nullptr) {
        return 0;
    }
    ImageFileFormat format = ImageFileFormat::Unknown;
    const std::wstring extension = ExportExtension(*entry, format);

    std::vector<uint8_t> data;
    if (IsResourceType(entry->type, ResourceType::GroupIcon)) {
        auto ico = BuildIcoFromGroup(m_model, *entry);
        if (!ico) {
            ShowError(L"导出失败", ico.error());
            return 0;
        }
        data = std::move(*ico);
    } else if (IsResourceType(entry->type, ResourceType::Icon)) {
        auto ico = BuildIcoFromResource(entry->data);
        if (!ico) {
            ShowError(L"导出失败", ico.error());
            return 0;
        }
        data = std::move(*ico);
    } else if (IsResourceType(entry->type, ResourceType::Bitmap) &&
               DetectImageFormat(entry->data) == ImageFileFormat::Unknown) {
        auto bmp = DibToBmpFile(entry->data);
        if (!bmp) {
            ShowError(L"导出失败", bmp.error());
            return 0;
        }
        data = std::move(*bmp);
    } else {
        data = entry->data;
    }

    auto path = AskExportPath(extension, BuildExportName(*entry, extension),
                              FilterForExtension(extension));
    if (!path) {
        return 0;
    }
    std::wstring error;
    if (!WriteFileAtomic(*path, data, error)) {
        ShowError(L"导出失败", error);
        return 0;
    }
    SetStatus(L"已导出：" + path->wstring());
    return 0;
}

LRESULT CMainFrame::OnResourceReplace(WORD, WORD, HWND, BOOL&) {
    if (m_editDirty) {
        ShowInfo(L"替换资源", L"请先应用或放弃当前编辑。");
        return 0;
    }
    const ResourceEntry* entry = SelectedEntry();
    if (entry == nullptr) {
        return 0;
    }
    if (IsResourceType(entry->type, ResourceType::Icon)) {
        ShowInfo(L"替换图标", L"RT_ICON 是单个图标图像。\r\n"
                               L"请选择同名的 RT_GROUP_ICON 语言条目，通过 .ico 文件整体替换图标组。");
        return 0;
    }

    const ResourceId type = entry->type;
    const ResourceId name = entry->name;
    const uint16_t language = entry->language;
    const uint32_t codePage = entry->codePage;
    const ResourceKey key{type, name, language};
    const std::wstring display = ResourceDisplayName(*entry);

    auto path = AskImageReplacePath(*entry);
    if (!path) {
        return 0;
    }
    std::vector<uint8_t> fileBytes;
    std::wstring error;
    if (!ReadFileBytes(*path, fileBytes, error)) {
        ShowError(L"读取文件失败", error);
        return 0;
    }

    if (IsResourceType(type, ResourceType::GroupIcon)) {
        auto result = ReplaceIconGroup(m_model, *entry, fileBytes);
        if (!result) {
            ShowError(L"替换图标组失败", result.error());
            return 0;
        }
    } else if (IsResourceType(type, ResourceType::Bitmap)) {
        std::vector<uint8_t> dib;
        const ImageFileFormat format = DetectImageFormat(fileBytes);
        if (format == ImageFileFormat::Bmp) {
            auto converted = BmpFileToDib(fileBytes);
            if (!converted) {
                ShowError(L"替换位图失败", converted.error());
                return 0;
            }
            dib = std::move(*converted);
        } else if (format != ImageFileFormat::Unknown) {
            auto converted = ImageToDib(fileBytes);
            if (!converted) {
                ShowError(L"转换图像失败", converted.error());
                return 0;
            }
            dib = std::move(*converted);
        } else if (InspectBitmapDib(fileBytes)) {
            dib = std::move(fileBytes);
        } else {
            ShowError(L"替换位图失败",
                      L"文件既不是 BMP，也不是可解码的 PNG/JPEG/GIF/TIFF，或不是有效的 DIB。");
            return 0;
        }
        m_model.SetData(type, name, language, codePage, std::move(dib));
    } else {
        m_model.SetData(type, name, language, codePage, std::move(fileBytes));
    }

    std::wstring message = L"资源已替换，保存后生效：" + display;
    AfterModelChanged(key, message);
    return 0;
}

LRESULT CMainFrame::OnResourceDelete(WORD, WORD, HWND, BOOL&) {
    if (m_editDirty) {
        ShowInfo(L"删除资源", L"请先应用或放弃当前编辑。");
        return 0;
    }
    const ResourceEntry* entry = SelectedEntry();
    if (entry == nullptr) {
        return 0;
    }
    const ResourceId type = entry->type;
    const ResourceId name = entry->name;
    const uint16_t language = entry->language;
    const std::wstring display = ResourceDisplayName(*entry);
    if (!Confirm(L"删除资源", L"确定删除以下资源？\r\n\r\n" + display)) {
        return 0;
    }

    if (IsResourceType(type, ResourceType::GroupIcon)) {
        auto result = RemoveIconGroup(m_model, *entry);
        if (!result) {
            ShowError(L"删除失败", result.error());
            return 0;
        }
    } else if (!m_model.Remove(type, name, language)) {
        ShowError(L"删除失败", L"资源条目不存在。");
        return 0;
    }

    AfterModelChanged(std::nullopt, L"已删除资源：" + display);
    return 0;
}

LRESULT CMainFrame::OnToolUnpack(WORD, WORD, HWND, BOOL&) {
    if (!m_hasFile) {
        ShowInfo(L"UPX 脱壳", L"请先打开一个 PE 文件。");
        return 0;
    }
    if (m_model.IsDirty() || m_editDirty) {
        ShowInfo(L"UPX 脱壳", L"请先保存或放弃当前修改，再对磁盘文件进行脱壳。");
        return 0;
    }

    auto config = LoadConfig();
    if (!config) {
        ShowError(L"UPX 脱壳", config.error());
        return 0;
    }
    if (!std::filesystem::exists(config->upxPath)) {
        ShowError(L"UPX 脱壳", L"UPX 可执行文件不存在：\r\n" + config->upxPath.wstring() +
                                    L"\r\n\r\n请通过“工具 -> 编辑配置”修改 [upx] path。");
        return 0;
    }
    if (!m_pe->Info().upxPacked &&
        !Confirm(L"UPX 脱壳", L"未检测到 UPX 特征，仍要尝试调用 UPX 脱壳吗？")) {
        return 0;
    }

    std::filesystem::path outputDir =
        std::filesystem::temp_directory_path() / L"peres_unpacked";
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        ShowError(L"UPX 脱壳", L"无法创建临时输出目录：" + outputDir.wstring());
        return 0;
    }
    const std::wstring stem = m_path.stem().wstring();
    std::wstring extension = m_path.extension().wstring();
    const std::filesystem::path output =
        outputDir / (stem + L"_unpacked_" + std::to_wstring(::GetTickCount64()) + extension);

    SetStatus(L"正在调用 UPX 脱壳，请稍候...");
    ::UpdateWindow(m_hWndStatusBar);
    WaitCursorGuard waitCursor;
    auto result = UnpackWithUpx(m_path, config->upxPath, output);
    if (!result) {
        SetStatus(L"UPX 脱壳失败");
        ShowError(L"UPX 脱壳失败", result.error());
        return 0;
    }

    SetStatus(L"UPX 脱壳完成：" + result->outputPath.wstring());
    if (OpenFile(result->outputPath)) {
        ShowInfo(L"UPX 脱壳完成",
                 L"脱壳文件已生成并打开：\r\n" + result->outputPath.wstring());
    }
    return 0;
}

LRESULT CMainFrame::OnToolConfig(WORD, WORD, HWND, BOOL&) {
    auto created = EnsureConfigIniExists();
    if (!created) {
        ShowError(L"编辑配置", created.error());
        return 0;
    }
    const std::filesystem::path iniPath = ConfigIniPath();
    HINSTANCE result = ::ShellExecuteW(m_hWnd, L"open", iniPath.c_str(), nullptr, nullptr,
                                       SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        std::wstring parameters = L"\"" + iniPath.wstring() + L"\"";
        result = ::ShellExecuteW(m_hWnd, L"open", L"notepad.exe", parameters.c_str(), nullptr,
                                 SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowError(L"编辑配置", L"无法打开配置文件：\r\n" + iniPath.wstring());
    }
    return 0;
}

LRESULT CMainFrame::OnHelpAbout(WORD, WORD, HWND, BOOL&) {
    const std::wstring message =
        L"eXe Res 1.0.0\r\n"
        L"PE 资源浏览器（C++23 / WTL）\r\n\r\n"
        L"支持 PE32 与 PE32+（EXE / DLL）\r\n"
        L"查看、导出、替换和删除资源\r\n"
        L"图标组整体替换（自动拆分 RT_ICON）\r\n"
        L"图像预览：BMP / PNG / JPEG / GIF / TIFF（WIC）\r\n"
        L"UPX 脱壳（通过 peres.ini 配置 upx.exe 路径）\r\n\r\n"
        L"配置文件：" +
        ConfigIniPath().wstring();
    ShowInfo(L"关于 eXe Res", message);
    return 0;
}

}  // namespace peres::ui
