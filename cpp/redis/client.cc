// Copyright 2026 The A11 Authors.

#include "redis/client.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <uvw.hpp>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/container/flat_hash_map.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/cord.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#if __has_include(<hiredis/adapters/libuv.h>)
#include <hiredis/adapters/libuv.h>
#else
#include <adapters/libuv.h>
#endif

#include "a11/concurrency/future.h"
#include "redis/reply.h"
#include "thread/boost_primitives.h"
#include "thread/executor.h"

namespace a11::redis {
namespace {

constexpr size_t kMaximumReplyDepth = 64;
constexpr size_t kMaximumReplyElements = 1000000;
constexpr std::string_view kRedisErrorPayload = "type.a11.dev/redis-error+text";

absl::Status ExceptionStatus(const std::exception& error,
                             std::string_view operation) {
  return absl::UnknownError(absl::StrCat(operation, ": ", error.what()));
}

absl::Status RedisErrorStatus(std::string message) {
  const std::string upper = absl::AsciiStrToUpper(message);
  absl::Status status;
  if (absl::StartsWith(upper, "NOAUTH") ||
      absl::StartsWith(upper, "WRONGPASS")) {
    status = absl::UnauthenticatedError(absl::StrCat("Redis: ", message));
  } else if (absl::StartsWith(upper, "NOPERM")) {
    status = absl::PermissionDeniedError(absl::StrCat("Redis: ", message));
  } else if (absl::StartsWith(upper, "NOSCRIPT")) {
    status = absl::NotFoundError(absl::StrCat("Redis: ", message));
  } else if (absl::StartsWith(upper, "LOADING") ||
             absl::StartsWith(upper, "BUSY")) {
    status = absl::UnavailableError(absl::StrCat("Redis: ", message));
  } else {
    status = absl::FailedPreconditionError(absl::StrCat("Redis: ", message));
  }
  status.SetPayload(kRedisErrorPayload, absl::Cord(std::move(message)));
  return status;
}

absl::StatusOr<Reply> ParseReply(const redisReply* reply, size_t depth) {
  if (reply == nullptr)
    return absl::UnavailableError("Redis returned a null reply");
  if (depth > kMaximumReplyDepth)
    return absl::ResourceExhaustedError("Redis reply nesting is too deep");
  if (reply->elements > kMaximumReplyElements) {
    return absl::ResourceExhaustedError("Redis reply has too many elements");
  }

  const auto bytes = [reply]() -> absl::StatusOr<std::string> {
    if (reply->str == nullptr && reply->len != 0) {
      return absl::DataLossError(
          "Redis returned a null string pointer with a nonzero length");
    }
    return std::string(reply->str == nullptr ? "" : reply->str, reply->len);
  };
  const auto aggregate = [reply,
                          depth](ReplyType type) -> absl::StatusOr<Reply> {
    std::vector<Reply> values;
    values.reserve(reply->elements);
    for (size_t index = 0; index < reply->elements; ++index) {
      absl::StatusOr<Reply> child =
          ParseReply(reply->element[index], depth + 1);
      if (!child.ok())
        return child.status();
      values.push_back(std::move(*child));
    }
    if (type == ReplyType::kMap) {
      if (values.size() % 2 != 0)
        return absl::DataLossError("Redis map reply has an odd element count");
      return Reply::Map(std::move(values));
    }
    if (type == ReplyType::kSet)
      return Reply::Set(std::move(values));
    return Reply::Array(std::move(values));
  };

  switch (reply->type) {
    case REDIS_REPLY_NIL:
      return Reply::Null();
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_STATUS: {
      absl::StatusOr<std::string> value = bytes();
      if (!value.ok())
        return value.status();
      return Reply::String(std::move(*value));
    }
    case REDIS_REPLY_ERROR: {
      absl::StatusOr<std::string> value = bytes();
      if (!value.ok())
        return value.status();
      return RedisErrorStatus(std::move(*value));
    }
    case REDIS_REPLY_INTEGER:
      return Reply::Integer(static_cast<std::int64_t>(reply->integer));
    case REDIS_REPLY_ARRAY:
      return aggregate(ReplyType::kArray);
#ifdef REDIS_REPLY_DOUBLE
    case REDIS_REPLY_DOUBLE:
      return Reply::Double(reply->dval);
#endif
#ifdef REDIS_REPLY_BOOL
    case REDIS_REPLY_BOOL:
      return Reply::Boolean(reply->integer != 0);
#endif
#ifdef REDIS_REPLY_MAP
    case REDIS_REPLY_MAP:
      return aggregate(ReplyType::kMap);
#endif
#ifdef REDIS_REPLY_SET
    case REDIS_REPLY_SET:
      return aggregate(ReplyType::kSet);
#endif
#ifdef REDIS_REPLY_ATTR
    case REDIS_REPLY_ATTR:
      return aggregate(ReplyType::kMap);
#endif
#ifdef REDIS_REPLY_PUSH
    case REDIS_REPLY_PUSH:
      return aggregate(ReplyType::kArray);
#endif
#ifdef REDIS_REPLY_BIGNUM
    case REDIS_REPLY_BIGNUM: {
      absl::StatusOr<std::string> value = bytes();
      if (!value.ok())
        return value.status();
      return Reply::String(std::move(*value));
    }
#endif
#ifdef REDIS_REPLY_VERB
    case REDIS_REPLY_VERB: {
      absl::StatusOr<std::string> value = bytes();
      if (!value.ok())
        return value.status();
      return Reply::String(std::move(*value));
    }
#endif
    default:
      return absl::UnimplementedError(
          absl::StrCat("Unsupported hiredis reply type ", reply->type));
  }
}

absl::StatusOr<Reply> ParseReply(const redisReply* reply) {
  return ParseReply(reply, 0);
}

absl::StatusOr<int> ParseInteger(std::string_view value, std::string_view name,
                                 int minimum, int maximum) {
  if (value.empty())
    return absl::InvalidArgumentError(absl::StrCat(name, " is empty"));
  int result = 0;
  const char* first = value.data();
  const char* last = first + value.size();
  const auto parsed = std::from_chars(first, last, result);
  if (parsed.ec != std::errc{} || parsed.ptr != last || result < minimum ||
      result > maximum) {
    return absl::InvalidArgumentError(
        absl::StrCat(name, " is not a valid integer"));
  }
  return result;
}

absl::StatusOr<std::string> PercentDecode(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  constexpr std::string_view kHex = "0123456789abcdef";
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      decoded.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size())
      return absl::InvalidArgumentError(
          "Truncated percent escape in Redis URL");
    const char high =
        absl::ascii_tolower(static_cast<unsigned char>(value[index + 1]));
    const char low =
        absl::ascii_tolower(static_cast<unsigned char>(value[index + 2]));
    const size_t high_value = kHex.find(high);
    const size_t low_value = kHex.find(low);
    if (high_value == std::string_view::npos ||
        low_value == std::string_view::npos) {
      return absl::InvalidArgumentError("Invalid percent escape in Redis URL");
    }
    decoded.push_back(
        static_cast<char>((high_value << 4U) | static_cast<size_t>(low_value)));
    index += 2;
  }
  return decoded;
}

