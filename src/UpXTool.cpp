#include "UpXTool.h"

#include "ProcessRunner.h"
#include "TextUtil.h"

#include <fmt/format.h>

namespace peres {

Result<UnpackResult> UnpackWithUpx(const std::filesystem::path& inputPath,
                                   const std::filesystem::path& upxPath,
                                   const std::filesystem::path& outputPath,
                                   uint32_t timeoutMs) {
    if (!std::filesystem::exists(inputPath)) {
        return MakeError(L"输入文件不存在：" + inputPath.wstring());
    }
    if (!std::filesystem::exists(upxPath)) {
        return MakeError(L"UPX 可执行文件不存在：" + upxPath.wstring() +
                         L"\n请检查 peres.ini 中的 [upx] path。");
    }

    std::filesystem::path finalOutput = outputPath;
    std::error_code ec;
    if (std::filesystem::exists(finalOutput)) {
        std::filesystem::remove(finalOutput, ec);
        if (ec) {
            return MakeError(L"无法删除旧的脱壳输出文件：" + finalOutput.wstring());
        }
    }

    const std::wstring command = QuoteWindowsArgument(upxPath.wstring()) + L" -d -o " +
                                 QuoteWindowsArgument(finalOutput.wstring()) + L" " +
                                 QuoteWindowsArgument(inputPath.wstring());

    auto process = RunProcessCapture(command, timeoutMs);
    if (!process) {
        return std::unexpected(process.error());
    }
    if (process->exitCode != 0) {
        const std::wstring output = process->output.empty()
                                        ? L"(UPX 没有输出)"
                                        : Utf8ToWide(process->output);
        return MakeError(Utf8ToWide(fmt::format("UPX 脱壳失败，退出码 {}。\n{}",
                                                process->exitCode,
                                                WideToUtf8(output))));
    }

    if (!std::filesystem::exists(finalOutput, ec) || ec) {
        return MakeError(L"UPX 返回成功，但输出文件不存在：" + finalOutput.wstring());
    }
    if (std::filesystem::file_size(finalOutput, ec) == 0 || ec) {
        return MakeError(L"UPX 输出文件为空：" + finalOutput.wstring());
    }

    return UnpackResult{finalOutput, process->output};
}

}  // namespace peres
