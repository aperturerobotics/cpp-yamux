#ifndef YAMUX_STREAM_HPP
#define YAMUX_STREAM_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "yamux/errors.hpp"
#include "yamux/types.hpp"

namespace yamux {

class Session;

// A yamux stream with flow control and state machine.
// Streams are created by the Session and should not be constructed directly.
class Stream : public std::enable_shared_from_this<Stream> {
 public:
  // Create a new stream (called by Session)
  Stream(Session* session, StreamID id, bool is_initiator,
         uint32_t initial_window = kInitialWindowSize);

  ~Stream();

  // Get the stream ID
  StreamID ID() const { return id_; }

  // Get the current state
  StreamState State() const;

  // Read data from the stream.
  // Blocks until data is available, the stream is closed, or an error occurs.
  // Returns the number of bytes read, or 0 with an error.
  Result<size_t> Read(uint8_t* buf, size_t max_len);

  // Write data to the stream.
  // Blocks if the flow control window is exhausted.
  // Returns Error::OK on success.
  Error Write(const uint8_t* data, size_t len);

  // Half-close the stream (send FIN).
  // We won't send any more data, but can still receive.
  Error Close();

  // Reset the stream (send RST).
  // Immediately terminates the stream in both directions.
  Error Reset();

  // Check if the stream is writable (not locally closed)
  bool CanWrite() const { return !local_fin_sent_.load(); }

  // Check if the stream is readable (not remotely closed)
  bool CanRead() const { return !remote_fin_received_.load(); }

  // --- Internal methods called by Session ---

  // Handle incoming data from the session
  Error HandleData(const uint8_t* data, size_t len, Flags flags);

  // Handle incoming window update from the session
  Error HandleWindowUpdate(uint32_t delta, Flags flags);

  // Handle stream reset from the session
  void HandleReset();

  // Notify that the stream is established (received ACK)
  void NotifyEstablished();

  // Notify that the session is closing
  void NotifySessionClosed();

  // Get the state for the session to check
  StreamState GetState() const;

  // Check if we need to send an ACK (for incoming streams)
  bool NeedsAck() const { return needs_ack_.load(); }

  // Mark ACK as sent
  void AckSent() { needs_ack_.store(false); }

  // Set state directly (used during stream creation)
  void SetState(StreamState state);

 private:
  // Send a window update to peer if needed
  Error MaybeSendWindowUpdate();

  Session* session_;
  StreamID id_;
  bool is_initiator_;

  // State management
  mutable std::mutex state_mtx_;
  StreamState state_ = StreamState::Init;

  // Read buffer and synchronization
  mutable std::mutex read_mtx_;
  std::condition_variable read_cv_;
  std::deque<std::vector<uint8_t>> read_queue_;
  std::atomic<bool> remote_fin_received_{false};
  std::atomic<bool> reset_{false};
  std::atomic<bool> session_closed_{false};

  // Write synchronization (flow control)
  mutable std::mutex send_mtx_;
  std::condition_variable send_cv_;
  uint32_t send_window_;  // How much we can send to peer
  std::atomic<bool> local_fin_sent_{false};

  // Receive window tracking
  mutable std::mutex recv_mtx_;
  uint32_t initial_recv_window_;
  std::atomic<uint32_t> recv_window_;  // Our receive window
  uint32_t bytes_consumed_ = 0;        // Bytes consumed since last window update

  // ACK tracking for incoming streams
  std::atomic<bool> needs_ack_{false};
};

}  // namespace yamux

#endif  // YAMUX_STREAM_HPP
