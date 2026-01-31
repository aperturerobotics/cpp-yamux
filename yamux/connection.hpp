#ifndef YAMUX_CONNECTION_HPP
#define YAMUX_CONNECTION_HPP

#include <cstdint>

#include "yamux/errors.hpp"

namespace yamux {

// Abstract interface for a bidirectional byte stream connection.
// Implementations can wrap TCP sockets, in-memory pipes, TLS connections, etc.
class Connection {
public:
  virtual ~Connection() = default;

  // Write data to the connection.
  // Returns Error::OK on success, or an error code on failure.
  // This is a blocking call that writes all data or fails.
  virtual Error Write(const uint8_t *data, size_t len) = 0;

  // Read data from the connection.
  // Returns the number of bytes read, or 0 on EOF/error.
  // The error field in the result indicates any error condition.
  // This is a blocking call that waits for at least some data.
  virtual Result<size_t> Read(uint8_t *buf, size_t max_len) = 0;

  // Close the connection.
  virtual Error Close() = 0;

  // Check if the connection is closed.
  virtual bool IsClosed() const = 0;
};

} // namespace yamux

#endif // YAMUX_CONNECTION_HPP
