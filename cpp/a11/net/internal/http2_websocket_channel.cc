// Copyright 2026 The A11 Authors.

#include "a11/net/internal/http2_websocket_channel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "a11/net/internal/binary_channel.h"
#include "a11/status.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"

namespace a11::net::internal {
namespace {

constexpr std::uint8_t kContinuation = 0x0;
constexpr std::uint8_t kText = 0x1;
constexpr std::uint8_t kBinary = 0x2;
constexpr std::uint8_t kClose = 0x8;
constexpr std::uint8_t kPing = 0x9;
constexpr std::uint8_t kPong = 0xa;

void AppendBigEndian16(std::string* output, std::uint16_t value) {
  output->push_back(static_cast<char>((value >> 8U) & 0xffU));
  output->push_back(static_cast<char>(value & 0xffU));
}

void AppendBigEndian64(std::string* output, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output->push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

/**
 * XORs `length` bytes at `data` with a repeating 4-byte WebSocket mask.
 *
 * A word at a time, because this runs over every byte a client sends and every
 * byte a server receives: RFC 6455 masking is not optional and not negotiable.
 * The obvious `data[i] ^= mask[i % 4]` costs a division per byte and defeats
 * vectorisation, which at 64 KiB per message is a real fraction of the
 * message's whole cost. Reading the mask as one 32-bit word and XORing
 * word-wise leaves the compiler free to widen it further.
 *
 * Always from the start of the mask, which is all RFC 6455 needs: every frame
 * carries its own masking key and applies it from its own first payload byte,
 * so a fragmented message's continuations do not continue the previous frame's
 * mask.
 */
void MaskInPlace(char* data, size_t length, const char (&mask)[4]) {
  std::uint32_t word = 0;
  std::memcpy(&word, mask, sizeof(word));
  size_t index = 0;
  for (; index + sizeof(word) <= length; index += sizeof(word)) {
    std::uint32_t chunk = 0;
    std::memcpy(&chunk, data + index, sizeof(chunk));
    chunk ^= word;
    std::memcpy(data + index, &chunk, sizeof(chunk));
  }
  for (; index < length; ++index) {
    data[index] ^= mask[index % 4];
  }
}

std::uint16_t ReadBigEndian16(std::string_view input, size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(static_cast<unsigned char>(input[offset]))
       << 8U) |
      static_cast<std::uint16_t>(
          static_cast<unsigned char>(input[offset + 1])));
}

std::uint64_t ReadBigEndian64(std::string_view input, size_t offset) {
  std::uint64_t result = 0;
  for (size_t index = 0; index < sizeof(result); ++index) {
    result = (result << 8U) | static_cast<unsigned char>(input[offset + index]);
  }
  return result;
}

class Http2WebSocketChannel final
    : public BinaryChannel,
      public std::enable_shared_from_this<Http2WebSocketChannel> {
 public:
  explicit Http2WebSocketChannel(Http2WebSocketClientConfig config)
      : role_(Role::kClient),
        max_message_size_(config.max_message_size),
        client_config_(std::move(config)) {}

  Http2WebSocketChannel(std::shared_ptr<Http2RequestBodyStream> request,
                        std::shared_ptr<Http2ResponseWriter> response,
                        size_t max_message_size)
      : role_(Role::kServer),
        max_message_size_(max_message_size),
        request_(std::move(request)),
        response_(std::move(response)) {}

  absl::Status SetCallbacks(BinaryChannelCallbacks callbacks) override {
    std::function<void()> notify_open;
    {
      thread::MutexLock lock(&mu_);
      callbacks_ = std::move(callbacks);
      if (open_ && !closed_) {
        notify_open = callbacks_.on_open;
      }
    }
    if (notify_open) {
      notify_open();
    }
    return absl::OkStatus();
  }

  absl::Status ResetCallbacks() override {
    thread::MutexLock lock(&mu_);
    callbacks_ = {};
    return absl::OkStatus();
  }

  absl::Status Open() override {
    {
      thread::MutexLock lock(&mu_);
      if (open_) {
        return absl::OkStatus();
      }
      if (closed_) {
        return status_.ok()
                   ? absl::FailedPreconditionError("HTTP/2 WebSocket is closed")
                   : status_;
      }
      if (opening_) {
        return absl::FailedPreconditionError(
            "HTTP/2 WebSocket open is already in progress");
      }
      opening_ = true;
    }

    absl::Status opened = role_ == Role::kClient ? OpenClient() : OpenServer();
    if (!opened.ok()) {
      Fail(opened);
      return opened;
    }

    std::function<void()> callback;
    {
      thread::MutexLock lock(&mu_);
      opening_ = false;
      open_ = true;
      callback = callbacks_.on_open;
    }
    StartReader();
    if (callback) {
      callback();
    }
    return absl::OkStatus();
  }

  absl::Status Send(std::string bytes) override {
    {
      thread::MutexLock lock(&mu_);
      if (!open_ || closed_) {
        return status_.ok() ? absl::FailedPreconditionError(
                                  "HTTP/2 WebSocket is not open")
                            : status_;
      }
      if (bytes.size() > max_message_size_) {
        return absl::OutOfRangeError(
            "HTTP/2 WebSocket message exceeds its configured limit");
      }
    }
    return WriteFrame(kBinary, std::move(bytes));
  }

  absl::StatusOr<size_t> BufferedAmount() const override { return 0; }

  absl::StatusOr<bool> IsOpen() const override {
    thread::MutexLock lock(&mu_);
    return open_ && !closed_;
  }

  absl::Status Close() override {
    bool send_close = false;
    {
      thread::MutexLock lock(&mu_);
      if (closed_) {
        return absl::OkStatus();
      }
      send_close = open_ && !close_sent_;
      close_sent_ = close_sent_ || send_close;
    }
    absl::Status first;
    if (send_close) {
      first = WriteFrame(kClose, std::string());
    }
    absl::Status finished = FinishTransport();
    if (first.ok() && !finished.ok()) {
      first = finished;
    }
    CompleteClose();
    return first;
  }

  // Fail() already does exactly what an abortive close is: it aborts the duplex
  // stream (or cancels the request / aborts the response writer), which drops
  // the connection rather than half-closing it. Close() cannot, because ending
  // the request is a graceful HTTP operation by construction.
  absl::Status Abort(absl::Status status) override {
    Fail(std::move(status));
    return absl::OkStatus();
  }

  void* absl_nullable GetImpl() const override {
    thread::MutexLock lock(&mu_);
    if (duplex_ != nullptr) {
      return duplex_.get();
    }
    return request_.get();
  }

 private:
  enum class Role { kClient, kServer };

  struct ParsedActions {
    std::vector<std::string> messages;
    std::vector<std::string> pongs;
    std::optional<std::string> close;
  };

  absl::Status OpenClient() {
    Http2WebSocketClientConfig config;
    {
      thread::MutexLock lock(&mu_);
      config = *client_config_;
    }
    ABSL_ASSIGN_OR_RETURN(
        std::shared_ptr<Http2Client> client,
        Http2Client::Connect(config.host, config.port, config.http2_options)
            .Await(config.http2_options.deadline));
    ABSL_ASSIGN_OR_RETURN(
        std::shared_ptr<Http2DuplexStream> duplex,
        client->ExtendedConnect("websocket", std::move(config.path),
                                std::move(config.headers)));
    ABSL_ASSIGN_OR_RETURN(
        HttpResponseHead head,
        duplex->Headers().Await(config.http2_options.deadline));
    if (head.status < 200 || head.status >= 300) {
      return absl::Status(
          StatusCodeFromHttp(head.status),
          absl::StrCat("HTTP/2 WebSocket CONNECT returned ", head.status));
    }
    thread::MutexLock lock(&mu_);
    client_ = std::move(client);
    duplex_ = std::move(duplex);
    client_config_.reset();
    return absl::OkStatus();
  }

  absl::Status OpenServer() {
    std::shared_ptr<Http2ResponseWriter> response;
    {
      thread::MutexLock lock(&mu_);
      response = response_;
    }
    if (response == nullptr) {
      return absl::FailedPreconditionError("WebSocket response is missing");
    }
    return response->SendHeaders(200);
  }

  void StartReader() {
    std::weak_ptr<Http2WebSocketChannel> weak = shared_from_this();
    a11::Schedule([weak]() {
      while (true) {
        std::shared_ptr<Http2WebSocketChannel> self = weak.lock();
        if (self == nullptr) {
          return;
        }
        a11::Future<std::optional<std::string>> next = self->ReadTransport();
        self.reset();
        absl::StatusOr<std::optional<std::string>> chunk = next.Await();
        self = weak.lock();
        if (self == nullptr) {
          return;
        }
        if (!chunk.ok()) {
          self->Fail(chunk.status());
          return;
        }
        if (!chunk->has_value()) {
          self->CompleteClose();
          return;
        }
        absl::Status consumed = self->Consume(std::move(**chunk));
        if (!consumed.ok()) {
          self->Fail(consumed);
          return;
        }
        {
          thread::MutexLock lock(&self->mu_);
          if (self->closed_) {
            return;
          }
        }
      }
    });
  }

  a11::Future<std::optional<std::string>> ReadTransport() {
    std::shared_ptr<Http2DuplexStream> duplex;
    std::shared_ptr<Http2RequestBodyStream> request;
    {
      thread::MutexLock lock(&mu_);
      duplex = duplex_;
      request = request_;
    }
    if (duplex != nullptr) {
      return duplex->Read();
    }
    if (request != nullptr) {
      return request->Read();
    }
    return a11::FailedFuture<std::optional<std::string>>(
        absl::FailedPreconditionError("WebSocket HTTP/2 stream is missing"));
  }

  absl::Status Consume(std::string data) {
    ParsedActions actions;
    {
      thread::MutexLock lock(&mu_);
      if (closed_) {
        return absl::OkStatus();
      }
      if (input_.size() + data.size() > max_message_size_ + 14) {
        return absl::ResourceExhaustedError(
            "Buffered WebSocket frame exceeds max_message_size");
      }
      // Adopt the read outright when nothing is part-parsed. A whole message
      // usually arrives in one read, so this is the ordinary path, and
      // appending would copy every byte of it into a buffer that is about to be
      // handed straight back out again.
      if (input_.empty()) {
        input_ = std::move(data);
      } else {
        input_.append(data);
      }
      ABSL_RETURN_IF_ERROR(ParseFrames(&actions));
    }

    for (std::string& pong : actions.pongs) {
      ABSL_RETURN_IF_ERROR(WriteFrame(kPong, std::move(pong)));
    }
    for (std::string& message : actions.messages) {
      std::function<void(std::string)> callback;
      {
        thread::MutexLock lock(&mu_);
        callback = callbacks_.on_message;
      }
      if (callback) {
        callback(std::move(message));
      }
    }
    if (actions.close.has_value()) {
      bool reply = false;
      {
        thread::MutexLock lock(&mu_);
        reply = !close_sent_;
        close_sent_ = true;
      }
      if (reply) {
        ABSL_RETURN_IF_ERROR(WriteFrame(kClose, std::move(*actions.close)));
      }
      ABSL_RETURN_IF_ERROR(FinishTransport());
      CompleteClose();
    }
    return absl::OkStatus();
  }

  absl::Status ParseFrames(ParsedActions* actions)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    size_t consumed = 0;
    while (input_.size() - consumed >= 2) {
      const std::uint8_t first = static_cast<unsigned char>(input_[consumed]);
      const std::uint8_t second =
          static_cast<unsigned char>(input_[consumed + 1]);
      const bool final = (first & 0x80U) != 0;
      const std::uint8_t opcode = first & 0x0fU;
      if ((first & 0x70U) != 0) {
        return absl::InvalidArgumentError("WebSocket RSV bits are not zero");
      }
      const bool masked = (second & 0x80U) != 0;
      if (masked != (role_ == Role::kServer)) {
        return absl::InvalidArgumentError(
            role_ == Role::kServer
                ? "WebSocket client frame is not masked"
                : "WebSocket server frame is unexpectedly masked");
      }

      std::uint64_t payload_size = second & 0x7fU;
      size_t header_size = 2;
      if (payload_size == 126) {
        if (input_.size() - consumed < 4) {
          break;
        }
        payload_size = ReadBigEndian16(input_, consumed + 2);
        header_size = 4;
      } else if (payload_size == 127) {
        if (input_.size() - consumed < 10) {
          break;
        }
        payload_size = ReadBigEndian64(input_, consumed + 2);
        if ((payload_size & (std::uint64_t{1} << 63U)) != 0) {
          return absl::InvalidArgumentError("WebSocket length is invalid");
        }
        header_size = 10;
      }
      if (payload_size > max_message_size_ ||
          payload_size > std::numeric_limits<size_t>::max()) {
        return absl::OutOfRangeError(
            "WebSocket frame exceeds max_message_size");
      }
      const bool control = opcode >= kClose;
      if (control && (!final || payload_size > 125)) {
        return absl::InvalidArgumentError(
            "WebSocket control frame must be final and at most 125 bytes");
      }
      const size_t mask_size = masked ? 4 : 0;
      const size_t full_size =
          header_size + mask_size + static_cast<size_t>(payload_size);
      if (input_.size() - consumed < full_size) {
        break;
      }

      const size_t mask_offset = consumed + header_size;
      const size_t payload_offset = mask_offset + mask_size;
      // Copied out before the payload is taken, because taking it may move the
      // buffer the mask lives in.
      char mask[4] = {0, 0, 0, 0};
      if (masked) {
        std::memcpy(mask, input_.data() + mask_offset, sizeof(mask));
      }
      std::string payload;
      bool taken = false;
      if (consumed == 0 && payload_offset + payload_size == input_.size()) {
        // The buffer holds exactly this one frame and nothing before it, which
        // is the common case for a message that arrived in its own TCP read.
        // Taking the buffer whole and erasing the header off the front is one
        // memmove of a 2-14 byte header instead of a copy of the payload -- and
        // at 64 KiB that copy is a measurable share of the message's cost.
        payload = std::move(input_);
        input_.clear();
        payload.erase(0, payload_offset);
        taken = true;
      } else {
        payload =
            input_.substr(payload_offset, static_cast<size_t>(payload_size));
      }
      if (masked) {
        MaskInPlace(payload.data(), payload.size(), mask);
      }
      // Taking the buffer already consumed it: there is nothing left to skip
      // past and nothing left to erase, and leaving `consumed` set would make
      // the loop's `input_.size() - consumed` underflow on the next pass.
      consumed = taken ? 0 : consumed + full_size;

      if (opcode == kPing) {
        actions->pongs.push_back(std::move(payload));
      } else if (opcode == kPong) {
        continue;
      } else if (opcode == kClose) {
        if (payload.size() == 1) {
          return absl::InvalidArgumentError(
              "WebSocket close code is truncated");
        }
        actions->close = std::move(payload);
        break;
      } else if (opcode == kText) {
        return absl::InvalidArgumentError(
            "A11 binary channel received a text WebSocket frame");
      } else if (opcode == kBinary) {
        if (fragment_opcode_.has_value()) {
          return absl::InvalidArgumentError(
              "WebSocket data frame interrupted a fragmented message");
        }
        if (final) {
          actions->messages.push_back(std::move(payload));
        } else {
          fragment_opcode_ = opcode;
          fragmented_ = std::move(payload);
        }
      } else if (opcode == kContinuation) {
        if (!fragment_opcode_.has_value()) {
          return absl::InvalidArgumentError(
              "WebSocket continuation has no initial frame");
        }
        if (fragmented_.size() + payload.size() > max_message_size_) {
          return absl::OutOfRangeError(
              "Fragmented WebSocket message exceeds max_message_size");
        }
        fragmented_.append(payload);
        if (final) {
          actions->messages.push_back(std::move(fragmented_));
          fragmented_.clear();
          fragment_opcode_.reset();
        }
      } else {
        return absl::InvalidArgumentError("WebSocket opcode is unsupported");
      }
    }
    if (consumed != 0) {
      input_.erase(0, consumed);
    }
    return absl::OkStatus();
  }

