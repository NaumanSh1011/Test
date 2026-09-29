#include "va_ring.h"

#include <algorithm>
#include <cstring>

size_t VaRing::write(const uint8_t* src, size_t n) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t free_space = (tail + cap_ - head - 1) % cap_;
    n = std::min(n, free_space);

    const size_t first = std::min(n, cap_ - head);
    std::memcpy(&buf_[head], src, first);
    if (n > first) std::memcpy(&buf_[0], src + first, n - first);

    head_.store((head + n) % cap_, std::memory_order_release);
    return n;
}

size_t VaRing::read(uint8_t* dst, size_t n) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t ready = (head + cap_ - tail) % cap_;
    n = std::min(n, ready);

    const size_t first = std::min(n, cap_ - tail);
    std::memcpy(dst, &buf_[tail], first);
    if (n > first) std::memcpy(dst + first, &buf_[0], n - first);

    tail_.store((tail + n) % cap_, std::memory_order_release);
    return n;
}

size_t VaRing::free_space() const {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return (tail + cap_ - head - 1) % cap_;
}

size_t VaRing::available() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    return (head + cap_ - tail) % cap_;
}
