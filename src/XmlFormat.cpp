#include "XmlFormat.h"

#include "TextUtil.h"

#include <cwctype>
#include <vector>

namespace peres {

namespace {

enum class TokenKind {
    StartTag,
    EndTag,
    Text,
    Comment,
    CData,
    ProcessingInstruction,
    Doctype,
};

struct XmlToken {
    TokenKind kind = TokenKind::Text;
    std::wstring text;
    std::wstring name;
    bool selfClosing = false;
    bool whitespaceOnly = true;
};

bool IsSpace(wchar_t ch) {
    return std::iswspace(ch) != 0;
}

bool WhitespaceOnly(std::wstring_view text) {
    for (const wchar_t ch : text) {
        if (!IsSpace(ch)) {
            return false;
        }
    }
    return true;
}

std::wstring Trimmed(std::wstring_view text) {
    return Trim(text);
}

size_t FindDelimiter(std::wstring_view xml, size_t from, std::wstring_view delimiter) {
    return xml.find(delimiter, from);
}

// Finds the closing '>' of a tag, ignoring '>' characters inside quoted
// attribute values.  Also tracks [] nesting for DOCTYPE internal subsets.
size_t FindTagEnd(std::wstring_view xml, size_t start, bool brackets) {
    bool inSingle = false;
    bool inDouble = false;
    int squareDepth = 0;
    for (size_t i = start; i < xml.size(); ++i) {
        const wchar_t ch = xml[i];
        if (inSingle) {
            if (ch == L'\'') {
                inSingle = false;
            }
            continue;
        }
        if (inDouble) {
            if (ch == L'"') {
                inDouble = false;
            }
            continue;
        }
        if (ch == L'\'') {
            inSingle = true;
            continue;
        }
        if (ch == L'"') {
            inDouble = true;
            continue;
        }
        if (brackets) {
            if (ch == L'[') {
                ++squareDepth;
            } else if (ch == L']' && squareDepth > 0) {
                --squareDepth;
            }
        }
        if (ch == L'>' && squareDepth == 0) {
            return i;
        }
    }
    return std::wstring_view::npos;
}

Result<std::vector<XmlToken>> Tokenize(std::wstring_view xml) {
    std::vector<XmlToken> tokens;
    size_t pos = 0;
    while (pos < xml.size()) {
        if (xml[pos] != L'<') {
            const size_t next = xml.find(L'<', pos);
            XmlToken token;
            token.kind = TokenKind::Text;
            token.text = std::wstring(xml.substr(
                pos, next == std::wstring_view::npos ? xml.size() - pos : next - pos));
            token.whitespaceOnly = WhitespaceOnly(token.text);
            tokens.push_back(std::move(token));
            if (next == std::wstring_view::npos) {
                break;
            }
            pos = next;
            continue;
        }

        if (xml.substr(pos, 4) == L"<!--") {
            const size_t end = FindDelimiter(xml, pos + 4, L"-->");
            if (end == std::wstring_view::npos) {
                return MakeError(L"XML 注释未闭合");
            }
            XmlToken token;
            token.kind = TokenKind::Comment;
            token.text = std::wstring(xml.substr(pos, end + 3 - pos));
            token.whitespaceOnly = false;
            tokens.push_back(std::move(token));
            pos = end + 3;
            continue;
        }
        if (xml.substr(pos, 9) == L"<![CDATA[") {
            const size_t end = FindDelimiter(xml, pos + 9, L"]]>");
            if (end == std::wstring_view::npos) {
                return MakeError(L"XML CDATA 未闭合");
            }
            XmlToken token;
            token.kind = TokenKind::CData;
            token.text = std::wstring(xml.substr(pos, end + 3 - pos));
            token.whitespaceOnly = false;
            tokens.push_back(std::move(token));
            pos = end + 3;
            continue;
        }
        if (xml.substr(pos, 2) == L"<?") {
            const size_t end = FindDelimiter(xml, pos + 2, L"?>");
            if (end == std::wstring_view::npos) {
                return MakeError(L"XML 处理指令未闭合");
            }
            XmlToken token;
            token.kind = TokenKind::ProcessingInstruction;
            token.text = std::wstring(xml.substr(pos, end + 2 - pos));
            token.whitespaceOnly = false;
            tokens.push_back(std::move(token));
            pos = end + 2;
            continue;
        }
        if (xml.substr(pos, 2) == L"</") {
            const size_t end = FindTagEnd(xml, pos + 2, false);
            if (end == std::wstring_view::npos) {
                return MakeError(L"XML 结束标签未闭合");
            }
            XmlToken token;
            token.kind = TokenKind::EndTag;
            token.text = std::wstring(xml.substr(pos, end + 1 - pos));
            size_t nameStart = pos + 2;
            while (nameStart < end && IsSpace(xml[nameStart])) {
                ++nameStart;
            }
            size_t nameEnd = nameStart;
            while (nameEnd < end && !IsSpace(xml[nameEnd]) && xml[nameEnd] != L'>') {
                ++nameEnd;
            }
            token.name = std::wstring(xml.substr(nameStart, nameEnd - nameStart));
            token.whitespaceOnly = false;
            tokens.push_back(std::move(token));
            pos = end + 1;
            continue;
        }
        if (xml.substr(pos, 9) == L"<!DOCTYPE") {
            const size_t end = FindTagEnd(xml, pos + 2, true);
            if (end == std::wstring_view::npos) {
                return MakeError(L"XML DOCTYPE 未闭合");
            }
            XmlToken token;
            token.kind = TokenKind::Doctype;
            token.text = std::wstring(xml.substr(pos, end + 1 - pos));
            token.whitespaceOnly = false;
            tokens.push_back(std::move(token));
            pos = end + 1;
            continue;
        }
        if (xml.substr(pos, 2) == L"<!") {
            const size_t end = FindTagEnd(xml, pos + 2, false);
            if (end == std::wstring_view::npos) {
                return MakeError(L"XML 声明未闭合");
            }
            XmlToken token;
            token.kind = TokenKind::Doctype;
            token.text = std::wstring(xml.substr(pos, end + 1 - pos));
            token.whitespaceOnly = false;
            tokens.push_back(std::move(token));
            pos = end + 1;
            continue;
        }

        const size_t end = FindTagEnd(xml, pos + 1, false);
        if (end == std::wstring_view::npos) {
            return MakeError(L"XML 开始标签未闭合");
        }
        XmlToken token;
        token.kind = TokenKind::StartTag;
        token.text = std::wstring(xml.substr(pos, end + 1 - pos));
        size_t nameStart = pos + 1;
        while (nameStart < end && IsSpace(xml[nameStart])) {
            ++nameStart;
        }
        size_t nameEnd = nameStart;
        while (nameEnd < end && !IsSpace(xml[nameEnd]) && xml[nameEnd] != L'/' &&
               xml[nameEnd] != L'>') {
            ++nameEnd;
        }
        token.name = std::wstring(xml.substr(nameStart, nameEnd - nameStart));
        size_t slash = end;
        while (slash > pos && IsSpace(xml[slash - 1])) {
            --slash;
        }
        token.selfClosing = slash > pos && xml[slash - 1] == L'/';
        token.whitespaceOnly = false;
        tokens.push_back(std::move(token));
        pos = end + 1;
    }
    return tokens;
}

void AppendLine(std::wstring& output, int indent, std::wstring_view line) {
    if (!output.empty()) {
        output += L"\r\n";
    }
    output.append(static_cast<size_t>(indent) * 2, L' ');
    output += line;
}

void AppendMultiline(std::wstring& output, int indent, std::wstring_view text) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        const size_t length = end == std::wstring_view::npos ? text.size() - start : end - start;
        AppendLine(output, indent, text.substr(start, length));
        if (end == std::wstring_view::npos) {
            break;
        }
        if (text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n') {
            start = end + 2;
        } else {
            start = end + 1;
        }
    }
}

}  // namespace

