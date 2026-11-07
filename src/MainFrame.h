#pragma once

#include "stdafx.h"

#include "PeImage.h"
#include "PreviewWnd.h"
#include "ResourceModel.h"

#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace peres::ui {

class CMainFrame final : public CFrameWindowImpl<CMainFrame>, public CMessageFilter {
public:
    DECLARE_FRAME_WND_CLASS(L"PeresMainFrame", 0)

    BEGIN_MSG_MAP(CMainFrame)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
        MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
        MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
        COMMAND_ID_HANDLER(ID_CMD_FILE_OPEN, OnFileOpen)
        COMMAND_ID_HANDLER(ID_CMD_FILE_SAVE, OnFileSave)
        COMMAND_ID_HANDLER(ID_CMD_FILE_SAVEAS, OnFileSaveAs)
        COMMAND_ID_HANDLER(ID_CMD_FILE_EXIT, OnFileExit)
        COMMAND_ID_HANDLER(ID_CMD_RES_EXPORT, OnResourceExport)
        COMMAND_ID_HANDLER(ID_CMD_RES_REPLACE, OnResourceReplace)
        COMMAND_ID_HANDLER(ID_CMD_RES_DELETE, OnResourceDelete)
        COMMAND_ID_HANDLER(ID_CMD_RES_APPLY, OnResourceApply)
        COMMAND_ID_HANDLER(ID_CMD_RES_DISCARD, OnResourceDiscard)
        COMMAND_ID_HANDLER(ID_CMD_TOOL_UNPACK, OnToolUnpack)
        COMMAND_ID_HANDLER(ID_CMD_TOOL_CONFIG, OnToolConfig)
        COMMAND_ID_HANDLER(ID_CMD_HELP_ABOUT, OnHelpAbout)
        NOTIFY_HANDLER(IDC_RES_TREE, TVN_SELCHANGING, OnTreeSelChanging)
        NOTIFY_HANDLER(IDC_RES_TREE, TVN_SELCHANGED, OnTreeSelChanged)
        NOTIFY_HANDLER(IDC_RES_TREE, NM_RCLICK, OnTreeRightClick)
        COMMAND_CODE_HANDLER(EN_CHANGE, OnEditChanged)
        CHAIN_MSG_MAP(CFrameWindowImpl<CMainFrame>)
    END_MSG_MAP()

    CMainFrame() = default;
    ~CMainFrame() override;

    BOOL PreTranslateMessage(MSG* message) override {
        return CFrameWindowImpl<CMainFrame>::PreTranslateMessage(message);
    }

    bool OpenFile(const std::filesystem::path& path);

private:
    enum class TreeLevel {
        ExportRoot,
        ExportSymbol,
        ImportRoot,
        ImportModule,
        ImportSymbol,
        Type,
        Name,
        Language
    };

