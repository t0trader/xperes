#pragma once

#include <expected>
#include <string>
#include <utility>

namespace peres {

using Error = std::wstring;

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> MakeError(std::wstring message) {
    return std::unexpected<Error>(std::move(message));
}

}  // namespace peres