std::optional<std::string> EnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr)
    return std::nullopt;
  return std::string(value);
}

class RedisIoLoop {
 public:
  static RedisIoLoop& Instance() {
    static absl::NoDestructor<RedisIoLoop> loop;
    return *loop;
  }

  absl::Status Post(std::function<void()> work) {
    if (!work)
      return absl::InvalidArgumentError("Redis I/O work must be callable");
    {
      thread::MutexLock lock(&mu_);
      if (!running_)
        return absl::FailedPreconditionError("The Redis I/O loop is stopped");
      try {
        work_.push_back(std::move(work));
      } catch (const std::exception& error) {
        return ExceptionStatus(error, "Queueing Redis I/O work");
      } catch (...) {
        return absl::UnknownError(
            "Queueing Redis I/O work raised a non-standard exception");
      }
    }
    const int result = async_->send();
    if (result != 0) {
      return absl::InternalError(
          absl::StrCat("uv_async_send failed: ", uv_strerror(result)));
    }
    return absl::OkStatus();
  }

  [[nodiscard]] uv_loop_t* loop() const { return loop_->raw(); }

 private:
  friend class absl::NoDestructor<RedisIoLoop>;

  RedisIoLoop() {
    try {
      loop_ = uvw::loop::create();
      async_ = loop_->resource<uvw::async_handle>();
      const int initialized = async_->init();
      if (initialized != 0) {
        LOG(FATAL) << "Could not initialize the Redis libuv executor: "
                   << uv_strerror(initialized);
      }
      async_->on<uvw::async_event>(
          [this](const uvw::async_event&, uvw::async_handle&) { Drain(); });
      thread_ = std::thread([this] {
        {
          thread::MutexLock lock(&mu_);
          started_ = true;
          cv_.SignalAll();
        }
        loop_->run();
        thread::MutexLock lock(&mu_);
        running_ = false;
      });
    } catch (const std::exception& error) {
      LOG(FATAL) << "Could not create the Redis libuv executor: "
                 << error.what();
    } catch (...) {
      LOG(FATAL) << "Could not create the Redis libuv executor";
    }
    thread::MutexLock lock(&mu_);
    while (!started_)
      cv_.Wait(&mu_);
  }

  void Drain() {
    std::deque<std::function<void()>> work;
    {
      thread::MutexLock lock(&mu_);
      work.swap(work_);
    }
    for (auto& item : work) {
      try {
        item();
      } catch (const std::exception& error) {
        LOG(ERROR) << "Redis I/O work raised: " << error.what();
      } catch (...) {
        LOG(ERROR) << "Redis I/O work raised a non-standard exception";
      }
    }
  }

  thread::Mutex mu_;
  thread::CondVar cv_;
  bool running_ ABSL_GUARDED_BY(mu_) = true;
  bool started_ ABSL_GUARDED_BY(mu_) = false;
  std::deque<std::function<void()>> work_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<uvw::loop> loop_;
  std::shared_ptr<uvw::async_handle> async_;
  std::thread thread_;
};

template <typename T>
struct PendingResult {
  std::atomic<bool> completed = false;
  a11::Promise<T> promise;

  [[nodiscard]] bool TryComplete() {
    return !completed.exchange(true, std::memory_order_acq_rel);
  }

  void Complete(absl::StatusOr<T> result) {
    if (TryComplete())
      promise.SetResult(std::move(result)).IgnoreError();
  }

  void Fail(absl::Status status) {
    if (!status.ok() && TryComplete())
      promise.SetStatus(std::move(status)).IgnoreError();
  }
};

struct CommandRequest : PendingResult<Reply> {
  std::vector<std::string> parts;
};

struct CommandToken {
  std::shared_ptr<CommandRequest> request;
};

void CommandCallback(redisAsyncContext* context, void* hiredis_reply,
                     void* private_data) {
  std::unique_ptr<CommandToken> token(static_cast<CommandToken*>(private_data));
  if (token == nullptr || token->request == nullptr ||
      token->request->completed.load(std::memory_order_acquire)) {
    return;
  }
  if (hiredis_reply == nullptr) {
    const std::string detail = context == nullptr || context->errstr == nullptr
                                   ? "connection closed"
                                   : context->errstr;
    token->request->Fail(
        absl::UnavailableError(absl::StrCat("Redis command failed: ", detail)));
    return;
  }
  token->request->Complete(
      ParseReply(static_cast<const redisReply*>(hiredis_reply)));
}

thread::Mutex& DefaultClientMutex() {
  static absl::NoDestructor<thread::Mutex> mu;
  return *mu;
}

std::shared_ptr<Client>& DefaultClientStorage() {
  static absl::NoDestructor<std::shared_ptr<Client>> client;
  return *client;
}

}  // namespace

