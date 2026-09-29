// Bridge server implementation. Portable POSIX (no AOSP headers), so it is
// compiled into the HAL .so under hal/CMakeLists.txt. Threading:
//   accept thread : waits for the app client, sends the hello
//   rx thread     : socket -> uplink ring   (agent voice for the target mic)
//   tx thread     : downlink ring -> socket (target playback for the app)
// The HAL audio callbacks only touch the rings via pull_uplink/push_downlink.
#include "va_server.h"
#include "va_ring.h"
#include "va_protocol.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VaServer", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "VaServer", __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) do { fprintf(stderr, "[VaServer] " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOGW LOGI
#endif

namespace {

struct Server {
    int listen_fd = -1;
    std::atomic<int> client_fd{-1};
    std::atomic<bool> running{false};
    uint32_t sample_rate = 0;
    uint32_t frame_samples = 0;
    size_t frame_bytes = 0;

    // ~500 ms of buffering each way absorbs socket jitter without adding much latency.
    std::unique_ptr<VaRing> uplink;    // socket -> HAL
    std::unique_ptr<VaRing> downlink;  // HAL -> socket

    // Set when a new client connects; each ring's CONSUMER drains its ring once, so a previous
    // call's leftover audio (up to ~500 ms) isn't served into the new call. Consumers: the HAL
    // (pull_uplink) for uplink, the tx thread for downlink.
    std::atomic<bool> flush_uplink{false};
    std::atomic<bool> flush_downlink{false};

    std::thread accept_th, rx_th, tx_th;
};

Server g;

bool read_full(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n) { ssize_t r = ::read(fd, p, n); if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; } p += r; n -= r; }
    return true;
}
bool write_full(int fd, const void* buf, size_t n) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (n) { ssize_t w = ::write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; return false; } p += w; n -= w; }
    return true;
}

// Only forget the client if it is still the one that failed: after a reconnect
// the other pump may already be serving the new fd.
void drop_client(int failed_fd) {
    g.client_fd.compare_exchange_strong(failed_fd, -1);
}

// Write a whole frame or none. A short ring write would split a frame and
// knock every later int16 off its byte alignment (heard as loud noise).
void push_whole(VaRing& ring, const uint8_t* data, size_t n) {
    if (ring.free_space() >= n) ring.write(data, n);
}

void rx_loop() {
    std::vector<uint8_t> frame(g.frame_bytes);
    while (g.running) {
        int fd = g.client_fd.load();
        if (fd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        if (!read_full(fd, frame.data(), frame.size())) { drop_client(fd); continue; }
        push_whole(*g.uplink, frame.data(), frame.size());  // full ring: drop newest frame
    }
}

void tx_loop() {
    std::vector<uint8_t> frame(g.frame_bytes);
    while (g.running) {
        // Consumer-side drain of the downlink ring on reconnect, before sending anything to the new
        // client — otherwise the new call's app would receive the tail of the previous call.
        if (g.flush_downlink.exchange(false)) g.downlink->clear();
        int fd = g.client_fd.load();
        if (fd < 0 || g.downlink->available() < g.frame_bytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        g.downlink->read(frame.data(), frame.size());
        if (!write_full(fd, frame.data(), frame.size())) drop_client(fd);
    }
}

void accept_loop() {
    while (g.running) {
        int fd = ::accept(g.listen_fd, nullptr, nullptr);
        if (fd < 0) { if (g.running) LOGW("accept: %s", strerror(errno)); continue; }
        va_hello hello{VA_MAGIC, VA_VERSION, g.sample_rate, g.frame_samples};
        if (!write_full(fd, &hello, sizeof(hello))) { ::close(fd); continue; }
        // Ask both consumers to drain their rings before serving this new client, so leftover audio
        // from the previous call isn't heard at the start of the new one. Set before publishing the
        // new fd so the consumers see the flag no later than the new client.
        g.flush_uplink.store(true);
        g.flush_downlink.store(true);
        int old = g.client_fd.exchange(fd);
        // close() alone does not wake rx/tx threads blocked in read()/write() on
        // `old`; they'd stay wedged there and never serve the new client.
        if (old >= 0) { ::shutdown(old, SHUT_RDWR); ::close(old); }
        LOGI("client connected fd=%d", fd);
    }
}

}  // namespace

bool va_server_start(const char* socket_path, uint32_t sample_rate, uint32_t frame_samples) {
    va_server_stop();
    g.sample_rate = sample_rate;
    g.frame_samples = frame_samples;
    g.frame_bytes = static_cast<size_t>(frame_samples) * sizeof(int16_t);
    const size_t cap = g.frame_bytes * 25;  // ~500ms @ 20ms frames
    g.uplink = std::make_unique<VaRing>(cap);
    g.downlink = std::make_unique<VaRing>(cap);

    g.listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (g.listen_fd < 0) { LOGW("socket: %s", strerror(errno)); return false; }
    sockaddr_un addr{};
    int alen = va_fill_addr(&addr, socket_path);
    if (alen < 0) { LOGW("socket path too long: %s", socket_path); ::close(g.listen_fd); g.listen_fd = -1; return false; }
    if (socket_path[0] != '@') ::unlink(socket_path);  // abstract sockets have no file
    if (::bind(g.listen_fd, reinterpret_cast<sockaddr*>(&addr), alen) < 0 ||
        ::listen(g.listen_fd, 1) < 0) {
        LOGW("bind/listen %s: %s", socket_path, strerror(errno));
        ::close(g.listen_fd); g.listen_fd = -1;
        return false;
    }

    g.running = true;
    g.accept_th = std::thread(accept_loop);
    g.rx_th = std::thread(rx_loop);
    g.tx_th = std::thread(tx_loop);
    LOGI("listening on %s rate=%u frame=%u", socket_path, sample_rate, frame_samples);
    return true;
}

void va_server_stop() {
    if (!g.running.exchange(false) && g.listen_fd < 0) return;
    if (g.listen_fd >= 0) { ::shutdown(g.listen_fd, SHUT_RDWR); ::close(g.listen_fd); g.listen_fd = -1; }
    int fd = g.client_fd.exchange(-1);
    if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
    if (g.accept_th.joinable()) g.accept_th.join();
    if (g.rx_th.joinable()) g.rx_th.join();
    if (g.tx_th.joinable()) g.tx_th.join();
    g.uplink.reset();
    g.downlink.reset();
}

bool va_server_client_connected() { return g.client_fd.load() >= 0; }

bool va_server_pull_uplink(int16_t* frame, int samples) {
    if (!g.uplink) return false;
    // Consumer-side drain on reconnect: discard a previous call's leftover uplink so it isn't served
    // as the new call's microphone. Safe here — this is the uplink ring's only consumer.
    if (g.flush_uplink.exchange(false)) g.uplink->clear();
    const size_t bytes = static_cast<size_t>(samples) * sizeof(int16_t);
    if (g.uplink->available() < bytes) return false;
    return g.uplink->read(reinterpret_cast<uint8_t*>(frame), bytes) == bytes;
}

void va_server_push_downlink(const int16_t* frame, int samples) {
    if (!g.downlink) return;
    push_whole(*g.downlink, reinterpret_cast<const uint8_t*>(frame),
               static_cast<size_t>(samples) * sizeof(int16_t));
}
