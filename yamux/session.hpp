#ifndef YAMUX_SESSION_HPP
#define YAMUX_SESSION_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "yamux/connection.hpp"
#include "yamux/errors.hpp"
#include "yamux/frame.hpp"
#include "yamux/stream.hpp"
#include "yamux/types.hpp"

namespace yamux {

// Handler for accepting new streams (server-side)
using AcceptHandler = std::function<void(std::shared_ptr<Stream>)>;

// Session configuration
struct SessionConfig {
  uint32_t initial_window_size = kInitialWindowSize;
  bool enable_keepalive = true;
  uint32_t keepalive_interval_ms = 30000;
  uint32_t accept_backlog = 256;
};

// A yamux session that manages streams over a connection.
class Session : public std::enable_shared_from_this<Session> {
 public:
  ~Session();

  // Create a client session
  static std::shared_ptr<Session> Client(std::unique_ptr<Connection> conn,
                                         const SessionConfig& config = {});

  // Create a server session with accept handler
  static std::shared_ptr<Session> Server(std::unique_ptr<Connection> conn,
                                         AcceptHandler handler,
                                         const SessionConfig& config = {});

  // Create a server session without handler (use Accept() instead)
  static std::shared_ptr<Session> Server(std::unique_ptr<Connection> conn,
                                         const SessionConfig& config = {});

  // Open a new stream (client-side: odd IDs, server-side: even IDs)
  Result<std::shared_ptr<Stream>> OpenStream();

  // Accept an incoming stream (blocking, alternative to AcceptHandler)
  Result<std::shared_ptr<Stream>> Accept();

  // Close the session gracefully (sends GoAway)
  Error Close();

  // Send GoAway with specific code
  Error GoAway(GoAwayCode code);

  // Ping the remote and get RTT (blocking)
  Result<uint32_t> Ping();

  // Check if session is closed
  bool IsClosed() const { return closed_.load(); }

  // Check if this is a client session
  bool IsClient() const { return is_client_; }

  // Get session error (if any)
  Error GetError() const { return session_error_.load(); }

  // --- Internal methods called by Stream ---

  // Send a data frame
  Error SendData(StreamID id, const uint8_t* data, size_t len, Flags flags);

  // Send a window update frame
  Error SendWindowUpdate(StreamID id, uint32_t delta, Flags flags);

 private:
  Session(std::unique_ptr<Connection> conn, bool is_client,
          AcceptHandler handler, const SessionConfig& config);

  // Start the read loop (called after construction)
  void Start();

  // Background read loop
  void ReadLoop();

  // Handle incoming frames
  Error HandleFrame(const Frame& frame);
  Error HandleDataFrame(const Frame& frame);
  Error HandleWindowUpdateFrame(const Frame& frame);
  Error HandlePingFrame(const Frame& frame);
  Error HandleGoAwayFrame(const Frame& frame);

  // Get or create a stream for incoming frames
  std::shared_ptr<Stream> GetStream(StreamID id);
  std::shared_ptr<Stream> GetOrCreateIncomingStream(StreamID id, Flags flags);

  // Send a frame (thread-safe)
  Error SendFrame(const Frame& frame);

  // Remove a stream from the session
  void RemoveStream(StreamID id);

  // Close all streams
  void CloseAllStreams();

  std::unique_ptr<Connection> conn_;
  bool is_client_;
  AcceptHandler accept_handler_;
  SessionConfig config_;

  // Write synchronization
  std::mutex write_mtx_;

  // Stream management
  std::shared_mutex streams_mtx_;
  std::unordered_map<StreamID, std::shared_ptr<Stream>> streams_;
  std::atomic<StreamID> next_stream_id_;

  // Accept queue (for blocking Accept())
  std::mutex accept_mtx_;
  std::condition_variable accept_cv_;
  std::deque<std::shared_ptr<Stream>> accept_queue_;

  // Ping tracking
  std::mutex ping_mtx_;
  std::condition_variable ping_cv_;
  uint32_t ping_id_ = 0;
  bool ping_pending_ = false;
  uint32_t ping_rtt_ms_ = 0;

  // Session state
  std::thread read_thread_;
  std::atomic<bool> closed_{false};
  std::atomic<bool> go_away_sent_{false};
  std::atomic<bool> go_away_received_{false};
  std::atomic<Error> session_error_{Error::OK};
};

}  // namespace yamux

#endif  // YAMUX_SESSION_HPP