absl::Status ClientOptions::Validate() const {
  if (host.empty())
    return absl::InvalidArgumentError("Redis host must not be empty");
  if (port <= 0 || port > 65535)
    return absl::InvalidArgumentError("Redis port must be between 1 and 65535");
  if (database < 0)
    return absl::InvalidArgumentError("Redis database must be non-negative");
  if (connect_timeout <= absl::ZeroDuration() ||
      connect_timeout == absl::InfiniteDuration()) {
    return absl::InvalidArgumentError(
        "Redis connect_timeout must be positive and finite");
  }
  if (command_timeout <= absl::ZeroDuration()) {
    return absl::InvalidArgumentError("Redis command_timeout must be positive");
  }
  return absl::OkStatus();
}

absl::StatusOr<ClientOptions> ClientOptions::FromUrl(std::string_view url) {
  constexpr std::string_view kScheme = "redis://";
  if (absl::StartsWith(url, "rediss://")) {
    return absl::UnimplementedError(
        "rediss:// is not enabled; use a TLS-terminating Redis proxy");
  }
  if (!absl::StartsWith(url, kScheme)) {
    return absl::InvalidArgumentError("Redis URL must start with redis://");
  }
  std::string_view remainder = url.substr(kScheme.size());
  const size_t query = remainder.find_first_of("?#");
  if (query != std::string_view::npos) {
    return absl::InvalidArgumentError(
        "Redis URL query strings and fragments are not supported");
  }

  ClientOptions options;
  std::string_view authority = remainder;
  std::string_view path;
  if (const size_t slash = remainder.find('/');
      slash != std::string_view::npos) {
    authority = remainder.substr(0, slash);
    path = remainder.substr(slash + 1);
  }

  std::string_view host_port = authority;
  if (const size_t at = authority.rfind('@'); at != std::string_view::npos) {
    const std::string_view user_info = authority.substr(0, at);
    host_port = authority.substr(at + 1);
    const size_t colon = user_info.find(':');
    absl::StatusOr<std::string> username =
        PercentDecode(user_info.substr(0, colon));
    if (!username.ok())
      return username.status();
    options.username = std::move(*username);
    if (colon != std::string_view::npos) {
      absl::StatusOr<std::string> password =
          PercentDecode(user_info.substr(colon + 1));
      if (!password.ok())
        return password.status();
      options.password = std::move(*password);
    }
  }

  std::string_view host;
  std::string_view port;
  if (absl::StartsWith(host_port, "[")) {
    const size_t close = host_port.find(']');
    if (close == std::string_view::npos)
      return absl::InvalidArgumentError(
          "Redis URL has an unterminated IPv6 host");
    host = host_port.substr(1, close - 1);
    if (close + 1 < host_port.size()) {
      if (host_port[close + 1] != ':')
        return absl::InvalidArgumentError("Invalid text after Redis IPv6 host");
      port = host_port.substr(close + 2);
    }
  } else if (const size_t colon = host_port.rfind(':');
             colon != std::string_view::npos) {
    host = host_port.substr(0, colon);
    port = host_port.substr(colon + 1);
  } else {
    host = host_port;
  }
  absl::StatusOr<std::string> decoded_host = PercentDecode(host);
  if (!decoded_host.ok())
    return decoded_host.status();
  options.host = std::move(*decoded_host);
  if (!port.empty()) {
    absl::StatusOr<int> parsed = ParseInteger(port, "Redis URL port", 1, 65535);
    if (!parsed.ok())
      return parsed.status();
    options.port = *parsed;
  }
  if (!path.empty()) {
    absl::StatusOr<int> database = ParseInteger(
        path, "Redis URL database", 0, std::numeric_limits<int>::max());
    if (!database.ok())
      return database.status();
    options.database = *database;
  }
  absl::Status validation = options.Validate();
  if (!validation.ok())
    return validation;
  return options;
}

absl::StatusOr<ClientOptions> ClientOptions::FromEnvironment() {
  if (std::optional<std::string> url = EnvironmentValue("A11_REDIS_URL"))
    return FromUrl(*url);

  ClientOptions options;
  if (std::optional<std::string> value = EnvironmentValue("A11_REDIS_HOST"))
    options.host = std::move(*value);
  if (std::optional<std::string> value = EnvironmentValue("A11_REDIS_PORT")) {
    absl::StatusOr<int> parsed =
        ParseInteger(*value, "A11_REDIS_PORT", 1, 65535);
    if (!parsed.ok())
      return parsed.status();
    options.port = *parsed;
  }
  if (std::optional<std::string> value =
          EnvironmentValue("A11_REDIS_USERNAME")) {
    options.username = std::move(*value);
  }
  if (std::optional<std::string> value =
          EnvironmentValue("A11_REDIS_PASSWORD")) {
    options.password = std::move(*value);
  }
  if (std::optional<std::string> value = EnvironmentValue("A11_REDIS_DB")) {
    absl::StatusOr<int> parsed = ParseInteger(*value, "A11_REDIS_DB", 0,
                                              std::numeric_limits<int>::max());
    if (!parsed.ok())
      return parsed.status();
    options.database = *parsed;
  }
  if (std::optional<std::string> value =
          EnvironmentValue("A11_REDIS_CLIENT_NAME")) {
    options.client_name = std::move(*value);
  }
  if (std::optional<std::string> value =
          EnvironmentValue("A11_REDIS_CONNECT_TIMEOUT_MS")) {
    absl::StatusOr<int> parsed =
        ParseInteger(*value, "A11_REDIS_CONNECT_TIMEOUT_MS", 1,
                     std::numeric_limits<int>::max());
    if (!parsed.ok())
      return parsed.status();
    options.connect_timeout = absl::Milliseconds(*parsed);
  }
  if (std::optional<std::string> value =
          EnvironmentValue("A11_REDIS_COMMAND_TIMEOUT_MS")) {
    absl::StatusOr<int> parsed =
        ParseInteger(*value, "A11_REDIS_COMMAND_TIMEOUT_MS", 1,
                     std::numeric_limits<int>::max());
    if (!parsed.ok())
      return parsed.status();
    options.command_timeout = absl::Milliseconds(*parsed);
  }
  absl::Status validation = options.Validate();
  if (!validation.ok())
    return validation;
  return options;
}