  absl::Status WriteFrame(std::uint8_t opcode, std::string payload) {
    thread::MutexLock write_lock(&write_mu_);
    const bool masked = role_ == Role::kClient;
    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80U | opcode));
    const std::uint8_t mask_flag = masked ? 0x80U : 0;
    if (payload.size() <= 125) {
      frame.push_back(static_cast<char>(mask_flag | payload.size()));
    } else if (payload.size() <= std::numeric_limits<std::uint16_t>::max()) {
      frame.push_back(static_cast<char>(mask_flag | 126U));
      AppendBigEndian16(&frame, static_cast<std::uint16_t>(payload.size()));
    } else {
      frame.push_back(static_cast<char>(mask_flag | 127U));
      AppendBigEndian64(&frame, payload.size());
    }
    if (masked) {
      const auto key = static_cast<std::uint32_t>(RandomUint64());
      const char mask[4] = {
          static_cast<char>((key >> 24U) & 0xffU),
          static_cast<char>((key >> 16U) & 0xffU),
          static_cast<char>((key >> 8U) & 0xffU),
          static_cast<char>(key & 0xffU),
      };
      frame.append(mask, sizeof(mask));
      const size_t body = frame.size();
      frame.append(payload);
      MaskInPlace(frame.data() + body, payload.size(), mask);
    } else {
      frame.append(payload);
    }
    return WriteTransport(std::move(frame));
  }

  absl::Status WriteTransport(std::string data)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(write_mu_) {
    std::shared_ptr<Http2DuplexStream> duplex;
    std::shared_ptr<Http2ResponseWriter> response;
    {
      thread::MutexLock lock(&mu_);
      duplex = duplex_;
      response = response_;
    }
    if (duplex != nullptr) {
      return duplex->Write(std::move(data));
    }
    if (response != nullptr) {
      return response->Write(std::move(data));
    }
    return absl::FailedPreconditionError("WebSocket HTTP/2 writer is missing");
  }

  absl::Status FinishTransport() {
    thread::MutexLock write_lock(&write_mu_);
    std::shared_ptr<Http2DuplexStream> duplex;
    std::shared_ptr<Http2ResponseWriter> response;
    {
      thread::MutexLock lock(&mu_);
      duplex = duplex_;
      response = response_;
    }
    if (duplex != nullptr) {
      return duplex->Finish();
    }
    if (response != nullptr) {
      return response->Finish();
    }
    return absl::OkStatus();
  }

  void Fail(absl::Status status) {
    if (status.ok()) {
      status = absl::UnknownError("HTTP/2 WebSocket failed");
    }
    std::function<void(absl::Status)> callback;
    std::shared_ptr<Http2DuplexStream> duplex;
    std::shared_ptr<Http2RequestBodyStream> request;
    std::shared_ptr<Http2ResponseWriter> response;
    {
      thread::MutexLock lock(&mu_);
      if (closed_) {
        return;
      }
      opening_ = false;
      closed_ = true;
      open_ = false;
      status_ = status;
      callback = callbacks_.on_error;
      duplex = duplex_;
      request = request_;
      response = response_;
    }
    if (duplex != nullptr) {
      (void)duplex->Abort(status);
    }
    if (request != nullptr) {
      (void)request->Cancel(status);
    }
    if (response != nullptr) {
      (void)response->Abort(status);
    }
    if (callback) {
      callback(std::move(status));
    }
  }

  void CompleteClose() {
    std::function<void()> callback;
    {
      thread::MutexLock lock(&mu_);
      if (closed_) {
        return;
      }
      opening_ = false;
      closed_ = true;
      open_ = false;
      callback = callbacks_.on_closed;
    }
    if (callback) {
      callback();
    }
  }

  const Role role_;
  const size_t max_message_size_;
  mutable thread::Mutex mu_;
  thread::Mutex write_mu_;
  BinaryChannelCallbacks callbacks_ ABSL_GUARDED_BY(mu_);
  std::optional<Http2WebSocketClientConfig> client_config_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<Http2Client> client_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<Http2DuplexStream> duplex_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<Http2RequestBodyStream> request_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<Http2ResponseWriter> response_ ABSL_GUARDED_BY(mu_);
  bool opening_ ABSL_GUARDED_BY(mu_) = false;
  bool open_ ABSL_GUARDED_BY(mu_) = false;
  bool closed_ ABSL_GUARDED_BY(mu_) = false;
  bool close_sent_ ABSL_GUARDED_BY(mu_) = false;
  absl::Status status_ ABSL_GUARDED_BY(mu_);
  std::string input_ ABSL_GUARDED_BY(mu_);
  std::optional<std::uint8_t> fragment_opcode_ ABSL_GUARDED_BY(mu_);
  std::string fragmented_ ABSL_GUARDED_BY(mu_);
};

}  // namespace

