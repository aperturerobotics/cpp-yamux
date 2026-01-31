#include "test_framework.hpp"

#include "yamux/frame.hpp"

#include <cstring>

TEST(HeaderEncodeDecode) {
  yamux::Header h;
  h.version = yamux::kVersion;
  h.type = yamux::FrameType::Data;
  h.flags = yamux::Flags::SYN;
  h.stream_id = 0x12345678;
  h.length = 0xABCDEF01;

  uint8_t buf[yamux::kHeaderSize];
  h.Encode(buf);

  // Check big-endian encoding
  ASSERT_EQ(buf[0], 0);     // version
  ASSERT_EQ(buf[1], 0);     // type (Data)
  ASSERT_EQ(buf[2], 0);     // flags high byte
  ASSERT_EQ(buf[3], 1);     // flags low byte (SYN)
  ASSERT_EQ(buf[4], 0x12);  // stream_id
  ASSERT_EQ(buf[5], 0x34);
  ASSERT_EQ(buf[6], 0x56);
  ASSERT_EQ(buf[7], 0x78);
  ASSERT_EQ(buf[8], 0xAB);  // length
  ASSERT_EQ(buf[9], 0xCD);
  ASSERT_EQ(buf[10], 0xEF);
  ASSERT_EQ(buf[11], 0x01);

  auto result = yamux::Header::Decode(buf);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value.version, yamux::kVersion);
  ASSERT_EQ(result.value.type, yamux::FrameType::Data);
  ASSERT_EQ(result.value.flags, yamux::Flags::SYN);
  ASSERT_EQ(result.value.stream_id, 0x12345678u);
  ASSERT_EQ(result.value.length, 0xABCDEF01u);
}

TEST(HeaderInvalidVersion) {
  uint8_t buf[yamux::kHeaderSize] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  auto result = yamux::Header::Decode(buf);
  ASSERT_EQ(result.error, yamux::Error::InvalidVersion);
}

