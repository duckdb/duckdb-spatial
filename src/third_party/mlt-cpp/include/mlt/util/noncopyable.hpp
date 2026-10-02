#pragma once

namespace mlt::util {

class noncopyable {
public:
    noncopyable(noncopyable&) = delete;
    noncopyable(noncopyable&&) = default;
    noncopyable& operator=(const noncopyable&) = delete;
    noncopyable& operator=(noncopyable&&) noexcept = default;

protected:
    constexpr noncopyable() noexcept = default;
    ~noncopyable() noexcept = default;
};

} // namespace mlt::util
