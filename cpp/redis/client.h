// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A binary-safe hiredis/libuv client integrated with A11 Futures.
 */

#ifndef A11_REDIS_CLIENT_H_
#define A11_REDIS_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "redis/reply.h"

namespace a11::redis {

/** Connection and timeout policy for a Redis Client. */
struct ClientOptions {
  std::string host = "127.0.0.1";
  int port = 6379;
  std::string username;
  std::string password;
  int database = 0;
  std::string client_name = "a11";
  absl::Duration connect_timeout = absl::Seconds(10);
  absl::Duration command_timeout = absl::Seconds(10);

  absl::Status Validate() const;

  /** Parse a `redis://[user:password@]host[:port][/database]` URL. */
  static absl::StatusOr<ClientOptions> FromUrl(std::string_view url);

  /**
   * Read A11_REDIS_URL or the individual A11_REDIS_* connection variables.
   */
  static absl::StatusOr<ClientOptions> FromEnvironment();

  friend bool operator==(const ClientOptions&, const ClientOptions&) = default;
};

class Client;

/**
 * A non-buffering, broadcast Redis Pub/Sub subscription.
 *
 * Each message advances a monotonically increasing generation and wakes every
 * waiter. Payloads are deliberately not queued: this makes the primitive safe
 * for invalidation notifications even when producers outpace consumers, and
 * prevents the libuv thread from blocking on a full channel.
 */
class Subscription {
 public:
  struct State;

  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  ~Subscription();

  [[nodiscard]] std::string channel() const;
  [[nodiscard]] std::uint64_t generation() const;

  /** Wait until a message advances the generation beyond `after`. */
  a11::Future<std::uint64_t> Wait(std::uint64_t after, absl::Time deadline);

  /** Wait indefinitely until a message advances the generation. */
  a11::Future<std::uint64_t> Wait(std::uint64_t after) {
    return Wait(after, absl::InfiniteFuture());
  }

 private:
  friend class Client;

  explicit Subscription(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

/**
 * An asynchronous, fiber-aware Redis client.
 *
 * Hiredis and libuv state is confined to one process-wide I/O loop. Calls from
 * fibers, Python, or ordinary threads enqueue binary-safe commands and receive
 * A11 Futures; callbacks never block the I/O loop. Awaiting a returned Future
 * from an A11 fiber cooperatively yields its worker.
 *
 * The command connection is multiplexed, so blocking Redis commands such as
 * `BLPOP` or `XREAD BLOCK` should not be issued through it. Use Subscribe for
 * responsive invalidation waits and ordinary non-blocking commands for state.
 */
class Client : public std::enable_shared_from_this<Client> {
 public:
  struct Impl;

  /** Validate options, start connecting, and return the client immediately. */
  static absl::StatusOr<std::shared_ptr<Client>> Create(ClientOptions options);

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client();

  [[nodiscard]] const ClientOptions& options() const;

  /** Resolve when both command and subscription connections are initialized. */
  a11::Future<a11::Unit> Ready() const;

  /** Execute a binary-safe command represented by command name plus arguments. */
  a11::Future<Reply> Command(std::vector<std::string> parts,
                             absl::Time deadline);

  a11::Future<Reply> Command(std::vector<std::string> parts) {
    return Command(std::move(parts), absl::InfiniteFuture());
  }

  /** Execute a Lua script, declaring every sharding-sensitive key explicitly. */
  a11::Future<Reply> Eval(std::string script, std::vector<std::string> keys,
                          std::vector<std::string> arguments,
                          absl::Time deadline);

  a11::Future<Reply> Eval(std::string script, std::vector<std::string> keys,
                          std::vector<std::string> arguments) {
    return Eval(std::move(script), std::move(keys), std::move(arguments),
                absl::InfiniteFuture());
  }

  /** Subscribe and resolve only after Redis acknowledges the subscription. */
  a11::Future<std::shared_ptr<Subscription>> Subscribe(std::string channel,
                                                       absl::Time deadline);

  a11::Future<std::shared_ptr<Subscription>> Subscribe(std::string channel) {
    return Subscribe(std::move(channel), absl::InfiniteFuture());
  }

  /** Begin an idempotent asynchronous disconnect. */
  absl::Status Close();

 private:
  explicit Client(ClientOptions options);

  std::shared_ptr<Impl> impl_;
};

/** Return the process-global client configured from A11_REDIS_* variables. */
absl::StatusOr<std::shared_ptr<Client>> DefaultClient();

/** Replace the process-global default; useful for dependency injection/tests. */
absl::Status SetDefaultClient(std::shared_ptr<Client> client);

/** Clear an injected/default client so the environment is read again. */
void ResetDefaultClient();

}  // namespace a11::redis

#endif  // A11_REDIS_CLIENT_H_
