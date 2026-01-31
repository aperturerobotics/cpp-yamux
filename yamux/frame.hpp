#ifndef YAMUX_FRAME_HPP
#define YAMUX_FRAME_HPP

#include <cstdint>
#include <cstring>
#include <vector>

#include "yamux/errors.hpp"
#include "yamux/types.hpp"

namespace yamux {

// Frame header (12 bytes)
struct Header {
  uint8_t version;
  FrameType type;
  Flags flags;
  StreamID stream_id;
  uint32_t length;

  // Encode header to buffer (must be at least kHeaderSize bytes)
  void Encode(uint8_t* buf) const;

  // Decode header from buffer (must be at least kHeaderSize bytes)
  static Result<Header> Decode(const uint8_t* buf);

  // Validate header
  Error Validate() const;
};

// Complete frame with header and payload
struct Frame {
  Header header;
  std::vector<uint8_t> payload;

  // Create a Data frame
  static Frame Data(StreamID id, Flags flags, const uint8_t* data, size_t len);

  // Create a WindowUpdate frame
  static Frame WindowUpdate(StreamID id, Flags flags, uint32_t delta);

  // Create a Ping frame
  static Frame Ping(uint32_t ping_id, bool is_response);

  // Create a GoAway frame
  static Frame GoAway(GoAwayCode code);

  // Encode entire frame (header + payload)
  std::vector<uint8_t> Encode() const;

  // Get total size of encoded frame
  size_t EncodedSize() const { return kHeaderSize + payload.size(); }
};

// Incremental frame reader for parsing frames from a byte stream
class FrameReader {
 public:
  FrameReader() = default;

  // Feed data to the reader. Returns number of bytes consumed.
  size_t Feed(const uint8_t* data, size_t len);

  // Check if a complete frame is available
  bool HasFrame() const { return state_ == State::Complete; }

  // Get the parsed frame (only valid when HasFrame() returns true)
  Frame TakeFrame();

  // Reset reader state
  void Reset();

  // Get any parsing error
  Error GetError() const { return error_; }

 private:
  enum class State { ReadingHeader, ReadingPayload, Complete, Error };

  State state_ = State::ReadingHeader;
  Error error_ = Error::OK;

  std::vector<uint8_t> header_buf_;
  Header header_;
  std::vector<uint8_t> payload_buf_;
};

}  // namespace yamux

#endif  // YAMUX_FRAME_HPP
