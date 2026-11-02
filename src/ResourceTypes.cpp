#include "ResourceTypes.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <array>

namespace peres {

namespace {

struct TypeName {
    uint16_t id;
    const wchar_t* label;
};

constexpr std::array kTypeNames{
    TypeName{1, L"光标 (RT_CURSOR)"},
    TypeName{2, L"位图 (RT_BITMAP)"},
    TypeName{3, L"图标 (RT_ICON)"},
    TypeName{4, L"菜单 (RT_MENU)"},
    TypeName{5, L"对话框 (RT_DIALOG)"},
    TypeName{6, L"字符串表 (RT_STRING)"},
    TypeName{7, L"字体目录 (RT_FONTDIR)"},
    TypeName{8, L"字体 (RT_FONT)"},
    TypeName{9, L"加速键 (RT_ACCELERATOR)"},
    TypeName{10, L"RC 数据 (RT_RCDATA)"},
    TypeName{11, L"消息表 (RT_MESSAGETABLE)"},
    TypeName{12, L"光标组 (RT_GROUP_CURSOR)"},
    TypeName{14, L"图标组 (RT_GROUP_ICON)"},
    TypeName{16, L"版本信息 (RT_VERSION)"},
    TypeName{17, L"对话框包含 (RT_DLGINCLUDE)"},
    TypeName{19, L"即插即用 (RT_PLUGPLAY)"},
    TypeName{20, L"VXD (RT_VXD)"},
    TypeName{21, L"动画光标 (RT_ANICURSOR)"},
    TypeName{22, L"动画图标 (RT_ANIICON)"},
    TypeName{23, L"HTML (RT_HTML)"},
    TypeName{24, L"清单 (RT_MANIFEST)"},
};

struct LanguageName {
    uint16_t id;
    const wchar_t* label;
};

constexpr std::array kLanguageNames{
    LanguageName{0x0000, L"中性"},
    LanguageName{0x0401, L"阿拉伯语 - 沙特阿拉伯"},
    LanguageName{0x0404, L"中文 - 繁体（台湾）"},
    LanguageName{0x0407, L"德语 - 德国"},
    LanguageName{0x0409, L"英语 - 美国"},
    LanguageName{0x040A, L"西班牙语 - 传统"},
    LanguageName{0x040C, L"法语 - 法国"},
    LanguageName{0x0410, L"意大利语 - 意大利"},
    LanguageName{0x0411, L"日语 - 日本"},
    LanguageName{0x0412, L"韩语 - 韩国"},
    LanguageName{0x0419, L"俄语 - 俄罗斯"},
    LanguageName{0x041D, L"瑞典语 - 瑞典"},
    LanguageName{0x0804, L"中文 - 简体（中国）"},
    LanguageName{0x0809, L"英语 - 英国"},
    LanguageName{0x0816, L"葡萄牙语 - 葡萄牙"},
    LanguageName{0x0C04, L"中文 - 繁体（香港）"},
    LanguageName{0x1004, L"中文 - 简体（新加坡）"},
    LanguageName{0x1404, L"中文 - 繁体（澳门）"},
};

}  // namespace

bool IsResourceType(const ResourceId& type, ResourceType expected) {
    return !type.named && type.id == static_cast<uint16_t>(expected);
}

std::wstring ResourceTypeLabel(const ResourceId& type) {
    if (type.named) {
        return L"类型 \"" + type.name + L"\"";
    }
    for (const auto& entry : kTypeNames) {
        if (entry.id == type.id) {
            return entry.label;
        }
    }
    return Utf8ToWide(fmt::format("资源类型 #{} (0x{:04X})", type.id, type.id));
}

std::wstring LanguageLabel(uint16_t language) {
    for (const auto& entry : kLanguageNames) {
        if (entry.id == language) {
            return Utf8ToWide(fmt::format("0x{:04X} · {}", language, WideToUtf8(entry.label)));
        }
    }
    return Utf8ToWide(fmt::format("0x{:04X}", language));
}

std::wstring QuoteResourceName(const ResourceId& id) {
    if (id.named) {
        return L"\"" + id.name + L"\"";
    }
    return L"#" + std::to_wstring(id.id);
}

}  // namespace peres