struct Subscription::State
    : public std::enable_shared_from_this<Subscription::State> {
  struct Waiter : PendingResult<std::uint64_t> {
    explicit Waiter(std::uint64_t requested_generation)
        : after(requested_generation) {}

    std::uint64_t after;
  };

  explicit State(std::string subscribed_channel)
      : channel(std::move(subscribed_channel)), ready(ready_promise.future()) {}

  void MarkReady() {
    bool complete = false;
    {
      thread::MutexLock lock(&mu);
      if (!ready_completed && !terminal_status.has_value()) {
        ready_completed = true;
        complete = true;
      }
    }
    if (complete)
      ready_promise.SetValue(a11::Unit{}).IgnoreError();
  }

  void Notify() {
    std::vector<std::shared_ptr<Waiter>> completions;
    std::uint64_t published = 0;
    absl::Status overflow;
    {
      thread::MutexLock lock(&mu);
      if (terminal_status.has_value())
        return;
      if (generation == std::numeric_limits<std::uint64_t>::max()) {
        overflow = absl::ResourceExhaustedError(
            "Redis subscription generation overflowed");
      } else {
        published = ++generation;
        for (auto iterator = waiters.begin(); iterator != waiters.end();) {
          if ((*iterator)->after < published && (*iterator)->TryComplete()) {
            completions.push_back(*iterator);
            iterator = waiters.erase(iterator);
          } else {
            ++iterator;
          }
        }
      }
    }
    if (!overflow.ok()) {
      Fail(std::move(overflow));
      return;
    }
    for (const auto& waiter : completions)
      waiter->promise.SetValue(published).IgnoreError();
  }

  void Fail(absl::Status status) {
    if (status.ok())
      status = absl::UnknownError("Redis subscription failed without an error");
    std::vector<std::shared_ptr<Waiter>> pending;
    bool fail_ready = false;
    {
      thread::MutexLock lock(&mu);
      if (terminal_status.has_value())
        return;
      terminal_status = status;
      fail_ready = !ready_completed;
      ready_completed = true;
      pending.swap(waiters);
    }
    if (fail_ready)
      ready_promise.SetStatus(status).IgnoreError();
    for (const auto& waiter : pending)
      waiter->Fail(status);
  }

  void RemoveWaiter(const Waiter* waiter) {
    thread::MutexLock lock(&mu);
    std::erase_if(waiters, [waiter](const std::shared_ptr<Waiter>& candidate) {
      return candidate.get() == waiter;
    });
  }

  a11::Future<std::uint64_t> Wait(std::uint64_t after, absl::Time deadline) {
    auto waiter = std::make_shared<Waiter>(after);
    a11::Future<std::uint64_t> future = waiter->promise.future();
    const std::weak_ptr<State> weak_state = weak_from_this();
    const std::weak_ptr<Waiter> weak_waiter = waiter;
    waiter->promise
        .SetCancellationCallback([weak_state, weak_waiter] {
          const std::shared_ptr<Waiter> pending = weak_waiter.lock();
          if (pending == nullptr || !pending->TryComplete())
            return;
          pending->promise
              .SetStatus(
                  absl::CancelledError("Redis subscription wait was cancelled"))
              .IgnoreError();
          if (const std::shared_ptr<State> state = weak_state.lock())
            state->RemoveWaiter(pending.get());
        })
        .IgnoreError();

    std::optional<std::uint64_t> immediate;
    std::optional<absl::Status> failure;
    {
      thread::MutexLock lock(&mu);
      if (generation > after) {
        immediate = generation;
      } else if (terminal_status.has_value()) {
        failure = *terminal_status;
      } else {
        waiters.push_back(waiter);
      }
    }
    if (immediate.has_value())
      waiter->Complete(*immediate);
    else if (failure.has_value())
      waiter->Fail(*failure);

    if (!waiter->completed.load(std::memory_order_acquire) &&
        deadline != absl::InfiniteFuture()) {
      thread::PostAt(deadline, [weak_state, weak_waiter] {
        const std::shared_ptr<Waiter> pending = weak_waiter.lock();
        if (pending == nullptr || !pending->TryComplete())
          return;
        pending->promise
            .SetStatus(absl::DeadlineExceededError(
                "Redis subscription did not change before the deadline"))
            .IgnoreError();
        if (const std::shared_ptr<State> state = weak_state.lock())
          state->RemoveWaiter(pending.get());
      });
    }
    return future;
  }

  mutable thread::Mutex mu;
  const std::string channel;
  std::uint64_t generation ABSL_GUARDED_BY(mu) = 0;
  bool ready_completed ABSL_GUARDED_BY(mu) = false;
  std::optional<absl::Status> terminal_status ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<Waiter>> waiters ABSL_GUARDED_BY(mu);
  a11::Promise<a11::Unit> ready_promise;
  a11::Future<a11::Unit> ready;
  std::function<void(const State*)> on_release;
};

struct Client::Impl : public std::enable_shared_from_this<Client::Impl> {
  struct QueuedCommand {
    std::shared_ptr<CommandRequest> request;
  };

  struct Channel {
    bool command_sent = false;
    bool subscribed = false;
    bool unsubscribe_after_ack = false;
    std::vector<std::weak_ptr<Subscription::State>> listeners;
  };

  struct InitializationToken {
    std::shared_ptr<Impl> impl;
    bool subscriber = false;
    size_t command_index = 0;
  };

  explicit Impl(ClientOptions client_options)
      : options(std::move(client_options)), ready(ready_promise.future()) {}

