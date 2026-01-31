#include "yamux/frame.hpp"

#include <algorithm>

namespace yamux {

namespace {

// Big-endian encoding helpers
inline void PutU16BE(uint8_t* buf, uint16_t val) {
  buf[0] = static_cast<uint8_t>(val >> 8);
  buf[1] = static_cast<uint8_t>(val);
}

inline void PutU32BE(uint8_t* buf, uint32_t val) {
  buf[0] = static_cast<uint8_t>(val >> 24);
  buf[1] = static_cast<uint8_t>(val >> 16);
  buf[2] = static_cast<uint8_t>(val >> 8);
  buf[3] = static_cast<uint8_t>(val);
}

inline uint16_t GetU16BE(const uint8_t* buf) {
  return (static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]);
}

inline uint32_t GetU32BE(const uint8_t* buf) {
  return (static_cast<uint32_t>(buf[0]) << 24) |
         (static_cast<uint32_t>(buf[1]) << 16) |
         (static_cast<uint32_t>(buf[2]) << 8) | static_cast<uint32_t>(buf[3]);
}

}  // namespace

void Header::Encode(uint8_t* buf) const {
  buf[0] = version;
  buf[1] = static_cast<uint8_t>(type);
  PutU16BE(buf + 2, static_cast<uint16_t>(flags));
  PutU32BE(buf + 4, stream_id);
  PutU32BE(buf + 8, length);
}

Result<Header> Header::Decode(const uint8_t* buf) {
  Header h;
  h.version = buf[0];
  h.type = static_cast<FrameType>(buf[1]);
  h.flags = static_cast<Flags>(GetU16BE(buf + 2));
  h.stream_id = GetU32BE(buf + 4);
  h.length = GetU32BE(buf + 8);

  Error err = h.Validate();
  if (err != Error::OK) {
    return {h, err};
  }
  return {h, Error::OK};
}

Error Header::Validate() const {
  if (version != kVersion) {
    return Error::InvalidVersion;
  }
  if (static_cast<uint8_t>(type) > 3) {
    return Error::InvalidFrameType;
  }
  return Error::OK;
}

Frame Frame::Data(StreamID id, Flags flags, const uint8_t* data, size_t len) {
  Frame f;
  f.header.version = kVersion;
  f.header.type = FrameType::Data;
  f.header.flags = flags;
  f.header.stream_id = id;
  f.header.length = static_cast<uint32_t>(len);
  if (data && len > 0) {
    f.payload.assign(data, data + len);
  }
  return f;
}

Frame Frame::WindowUpdate(StreamID id, Flags flags, uint32_t delta) {
  Frame f;
  f.header.version = kVersion;
  f.header.type = FrameType::WindowUpdate;
  f.header.flags = flags;
  f.header.stream_id = id;
  f.header.length = delta;
  return f;
}

Frame Frame::Ping(uint32_t ping_id, bool is_response) {
  Frame f;
  f.header.version = kVersion;
  f.header.type = FrameType::Ping;
  f.header.flags = is_response ? Flags::ACK : Flags::None;
  f.header.stream_id = 0;
  f.header.length = ping_id;
  return f;
}

Frame Frame::GoAway(GoAwayCode code) {
  Frame f;
  f.header.version = kVersion;
  f.header.type = FrameType::GoAway;
  f.header.flags = Flags::None;
  f.header.stream_id = 0;
  f.header.length = static_cast<uint32_t>(code);
  return f;
}

std::vector<uint8_t> Frame::Encode() const {
  std::vector<uint8_t> buf(kHeaderSize + payload.size());
  header.Encode(buf.data());
  if (!payload.empty()) {
    std::memcpy(buf.data() + kHeaderSize, payload.data(), payload.size());
  }
  return buf;
}

size_t FrameReader::Feed(const uint8_t* data, size_t len) {
  if (state_ == State::Complete || state_ == State::Error) {
    return 0;
  }

  size_t consumed = 0;

  while (consumed < len) {
    if (state_ == State::ReadingHeader) {
      // Read header bytes
      size_t need = kHeaderSize - header_buf_.size();
      size_t take = std::min(need, len - consumed);
      header_buf_.insert(header_buf_.end(), data + consumed,
                         data + consumed + take);
      consumed += take;

      if (header_buf_.size() == kHeaderSize) {
        // Parse header
        auto result = Header::Decode(header_buf_.data());
        if (result.error != Error::OK) {
          error_ = result.error;
          state_ = State::Error;
          return consumed;
        }
        header_ = result.value;

        if (header_.type == FrameType::Data && header_.length > 0) {
          payload_buf_.reserve(header_.length);
          state_ = State::ReadingPayload;
        } else {
          // No payload needed
          state_ = State::Complete;
          return consumed;
        }
      }
    } else if (state_ == State::ReadingPayload) {
      // Read payload bytes
      size_t need = header_.length - payload_buf_.size();
      size_t take = std::min(need, len - consumed);
      payload_buf_.insert(payload_buf_.end(), data + consumed,
                          data + consumed + take);
      consumed += take;

      if (payload_buf_.size() == header_.length) {
        state_ = State::Complete;
        return consumed;
      }
    } else {
      break;
    }
  }

  return consumed;
}

Frame FrameReader::TakeFrame() {
  Frame f;
  f.header = header_;
  f.payload = std::move(payload_buf_);
  Reset();
  return f;
}

void FrameReader::Reset() {
  state_ = State::ReadingHeader;
  error_ = Error::OK;
  header_buf_.clear();
  payload_buf_.clear();
}

}  // namespace yamux
