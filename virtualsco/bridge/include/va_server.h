// Bridge server embedded in the virtual audio HAL. Owns the listening Unix
// socket, accepts the app client, and shuttles PCM between the socket and two
// SPSC rings the HAL's audio callbacks touch. The audio callbacks call only
// pull_uplink()/push_downlink(), which never block.
#ifndef VA_SERVER_H
#define VA_SERVER_H

#include <cstdint>

// Start listening at `socket_path` and spawn the accept/rx/tx threads.
// Returns false if the socket can't be created/bound. Idempotent-safe: call
// va_server_stop() before starting again.
bool va_server_start(const char* socket_path, uint32_t sample_rate, uint32_t frame_samples);

void va_server_stop();

// True once an app client has connected and completed the handshake.
bool va_server_client_connected();

// HAL input stream read(): fill `frame` with the next uplink frame (agent
// voice → becomes the target app's mic). Returns false if no data is buffered
// (caller should emit silence).
bool va_server_pull_uplink(int16_t* frame, int samples);

// HAL output stream write(): hand the target app's played-out audio (downlink)
// to the bridge for delivery to the app.
void va_server_push_downlink(const int16_t* frame, int samples);

#endif  // VA_SERVER_H
