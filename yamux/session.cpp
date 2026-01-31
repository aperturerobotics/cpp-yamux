#include "yamux/session.hpp"

#include <chrono>

namespace yamux {

Session::Session(std::unique_ptr<Connection> conn, bool is_client,
                 AcceptHandler handler, const SessionConfig& config)
    : conn_(std::move(conn)),
      is_client_(is_client),
      accept_handler_(std::move(handler)),
      config_(config),
      next_stream_id_(is_client ? 1 : 2) {}

Session::~Session() {
  // Close first - this will signal the read thread to stop
  Close();

  // Detach instead of join to avoid deadlock
  // The thread will exit naturally when it sees the connection is closed
  if (read_thread_.joinable()) {
    read_thread_.detach();
  }
}

std::shared_ptr<Session> Session::Client(std::unique_ptr<Connection> conn,
                                         const SessionConfig& config) {
  auto session =
      std::shared_ptr<Session>(new Session(std::move(conn), true, nullptr, config));
  session->Start();
  return session;
}

std::shared_ptr<Session> Session::Server(std::unique_ptr<Connection> conn,
                                         AcceptHandler handler,
                                         const SessionConfig& config) {
  auto session = std::shared_ptr<Session>(
      new Session(std::move(conn), false, std::move(handler), config));
  session->Start();
  return session;
}

std::shared_ptr<Session> Session::Server(std::unique_ptr<Connection> conn,
                                         const SessionConfig& config) {
  return Server(std::move(conn), nullptr, config);
}

void Session::Start() {
  // Capture a shared_ptr to keep session alive during read loop
  // This is safe because Close() will signal the loop to exit
  auto self = shared_from_this();
  read_thread_ = std::thread([self]() { self->ReadLoop(); });
}

Result<std::shared_ptr<Stream>> Session::OpenStream() {
  if (closed_.load()) {
    return {nullptr, Error::SessionShutdown};
  }

  if (go_away_received_.load()) {
    return {nullptr, Error::GoAwayReceived};
  }

  // Allocate stream ID
  StreamID id = next_stream_id_.fetch_add(2);

  // Create stream
  auto stream =
      std::make_shared<Stream>(this, id, true, config_.initial_window_size);
  stream->SetState(StreamState::SYNSent);

  // Add to stream map
  {
    std::unique_lock<std::shared_mutex> lock(streams_mtx_);
    streams_[id] = stream;
  }

  // Send SYN via window update (can also use data frame)
  Error err = SendWindowUpdate(id, config_.initial_window_size, Flags::SYN);
  if (err != Error::OK) {
    std::unique_lock<std::shared_mutex> lock(streams_mtx_);
    streams_.erase(id);
    return {nullptr, err};
  }

  return {stream, Error::OK};
}

Result<std::shared_ptr<Stream>> Session::Accept() {
  std::unique_lock<std::mutex> lock(accept_mtx_);

  accept_cv_.wait(lock, [this]() {
    return !accept_queue_.empty() || closed_.load();
  });

  if (closed_.load() && accept_queue_.empty()) {
    return {nullptr, Error::SessionShutdown};
  }

  auto stream = std::move(accept_queue_.front());
  accept_queue_.pop_front();
  return {stream, Error::OK};
}

Error Session::Close() {
  bool expected = false;
  if (!closed_.compare_exchange_strong(expected, true)) {
    return Error::OK;  // Already closed
  }

  // Send GoAway if not already sent
  if (!go_away_sent_.exchange(true)) {
    SendFrame(Frame::GoAway(GoAwayCode::Normal));
  }

  // Close all streams
  CloseAllStreams();

  // Close the connection
  if (conn_) {
    conn_->Close();
  }

  // Wake up accept waiters
  accept_cv_.notify_all();
  ping_cv_.notify_all();

  return Error::OK;
}

Error Session::GoAway(GoAwayCode code) {
  if (go_away_sent_.exchange(true)) {
    return Error::OK;  // Already sent
  }

  return SendFrame(Frame::GoAway(code));
}

Result<uint32_t> Session::Ping() {
  if (closed_.load()) {
    return {0, Error::SessionShutdown};
  }

  std::unique_lock<std::mutex> lock(ping_mtx_);

  // Generate ping ID
  uint32_t id = ++ping_id_;
  ping_pending_ = true;

  auto start = std::chrono::steady_clock::now();

  // Send ping
  lock.unlock();
  Error err = SendFrame(Frame::Ping(id, false));
  if (err != Error::OK) {
    return {0, err};
  }
  lock.lock();

  // Wait for response
  ping_cv_.wait(lock, [this, id]() {
    return !ping_pending_ || closed_.load();
  });

  if (closed_.load()) {
    return {0, Error::SessionShutdown};
  }

  auto end = std::chrono::steady_clock::now();
  auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  return {static_cast<uint32_t>(rtt.count()), Error::OK};
}

Error Session::SendData(StreamID id, const uint8_t* data, size_t len,
                        Flags flags) {
  if (closed_.load()) {
    return Error::SessionShutdown;
  }

  return SendFrame(Frame::Data(id, flags, data, len));
}

Error Session::SendWindowUpdate(StreamID id, uint32_t delta, Flags flags) {
  if (closed_.load()) {
    return Error::SessionShutdown;
  }

  return SendFrame(Frame::WindowUpdate(id, flags, delta));
}

Error Session::SendFrame(const Frame& frame) {
  std::lock_guard<std::mutex> lock(write_mtx_);

  if (conn_->IsClosed()) {
    return Error::ConnectionClosed;
  }

  auto encoded = frame.Encode();
  return conn_->Write(encoded.data(), encoded.size());
}

void Session::ReadLoop() {
  std::vector<uint8_t> buf(4096);
  FrameReader reader;

  while (!closed_.load()) {
    auto result = conn_->Read(buf.data(), buf.size());
    if (result.error != Error::OK) {
      if (result.error == Error::Timeout) {
        continue;  // Retry on timeout
      }
      if (result.error != Error::EOF_) {
        session_error_.store(result.error);
      }
      break;
    }

    if (result.value == 0) {
      // EOF
      break;
    }

    // Feed data to frame reader
    size_t offset = 0;
    while (offset < result.value) {
      size_t consumed = reader.Feed(buf.data() + offset, result.value - offset);
      offset += consumed;

      if (reader.GetError() != Error::OK) {
        session_error_.store(reader.GetError());
        Close();
        return;
      }

      if (reader.HasFrame()) {
        Frame frame = reader.TakeFrame();
        Error err = HandleFrame(frame);
        if (err != Error::OK) {
          session_error_.store(err);
          Close();
          return;
        }
      }

      if (consumed == 0 && !reader.HasFrame()) {
        break;  // Need more data
      }
    }
  }

  // Clean shutdown
  closed_.store(true);
  CloseAllStreams();
  accept_cv_.notify_all();
  ping_cv_.notify_all();
}

Error Session::HandleFrame(const Frame& frame) {
  switch (frame.header.type) {
    case FrameType::Data:
      return HandleDataFrame(frame);
    case FrameType::WindowUpdate:
      return HandleWindowUpdateFrame(frame);
    case FrameType::Ping:
      return HandlePingFrame(frame);
    case FrameType::GoAway:
      return HandleGoAwayFrame(frame);
    default:
      return Error::InvalidFrameType;
  }
}

Error Session::HandleDataFrame(const Frame& frame) {
  StreamID id = frame.header.stream_id;
  if (id == 0) {
    return Error::InvalidStreamID;
  }

  auto stream = GetOrCreateIncomingStream(id, frame.header.flags);
  if (!stream) {
    // Stream doesn't exist and wasn't created (e.g., after GoAway)
    // Send RST if this is a new stream attempt
    if (HasFlag(frame.header.flags, Flags::SYN)) {
      SendWindowUpdate(id, 0, Flags::RST);
    }
    return Error::OK;  // Not fatal
  }

  Error err =
      stream->HandleData(frame.payload.data(), frame.payload.size(),
                         frame.header.flags);

  // Send ACK if needed (window delta=0, just acknowledging the stream)
  if (stream->NeedsAck()) {
    stream->AckSent();
    SendWindowUpdate(id, 0, Flags::ACK);
  }

  return err;
}

Error Session::HandleWindowUpdateFrame(const Frame& frame) {
  StreamID id = frame.header.stream_id;

  // Handle RST flag
  if (HasFlag(frame.header.flags, Flags::RST)) {
    if (id == 0) {
      return Error::InvalidStreamID;
    }
    auto stream = GetStream(id);
    if (stream) {
      stream->HandleReset();
    }
    return Error::OK;
  }

  if (id == 0) {
    // Session-level window update (not used in yamux but valid)
    return Error::OK;
  }

  auto stream = GetOrCreateIncomingStream(id, frame.header.flags);
  if (!stream) {
    if (HasFlag(frame.header.flags, Flags::SYN)) {
      SendWindowUpdate(id, 0, Flags::RST);
    }
    return Error::OK;
  }

  Error err = stream->HandleWindowUpdate(frame.header.length, frame.header.flags);

  // Send ACK if needed (window delta=0, just acknowledging the stream)
  if (stream->NeedsAck()) {
    stream->AckSent();
    SendWindowUpdate(id, 0, Flags::ACK);
  }

  return err;
}

Error Session::HandlePingFrame(const Frame& frame) {
  if (HasFlag(frame.header.flags, Flags::ACK)) {
    // Ping response
    std::lock_guard<std::mutex> lock(ping_mtx_);
    if (ping_pending_ && frame.header.length == ping_id_) {
      ping_pending_ = false;
      ping_cv_.notify_all();
    }
  } else {
    // Ping request - send response
    SendFrame(Frame::Ping(frame.header.length, true));
  }
  return Error::OK;
}

Error Session::HandleGoAwayFrame(const Frame& frame) {
  go_away_received_.store(true);
  // Don't accept new streams after GoAway
  // Existing streams can continue until they close
  return Error::OK;
}

std::shared_ptr<Stream> Session::GetStream(StreamID id) {
  std::shared_lock<std::shared_mutex> lock(streams_mtx_);
  auto it = streams_.find(id);
  if (it != streams_.end()) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<Stream> Session::GetOrCreateIncomingStream(StreamID id,
                                                           Flags flags) {
  // Check if stream exists
  {
    std::shared_lock<std::shared_mutex> lock(streams_mtx_);
    auto it = streams_.find(id);
    if (it != streams_.end()) {
      return it->second;
    }
  }

  // Only create if SYN flag is set
  if (!HasFlag(flags, Flags::SYN)) {
    return nullptr;
  }

  // Don't create new streams if we're closing
  if (go_away_received_.load() || closed_.load()) {
    return nullptr;
  }

  // Validate stream ID (client=odd, server=even)
  bool is_client_stream = IsClientStream(id);
  if ((is_client_ && is_client_stream) || (!is_client_ && !is_client_stream)) {
    // Invalid: we can't receive streams with IDs that should be ours
    return nullptr;
  }

  // Create new incoming stream
  auto stream =
      std::make_shared<Stream>(this, id, false, config_.initial_window_size);

  {
    std::unique_lock<std::shared_mutex> lock(streams_mtx_);
    // Double-check after acquiring write lock
    auto it = streams_.find(id);
    if (it != streams_.end()) {
      return it->second;
    }
    streams_[id] = stream;
  }

  // Notify via handler or queue
  if (accept_handler_) {
    accept_handler_(stream);
  } else {
    std::lock_guard<std::mutex> lock(accept_mtx_);
    accept_queue_.push_back(stream);
    accept_cv_.notify_one();
  }

  return stream;
}

void Session::RemoveStream(StreamID id) {
  std::unique_lock<std::shared_mutex> lock(streams_mtx_);
  streams_.erase(id);
}

void Session::CloseAllStreams() {
  std::shared_lock<std::shared_mutex> lock(streams_mtx_);
  for (auto& [id, stream] : streams_) {
    stream->NotifySessionClosed();
  }
}

}  // namespace yamux
