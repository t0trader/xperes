#include "ProcessRunner.h"

#include "TextUtil.h"

#include <windows.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>

namespace peres {

namespace {

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

}  // namespace

std::wstring QuoteWindowsArgument(std::wstring_view argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    const bool needsQuotes =
        std::any_of(argument.begin(), argument.end(), [](wchar_t ch) {
            return ch == L' ' || ch == L'\t' || ch == L'"';
        });
    if (!needsQuotes) {
        return std::wstring(argument);
    }

    std::wstring result;
    result.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

Result<ProcessResult> RunProcessCapture(const std::wstring& commandLine, uint32_t timeoutMs) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &security, 0)) {
        return MakeError(L"创建进程输出管道失败：" + FormatWin32Error(::GetLastError()));
    }
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nulInput = ::CreateFileW(L"NUL", GENERIC_READ, 0, &security, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nulInput != INVALID_HANDLE_VALUE ? nulInput : nullptr;

    std::wstring mutableCommand = commandLine;
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    ::CloseHandle(writePipe);
    writePipe = nullptr;
    if (!created) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(readPipe);
        if (nulInput != INVALID_HANDLE_VALUE) {
            ::CloseHandle(nulInput);
        }
        return MakeError(L"启动进程失败：" + FormatWin32Error(error));
    }
    if (nulInput != INVALID_HANDLE_VALUE) {
        ::CloseHandle(nulInput);
    }

    ProcessResult result;
    constexpr size_t kMaxStoredOutput = 4u * 1024u * 1024u;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool timedOut = false;

    while (true) {
        DWORD available = 0;
        const BOOL peek = ::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr);
        if (!peek) {
            break;
        }
        if (available != 0) {
            const DWORD toRead = std::min<DWORD>(available, 64 * 1024);
            char buffer[64 * 1024];
            DWORD read = 0;
            if (!::ReadFile(readPipe, buffer, toRead, &read, nullptr) || read == 0) {
                break;
            }
            if (result.output.size() < kMaxStoredOutput) {
                const size_t room = kMaxStoredOutput - result.output.size();
                result.output.append(buffer, std::min<size_t>(read, room));
            }
            continue;
        }

        if (::WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            ::TerminateProcess(process.hProcess, 1);
            ::WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (timedOut) {
        ::CloseHandle(readPipe);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return MakeError(Utf8ToWide(fmt::format("进程超时（{} ms）", timeoutMs)));
    }

    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    ::CloseHandle(readPipe);

    result.exitCode = exitCode;
    return result;
}

}  // namespace peres
