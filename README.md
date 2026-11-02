# eXe Res — PE 资源浏览器

`eXe Res` 是一个使用 C++23 与 WTL 编写的 Windows PE 资源浏览器，支持 PE32 / PE32+ 的 EXE、DLL，可查看、导出、替换、删除资源，并直接编辑清单、字符串表、菜单和对话框资源。

## 功能

- **PE 基本信息**：架构（x86/x64 等）、PE32/PE32+、EXE/DLL、节数量、文件大小、签名与 UPX 特征。
- **资源树**：按资源类型 / 名称 / 语言组织，显示编码、大小与语言。
- **图标**
  - 查看图标组与单个图标图像。
  - 将 `.ico` 拆分后整体替换 RT_GROUP_ICON，自动维护关联的 RT_ICON 条目。
  - 导出图标组为完整的 `.ico`。
- **图像**
  - RT_BITMAP 的 DIB 预览与 `.bmp` 导入。
  - 通过 Windows Imaging Component (WIC) 预览 PNG / JPEG / GIF / BMP / TIFF 图像，包括任意资源类型中保存的图像文件。
  - 将 PNG / JPEG / GIF / TIFF 转换为 32 位 DIB 后替换 RT_BITMAP。
- **清单（RT_MANIFEST）**
  - XML 按节点格式化换行，便于阅读。
  - 可直接编辑 XML，添加或删除节点；保存时保留原资源的 UTF-8 / UTF-16 编码约定（含 BOM）。
- **字符串表（RT_STRING）**
  - 按 UTF-16 解码为 `#ID: 文本` 的可编辑格式。
  - 支持修改、增加或删除字符串，自动回填 16 个字符串槽位。
- **版本信息（RT_VERSION）**
  - 解析 `VS_VERSION_INFO`、`VS_FIXEDFILEINFO`、`StringFileInfo` 多语言字符串表与 `VarFileInfo` Translation。
  - 解析 FileVersion / ProductVersion、公司名、产品名、版权、原始文件名等字段，按 UTF-16 只读显示。
  - 解析结果与 Windows `GetFileVersionInfoW` / `VerQueryValueW` 交叉验证。
- **菜单资源（RT_MENU）**
  - 支持经典 `MENU` 与 `MENUEX` 模板。
  - 上方预览区绘制菜单栏与展开的下拉菜单面板，显示层级、分隔线、禁用/勾选状态。
  - 解析为可编辑的 RC 风格文本，可修改文本、ID、类型/状态，增加或删除菜单项。
  - 编译回 UTF-16 菜单模板。
- **对话框资源（RT_DIALOG）**
  - 支持 `DIALOG` 与 `DIALOGEX`、字体、控件、扩展样式、帮助 ID、控件创建数据。
  - 上方预览区绘制模拟对话框窗口：标题栏、客户区与各控件的位置、文本和常用样式。
  - 解析为可编辑文本，可修改标题、字体、样式、控件文本/ID/位置/样式，增加或删除控件。
  - 编译回 UTF-16 对话框模板。
- **编辑流程**
  - 文本型资源在下方编辑器中直接编辑。
  - `Ctrl+Enter` 或“资源 → 应用编辑”校验并写回内存中的资源。
  - “资源 → 放弃编辑”恢复原内容。
  - 有未应用编辑时，切换资源会被阻止，避免误丢修改。
- **UPX**
  - 检测 UPX 特征。
  - 通过 `peres.ini` 配置 `upx.exe` 路径，调用 `upx -d` 脱壳并自动打开结果。
- **其它**
  - 拖放打开文件、命令行参数打开文件。
  - 资源导出、原始替换、删除；删除图标组时清理无引用的 RT_ICON。
  - 保存使用临时文件原子替换；重建后的 PE 在写盘前会重新解析校验。
  - 含 Authenticode 签名的文件在修改保存时会移除签名目录并提示。

## 应用图标

`resources/app.ico` 由 `tools/make_icon.py` 生成：深色渐变圆角背景，背景带 “xPEx” 水印，前景为两行 “eXe / Res”。只包含 16/32/64 px 三帧；16 px 小图标不绘制水印，避免糊成一团：

```powershell
python tools/make_icon.py
```

脚本仅使用 Python 标准库与 GDI 生成多尺寸 ICO，便于重新定制品牌图标。

## 构建

要求：

- Visual Studio 2026（MSVC 19.51 或更高）
- WTL（工具链的 `VC/Tools/MSVC/<版本>/include/WTL/Include`）
- ATL（`atlmfc/include`、`atlmfc/lib/x64`）
- fmtlib（`VC/Tools/MSVC/<版本>/lib/x64/fmt.lib`）
- CMake 3.25+、Ninja

