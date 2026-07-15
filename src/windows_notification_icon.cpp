#include "windows_notification_icon.hpp"

#include <windows.h>
#include <shellapi.h>

#include <future>
#include <string>
#include <thread>

#include "../resources/resource.h"

namespace nrpc {

namespace {

constexpr wchar_t window_class_name[] = L"nRPCServerNotificationIcon";
constexpr UINT icon_identifier = 1;

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

class WindowsNotificationIcon::Impl {
public:
    explicit Impl(std::uint16_t port) {
        std::promise<HWND> window_created;
        auto window_future = window_created.get_future();
        thread_ = std::thread([this, port, created = std::move(window_created)]() mutable {
            run(port, std::move(created));
        });
        window_ = window_future.get();
    }

    ~Impl() {
        if (window_ != nullptr) {
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    static void run(std::uint16_t port, std::promise<HWND> created) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{
            .cbSize = sizeof(WNDCLASSEXW),
            .lpfnWndProc = window_procedure,
            .hInstance = instance,
            .lpszClassName = window_class_name,
        };
        RegisterClassExW(&window_class);

        const HWND window = CreateWindowExW(
            0, window_class_name, L"nRPC Server", 0, 0, 0, 0, 0, nullptr,
            nullptr, instance, nullptr);
        if (window == nullptr) {
            created.set_value(nullptr);
            return;
        }

        NOTIFYICONDATAW icon_data{
            .cbSize = sizeof(NOTIFYICONDATAW),
            .hWnd = window,
            .uID = icon_identifier,
            .uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP,
            .hIcon = static_cast<HICON>(LoadImageW(
                instance, MAKEINTRESOURCEW(IDI_NRPC_SERVER), IMAGE_ICON, 0, 0,
                LR_DEFAULTSIZE | LR_SHARED)),
        };
        const std::wstring tooltip =
            L"nRPC Server - listening on port " + std::to_wstring(port);
        wcsncpy_s(icon_data.szTip, tooltip.c_str(), _TRUNCATE);

        const bool icon_added =
            icon_data.hIcon != nullptr && Shell_NotifyIconW(NIM_ADD, &icon_data);
        if (icon_added) {
            icon_data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &icon_data);
        }
        created.set_value(window);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (icon_added) {
            Shell_NotifyIconW(NIM_DELETE, &icon_data);
        }
        UnregisterClassW(window_class_name, instance);
    }

    HWND window_{};
    std::thread thread_;
};

WindowsNotificationIcon::WindowsNotificationIcon(std::uint16_t port)
    : impl_(std::make_unique<Impl>(port)) {}

WindowsNotificationIcon::~WindowsNotificationIcon() = default;

}  // namespace nrpc
