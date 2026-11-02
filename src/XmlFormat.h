#pragma once

#include "Result.h"

#include <string>
#include <string_view>

namespace peres {

// Pretty-prints well-formed XML into one element per line.  Text-only elements
// stay on a single line (<description>text</description>).
Result<std::wstring> FormatXmlText(std::wstring_view xml);

bool LooksLikeXml(std::wstring_view text);

}  // namespace peres