在 Developer PowerShell / 已执行 `vcvars64.bat` 的环境中：

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

如果 WTL 或 fmtlib 不在默认工具链目录，可显式指定：

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release `
  -DWTL_INCLUDE_DIR=<WTL Include 目录> `
  -DPERES_FMT_LIB=<fmt.lib 路径>
```

### WTL 与 C++23

WTL 9.10 在 C++20/23 的依赖基类查找规则下仍需要 permissive 模式，因此：

- PE / 资源核心库、测试使用严格 C++23（`/permissive-` + `/std:c++latest`）。
- GUI 翻译单元使用 `/permissive /Zc:twoPhase-`，仅用于兼容 WTL 9.10 头文件。

## 配置

`peres.ini` 与 `peres.exe` 放在同一目录：

```ini
[upx]
path=D:\soft\upx\upx.exe
```

`path` 支持绝对路径、相对于程序目录的路径以及 `%环境变量%`。可从“工具 → 编辑配置”打开。

## 资源编辑文本格式

### 清单

直接编辑 XML。应用时会验证标签配对、按节点重新格式化，并按原资源编码（UTF-8 / UTF-8 BOM / UTF-16LE / UTF-16BE）写回。

### 字符串表

```text
#96: 第一个字符串
#101: 另一个字符串
```

空行表示该槽位没有字符串。ID 必须落在当前字符串块范围内（块 ID 为资源名称，例如资源 `#7` 对应 ID 96–111）。

### 菜单

```text
MENU
{
  POPUP "文件(&F)"
  {
    MENUITEM "打开(&O)...", 40001
    MENUITEM SEPARATOR
    MENUITEM "退出(&X)", 40002
  }
}
```

MENUEX：

```text
MENUEX
HELPID 0x00000000
{
  POPUP "文件", 40010, 0x00000000, 0x00000000, 0x00000000
  {
    MENUITEM "打开", 40011, 0x00000000, 0x00000000
  }
}
```

经典菜单的 flags、MENUEX 的 type/state 使用十六进制数值，可自由修改。菜单层级结束标志由编辑器根据位置自动维护。

### 对话框

```text
DIALOGEX 0, 0, 200, 100
STYLE 0x80C80040
EXSTYLE 0x00000000
HELPID 0x00000000
CAPTION "示例对话框"
MENU NONE
CLASS NONE
FONT 9, 400, 0, 1, "Segoe UI"
{
  CONTROL 1101, STATIC, "标签", 10, 10, 120, 12, 0x50020000, 0x00000000, 0x00000000
  CONTROL 1102, BUTTON, "确定", 60, 70, 60, 14, 0x50010000, 0x00000000, 0x00000000, data=0102
}
```

- `DIALOG`（经典）与 `DIALOGEX`（扩展）均可。
- 控件类可使用 `BUTTON`、`EDIT`、`STATIC`、`LISTBOX`、`SCROLLBAR`、`COMBOBOX`、`#原子值` 或引号字符串。
- `data=` 为可选的控件创建数据十六进制字节串。
- 所有字符串按 UTF-16LE 写回，与 Win32 资源模板一致。

## 已知限制

- 菜单/对话框解析覆盖常见 Win32 模板。MENUEX 的额外 `bResInfo` 保留位、非弹出项的 help ID 等非常规字段会按简化模型处理。
- 对话框控件通过 `WS_CHILD` 与模板合法性扫描定位；极少数手写/非常规模板可能需要先确认结构。
- 图像解码与编码依赖系统 WIC 编解码器。
- 修改已签名文件会移除 Authenticode 签名。
- 修改 UPX 加壳文件前应先脱壳；程序在保存时会给出提示。
- GUI 主要面向 Windows 10/11 64 位；被浏览的 PE 文件支持 32 位与 64 位。

## 测试

`tests/` 中包含：

- 最小 32 位 PE 的资源重建、原地更新与追加资源节。
- 64 位样本 DLL 的图标组替换、清单替换，并通过 `LoadLibraryEx` 校验最终资源。
- PNG / JPEG 解码与 DIB 转换。
- MENU / MENUEX 解析、文本编辑、编译，并通过 `LoadMenuIndirectW` 验证。
- DIALOG / DIALOGEX 解析、文本编辑、编译，并通过 `CreateDialogIndirectParamW` 验证。
- RT_VERSION 解析，并与 Windows `GetFileVersionInfoW` / `VerQueryValueW` 交叉验证。
- XML 按节点格式化与编码保持。
- 可选 UPX 压缩/脱壳闭环（读取 `peres.ini`，UPX 不可用时跳过）。
