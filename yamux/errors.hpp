#ifndef YAMUX_ERRORS_HPP
#define YAMUX_ERRORS_HPP

#include <stdexcept>

namespace yamux {

// Error codes for yamux operations
enum class Error {
  OK = 0,
  ConnectionClosed,
  ConnectionReset,
  InvalidVersion,
  InvalidFrameType,
  InvalidStreamID,
  StreamClosed,
  StreamReset,
  DuplicateStream,
  WindowExceeded,
  SessionShutdown,
  GoAwayReceived,
  Canceled,
  EOF_,
  Timeout,
  UnknownStream,
  InvalidState,
  WriteError,
  ReadError,
};

// Convert error to human-readable string
inline const char* ErrorString(Error err) {
  switch (err) {
    case Error::OK:
      return "ok";
    case Error::ConnectionClosed:
      return "connection closed";
    case Error::ConnectionReset:
      return "connection reset";
    case Error::InvalidVersion:
      return "invalid protocol version";
    case Error::InvalidFrameType:
      return "invalid frame type";
    case Error::InvalidStreamID:
      return "invalid stream ID";
    case Error::StreamClosed:
      return "stream closed";
    case Error::StreamReset:
      return "stream reset";
    case Error::DuplicateStream:
      return "duplicate stream";
    case Error::WindowExceeded:
      return "flow control window exceeded";
    case Error::SessionShutdown:
      return "session shutdown";
    case Error::GoAwayReceived:
      return "go away received";
    case Error::Canceled:
      return "operation canceled";
    case Error::EOF_:
      return "end of file";
    case Error::Timeout:
      return "timeout";
    case Error::UnknownStream:
      return "unknown stream";
    case Error::InvalidState:
      return "invalid state";
    case Error::WriteError:
      return "write error";
    case Error::ReadError:
      return "read error";
    default:
      return "unknown error";
  }
}

// Exception class for yamux errors
class YamuxError : public std::runtime_error {
 public:
  explicit YamuxError(Error code)
      : std::runtime_error(ErrorString(code)), code_(code) {}

  Error code() const noexcept { return code_; }

 private:
  Error code_;
};

// Result type for operations that return a value or error
template <typename T>
struct Result {
  T value;
  Error error;

  bool ok() const { return error == Error::OK; }

  // Implicit conversion for checking
  explicit operator bool() const { return ok(); }
};

// Specialization for void result
template <>
struct Result<void> {
  Error error;

  bool ok() const { return error == Error::OK; }
  explicit operator bool() const { return ok(); }

  static Result<void> Ok() { return {Error::OK}; }
  static Result<void> Err(Error e) { return {e}; }
};

}  // namespace yamux

#endif  // YAMUX_ERRORS_HPP
