# Changelog

## 1.0.0

- 初始版本：PE32 / PE32+ EXE/DLL 资源浏览。
- 修复图标生成：文字改为绘制到同一 DIB，背景加入 “xPEx” 水印，前景保留 “eXe / Res”；仅保留 16/32/64 px 三帧。
- 修复关闭按钮 / “文件 → 退出”无响应的问题，改为同步销毁主窗口并退出进程。
- 图标组查看与 `.ico` 整体替换，自动拆分并维护 RT_ICON。
- RT_BITMAP 查看与 BMP 导入。
- 基于 WIC 的 PNG / JPEG / GIF / BMP / TIFF 图像预览；任意资源中的图像文件可自动识别。
- 清单 XML 按节点格式化，支持直接编辑，保留原编码与 BOM。
- 字符串表以 UTF-16 解析为可编辑文本。
- 新增 RT_VERSION 版本信息解析：VS_FIXEDFILEINFO、StringFileInfo 字符串表、VarFileInfo Translation，只读展示完整内容。
- MENU / MENUEX 菜单资源解析、可视化菜单预览、文本编辑与编译。
- DIALOG / DIALOGEX 对话框资源解析、可视化对话框预览、文本编辑与编译。
- 文本编辑应用/放弃流程、未应用编辑保护、Ctrl+Enter 应用。
- 资源导出、原始替换、删除。
- 保存前重新解析校验，原子写入；数字签名目录处理。
- peres.ini 配置 UPX 路径，检测并调用 `upx -d` 脱壳。
- WTL GUI（资源树、图标/位图/图像预览、可编辑文本面板、状态栏）。
- C++23 核心与 WTL 兼容编译配置。
