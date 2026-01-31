# cpp-yamux

A C++17 implementation of the [yamux](https://github.com/hashicorp/yamux) stream multiplexer protocol.

## Features

- Full yamux protocol implementation (version 0)
- Multiple concurrent streams over a single connection
- Bidirectional data transfer with flow control
- Stream lifecycle management (SYN/ACK/FIN/RST)
- Ping/pong for keepalive and RTT measurement
- Graceful shutdown with GoAway
- Thread-safe with minimal locking
- Works over any bidirectional byte channel

## Building

```bash
mkdir build && cd build
cmake ..
make
```

To run tests:

```bash
./yamux_tests
```

## Usage

### Client Session

```cpp
#include "yamux/yamux.hpp"

// Create a client session over your connection
auto session = yamux::Session::Client(std::move(connection));

// Open a stream
auto result = session->OpenStream();
if (result.ok()) {
    auto stream = result.value;

    // Write data
    uint8_t data[] = "Hello, yamux!";
    stream->Write(data, sizeof(data));

    // Read response
    uint8_t buf[1024];
    auto read_result = stream->Read(buf, sizeof(buf));
    if (read_result.ok()) {
        // Process read_result.value bytes
    }

    // Close the stream
    stream->Close();
}

// Close the session
session->Close();
```

### Server Session

```cpp
#include "yamux/yamux.hpp"

// Create a server session with accept handler
auto session = yamux::Session::Server(
    std::move(connection),
    [](std::shared_ptr<yamux::Stream> stream) {
        // Handle incoming stream
        uint8_t buf[1024];
        while (true) {
            auto result = stream->Read(buf, sizeof(buf));
            if (!result.ok()) break;

            // Echo back
            stream->Write(buf, result.value);
        }
    }
);

// Or use blocking Accept()
auto session = yamux::Session::Server(std::move(connection));
while (auto result = session->Accept()) {
    if (!result.ok()) break;
    auto stream = result.value;
    // Handle stream...
}
```

### Custom Connection

Implement the `Connection` interface to use yamux over any transport:

```cpp
class MyConnection : public yamux::Connection {
public:
    yamux::Error Write(const uint8_t* data, size_t len) override {
        // Write data to your transport
    }

    yamux::Result<size_t> Read(uint8_t* buf, size_t max_len) override {
        // Read data from your transport
    }

    yamux::Error Close() override {
        // Close the transport
    }

    bool IsClosed() const override {
        // Check if transport is closed
    }
};
```

## API Reference

### Session

- `Session::Client(connection, config)` - Create a client session
- `Session::Server(connection, handler, config)` - Create a server session with accept handler
- `Session::Server(connection, config)` - Create a server session for manual accept
- `OpenStream()` - Open a new stream (returns `Result<shared_ptr<Stream>>`)
- `Accept()` - Accept an incoming stream (blocking)
- `Ping()` - Ping remote and measure RTT
- `GoAway(code)` - Send GoAway frame
- `Close()` - Gracefully close the session

### Stream

- `ID()` - Get stream ID
- `State()` - Get current stream state
- `Read(buf, max_len)` - Read data (blocking)
- `Write(data, len)` - Write data (may block on flow control)
- `Close()` - Half-close (send FIN)
- `Reset()` - Hard reset (send RST)

### Configuration

```cpp
yamux::SessionConfig config;
config.initial_window_size = 256 * 1024;  // 256 KiB (default)
config.enable_keepalive = true;           // Enable keepalive pings
config.keepalive_interval_ms = 30000;     // Keepalive interval
config.accept_backlog = 256;              // Max pending accepts
```

## Protocol

Yamux uses a 12-byte header for all frames:

| Field     | Size   | Description |
|-----------|--------|-------------|
| Version   | 1 byte | Protocol version (0) |
| Type      | 1 byte | Frame type |
| Flags     | 2 bytes| Frame flags |
| StreamID  | 4 bytes| Stream identifier |
| Length    | 4 bytes| Payload length / window delta |

Frame types:
- `0x0` Data - Stream data
- `0x1` WindowUpdate - Flow control update
- `0x2` Ping - Keepalive / RTT measurement
- `0x3` GoAway - Session termination

Flags:
- `0x1` SYN - New stream
- `0x2` ACK - Acknowledge stream
- `0x4` FIN - Half-close
- `0x8` RST - Reset

## License

See LICENSE file.
