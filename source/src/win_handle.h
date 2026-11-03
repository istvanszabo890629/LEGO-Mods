#pragma once
#include <windows.h>
#include <utility>

namespace batman_mods {

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~UniqueHandle() { if (*this) CloseHandle(value_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (*this) CloseHandle(value_);
            value_ = other.release();
        }
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept { return std::exchange(value_, INVALID_HANDLE_VALUE); }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

} // namespace batman_mods
