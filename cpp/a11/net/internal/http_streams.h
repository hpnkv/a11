// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Buffering state for the public HTTP stream facades.
 *
 * The Http2RequestBodyStream / Http2ResponseStream / Http2ResponseWriter
 * classes are thin handles over these State objects: bounded, mutex-guarded
 * queues with a single-outstanding-read handoff between the libuv thread and
 * fibers. They are protocol-neutral -- both the HTTP/2 and HTTP/1.1 connections
 * push/pull through them -- so the definitions live here to be shared across
 * translation units. The facade method bodies (Read/Headers/Cancel/stream_id)
 * remain in http2.cc.
 */

#ifndef A11_NET_INTERNAL_HTTP_STREAMS_H_
#define A11_NET_INTERNAL_HTTP_STREAMS_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <absl/status/status.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "thread/boost_primitives.h"

namespace a11::net {

struct Http2RequestBodyStream::State {
  explicit State(size_t maximum_buffered_bytes)
      : max_buffered_bytes(maximum_buffered_bytes),
        done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()) {}

  mutable thread::Mutex mu;
  std::int32_t stream_id ABSL_GUARDED_BY(mu) = -1;
  const size_t max_buffered_bytes;
  size_t buffered_bytes ABSL_GUARDED_BY(mu) = 0;
  std::deque<std::string> chunks ABSL_GUARDED_BY(mu);
  bool done ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;
  std::shared_ptr<a11::Promise<std::optional<std::string>>> pending_read
      ABSL_GUARDED_BY(mu);
  std::function<absl::Status(absl::Status)> cancel ABSL_GUARDED_BY(mu);

  absl::Status Push(std::string data) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    {
      thread::MutexLock lock(&mu);
      if (done) {
        return status.ok() ? absl::CancelledError("Request body is done")
                           : status;
      }
      if (pending_read != nullptr) {
        reader = std::move(pending_read);
      } else {
        if (buffered_bytes + data.size() > max_buffered_bytes &&
            !chunks.empty()) {
          return absl::ResourceExhaustedError(
              "HTTP request exceeded max_buffered_request_bytes");
        }
        buffered_bytes += data.size();
        chunks.push_back(std::move(data));
        return absl::OkStatus();
      }
    }
    (void)reader->SetValue(std::optional<std::string>(std::move(data)));
    return absl::OkStatus();
  }

  void Finish(absl::Status completion) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    {
      thread::MutexLock lock(&mu);
      if (done) {
        return;
      }
      done = true;
      status = completion;
      reader = std::move(pending_read);
    }
    if (reader != nullptr) {
      if (completion.ok()) {
        (void)reader->SetValue(std::nullopt);
      } else {
        (void)reader->SetStatus(completion);
      }
    }
    if (completion.ok()) {
      (void)done_promise->SetValue(a11::Unit{});
    } else {
      (void)done_promise->SetStatus(completion);
    }
  }
};

struct Http2ResponseStream::State {
  explicit State(size_t maximum_buffered_bytes)
      : max_buffered_bytes(maximum_buffered_bytes),
        headers_promise(std::make_shared<a11::Promise<HttpResponseHead>>()),
        headers_future(headers_promise->future()),
        done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()) {}

  mutable thread::Mutex mu;
  std::int32_t stream_id ABSL_GUARDED_BY(mu) = -1;
  const size_t max_buffered_bytes;
  size_t buffered_bytes ABSL_GUARDED_BY(mu) = 0;
  std::deque<std::string> chunks ABSL_GUARDED_BY(mu);
  bool headers_ready ABSL_GUARDED_BY(mu) = false;
  bool done ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  const std::shared_ptr<a11::Promise<HttpResponseHead>> headers_promise;
  const a11::Future<HttpResponseHead> headers_future;
  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;
  std::shared_ptr<a11::Promise<std::optional<std::string>>> pending_read
      ABSL_GUARDED_BY(mu);
  std::function<absl::Status(absl::Status)> cancel ABSL_GUARDED_BY(mu);

  void SetHeaders(HttpResponseHead head) {
    bool publish = false;
    {
      thread::MutexLock lock(&mu);
      if (!headers_ready && !done) {
        headers_ready = true;
        publish = true;
      }
    }
    if (publish) {
      (void)headers_promise->SetValue(std::move(head));
    }
  }

  absl::Status Push(std::string data) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    {
      thread::MutexLock lock(&mu);
      if (done) {
        return status.ok() ? absl::CancelledError("Response is done") : status;
      }
      if (pending_read != nullptr) {
        reader = std::move(pending_read);
      } else {
        if (buffered_bytes + data.size() > max_buffered_bytes &&
            !chunks.empty()) {
          return absl::ResourceExhaustedError(
              "HTTP response exceeded max_buffered_response_bytes");
        }
        buffered_bytes += data.size();
        chunks.push_back(std::move(data));
        return absl::OkStatus();
      }
    }
    (void)reader->SetValue(std::optional<std::string>(std::move(data)));
    return absl::OkStatus();
  }

  void Finish(absl::Status completion) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    bool publish_headers_error = false;
    {
      thread::MutexLock lock(&mu);
      if (done) {
        return;
      }
      done = true;
      status = completion;
      reader = std::move(pending_read);
      if (!headers_ready) {
        headers_ready = true;
        publish_headers_error = true;
      }
    }
    if (publish_headers_error) {
      absl::Status error =
          completion.ok()
              ? absl::DataLossError("HTTP stream ended before response headers")
              : completion;
      (void)headers_promise->SetStatus(error);
    }
    if (reader != nullptr) {
      if (completion.ok()) {
        (void)reader->SetValue(std::nullopt);
      } else {
        (void)reader->SetStatus(completion);
      }
    }
    if (completion.ok()) {
      (void)done_promise->SetValue(a11::Unit{});
    } else {
      (void)done_promise->SetStatus(completion);
    }
  }
};

struct Http2ResponseWriter::State {
  State()
      : done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()) {}

  mutable thread::Mutex mu;
  bool done ABSL_GUARDED_BY(mu) = false;
  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;

  void Finish(const absl::Status& status) {
    {
      thread::MutexLock lock(&mu);
      if (done) {
        return;
      }
      done = true;
    }
    if (status.ok()) {
      (void)done_promise->SetValue(a11::Unit{});
    } else {
      (void)done_promise->SetStatus(status);
    }
  }
};

}  // namespace a11::net

#endif  // A11_NET_INTERNAL_HTTP_STREAMS_H_
