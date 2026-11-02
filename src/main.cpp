#include "MainFrame.h"

CAppModule _Module;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return 1;
    }

    _Module.Init(nullptr, hInstance);
    CMessageLoop loop;
    _Module.AddMessageLoop(&loop);

    peres::ui::CMainFrame frame;
    if (frame.Create(nullptr, CWindow::rcDefault, L"eXe Res",
                     WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0) == nullptr) {
        _Module.RemoveMessageLoop();
        _Module.Term();
        ::CoUninitialize();
        return 1;
    }
    loop.AddMessageFilter(&frame);

    frame.ShowWindow(nCmdShow);
    frame.UpdateWindow();

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv != nullptr) {
        if (argc > 1) {
            frame.OpenFile(std::filesystem::path(argv[1]));
        }
        ::LocalFree(argv);
    }

    const int result = loop.Run();

    _Module.RemoveMessageLoop();
    _Module.Term();
    ::CoUninitialize();
    return result;
}
