// Copyright 2026 The A11 Authors.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "redis/client.h"
#include "redis/reply.h"

namespace a11::redis {
namespace {

absl::StatusOr<std::shared_ptr<Client>> ConnectForTest() {
  ClientOptions options;
  options.client_name = "a11-redis-client-test";
  options.connect_timeout = absl::Milliseconds(250);
  options.command_timeout = absl::Seconds(2);
  absl::StatusOr<std::shared_ptr<Client>> client =
      Client::Create(std::move(options));
  if (!client.ok())
    return client.status();
  absl::Status ready =
      (*client)->Ready().Await(absl::Now() + absl::Seconds(2)).status();
  if (!ready.ok())
    return ready;
  return *client;
}

TEST(RedisClientOptionsTest, ParsesUrlsAndEnvironmentIndependentValidation) {
  absl::StatusOr<ClientOptions> options =
      ClientOptions::FromUrl("redis://user:p%40ss@[::1]:6380/7");
  ASSERT_TRUE(options.ok()) << options.status();
  EXPECT_EQ(options->host, "::1");
  EXPECT_EQ(options->port, 6380);
  EXPECT_EQ(options->username, "user");
  EXPECT_EQ(options->password, "p@ss");
  EXPECT_EQ(options->database, 7);
  EXPECT_TRUE(absl::IsUnimplemented(
      ClientOptions::FromUrl("rediss://localhost").status()));
  EXPECT_TRUE(absl::IsInvalidArgument(
      ClientOptions::FromUrl("http://localhost").status()));
}

TEST(RedisClientTest, CommandsAndPubSubAreBinarySafe) {
  absl::StatusOr<std::shared_ptr<Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<Client> client = *connected;
  const std::string suffix = std::to_string(absl::ToUnixNanos(absl::Now()));
  const std::string key = "a11:test:redis-client:" + suffix;
  const std::string channel = key + ":events";
  const std::string payload("a\0b\xff", 4);

  absl::StatusOr<Reply> set = client->Command({"SET", key, payload}).Await();
  ASSERT_TRUE(set.ok()) << set.status();
  ASSERT_EQ(*set->AsString(), "OK");
  absl::StatusOr<Reply> get = client->Command({"GET", key}).Await();
  ASSERT_TRUE(get.ok()) << get.status();
  EXPECT_EQ(*get->AsString(), payload);

  absl::StatusOr<std::shared_ptr<Subscription>> subscribed =
      client->Subscribe(channel).Await();
  ASSERT_TRUE(subscribed.ok()) << subscribed.status();
  const std::uint64_t generation = (*subscribed)->generation();
  absl::StatusOr<Reply> published =
      client->Command({"PUBLISH", channel, payload}).Await();
  ASSERT_TRUE(published.ok()) << published.status();
  EXPECT_GE(*published->AsInteger(), 1);
  absl::StatusOr<std::uint64_t> changed =
      (*subscribed)->Wait(generation, absl::Now() + absl::Seconds(2)).Await();
  ASSERT_TRUE(changed.ok()) << changed.status();
  EXPECT_GT(*changed, generation);

  EXPECT_TRUE(client->Command({"DEL", key}).Await().ok());
}

TEST(RedisClientTest, SubscriptionWaitSupportsCancellationAndReuse) {
  absl::StatusOr<std::shared_ptr<Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<Client> client = *connected;
  const std::string channel = "a11:test:redis-client:cancel:" +
                              std::to_string(absl::ToUnixNanos(absl::Now()));
  absl::StatusOr<std::shared_ptr<Subscription>> subscribed =
      client->Subscribe(channel).Await();
  ASSERT_TRUE(subscribed.ok()) << subscribed.status();
  const std::uint64_t generation = (*subscribed)->generation();

  a11::Future<std::uint64_t> waiting = (*subscribed)->Wait(generation);
  ASSERT_TRUE(waiting.Cancel().ok());
  absl::StatusOr<std::uint64_t> cancelled =
      waiting.Await(absl::Now() + absl::Seconds(2));
  EXPECT_TRUE(absl::IsCancelled(cancelled.status())) << cancelled.status();

  a11::Future<std::uint64_t> next = (*subscribed)->Wait(generation);
  ASSERT_TRUE(client->Command({"PUBLISH", channel, "wake"}).Await().ok());
  absl::StatusOr<std::uint64_t> changed =
      next.Await(absl::Now() + absl::Seconds(2));
  ASSERT_TRUE(changed.ok()) << changed.status();
  EXPECT_GT(*changed, generation);
}

TEST(RedisClientTest, ElapsedCommandDeadlineDoesNotPoisonConnection) {
  absl::StatusOr<std::shared_ptr<Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<Client> client = *connected;

  absl::StatusOr<Reply> elapsed =
      client->Command({"PING"}, absl::InfinitePast()).Await();
  EXPECT_TRUE(absl::IsDeadlineExceeded(elapsed.status())) << elapsed.status();
  absl::StatusOr<Reply> ping = client->Command({"PING"}).Await();
  ASSERT_TRUE(ping.ok()) << ping.status();
  EXPECT_EQ(*ping->AsString(), "PONG");
}

}  // namespace
}  // namespace a11::redis