TEST(HeaderInvalidFrameType) {
  uint8_t buf[yamux::kHeaderSize] = {0, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  auto result = yamux::Header::Decode(buf);
  ASSERT_EQ(result.error, yamux::Error::InvalidFrameType);
}

TEST(FrameDataCreate) {
  uint8_t data[] = {1, 2, 3, 4, 5};
  auto frame = yamux::Frame::Data(123, yamux::Flags::None, data, 5);

  ASSERT_EQ(frame.header.type, yamux::FrameType::Data);
  ASSERT_EQ(frame.header.stream_id, 123u);
  ASSERT_EQ(frame.header.length, 5u);
  ASSERT_EQ(frame.payload.size(), 5u);
  ASSERT_TRUE(std::memcmp(frame.payload.data(), data, 5) == 0);
}

TEST(FrameWindowUpdateCreate) {
  auto frame = yamux::Frame::WindowUpdate(42, yamux::Flags::SYN, 1000);

  ASSERT_EQ(frame.header.type, yamux::FrameType::WindowUpdate);
  ASSERT_EQ(frame.header.stream_id, 42u);
  ASSERT_EQ(frame.header.flags, yamux::Flags::SYN);
  ASSERT_EQ(frame.header.length, 1000u);
  ASSERT_TRUE(frame.payload.empty());
}

TEST(FramePingCreate) {
  auto ping = yamux::Frame::Ping(12345, false);
  ASSERT_EQ(ping.header.type, yamux::FrameType::Ping);
  ASSERT_EQ(ping.header.stream_id, 0u);
  ASSERT_EQ(ping.header.flags, yamux::Flags::None);
  ASSERT_EQ(ping.header.length, 12345u);

  auto pong = yamux::Frame::Ping(12345, true);
  ASSERT_EQ(pong.header.flags, yamux::Flags::ACK);
}

TEST(FrameGoAwayCreate) {
  auto frame = yamux::Frame::GoAway(yamux::GoAwayCode::ProtocolError);
  ASSERT_EQ(frame.header.type, yamux::FrameType::GoAway);
  ASSERT_EQ(frame.header.stream_id, 0u);
  ASSERT_EQ(frame.header.length,
            static_cast<uint32_t>(yamux::GoAwayCode::ProtocolError));
}

TEST(FrameEncode) {
  uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  auto frame = yamux::Frame::Data(1, yamux::Flags::FIN, data, 4);
  auto encoded = frame.Encode();

  ASSERT_EQ(encoded.size(), yamux::kHeaderSize + 4);
  // Check payload at end
  ASSERT_EQ(encoded[12], 0xDE);
  ASSERT_EQ(encoded[13], 0xAD);
  ASSERT_EQ(encoded[14], 0xBE);
  ASSERT_EQ(encoded[15], 0xEF);
}

TEST(FrameReaderSimple) {
  auto frame = yamux::Frame::Data(1, yamux::Flags::None, nullptr, 0);
  auto encoded = frame.Encode();

  yamux::FrameReader reader;
  size_t consumed = reader.Feed(encoded.data(), encoded.size());

  ASSERT_EQ(consumed, encoded.size());
  ASSERT_TRUE(reader.HasFrame());
  ASSERT_EQ(reader.GetError(), yamux::Error::OK);

  auto parsed = reader.TakeFrame();
  ASSERT_EQ(parsed.header.type, yamux::FrameType::Data);
  ASSERT_EQ(parsed.header.stream_id, 1u);
}

TEST(FrameReaderWithPayload) {
  uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
  auto frame = yamux::Frame::Data(99, yamux::Flags::SYN, data, 8);
  auto encoded = frame.Encode();

  yamux::FrameReader reader;
  size_t consumed = reader.Feed(encoded.data(), encoded.size());

  ASSERT_EQ(consumed, encoded.size());
  ASSERT_TRUE(reader.HasFrame());

  auto parsed = reader.TakeFrame();
  ASSERT_EQ(parsed.header.stream_id, 99u);
  ASSERT_EQ(parsed.payload.size(), 8u);
  ASSERT_TRUE(std::memcmp(parsed.payload.data(), data, 8) == 0);
}

TEST(FrameReaderIncremental) {
  uint8_t data[] = {1, 2, 3, 4};
  auto frame = yamux::Frame::Data(1, yamux::Flags::None, data, 4);
  auto encoded = frame.Encode();

  yamux::FrameReader reader;

  // Feed one byte at a time
  for (size_t i = 0; i < encoded.size() - 1; i++) {
    size_t consumed = reader.Feed(encoded.data() + i, 1);
    ASSERT_EQ(consumed, 1u);
    ASSERT_TRUE(!reader.HasFrame());
  }

  // Feed last byte
  size_t consumed = reader.Feed(encoded.data() + encoded.size() - 1, 1);
  ASSERT_EQ(consumed, 1u);
  ASSERT_TRUE(reader.HasFrame());

  auto parsed = reader.TakeFrame();
  ASSERT_EQ(parsed.payload.size(), 4u);
}

TEST(FrameReaderMultipleFrames) {
  auto frame1 = yamux::Frame::Ping(1, false);
  auto frame2 = yamux::Frame::Ping(2, true);
  auto enc1 = frame1.Encode();
  auto enc2 = frame2.Encode();

  std::vector<uint8_t> combined;
  combined.insert(combined.end(), enc1.begin(), enc1.end());
  combined.insert(combined.end(), enc2.begin(), enc2.end());

  yamux::FrameReader reader;
  size_t consumed = reader.Feed(combined.data(), combined.size());

  // Should parse first frame
  ASSERT_EQ(consumed, enc1.size());
  ASSERT_TRUE(reader.HasFrame());

  auto parsed1 = reader.TakeFrame();
  ASSERT_EQ(parsed1.header.length, 1u);  // ping ID

  // Feed remaining for second frame
  consumed = reader.Feed(combined.data() + enc1.size(), enc2.size());
  ASSERT_EQ(consumed, enc2.size());
  ASSERT_TRUE(reader.HasFrame());

  auto parsed2 = reader.TakeFrame();
  ASSERT_EQ(parsed2.header.length, 2u);  // ping ID
  ASSERT_EQ(parsed2.header.flags, yamux::Flags::ACK);
}

TEST(FlagsOperators) {
  auto flags = yamux::Flags::SYN | yamux::Flags::FIN;
  ASSERT_TRUE(yamux::HasFlag(flags, yamux::Flags::SYN));
  ASSERT_TRUE(yamux::HasFlag(flags, yamux::Flags::FIN));
  ASSERT_TRUE(!yamux::HasFlag(flags, yamux::Flags::ACK));
  ASSERT_TRUE(!yamux::HasFlag(flags, yamux::Flags::RST));
}

TEST(StreamIDHelpers) {
  ASSERT_TRUE(yamux::IsClientStream(1));
  ASSERT_TRUE(yamux::IsClientStream(3));
  ASSERT_TRUE(yamux::IsClientStream(99));
  ASSERT_TRUE(!yamux::IsClientStream(0));
  ASSERT_TRUE(!yamux::IsClientStream(2));
  ASSERT_TRUE(!yamux::IsClientStream(100));

  ASSERT_TRUE(!yamux::IsServerStream(0));
  ASSERT_TRUE(!yamux::IsServerStream(1));
  ASSERT_TRUE(yamux::IsServerStream(2));
  ASSERT_TRUE(yamux::IsServerStream(100));
}