    struct NodeData {
        TreeLevel level = TreeLevel::Type;
        ResourceId type;
        ResourceId name;
        uint16_t language = 0;
        uint32_t firstIndex = 0;
        uint32_t secondIndex = 0;
    };

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnGetMinMaxInfo(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDropFiles(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnCtlColorStatic(UINT, WPARAM, LPARAM, BOOL&);

    LRESULT OnFileOpen(WORD, WORD, HWND, BOOL&);
    LRESULT OnFileSave(WORD, WORD, HWND, BOOL&);
    LRESULT OnFileSaveAs(WORD, WORD, HWND, BOOL&);
    LRESULT OnFileExit(WORD, WORD, HWND, BOOL&);
    LRESULT OnResourceExport(WORD, WORD, HWND, BOOL&);
    LRESULT OnResourceReplace(WORD, WORD, HWND, BOOL&);
    LRESULT OnResourceDelete(WORD, WORD, HWND, BOOL&);
    LRESULT OnResourceApply(WORD, WORD, HWND, BOOL&);
    LRESULT OnResourceDiscard(WORD, WORD, HWND, BOOL&);
    LRESULT OnToolUnpack(WORD, WORD, HWND, BOOL&);
    LRESULT OnToolConfig(WORD, WORD, HWND, BOOL&);
    LRESULT OnHelpAbout(WORD, WORD, HWND, BOOL&);

    LRESULT OnTreeSelChanging(int, LPNMHDR, BOOL&);
    LRESULT OnTreeSelChanged(int, LPNMHDR, BOOL&);
    LRESULT OnTreeRightClick(int, LPNMHDR, BOOL&);
    LRESULT OnEditChanged(WORD, WORD, HWND, BOOL&);

    int Scale(int value) const;

    void BuildMenu();
    void BuildAccelerators();
    void RebuildTree(std::optional<ResourceKey> selectedKey);
    void UpdatePreview();
    void UpdateCommandStates();
    void UpdateStatusParts();
    void UpdateStatusSummary();
    void UpdateTitle();
    void AfterModelChanged(std::optional<ResourceKey> selectedKey, std::wstring status);

    NodeData* SelectedNode() const;
    const ResourceEntry* SelectedEntry() const;
    std::optional<ResourceKey> SelectedLeafKey() const;
    void SelectLeaf(const ResourceKey& key);

    bool QuerySaveChanges();
    bool SaveCurrent();
    bool SaveTo(const std::filesystem::path& path, bool saveAs);

    std::optional<std::filesystem::path> AskOpenFile();
    std::optional<std::filesystem::path> AskSavePath(const wchar_t* extension,
                                                     const std::wstring& defaultName,
                                                     const wchar_t* filter);
    std::optional<std::filesystem::path> AskImageReplacePath(const ResourceEntry& entry);
    std::optional<std::filesystem::path> AskExportPath(const std::wstring& extension,
                                                       const std::wstring& defaultName,
                                                       const wchar_t* filter);
    bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes,
                       std::wstring& error);
    bool WriteFileAtomic(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                         std::wstring& error);

    void ShowTreeContextMenu(POINT screenPoint);
    void SetStatus(const std::wstring& text);
    void SetEditText(const std::wstring& text, bool editable = false,
                     std::optional<ResourceKey> key = std::nullopt);
    std::wstring GetEditText() const;
    bool ApplyCurrentEditor();
    void DiscardCurrentEditor();
    void ShowError(const std::wstring& title, const std::wstring& message);
    void ShowInfo(const std::wstring& title, const std::wstring& message);
    bool Confirm(const std::wstring& title, const std::wstring& message);

    std::wstring ExportExtension(const ResourceEntry& entry, ImageFileFormat& format) const;
    std::wstring BuildExportName(const ResourceEntry& entry, const std::wstring& extension) const;
    std::wstring ResourceDisplayName(const ResourceEntry& entry) const;

    std::filesystem::path LastDirectory() const;
    std::optional<ResourceKey> MakeKey(const NodeData& node) const;

    CSplitterWindow m_splitter;
    CSplitterWindow m_rightSplitter;
    CTreeViewCtrl m_tree;
    CPreviewWnd m_preview;
    CEdit m_edit;
    CStatusBarCtrl m_status;

    std::optional<PeImage> m_pe;
    ResourceModel m_model;
    std::filesystem::path m_path;
    std::filesystem::path m_lastDir;
    bool m_hasFile = false;
    bool m_rebuilding = false;
    bool m_layoutInitialized = false;
    bool m_editDirty = false;
    bool m_editApplying = false;
    bool m_editEditable = false;
    std::optional<ResourceKey> m_editKey;

    std::deque<NodeData> m_nodes;
    std::map<ResourceKey, HTREEITEM, ResourceKeyLess> m_leafItems;

    HMENU m_menu = nullptr;
    HACCEL m_accel = nullptr;
    HFONT m_uiFont = nullptr;
    HFONT m_monoFont = nullptr;
    HBRUSH m_editBrush = nullptr;
};

}  // namespace peres::ui
