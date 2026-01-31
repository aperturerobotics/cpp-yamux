#include "test_framework.hpp"

#include "yamux/session.hpp"

TEST(SessionConfigDefaults) {
  yamux::SessionConfig config;
  ASSERT_EQ(config.initial_window_size, yamux::kInitialWindowSize);
  ASSERT_TRUE(config.enable_keepalive);
  ASSERT_EQ(config.keepalive_interval_ms, 30000u);
  ASSERT_EQ(config.accept_backlog, 256u);
}

TEST(ErrorStrings) {
  ASSERT_TRUE(std::string(yamux::ErrorString(yamux::Error::OK)) == "ok");
  ASSERT_TRUE(std::string(yamux::ErrorString(yamux::Error::ConnectionClosed)) ==
              "connection closed");
  ASSERT_TRUE(std::string(yamux::ErrorString(yamux::Error::StreamReset)) ==
              "stream reset");
  ASSERT_TRUE(std::string(yamux::ErrorString(yamux::Error::EOF_)) ==
              "end of file");
}

TEST(StreamStateStrings) {
  ASSERT_TRUE(std::string(yamux::StreamStateString(yamux::StreamState::Init)) ==
              "Init");
  ASSERT_TRUE(std::string(yamux::StreamStateString(
                  yamux::StreamState::Established)) == "Established");
  ASSERT_TRUE(
      std::string(yamux::StreamStateString(yamux::StreamState::Closed)) ==
      "Closed");
}

TEST(ResultType) {
  yamux::Result<int> ok_result{42, yamux::Error::OK};
  ASSERT_TRUE(ok_result.ok());
  ASSERT_EQ(ok_result.value, 42);

  yamux::Result<int> err_result{0, yamux::Error::EOF_};
  ASSERT_TRUE(!err_result.ok());
  ASSERT_EQ(err_result.error, yamux::Error::EOF_);

  // Void result
  auto void_ok = yamux::Result<void>::Ok();
  ASSERT_TRUE(void_ok.ok());

  auto void_err = yamux::Result<void>::Err(yamux::Error::Canceled);
  ASSERT_TRUE(!void_err.ok());
  ASSERT_EQ(void_err.error, yamux::Error::Canceled);
}

TEST(YamuxErrorException) {
  yamux::YamuxError err(yamux::Error::StreamClosed);
  ASSERT_EQ(err.code(), yamux::Error::StreamClosed);
  ASSERT_TRUE(std::string(err.what()) == "stream closed");
}
