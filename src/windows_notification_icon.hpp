#pragma once

#include <cstdint>
#include <memory>

namespace nrpc {

class WindowsNotificationIcon {
public:
    explicit WindowsNotificationIcon(std::uint16_t port);
    ~WindowsNotificationIcon();

    WindowsNotificationIcon(const WindowsNotificationIcon&) = delete;
    WindowsNotificationIcon& operator=(const WindowsNotificationIcon&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nrpc