absl::StatusOr<std::shared_ptr<BinaryChannel>> MakeHttp2WebSocketClientChannel(
    Http2WebSocketClientConfig config) {
  if (config.host.empty() || config.port == 0 || config.path.empty() ||
      config.path.front() != '/') {
    return absl::InvalidArgumentError(
        "HTTP/2 WebSocket client requires host, port, and absolute path");
  }
  if (config.max_message_size == 0) {
    return absl::InvalidArgumentError(
        "WebSocket message limit must be positive");
  }
  ABSL_RETURN_IF_ERROR(config.http2_options.Validate());
  ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(config.headers));
  return std::make_shared<Http2WebSocketChannel>(std::move(config));
}

absl::StatusOr<std::shared_ptr<BinaryChannel>> MakeHttp2WebSocketServerChannel(
    HttpRequest request, std::shared_ptr<Http2ResponseWriter> response,
    size_t max_message_size) {
  if (request.method != "CONNECT" || request.protocol != "websocket" ||
      request.body_stream == nullptr) {
    return absl::InvalidArgumentError(
        "HTTP/2 request is not a WebSocket extended CONNECT");
  }
  if (response == nullptr) {
    return absl::InvalidArgumentError("WebSocket response must not be null");
  }
  if (max_message_size == 0) {
    return absl::InvalidArgumentError(
        "WebSocket message limit must be positive");
  }
  return std::make_shared<Http2WebSocketChannel>(
      std::move(request.body_stream), std::move(response), max_message_size);
}

}  // namespace a11::net::internal