  void Start() {
    self_keepalive = shared_from_this();
    absl::Status queued = RedisIoLoop::Instance().Post(
        [self = shared_from_this()] { self->StartOnLoop(); });
    if (!queued.ok()) {
      Fail(std::move(queued));
      self_keepalive.reset();
    }
  }

  static void ConnectCallback(const redisAsyncContext* context, int status) {
    auto* impl = static_cast<Impl*>(context->data);
    if (impl != nullptr)
      impl->OnConnect(const_cast<redisAsyncContext*>(context), status);
  }

  static void DisconnectCallback(const redisAsyncContext* context, int status) {
    auto* impl = static_cast<Impl*>(context->data);
    if (impl != nullptr)
      impl->OnDisconnect(const_cast<redisAsyncContext*>(context), status);
  }

  static void InitializationCallback(redisAsyncContext* context,
                                     void* hiredis_reply, void* private_data) {
    std::unique_ptr<InitializationToken> token(
        static_cast<InitializationToken*>(private_data));
    if (token == nullptr || token->impl == nullptr)
      return;
    if (hiredis_reply == nullptr) {
      const std::string detail =
          context == nullptr || context->errstr == nullptr ? "connection closed"
                                                           : context->errstr;
      token->impl->Fail(absl::UnavailableError(
          absl::StrCat("Redis initialization failed: ", detail)));
      return;
    }
    absl::StatusOr<Reply> parsed =
        ParseReply(static_cast<const redisReply*>(hiredis_reply));
    if (!parsed.ok()) {
      token->impl->Fail(parsed.status());
      return;
    }
    token->impl->SendInitialization(context, token->subscriber,
                                    token->command_index + 1);
  }

  static void PubSubCallback(redisAsyncContext* context, void* hiredis_reply,
                             void* private_data) {
    auto* impl = static_cast<Impl*>(private_data);
    if (impl == nullptr)
      return;
    if (hiredis_reply == nullptr) {
      const std::string detail =
          context == nullptr || context->errstr == nullptr ? "connection closed"
                                                           : context->errstr;
      impl->Fail(absl::UnavailableError(
          absl::StrCat("Redis subscription failed: ", detail)));
      return;
    }
    impl->OnPubSubReply(static_cast<const redisReply*>(hiredis_reply));
  }

  void StartOnLoop() {
    if (closing || terminal_status.has_value())
      return;
    absl::StatusOr<redisAsyncContext*> command = CreateContext();
    if (!command.ok()) {
      Fail(command.status());
      return;
    }
    command_context = *command;
    absl::StatusOr<redisAsyncContext*> subscriber = CreateContext();
    if (!subscriber.ok()) {
      Fail(subscriber.status());
      return;
    }
    subscriber_context = *subscriber;
  }

  absl::StatusOr<redisAsyncContext*> CreateContext() {
    redisOptions hiredis_options{};
    timeval timeout = absl::ToTimeval(options.connect_timeout);
    hiredis_options.connect_timeout = &timeout;
    REDIS_OPTIONS_SET_TCP(&hiredis_options, options.host.c_str(), options.port);
    redisAsyncContext* context = redisAsyncConnectWithOptions(&hiredis_options);
    if (context == nullptr)
      return absl::ResourceExhaustedError("Could not allocate Redis context");
    if (context->err != REDIS_OK) {
      const std::string error = context->errstr == nullptr
                                    ? "unknown hiredis connection error"
                                    : context->errstr;
      redisAsyncFree(context);
      return absl::UnavailableError(error);
    }
    context->data = this;
    if (redisLibuvAttach(context, RedisIoLoop::Instance().loop()) != REDIS_OK) {
      context->data = nullptr;
      redisAsyncFree(context);
      return absl::InternalError("Could not attach hiredis to libuv");
    }
    if (redisAsyncSetConnectCallback(context, ConnectCallback) != REDIS_OK ||
        redisAsyncSetDisconnectCallback(context, DisconnectCallback) !=
            REDIS_OK) {
      context->data = nullptr;
      redisAsyncFree(context);
      return absl::InternalError("Could not install hiredis callbacks");
    }
    return context;
  }

  std::vector<std::vector<std::string>> InitializationCommands(
      bool subscriber) const {
    std::vector<std::vector<std::string>> commands;
    if (!options.password.empty() || !options.username.empty()) {
      if (options.username.empty())
        commands.push_back({"AUTH", options.password});
      else
        commands.push_back({"AUTH", options.username, options.password});
    }
    if (options.database != 0)
      commands.push_back({"SELECT", std::to_string(options.database)});
    if (!options.client_name.empty()) {
      commands.push_back(
          {"CLIENT", "SETNAME",
           absl::StrCat(options.client_name, subscriber ? "-subscriber" : "")});
    }
    return commands;
  }

  void SendInitialization(redisAsyncContext* context, bool subscriber,
                          size_t command_index) {
    if (terminal_status.has_value() || closing)
      return;
    const std::vector<std::vector<std::string>> commands =
        InitializationCommands(subscriber);
    if (command_index == commands.size()) {
      if (subscriber)
        subscriber_initialized = true;
      else
        command_initialized = true;
      MaybeReady();
      return;
    }
    const std::vector<std::string>& parts = commands[command_index];
    std::vector<const char*> values;
    std::vector<size_t> lengths;
    values.reserve(parts.size());
    lengths.reserve(parts.size());
    for (const std::string& part : parts) {
      values.push_back(part.data());
      lengths.push_back(part.size());
    }
    auto* token = new InitializationToken{
        .impl = shared_from_this(),
        .subscriber = subscriber,
        .command_index = command_index,
    };
    const int result = redisAsyncCommandArgv(
        context, InitializationCallback, token, static_cast<int>(values.size()),
        values.data(), lengths.data());
    if (result != REDIS_OK) {
      delete token;
      Fail(absl::UnavailableError("Could not queue Redis initialization"));
    }
  }

