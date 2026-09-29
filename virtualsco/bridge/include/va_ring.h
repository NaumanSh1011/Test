// Single-producer / single-consumer lock-free byte ring. Decouples the HAL's
// real-time audio callbacks (which must never block) from the bridge socket
// I/O threads. One producer thread and one consumer thread per instance.
#ifndef VA_RING_H
#define VA_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class VaRing {
public:
    explicit VaRing(size_t capacity_bytes)
        : buf_(capacity_bytes + 1), cap_(capacity_bytes + 1) {}

    // Copies up to n bytes in; returns bytes actually written (short on full).
    size_t write(const uint8_t* src, size_t n);

    // Copies up to n bytes out; returns bytes actually read (short on empty).
    size_t read(uint8_t* dst, size_t n);

    size_t available() const;   // bytes ready to read
    size_t free_space() const;  // bytes writable; call from the producer side only

    // Discard all currently-readable bytes. CONSUMER-SIDE ONLY: call from the same thread that calls
    // read(), so it never races the consumer's own tail_ update. Used to flush a previous call's
    // leftover audio when a new client connects, so stale frames aren't served to the new call.
    void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<uint8_t> buf_;
    size_t cap_;
    std::atomic<size_t> head_{0};  // write index (producer)
    std::atomic<size_t> tail_{0};  // read index (consumer)
};

#endif  // VA_RING_H