bool LooksLikeXml(std::wstring_view text) {
    size_t pos = 0;
    while (pos < text.size() && IsSpace(text[pos])) {
        ++pos;
    }
    if (pos >= text.size() || text[pos] != L'<') {
        return false;
    }
    return text.substr(pos, 5) == L"<?xml" || text.substr(pos, 1) == L"<";
}

Result<std::wstring> FormatXmlText(std::wstring_view xml) {
    auto tokensResult = Tokenize(xml);
    if (!tokensResult) {
        return std::unexpected(tokensResult.error());
    }
    const auto& tokens = *tokensResult;

    std::wstring output;
    std::vector<std::wstring> stack;
    int indent = 0;

    size_t i = 0;
    while (i < tokens.size()) {
        const XmlToken& token = tokens[i];
        switch (token.kind) {
            case TokenKind::StartTag: {
                if (token.selfClosing) {
                    AppendLine(output, indent, token.text);
                    ++i;
                    break;
                }

                size_t textIndex = i + 1;
                while (textIndex < tokens.size() &&
                       tokens[textIndex].kind == TokenKind::Text &&
                       tokens[textIndex].whitespaceOnly) {
                    ++textIndex;
                }

                if (textIndex + 1 < tokens.size() &&
                    tokens[textIndex].kind == TokenKind::Text &&
                    !tokens[textIndex].whitespaceOnly &&
                    tokens[textIndex + 1].kind == TokenKind::EndTag &&
                    tokens[textIndex + 1].name == token.name) {
                    AppendLine(output, indent,
                               token.text + Trimmed(tokens[textIndex].text) +
                                   tokens[textIndex + 1].text);
                    i = textIndex + 2;
                    break;
                }

                AppendLine(output, indent, token.text);
                stack.push_back(token.name);
                ++indent;
                ++i;
                break;
            }
            case TokenKind::EndTag:
                if (stack.empty() || stack.back() != token.name) {
                    return MakeError(L"XML 结束标签与开始标签不匹配：" + token.name);
                }
                stack.pop_back();
                if (indent > 0) {
                    --indent;
                }
                AppendLine(output, indent, token.text);
                ++i;
                break;
            case TokenKind::Text:
                if (!token.whitespaceOnly) {
                    AppendMultiline(output, indent, Trimmed(token.text));
                }
                ++i;
                break;
            case TokenKind::Comment:
            case TokenKind::ProcessingInstruction:
            case TokenKind::Doctype:
                AppendLine(output, indent, token.text);
                ++i;
                break;
            case TokenKind::CData:
                AppendMultiline(output, indent, token.text);
                ++i;
                break;
        }
    }

    if (!stack.empty()) {
        return MakeError(L"XML 标签未闭合：" + stack.back());
    }
    if (output.empty()) {
        return MakeError(L"XML 内容为空");
    }
    return output;
}

}  // namespace peres