  void OnConnect(redisAsyncContext* context, int status) {
    if (status != REDIS_OK) {
      const std::string detail =
          context->errstr == nullptr ? "connection failed" : context->errstr;
      Fail(absl::UnavailableError(absl::StrCat("Could not connect to Redis at ",
                                               options.host, ":", options.port,
                                               ": ", detail)));
      return;
    }
    if (context == command_context) {
      SendInitialization(context, false, 0);
    } else if (context == subscriber_context) {
      SendInitialization(context, true, 0);
    } else {
      Fail(absl::InternalError("Unknown hiredis context connected"));
    }
  }

  void OnDisconnect(redisAsyncContext* context, int status) {
    if (context == command_context)
      command_context = nullptr;
    if (context == subscriber_context)
      subscriber_context = nullptr;
    if (!closing && !terminal_status.has_value()) {
      const std::string detail =
          status == REDIS_OK || context->errstr == nullptr ? "connection closed"
                                                           : context->errstr;
      Fail(absl::UnavailableError(
          absl::StrCat("Disconnected from Redis: ", detail)));
    }
    MaybeRelease();
  }

  void MaybeReady() {
    if (!command_initialized || !subscriber_initialized || ready_completed ||
        terminal_status.has_value() || closing) {
      return;
    }
    ready_completed = true;
    ready_promise.SetValue(a11::Unit{}).IgnoreError();

    std::vector<QueuedCommand> commands;
    commands.swap(queued_commands);
    for (QueuedCommand& command : commands)
      SendCommand(std::move(command.request));
    for (auto& [channel, state] : channels)
      EnsureSubscribed(channel, &state);
  }

  void QueueOrSend(std::shared_ptr<CommandRequest> request) {
    if (request->completed.load(std::memory_order_acquire))
      return;
    if (terminal_status.has_value()) {
      request->Fail(*terminal_status);
      return;
    }
    if (closing) {
      request->Fail(absl::CancelledError("Redis client is closing"));
      return;
    }
    if (!ready_completed) {
      queued_commands.push_back(QueuedCommand{.request = std::move(request)});
      return;
    }
    SendCommand(std::move(request));
  }

  void SendCommand(std::shared_ptr<CommandRequest> request) {
    if (request->completed.load(std::memory_order_acquire))
      return;
    if (command_context == nullptr || terminal_status.has_value()) {
      request->Fail(terminal_status.value_or(
          absl::UnavailableError("Redis command connection is unavailable")));
      return;
    }
    std::vector<const char*> values;
    std::vector<size_t> lengths;
    values.reserve(request->parts.size());
    lengths.reserve(request->parts.size());
    for (const std::string& part : request->parts) {
      values.push_back(part.data());
      lengths.push_back(part.size());
    }
    auto* token = new CommandToken{.request = request};
    const int result = redisAsyncCommandArgv(
        command_context, CommandCallback, token,
        static_cast<int>(values.size()), values.data(), lengths.data());
    if (result != REDIS_OK) {
      delete token;
      request->Fail(absl::UnavailableError("Could not queue Redis command"));
    }
  }

  void RegisterSubscription(
      const std::shared_ptr<Subscription::State>& listener) {
    if (terminal_status.has_value()) {
      listener->Fail(*terminal_status);
      return;
    }
    if (closing) {
      listener->Fail(absl::CancelledError("Redis client is closing"));
      return;
    }
    Channel& channel = channels[listener->channel];
    channel.listeners.push_back(listener);
    if (channel.subscribed) {
      listener->MarkReady();
      return;
    }
    if (ready_completed)
      EnsureSubscribed(listener->channel, &channel);
  }

  void EnsureSubscribed(const std::string& name, Channel* channel) {
    RemoveExpiredListeners(channel);
    if (channel->listeners.empty() || channel->command_sent ||
        channel->subscribed || subscriber_context == nullptr) {
      return;
    }
    std::vector<std::string> parts{"SUBSCRIBE", name};
    std::vector<const char*> values;
    std::vector<size_t> lengths;
    for (const std::string& part : parts) {
      values.push_back(part.data());
      lengths.push_back(part.size());
    }
    const int result = redisAsyncCommandArgv(
        subscriber_context, PubSubCallback, this,
        static_cast<int>(values.size()), values.data(), lengths.data());
    if (result != REDIS_OK) {
      Fail(absl::UnavailableError("Could not queue Redis SUBSCRIBE"));
      return;
    }
    channel->command_sent = true;
  }

  void RemoveSubscription(std::string channel_name,
                          const Subscription::State* listener) {
    const auto found = channels.find(channel_name);
    if (found == channels.end())
      return;
    Channel& channel = found->second;
    std::erase_if(channel.listeners,
                  [listener](const std::weak_ptr<Subscription::State>& weak) {
                    const std::shared_ptr<Subscription::State> candidate =
                        weak.lock();
                    return candidate == nullptr || candidate.get() == listener;
                  });
    if (!channel.listeners.empty())
      return;
    if (channel.subscribed)
      SendUnsubscribe(channel_name, &channel);
    else if (channel.command_sent)
      channel.unsubscribe_after_ack = true;
    else
      channels.erase(found);
  }

  void SendUnsubscribe(const std::string& name, Channel* channel) {
    if (subscriber_context == nullptr)
      return;
    std::vector<std::string> parts{"UNSUBSCRIBE", name};
    std::vector<const char*> values;
    std::vector<size_t> lengths;
    for (const std::string& part : parts) {
      values.push_back(part.data());
      lengths.push_back(part.size());
    }
    if (redisAsyncCommandArgv(subscriber_context, PubSubCallback, this,
                              static_cast<int>(values.size()), values.data(),
                              lengths.data()) != REDIS_OK) {
      Fail(absl::UnavailableError("Could not queue Redis UNSUBSCRIBE"));
      return;
    }
    channel->subscribed = false;
    channel->unsubscribe_after_ack = false;
  }

