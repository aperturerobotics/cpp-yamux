#include "test_framework.hpp"

#include "yamux/yamux.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace {

// In-memory connection endpoint for testing
class InMemoryEndpoint {
 public:
  void Write(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!closed_) {
      data_.insert(data_.end(), data, data + len);
      cv_.notify_all();
    }
  }

  yamux::Result<size_t> Read(uint8_t* buf, size_t max_len,
                             int timeout_ms = 5000) {
    std::unique_lock<std::mutex> lock(mtx_);

    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
          return !data_.empty() || closed_;
        })) {
      return {0, yamux::Error::Timeout};
    }

    if (data_.empty()) {
      return {0, yamux::Error::EOF_};
    }

    size_t to_read = std::min(max_len, data_.size());
    std::copy(data_.begin(), data_.begin() + to_read, buf);
    data_.erase(data_.begin(), data_.begin() + to_read);
    return {to_read, yamux::Error::OK};
  }

  void Close() {
    std::lock_guard<std::mutex> lock(mtx_);
    closed_ = true;
    cv_.notify_all();
  }

  bool IsClosed() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return closed_;
  }

 private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<uint8_t> data_;
  bool closed_ = false;
};

// In-memory connection that connects two endpoints
class InMemoryConnection : public yamux::Connection {
 public:
  InMemoryConnection(std::shared_ptr<InMemoryEndpoint> read_ep,
                     std::shared_ptr<InMemoryEndpoint> write_ep)
      : read_ep_(std::move(read_ep)), write_ep_(std::move(write_ep)) {}

  yamux::Error Write(const uint8_t* data, size_t len) override {
    if (closed_) {
      return yamux::Error::ConnectionClosed;
    }
    write_ep_->Write(data, len);
    return yamux::Error::OK;
  }

  yamux::Result<size_t> Read(uint8_t* buf, size_t max_len) override {
    if (closed_) {
      return {0, yamux::Error::ConnectionClosed};
    }
    return read_ep_->Read(buf, max_len);
  }

  yamux::Error Close() override {
    closed_ = true;
    read_ep_->Close();
    write_ep_->Close();
    return yamux::Error::OK;
  }

  bool IsClosed() const override { return closed_; }

 private:
  std::shared_ptr<InMemoryEndpoint> read_ep_;
  std::shared_ptr<InMemoryEndpoint> write_ep_;
  std::atomic<bool> closed_{false};
};

// Create a pair of connected in-memory connections
std::pair<std::unique_ptr<yamux::Connection>,
          std::unique_ptr<yamux::Connection>>
MakeConnPair() {
  auto client_to_server = std::make_shared<InMemoryEndpoint>();
  auto server_to_client = std::make_shared<InMemoryEndpoint>();

  auto client_conn =
      std::make_unique<InMemoryConnection>(server_to_client, client_to_server);
  auto server_conn =
      std::make_unique<InMemoryConnection>(client_to_server, server_to_client);

  return {std::move(client_conn), std::move(server_conn)};
}

}  // namespace

TEST(E2EOpenStream) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  // Client opens a stream
  auto result = client->OpenStream();
  ASSERT_TRUE(result.ok());
  auto client_stream = result.value;
  ASSERT_EQ(client_stream->ID(), 1u);  // First client stream is odd

  // Server accepts the stream
  auto accept_result = server->Accept();
  ASSERT_TRUE(accept_result.ok());
  auto server_stream = accept_result.value;
  ASSERT_EQ(server_stream->ID(), 1u);

  client->Close();
  server->Close();
}

TEST(E2EStreamData) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  auto client_stream = client->OpenStream().value;
  auto server_stream = server->Accept().value;

  // Client sends data
  uint8_t send_data[] = "Hello, yamux!";
  yamux::Error err = client_stream->Write(send_data, sizeof(send_data));
  ASSERT_EQ(err, yamux::Error::OK);

  // Server receives data
  uint8_t recv_buf[64];
  auto read_result = server_stream->Read(recv_buf, sizeof(recv_buf));
  ASSERT_TRUE(read_result.ok());
  ASSERT_EQ(read_result.value, sizeof(send_data));
  ASSERT_TRUE(std::memcmp(recv_buf, send_data, sizeof(send_data)) == 0);

  client->Close();
  server->Close();
}

TEST(E2EBidirectionalData) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  auto client_stream = client->OpenStream().value;
  auto server_stream = server->Accept().value;

  // Client sends
  uint8_t msg1[] = "ping";
  client_stream->Write(msg1, sizeof(msg1));

  // Server receives and sends reply
  uint8_t buf[64];
  server_stream->Read(buf, sizeof(buf));

  uint8_t msg2[] = "pong";
  server_stream->Write(msg2, sizeof(msg2));

  // Client receives reply
  auto result = client_stream->Read(buf, sizeof(buf));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::memcmp(buf, msg2, sizeof(msg2)) == 0);

  client->Close();
  server->Close();
}

