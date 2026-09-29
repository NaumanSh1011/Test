// Shared wire protocol between the app-side JNI client and the virtual audio
// HAL's bridge server. Pure C so both the NDK client and an AOSP/vendor HAL
// build can include it unchanged. Mirror any change in NativeAudioBridge.kt.
#ifndef VA_PROTOCOL_H
#define VA_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifdef __cplusplus
extern "C" {
#endif

// Socket address the HAL binds and the app connects to. A leading '@' selects
// the abstract namespace (no filesystem entry) — chosen so the HAL, running in
// the mtk_hal_audio domain, can bind without write access to /dev/socket, and
// only a single SELinux connectto rule is needed (see magisk-module/sepolicy.rule).
#define VA_SOCKET_PATH "@virtual_audio"

// Fill a sockaddr_un for `path`, supporting the '@' abstract convention.
// Returns the addrlen to pass to bind()/connect(), or -1 on overflow.
static inline int va_fill_addr(struct sockaddr_un* addr, const char* path) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (path[0] == '@') {
        if (n > sizeof(addr->sun_path)) return -1;
        addr->sun_path[0] = '\0';                 // abstract namespace
        memcpy(addr->sun_path + 1, path + 1, n - 1);
        return (int)(offsetof(struct sockaddr_un, sun_path) + n);
    }
    if (n >= sizeof(addr->sun_path)) return -1;
    memcpy(addr->sun_path, path, n);
    return (int)sizeof(struct sockaddr_un);
}

#define VA_MAGIC   0x56414244u  // 'VABD'
#define VA_VERSION 1u

// Full-duplex framing over one stream socket:
//   - client -> server payload  == uplink  (agent voice, becomes target's mic)
//   - server -> client payload  == downlink (target's playback, tapped by HAL)
// After connect the server sends exactly one va_hello, then both sides exchange
// raw little-endian int16 mono frames of `frame_samples` samples each.
typedef struct va_hello {
    uint32_t magic;         // VA_MAGIC
    uint32_t version;       // VA_VERSION
    uint32_t sample_rate;   // e.g. 16000
    uint32_t frame_samples; // samples per mono frame (e.g. 320 for 20ms@16k)
} va_hello;

#ifdef __cplusplus
}
#endif

#endif  // VA_PROTOCOL_H