  static void RemoveExpiredListeners(Channel* channel) {
    std::erase_if(channel->listeners,
                  [](const std::weak_ptr<Subscription::State>& listener) {
                    return listener.expired();
                  });
  }

  void OnPubSubReply(const redisReply* hiredis_reply) {
    absl::StatusOr<Reply> parsed = ParseReply(hiredis_reply);
    if (!parsed.ok()) {
      Fail(parsed.status());
      return;
    }
    absl::StatusOr<const std::vector<Reply>*> elements = parsed->AsElements();
    if (!elements.ok() || (*elements)->size() < 3) {
      Fail(absl::DataLossError("Malformed Redis Pub/Sub reply"));
      return;
    }
    absl::StatusOr<std::string> type = (**elements)[0].AsString();
    absl::StatusOr<std::string> channel_name = (**elements)[1].AsString();
    if (!type.ok() || !channel_name.ok()) {
      Fail(absl::DataLossError("Redis Pub/Sub reply fields are not strings"));
      return;
    }
    const auto found = channels.find(*channel_name);
    if (found == channels.end())
      return;
    Channel& channel = found->second;
    RemoveExpiredListeners(&channel);
    if (*type == "subscribe") {
      channel.command_sent = false;
      channel.subscribed = true;
      for (const auto& weak : channel.listeners) {
        if (const std::shared_ptr<Subscription::State> listener = weak.lock())
          listener->MarkReady();
      }
      if (channel.listeners.empty() || channel.unsubscribe_after_ack)
        SendUnsubscribe(*channel_name, &channel);
      return;
    }
    if (*type == "unsubscribe") {
      channel.command_sent = false;
      channel.subscribed = false;
      if (channel.listeners.empty())
        channels.erase(found);
      else
        EnsureSubscribed(*channel_name, &channel);
      return;
    }
    if (*type != "message") {
      Fail(absl::DataLossError(
          absl::StrCat("Unexpected Redis Pub/Sub frame: ", *type)));
      return;
    }
    for (const auto& weak : channel.listeners) {
      if (const std::shared_ptr<Subscription::State> listener = weak.lock())
        listener->Notify();
    }
  }

  void Fail(absl::Status status) {
    if (status.ok())
      status = absl::UnknownError("Redis client failed without an error");
    if (terminal_status.has_value())
      return;
    terminal_status = status;
    if (!ready_completed) {
      ready_completed = true;
      ready_promise.SetStatus(status).IgnoreError();
    }
    std::vector<QueuedCommand> commands;
    commands.swap(queued_commands);
    for (QueuedCommand& command : commands)
      command.request->Fail(status);
    for (auto& [name, channel] : channels) {
      (void)name;
      for (const auto& weak : channel.listeners) {
        if (const std::shared_ptr<Subscription::State> listener = weak.lock())
          listener->Fail(status);
      }
    }
    channels.clear();
    if (command_context != nullptr)
      redisAsyncDisconnect(command_context);
    if (subscriber_context != nullptr)
      redisAsyncDisconnect(subscriber_context);
    MaybeRelease();
  }

  absl::Status RequestClose() {
    if (close_requested.exchange(true, std::memory_order_acq_rel))
      return absl::OkStatus();
    absl::Status queued = RedisIoLoop::Instance().Post(
        [self = shared_from_this()] { self->CloseOnLoop(); });
    if (!queued.ok())
      return queued;
    return absl::OkStatus();
  }

  void CloseOnLoop() {
    if (closing)
      return;
    closing = true;
    Fail(absl::CancelledError("Redis client was closed"));
  }

  void MaybeRelease() {
    if ((terminal_status.has_value() || closing) &&
        command_context == nullptr && subscriber_context == nullptr) {
      std::shared_ptr<Impl> keepalive = std::move(self_keepalive);
      (void)keepalive;
    }
  }

  const ClientOptions options;
  a11::Promise<a11::Unit> ready_promise;
  a11::Future<a11::Unit> ready;
  std::shared_ptr<Impl> self_keepalive;
  std::atomic<bool> close_requested = false;

  // All fields below are confined to RedisIoLoop's thread.
  redisAsyncContext* command_context = nullptr;
  redisAsyncContext* subscriber_context = nullptr;
  bool command_initialized = false;
  bool subscriber_initialized = false;
  bool ready_completed = false;
  bool closing = false;
  std::optional<absl::Status> terminal_status;
  std::vector<QueuedCommand> queued_commands;
  absl::flat_hash_map<std::string, Channel> channels;
};

Subscription::~Subscription() {
  if (state_ == nullptr)
    return;
  State* identity = state_.get();
  std::function<void(const State*)> release = state_->on_release;
  state_->Fail(absl::CancelledError("Redis subscription was released"));
  if (release)
    release(identity);
}

std::string Subscription::channel() const {
  return state_->channel;
}

std::uint64_t Subscription::generation() const {
  thread::MutexLock lock(&state_->mu);
  return state_->generation;
}

a11::Future<std::uint64_t> Subscription::Wait(std::uint64_t after,
                                              absl::Time deadline) {
  return state_->Wait(after, deadline);
}

Client::Client(ClientOptions options)
    : impl_(std::make_shared<Impl>(std::move(options))) {}

absl::StatusOr<std::shared_ptr<Client>> Client::Create(ClientOptions options) {
  absl::Status validation = options.Validate();
  if (!validation.ok())
    return validation;
  std::shared_ptr<Client> client(new Client(std::move(options)));
  client->impl_->Start();
  return client;
}

Client::~Client() {
  if (impl_ != nullptr)
    impl_->RequestClose().IgnoreError();
}

const ClientOptions& Client::options() const {
  return impl_->options;
}

a11::Future<a11::Unit> Client::Ready() const {
  return impl_->ready;
}