TEST(E2EMultipleStreams) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  // Open multiple streams
  auto stream1 = client->OpenStream().value;
  auto stream2 = client->OpenStream().value;
  auto stream3 = client->OpenStream().value;

  ASSERT_EQ(stream1->ID(), 1u);
  ASSERT_EQ(stream2->ID(), 3u);
  ASSERT_EQ(stream3->ID(), 5u);

  // Accept all on server
  auto s1 = server->Accept().value;
  auto s2 = server->Accept().value;
  auto s3 = server->Accept().value;

  // Send different data on each stream
  uint8_t data1[] = "stream1";
  uint8_t data2[] = "stream2";
  uint8_t data3[] = "stream3";

  stream1->Write(data1, sizeof(data1));
  stream2->Write(data2, sizeof(data2));
  stream3->Write(data3, sizeof(data3));

  // Read and verify
  uint8_t buf[64];
  s1->Read(buf, sizeof(buf));
  ASSERT_TRUE(std::memcmp(buf, data1, sizeof(data1)) == 0);

  s2->Read(buf, sizeof(buf));
  ASSERT_TRUE(std::memcmp(buf, data2, sizeof(data2)) == 0);

  s3->Read(buf, sizeof(buf));
  ASSERT_TRUE(std::memcmp(buf, data3, sizeof(data3)) == 0);

  client->Close();
  server->Close();
}

TEST(E2EStreamClose) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  auto client_stream = client->OpenStream().value;
  auto server_stream = server->Accept().value;

  // Send some data then close
  uint8_t data[] = "goodbye";
  client_stream->Write(data, sizeof(data));
  client_stream->Close();

  // Server should still receive the data
  uint8_t buf[64];
  auto result = server_stream->Read(buf, sizeof(buf));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::memcmp(buf, data, sizeof(data)) == 0);

  // Next read should get EOF
  result = server_stream->Read(buf, sizeof(buf));
  ASSERT_EQ(result.error, yamux::Error::EOF_);

  client->Close();
  server->Close();
}

TEST(E2EPing) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  // Ping from client
  auto result = client->Ping();
  ASSERT_TRUE(result.ok());
  // RTT should be small for in-memory connection
  ASSERT_TRUE(result.value < 1000);  // Less than 1 second

  client->Close();
  server->Close();
}

TEST(E2EServerOpensStream) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  // Server opens a stream
  auto result = server->OpenStream();
  ASSERT_TRUE(result.ok());
  auto server_stream = result.value;
  ASSERT_EQ(server_stream->ID(), 2u);  // First server stream is even

  // Client accepts
  auto accept_result = client->Accept();
  ASSERT_TRUE(accept_result.ok());
  auto client_stream = accept_result.value;
  ASSERT_EQ(client_stream->ID(), 2u);

  // Exchange data
  uint8_t data[] = "server initiated";
  server_stream->Write(data, sizeof(data));

  uint8_t buf[64];
  client_stream->Read(buf, sizeof(buf));
  ASSERT_TRUE(std::memcmp(buf, data, sizeof(data)) == 0);

  client->Close();
  server->Close();
}

TEST(E2EAcceptHandler) {
  auto [client_conn, server_conn] = MakeConnPair();

  std::mutex mtx;
  std::condition_variable cv;
  std::shared_ptr<yamux::Stream> accepted_stream;

  auto handler = [&](std::shared_ptr<yamux::Stream> stream) {
    std::lock_guard<std::mutex> lock(mtx);
    accepted_stream = stream;
    cv.notify_all();
  };

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn), handler);

  // Client opens stream
  auto client_stream = client->OpenStream().value;

  // Wait for handler to be called
  {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait_for(lock, std::chrono::seconds(5),
                [&]() { return accepted_stream != nullptr; });
  }

  ASSERT_TRUE(accepted_stream != nullptr);
  ASSERT_EQ(accepted_stream->ID(), client_stream->ID());

  client->Close();
  server->Close();
}

TEST(E2ELargeData) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  auto client_stream = client->OpenStream().value;
  auto server_stream = server->Accept().value;

  // Send 512KB of data (tests flow control across window boundary)
  const size_t data_size = 512 * 1024;
  std::vector<uint8_t> send_data(data_size);
  for (size_t i = 0; i < data_size; i++) {
    send_data[i] = static_cast<uint8_t>(i % 256);
  }

  // Send in a separate thread
  std::thread sender(
      [&]() { client_stream->Write(send_data.data(), send_data.size()); });

  // Receive all data
  std::vector<uint8_t> recv_data;
  uint8_t buf[8192];
  while (recv_data.size() < data_size) {
    auto result = server_stream->Read(buf, sizeof(buf));
    if (result.error != yamux::Error::OK) {
      break;
    }
    recv_data.insert(recv_data.end(), buf, buf + result.value);
  }

  sender.join();

  ASSERT_EQ(recv_data.size(), data_size);
  ASSERT_TRUE(recv_data == send_data);

  client->Close();
  server->Close();
}

TEST(E2EStreamReset) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  auto client_stream = client->OpenStream().value;
  auto server_stream = server->Accept().value;

  // Reset from client
  client_stream->Reset();

  // Server read should get reset error
  uint8_t buf[64];
  auto result = server_stream->Read(buf, sizeof(buf));
  ASSERT_EQ(result.error, yamux::Error::StreamReset);

  client->Close();
  server->Close();
}

TEST(E2EGoAway) {
  auto [client_conn, server_conn] = MakeConnPair();

  auto client = yamux::Session::Client(std::move(client_conn));
  auto server = yamux::Session::Server(std::move(server_conn));

  // Open a stream first
  auto stream1 = client->OpenStream().value;
  server->Accept();

  // Send GoAway
  client->GoAway(yamux::GoAwayCode::Normal);

  // Small delay for GoAway to be processed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Server should not be able to open new streams to client
  // (but existing streams should still work)

  client->Close();
  server->Close();
}
