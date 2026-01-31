#ifndef YAMUX_TYPES_HPP
#define YAMUX_TYPES_HPP

#include <cstdint>

namespace yamux {

// Protocol version
constexpr uint8_t kVersion = 0;

// Header size in bytes
constexpr size_t kHeaderSize = 12;

// Initial flow control window size (256 KiB)
constexpr uint32_t kInitialWindowSize = 256 * 1024;

// Maximum frame payload size
constexpr uint32_t kMaxFrameSize = 1024 * 1024; // 1 MiB

// Stream ID type
using StreamID = uint32_t;

// Frame types
enum class FrameType : uint8_t {
  Data = 0,
  WindowUpdate = 1,
  Ping = 2,
  GoAway = 3,
};

// Frame flags (can be combined)
enum class Flags : uint16_t {
  None = 0x0,
  SYN = 0x1, // Start of new stream
  ACK = 0x2, // Acknowledge stream start
  FIN = 0x4, // Half-close (no more data from sender)
  RST = 0x8, // Reset stream immediately
};

// Bitwise operators for Flags
inline Flags operator|(Flags a, Flags b) {
  return static_cast<Flags>(static_cast<uint16_t>(a) |
                            static_cast<uint16_t>(b));
}

inline Flags operator&(Flags a, Flags b) {
  return static_cast<Flags>(static_cast<uint16_t>(a) &
                            static_cast<uint16_t>(b));
}

inline Flags &operator|=(Flags &a, Flags b) {
  a = a | b;
  return a;
}

inline bool HasFlag(Flags flags, Flags flag) {
  return (static_cast<uint16_t>(flags) & static_cast<uint16_t>(flag)) != 0;
}

// GoAway error codes
enum class GoAwayCode : uint32_t {
  Normal = 0,
  ProtocolError = 1,
  InternalError = 2,
};

// Stream states
enum class StreamState {
  Init,        // Initial state
  SYNSent,     // SYN sent, waiting for ACK
  SYNReceived, // SYN received, need to send ACK
  Established, // Connection established
  LocalClose,  // We sent FIN
  RemoteClose, // We received FIN
  Closed,      // Both sides closed (FIN exchanged)
  Reset,       // Stream was reset
};

// Convert stream state to string for debugging
inline const char *StreamStateString(StreamState state) {
  switch (state) {
  case StreamState::Init:
    return "Init";
  case StreamState::SYNSent:
    return "SYNSent";
  case StreamState::SYNReceived:
    return "SYNReceived";
  case StreamState::Established:
    return "Established";
  case StreamState::LocalClose:
    return "LocalClose";
  case StreamState::RemoteClose:
    return "RemoteClose";
  case StreamState::Closed:
    return "Closed";
  case StreamState::Reset:
    return "Reset";
  default:
    return "Unknown";
  }
}

// Check if stream ID is client-initiated (odd)
inline bool IsClientStream(StreamID id) { return (id & 1) == 1; }

// Check if stream ID is server-initiated (even, non-zero)
inline bool IsServerStream(StreamID id) { return id != 0 && (id & 1) == 0; }

} // namespace yamux

#endif // YAMUX_TYPES_HPP
