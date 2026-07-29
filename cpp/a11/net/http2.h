// Copyright 2026 The A11 Authors.

#ifndef A11_NET_HTTP2_H_
#define A11_NET_HTTP2_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"

namespace a11::net {

class Http2RequestBodyStream;

// HTTP/2 field names are normalized to lowercase. A compact sequence preserves
// repeated fields and wire order without allocating a tree node per field.
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

std::optional<std::string> GetHttpHeader(const HttpHeaders& headers,
                                         std::string_view name);
void EraseHttpHeader(HttpHeaders* absl_nonnull headers, std::string_view name);
void SetHttpHeader(HttpHeaders* absl_nonnull headers, std::string name,
                   std::string value);
absl::Status ValidateHttpHeaders(const HttpHeaders& headers);

struct HttpRequest {
  std::string method;
  std::string protocol;
  std::string scheme;
  std::string authority;
  std::string path;
  HttpHeaders headers;
  std::string body;
  std::shared_ptr<Http2RequestBodyStream> body_stream;
};

struct HttpResponseHead {
  int status = 0;
  HttpHeaders headers;
};

struct HttpResponse {
  HttpResponseHead head;
  std::string body;
};

struct Http2TlsOptions {
  bool enabled = false;
  bool verify_peer = true;
  std::string certificate_pem_file;
  std::string key_pem_file;
  std::string ca_certificate_pem_file;

  absl::Status Validate() const;
};

struct Http2Options {
  size_t max_request_body_size = 32 * 1024 * 1024;
  size_t max_response_body_size = 32 * 1024 * 1024;
  size_t max_buffered_request_bytes = 4 * 1024 * 1024;
  size_t max_buffered_response_bytes = 4 * 1024 * 1024;
  absl::Time deadline = absl::InfiniteFuture();
  Http2TlsOptions tls;

  absl::Status Validate() const;
};

class Http2Connection;

// Pull-oriented request DATA for an extended CONNECT stream. It is present on
// HttpRequest::body_stream only when a request remains open after its headers.
class Http2RequestBodyStream
    : public std::enable_shared_from_this<Http2RequestBodyStream> {
 public:
  a11::Future<std::optional<std::string>> Read();
  a11::Task Done() const;
  absl::Status Cancel(absl::Status status = absl::CancelledError(
                          "HTTP/2 request body cancelled"));
  [[nodiscard]] std::int32_t stream_id() const;

 private:
  struct State;

  explicit Http2RequestBodyStream(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;

  friend class Http2Connection;
};

// A pull-oriented HTTP/2 response. Read() returns nullopt after a clean
// END_STREAM. Only one outstanding Read() is permitted, which provides a
// bounded handoff from the libuv thread to fibers and ordinary threads.
class Http2ResponseStream
    : public std::enable_shared_from_this<Http2ResponseStream> {
 public:
  a11::Future<HttpResponseHead> Headers() const;
  a11::Future<std::optional<std::string>> Read();
  a11::Task Done() const;
  absl::Status Cancel(
      absl::Status status = absl::CancelledError("HTTP/2 request cancelled"));
  [[nodiscard]] std::int32_t stream_id() const;

 private:
  struct State;

  explicit Http2ResponseStream(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;

  friend class Http2Connection;
  friend class Http2Client;
};

// Client side of an HTTP/2 extended CONNECT stream. Request DATA remains
// writable while response DATA is read independently on the same stream.
class Http2DuplexStream
    : public std::enable_shared_from_this<Http2DuplexStream> {
 public:
  a11::Future<HttpResponseHead> Headers() const;
  a11::Future<std::optional<std::string>> Read();
  absl::Status Write(std::string data);
  absl::Status Finish();
  absl::Status Abort(absl::Status status = absl::CancelledError(
                         "HTTP/2 duplex stream cancelled"));
  a11::Task Done() const;
  [[nodiscard]] std::int32_t stream_id() const;

 private:
  Http2DuplexStream(std::weak_ptr<Http2Connection> connection,
                    std::shared_ptr<Http2ResponseStream> response)
      : connection_(std::move(connection)), response_(std::move(response)) {}

  std::weak_ptr<Http2Connection> connection_;
  std::shared_ptr<Http2ResponseStream> response_;

  friend class Http2Connection;
  friend class Http2Client;
};

class Http2ResponseWriter
    : public std::enable_shared_from_this<Http2ResponseWriter> {
 public:
  absl::Status SendHeaders(int status, HttpHeaders headers = {});
  absl::Status Write(std::string data);
  absl::Status Finish();
  absl::Status SendResponse(int status, HttpHeaders headers = {},
                            std::string body = {});
  absl::Status Abort(absl::Status status);
  a11::Task Done() const;

  [[nodiscard]] bool headers_sent() const;
  [[nodiscard]] bool finished() const;

  [[nodiscard]] std::int32_t stream_id() const { return stream_id_; }

 private:
  struct State;

  Http2ResponseWriter(std::weak_ptr<Http2Connection> connection,
                      std::int32_t stream_id, std::shared_ptr<State> state)
      : connection_(std::move(connection)),
        stream_id_(stream_id),
        state_(std::move(state)) {}

  std::weak_ptr<Http2Connection> connection_;
  std::int32_t stream_id_;
  std::shared_ptr<State> state_;

  friend class Http2Connection;
};

using Http2RequestHandler = std::function<a11::Task(
    HttpRequest request, std::shared_ptr<Http2ResponseWriter> response)>;

class Http2Server : public std::enable_shared_from_this<Http2Server> {
 public:
  static absl::StatusOr<std::shared_ptr<Http2Server>> Create(
      std::string bind_address, std::uint16_t port, Http2RequestHandler handler,
      Http2Options options = {});

  ~Http2Server();

  absl::Status Stop();
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] std::string bind_address() const;
  [[nodiscard]] bool running() const;
  [[nodiscard]] bool secure() const;
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct State;

  explicit Http2Server(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

class Http2Client : public std::enable_shared_from_this<Http2Client> {
 public:
  static a11::Future<std::shared_ptr<Http2Client>> Connect(
      std::string host, std::uint16_t port, Http2Options options = {});

  ~Http2Client();

  absl::StatusOr<std::shared_ptr<Http2ResponseStream>> RequestStream(
      std::string method, std::string path, HttpHeaders headers = {},
      std::string body = {}, std::string scheme = {});
  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> ExtendedConnect(
      std::string protocol, std::string path, HttpHeaders headers = {},
      std::string scheme = {});
  a11::Future<HttpResponse> Request(std::string method, std::string path,
                                    HttpHeaders headers = {},
                                    std::string body = {},
                                    std::string scheme = {});
  absl::Status Close();

  [[nodiscard]] std::string host() const;
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] bool connected() const;
  [[nodiscard]] bool secure() const;
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct Impl;
  static constexpr size_t kImplSize = 272;
  static constexpr size_t kImplAlignment = alignof(std::max_align_t);

  Http2Client(std::string host, std::uint16_t port, Http2Options options,
              std::shared_ptr<Http2Connection> connection);

  Impl* absl_nonnull state();
  const Impl* absl_nonnull state() const;

  alignas(kImplAlignment) std::byte impl_[kImplSize];
};

}  // namespace a11::net

#endif  // A11_NET_HTTP2_H_
