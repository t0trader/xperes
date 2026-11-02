#include "Config.h"

#include "TextUtil.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace peres {

namespace {

std::wstring ExpandEnvironment(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const DWORD required = ::ExpandEnvironmentStringsW(
        std::wstring(value).c_str(), nullptr, 0);
    if (required == 0) {
        return std::wstring(value);
    }
    std::wstring expanded(required, L'\0');
    if (::ExpandEnvironmentStringsW(std::wstring(value).c_str(), expanded.data(), required) == 0) {
        return std::wstring(value);
    }
    while (!expanded.empty() && expanded.back() == L'\0') {
        expanded.pop_back();
    }
    return expanded;
}

std::wstring StripQuotes(std::wstring value) {
    value = Trim(value);
    if (value.size() >= 2) {
        if ((value.front() == L'"' && value.back() == L'"') ||
            (value.front() == L'\'' && value.back() == L'\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return value;
}

}  // namespace

std::filesystem::path ExecutableDirectory() {
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path ConfigIniPath() {
    return ExecutableDirectory() / L"peres.ini";
}

Result<AppConfig> LoadConfig() {
    const auto iniPath = ConfigIniPath();
    if (!std::filesystem::exists(iniPath)) {
        return MakeError(L"配置文件不存在：" + iniPath.wstring() +
                         L"\n请通过“工具 -> 编辑配置”创建并填写 [upx] path。");
    }

    std::ifstream file(iniPath, std::ios::binary);
    if (!file) {
        return MakeError(L"无法读取配置文件：" + iniPath.wstring());
    }

    std::vector<std::wstring> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (lines.empty() && line.size() >= 3 && line[0] == '\xEF' && line[1] == '\xBB' &&
            line[2] == '\xBF') {
            line.erase(0, 3);
        }
        lines.push_back(Utf8ToWide(line));
    }

    std::wstring currentSection;
    std::wstring upxPathValue;
    for (const auto& rawLine : lines) {
        std::wstring text = Trim(rawLine);
        if (text.empty() || text[0] == L';' || text[0] == L'#') {
            continue;
        }
        if (text.front() == L'[' && text.back() == L']') {
            currentSection = ToLowerAscii(Trim(text.substr(1, text.size() - 2)));
            continue;
        }
        const size_t equals = text.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        const std::wstring key = ToLowerAscii(Trim(text.substr(0, equals)));
        const std::wstring value = Trim(text.substr(equals + 1));
        if (currentSection == L"upx" && key == L"path") {
            upxPathValue = value;
        }
    }

    if (upxPathValue.empty()) {
        return MakeError(L"peres.ini 中未配置 [upx] path：\n" + iniPath.wstring());
    }

    std::wstring expanded = ExpandEnvironment(StripQuotes(upxPathValue));
    if (expanded.empty()) {
        return MakeError(L"[upx] path 为空：" + iniPath.wstring());
    }
    std::filesystem::path upxPath(expanded);
    if (upxPath.is_relative()) {
        upxPath = ExecutableDirectory() / upxPath;
    }

    AppConfig config{iniPath, upxPath};
    return config;
}

Result<void> EnsureConfigIniExists() {
    const auto iniPath = ConfigIniPath();
    if (std::filesystem::exists(iniPath)) {
        return {};
    }
    std::ofstream file(iniPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        return MakeError(L"无法写入配置文件：" + iniPath.wstring());
    }
    file << "; eXe Res configuration\r\n"
            "; Place this file next to peres.exe.\r\n"
            "; [upx] path may be absolute, relative to the executable, or use %ENV%.\r\n"
            "\r\n"
            "[upx]\r\n"
            "path=\r\n";
    file.close();
    if (!file) {
        return MakeError(L"写入配置文件失败：" + iniPath.wstring());
    }
    return {};
}

}  // namespace peres