a11::Future<Reply> Client::Command(std::vector<std::string> parts,
                                   absl::Time deadline) {
  if (parts.empty() || parts.front().empty()) {
    return a11::FailedFuture<Reply>(
        absl::InvalidArgumentError("Redis command must not be empty"));
  }
  if (parts.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return a11::FailedFuture<Reply>(
        absl::ResourceExhaustedError("Redis command has too many arguments"));
  }
  auto request = std::make_shared<CommandRequest>();
  request->parts = std::move(parts);
  a11::Future<Reply> future = request->promise.future();
  const std::weak_ptr<CommandRequest> weak_request = request;
  request->promise
      .SetCancellationCallback([weak_request] {
        if (const std::shared_ptr<CommandRequest> pending = weak_request.lock())
          pending->Fail(absl::CancelledError("Redis command was cancelled"));
      })
      .IgnoreError();

  const absl::Time timeout_deadline =
      impl_->options.command_timeout == absl::InfiniteDuration()
          ? absl::InfiniteFuture()
          : absl::Now() + impl_->options.command_timeout;
  if (deadline == absl::InfiniteFuture() || timeout_deadline < deadline)
    deadline = timeout_deadline;
  if (deadline != absl::InfiniteFuture()) {
    thread::PostAt(deadline, [weak_request] {
      if (const std::shared_ptr<CommandRequest> pending = weak_request.lock()) {
        pending->Fail(absl::DeadlineExceededError(
            "Redis command did not complete before the deadline"));
      }
    });
  }

  absl::Status queued = RedisIoLoop::Instance().Post(
      [impl = impl_, request] { impl->QueueOrSend(request); });
  if (!queued.ok())
    request->Fail(std::move(queued));
  return future;
}

a11::Future<Reply> Client::Eval(std::string script,
                                std::vector<std::string> keys,
                                std::vector<std::string> arguments,
                                absl::Time deadline) {
  if (script.empty()) {
    return a11::FailedFuture<Reply>(
        absl::InvalidArgumentError("Redis script must not be empty"));
  }
  std::vector<std::string> command;
  command.reserve(keys.size() + arguments.size() + 3);
  command.push_back("EVAL");
  command.push_back(std::move(script));
  command.push_back(std::to_string(keys.size()));
  for (std::string& key : keys)
    command.push_back(std::move(key));
  for (std::string& argument : arguments)
    command.push_back(std::move(argument));
  return Command(std::move(command), deadline);
}

a11::Future<std::shared_ptr<Subscription>> Client::Subscribe(
    std::string channel, absl::Time deadline) {
  if (channel.empty()) {
    return a11::FailedFuture<std::shared_ptr<Subscription>>(
        absl::InvalidArgumentError("Redis channel must not be empty"));
  }
  auto state = std::make_shared<Subscription::State>(std::move(channel));
  auto subscription = std::shared_ptr<Subscription>(new Subscription(state));
  const std::weak_ptr<Impl> weak_impl = impl_;
  const std::string channel_name = state->channel;
  state->on_release = [weak_impl,
                       channel_name](const Subscription::State* value) {
    if (const std::shared_ptr<Impl> impl = weak_impl.lock()) {
      RedisIoLoop::Instance()
          .Post([impl, channel_name, value] {
            impl->RemoveSubscription(channel_name, value);
          })
          .IgnoreError();
    }
  };

  auto result =
      std::make_shared<PendingResult<std::shared_ptr<Subscription>>>();
  a11::Future<std::shared_ptr<Subscription>> future = result->promise.future();
  const std::weak_ptr<Subscription::State> weak_state = state;
  result->promise
      .SetCancellationCallback([weak_state, result] {
        if (const std::shared_ptr<Subscription::State> active =
                weak_state.lock())
          active->Fail(
              absl::CancelledError("Redis subscribe operation was cancelled"));
        result->Fail(
            absl::CancelledError("Redis subscribe operation was cancelled"));
      })
      .IgnoreError();
  state->ready.OnReady(
      [result, subscription](const absl::StatusOr<a11::Unit>& ready) {
        if (!ready.ok())
          result->Fail(ready.status());
        else
          result->Complete(subscription);
      });

  const absl::Time timeout_deadline =
      impl_->options.command_timeout == absl::InfiniteDuration()
          ? absl::InfiniteFuture()
          : absl::Now() + impl_->options.command_timeout;
  if (deadline == absl::InfiniteFuture() || timeout_deadline < deadline)
    deadline = timeout_deadline;
  if (deadline != absl::InfiniteFuture()) {
    thread::PostAt(deadline, [weak_state] {
      if (const std::shared_ptr<Subscription::State> active =
              weak_state.lock()) {
        active->Fail(absl::DeadlineExceededError(
            "Redis did not acknowledge the subscription before the deadline"));
      }
    });
  }

  absl::Status queued = RedisIoLoop::Instance().Post(
      [impl = impl_, state] { impl->RegisterSubscription(state); });
  if (!queued.ok())
    state->Fail(std::move(queued));
  return future;
}

absl::Status Client::Close() {
  return impl_->RequestClose();
}

absl::StatusOr<std::shared_ptr<Client>> DefaultClient() {
  thread::MutexLock lock(&DefaultClientMutex());
  std::shared_ptr<Client>& stored = DefaultClientStorage();
  if (stored != nullptr)
    return stored;
  absl::StatusOr<ClientOptions> options = ClientOptions::FromEnvironment();
  if (!options.ok())
    return options.status();
  absl::StatusOr<std::shared_ptr<Client>> client =
      Client::Create(std::move(*options));
  if (!client.ok())
    return client.status();
  stored = *client;
  return stored;
}

absl::Status SetDefaultClient(std::shared_ptr<Client> client) {
  if (client == nullptr)
    return absl::InvalidArgumentError("Default Redis client must not be null");
  thread::MutexLock lock(&DefaultClientMutex());
  DefaultClientStorage() = std::move(client);
  return absl::OkStatus();
}

void ResetDefaultClient() {
  thread::MutexLock lock(&DefaultClientMutex());
  DefaultClientStorage().reset();
}

}  // namespace a11::redis
