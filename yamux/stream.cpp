#include "yamux/stream.hpp"

#include "yamux/session.hpp"

namespace yamux {

Stream::Stream(Session* session, StreamID id, bool is_initiator,
               uint32_t initial_window)
    : session_(session),
      id_(id),
      is_initiator_(is_initiator),
      send_window_(initial_window),
      initial_recv_window_(initial_window),
      recv_window_(initial_window) {}

Stream::~Stream() = default;

StreamState Stream::State() const {
  std::lock_guard<std::mutex> lock(state_mtx_);
  return state_;
}

StreamState Stream::GetState() const { return State(); }

void Stream::SetState(StreamState state) {
  std::lock_guard<std::mutex> lock(state_mtx_);
  state_ = state;
}

Result<size_t> Stream::Read(uint8_t* buf, size_t max_len) {
  std::unique_lock<std::mutex> lock(read_mtx_);

  // Wait for data, close, or reset
  read_cv_.wait(lock, [this]() {
    return !read_queue_.empty() || remote_fin_received_.load() ||
           reset_.load() || session_closed_.load();
  });

  // Check for reset first
  if (reset_.load()) {
    return {0, Error::StreamReset};
  }

  // Check for session closed
  if (session_closed_.load() && read_queue_.empty()) {
    return {0, Error::SessionShutdown};
  }

  // Return data if available
  if (!read_queue_.empty()) {
    auto& front = read_queue_.front();
    size_t to_copy = std::min(max_len, front.size());
    std::memcpy(buf, front.data(), to_copy);

    if (to_copy == front.size()) {
      read_queue_.pop_front();
    } else {
      // Partial read - remove consumed bytes
      front.erase(front.begin(), front.begin() + to_copy);
    }

    // Track consumed bytes for window updates
    bytes_consumed_ += to_copy;

    // Update receive window and maybe send update
    lock.unlock();
    MaybeSendWindowUpdate();

    return {to_copy, Error::OK};
  }

  // No data and remote closed - EOF
  if (remote_fin_received_.load()) {
    return {0, Error::EOF_};
  }

  return {0, Error::EOF_};
}

Error Stream::Write(const uint8_t* data, size_t len) {
  if (local_fin_sent_.load()) {
    return Error::StreamClosed;
  }

  if (reset_.load()) {
    return Error::StreamReset;
  }

  if (session_closed_.load()) {
    return Error::SessionShutdown;
  }

  size_t offset = 0;
  while (offset < len) {
    // Wait for send window
    std::unique_lock<std::mutex> lock(send_mtx_);
    send_cv_.wait(lock, [this]() {
      return send_window_ > 0 || reset_.load() || session_closed_.load() ||
             local_fin_sent_.load();
    });

    if (reset_.load()) {
      return Error::StreamReset;
    }
    if (session_closed_.load()) {
      return Error::SessionShutdown;
    }
    if (local_fin_sent_.load()) {
      return Error::StreamClosed;
    }

    // Calculate how much we can send
    size_t remaining = len - offset;
    size_t to_send = std::min(remaining, static_cast<size_t>(send_window_));
    send_window_ -= static_cast<uint32_t>(to_send);
    lock.unlock();

    // Send the data
    Error err = session_->SendData(id_, data + offset, to_send, Flags::None);
    if (err != Error::OK) {
      return err;
    }

    offset += to_send;
  }

  return Error::OK;
}

Error Stream::Close() {
  bool expected = false;
  if (!local_fin_sent_.compare_exchange_strong(expected, true)) {
    return Error::OK;  // Already closed
  }

  // Update state
  {
    std::lock_guard<std::mutex> lock(state_mtx_);
    if (state_ == StreamState::Established) {
      state_ = StreamState::LocalClose;
    } else if (state_ == StreamState::RemoteClose) {
      state_ = StreamState::Closed;
    }
  }

  // Send FIN
  Error err = session_->SendData(id_, nullptr, 0, Flags::FIN);
  if (err != Error::OK) {
    return err;
  }

  // Wake up any blocked writers
  send_cv_.notify_all();

  return Error::OK;
}

Error Stream::Reset() {
  bool expected = false;
  if (!reset_.compare_exchange_strong(expected, true)) {
    return Error::OK;  // Already reset
  }

  // Update state
  {
    std::lock_guard<std::mutex> lock(state_mtx_);
    state_ = StreamState::Reset;
  }

  // Send RST
  Error err = session_->SendWindowUpdate(id_, 0, Flags::RST);

  // Wake up blocked readers/writers
  read_cv_.notify_all();
  send_cv_.notify_all();

  return err;
}

Error Stream::HandleData(const uint8_t* data, size_t len, Flags flags) {
  // Handle SYN flag (stream opening)
  if (HasFlag(flags, Flags::SYN)) {
    std::lock_guard<std::mutex> lock(state_mtx_);
    if (state_ == StreamState::Init) {
      state_ = StreamState::SYNReceived;
      needs_ack_.store(true);
    }
  }

  // Handle ACK flag
  if (HasFlag(flags, Flags::ACK)) {
    NotifyEstablished();
  }

  // Handle data payload
  if (len > 0) {
    // Check window
    uint32_t current_window = recv_window_.load();
    if (len > current_window) {
      return Error::WindowExceeded;
    }
    recv_window_.fetch_sub(static_cast<uint32_t>(len));

    // Queue data
    std::lock_guard<std::mutex> lock(read_mtx_);
    read_queue_.emplace_back(data, data + len);
    read_cv_.notify_all();
  }

  // Handle FIN flag
  if (HasFlag(flags, Flags::FIN)) {
    remote_fin_received_.store(true);

    {
      std::lock_guard<std::mutex> lock(state_mtx_);
      if (state_ == StreamState::Established ||
          state_ == StreamState::SYNReceived) {
        state_ = StreamState::RemoteClose;
      } else if (state_ == StreamState::LocalClose) {
        state_ = StreamState::Closed;
      }
    }

    read_cv_.notify_all();
  }

  return Error::OK;
}

Error Stream::HandleWindowUpdate(uint32_t delta, Flags flags) {
  // Handle SYN flag (stream opening via window update)
  if (HasFlag(flags, Flags::SYN)) {
    std::lock_guard<std::mutex> lock(state_mtx_);
    if (state_ == StreamState::Init) {
      state_ = StreamState::SYNReceived;
      needs_ack_.store(true);
    }
  }

  // Handle ACK flag
  if (HasFlag(flags, Flags::ACK)) {
    NotifyEstablished();
  }

  // Handle RST flag
  if (HasFlag(flags, Flags::RST)) {
    HandleReset();
    return Error::OK;
  }

  // Update send window
  if (delta > 0) {
    std::lock_guard<std::mutex> lock(send_mtx_);
    send_window_ += delta;
    send_cv_.notify_all();
  }

  // Handle FIN flag
  if (HasFlag(flags, Flags::FIN)) {
    remote_fin_received_.store(true);

    {
      std::lock_guard<std::mutex> lock(state_mtx_);
      if (state_ == StreamState::Established ||
          state_ == StreamState::SYNReceived) {
        state_ = StreamState::RemoteClose;
      } else if (state_ == StreamState::LocalClose) {
        state_ = StreamState::Closed;
      }
    }

    read_cv_.notify_all();
  }

  return Error::OK;
}

void Stream::HandleReset() {
  reset_.store(true);

  {
    std::lock_guard<std::mutex> lock(state_mtx_);
    state_ = StreamState::Reset;
  }

  read_cv_.notify_all();
  send_cv_.notify_all();
}

void Stream::NotifyEstablished() {
  std::lock_guard<std::mutex> lock(state_mtx_);
  if (state_ == StreamState::SYNSent || state_ == StreamState::SYNReceived) {
    state_ = StreamState::Established;
  }
}

void Stream::NotifySessionClosed() {
  session_closed_.store(true);
  read_cv_.notify_all();
  send_cv_.notify_all();
}

Error Stream::MaybeSendWindowUpdate() {
  // Send window update when we've consumed more than half the initial window
  std::lock_guard<std::mutex> lock(recv_mtx_);
  if (bytes_consumed_ >= initial_recv_window_ / 2) {
    uint32_t delta = bytes_consumed_;
    bytes_consumed_ = 0;
    recv_window_.fetch_add(delta);
    return session_->SendWindowUpdate(id_, delta, Flags::None);
  }
  return Error::OK;
}

}  // namespace yamux
