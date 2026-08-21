// Copyright 2026 The A11 Authors.

#include "sdk/http/actions/http_actions.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/json_codec.h"
#include "a11/net/http/connection_pool.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/status.h"
#include "thread/concurrency.h"

namespace a11::sdk::http {
namespace {

using ::a11::actions::Action;
using ::a11::actions::ActionHandler;
using ::a11::actions::ActionHeaderSchema;
using ::a11::actions::ActionPortSchema;
using ::a11::actions::ActionSchema;
using ::a11::net::GetHttpHeader;
using ::a11::net::HttpConnectionLease;
using ::a11::net::HttpConnectionPool;
using ::a11::net::HttpHeaders;
using ::a11::net::HttpPushedResponse;
using ::a11::net::HttpResponseHead;
using ::a11::net::Http2Client;
using ::a11::net::Http2DuplexStream;
using ::a11::net::Http2Options;
using ::a11::net::Http2ResponseStream;
using ::a11::net::ParsedUrl;
using ::a11::nodes::AsyncNode;

/** @brief Name of the absolute-execution-deadline header. */
constexpr std::string_view kDeadlineHeader = "x-a11-deadline";
/** The prefix A11 reserves for its own headers, and so never sends as HTTP. */
constexpr std::string_view kFrameworkHeaderPrefix = "x-a11-";
constexpr std::string_view kDefaultUserAgent = "a11-http/1";
constexpr std::string_view kOctetStream = "application/octet-stream";

// ---------------------------------------------------------------------------
// Small chunk helpers.
//
// Values travel as plain JSON or as octet-stream bytes; no type is registered
// for any of this, because an HTTP header map really is a map and a body really
// is bytes. That also keeps the cross-language tag table out of it.
// ---------------------------------------------------------------------------

/// A JSON chunk, or the reason the value will not fit JSON.
///
/// StatusOr rather than a plain Chunk, and `DumpJson` rather than `dump()`,
/// because everything this encodes came off a socket: a response header's value
/// is opaque octets on the wire, and a server is entitled to send one that is
/// not UTF-8. `dump()` answers that with `std::abort()` in every
/// `-fno-exceptions` translation unit -- which is most of A11 -- so a remote
/// peer could end this process with no output at all by sending a header. The
/// check lives in `DumpJson`; this only has to ask it and pass the answer on.
absl::StatusOr<data::Chunk> JsonChunk(const nlohmann::json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string encoded,
                        DumpJson(value, "an HTTP action's JSON output"));
  data::Chunk chunk;
  chunk.metadata =
      data::ChunkMetadata{.mimetype = std::string(data::kJsonMimetype)};
  chunk.data = std::move(encoded);
  return chunk;
}

data::Chunk BytesChunk(std::string bytes) {
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = std::string(kOctetStream)};
  chunk.data = std::move(bytes);
  return chunk;
}

/// Reads a unary input as JSON. A closed or null port yields nullopt. A bare
/// text payload is taken as a JSON string, so `url: "..."` works either way.
absl::StatusOr<std::optional<nlohmann::json>> ReadJson(
    const std::shared_ptr<AsyncNode>& node) {
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Chunk> chunk,
                        node->NextChunk().Await());
  if (!chunk.has_value() || chunk->IsNull()) {
    return std::optional<nlohmann::json>(std::nullopt);
  }
  const std::string mimetype = chunk->GetMimetype();
  if (!absl::StartsWith(mimetype, data::kJsonMimetype)) {
    return std::optional<nlohmann::json>(nlohmann::json(chunk->data));
  }
  nlohmann::json parsed = nlohmann::json::parse(chunk->data, nullptr, false);
  if (parsed.is_discarded()) {
    return absl::InvalidArgumentError(
        "An input port declared as JSON carried something that is not JSON");
  }
  return std::optional<nlohmann::json>(std::move(parsed));
}

/// Reads a unary input as text, accepting a JSON string or raw bytes.
absl::StatusOr<std::optional<std::string>> ReadText(
    const std::shared_ptr<AsyncNode>& node) {
  ABSL_ASSIGN_OR_RETURN(std::optional<nlohmann::json> value, ReadJson(node));
  if (!value.has_value() || value->is_null()) {
    return std::optional<std::string>(std::nullopt);
  }
  if (value->is_string()) {
    return std::optional<std::string>(value->get<std::string>());
  }
  return std::optional<std::string>(value->dump());
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------

/**
 * The action's output ports, resolved once and closed exactly once.
 *
 * Every port an action declares has to be ended, whether or not it was used:
 * a reader waiting on a port nothing was ever written to would wait forever. So
 * ports named in `options.omit` are closed up front, and the rest are closed by
 * Finish() on the way out however the run ended.
 */
class Outputs {
 public:
  static absl::StatusOr<Outputs> Open(
      const std::shared_ptr<Action>& action,
      const std::vector<std::string>& names,
      const std::vector<std::string>& omitted) {
    Outputs outputs;
    for (const std::string& name : names) {
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> node,
                            action->GetOutput(name));
      const bool omit = std::find(omitted.begin(), omitted.end(), name) !=
                        omitted.end();
      if (omit) {
        // Nothing will be written here, and saying so now is what lets a caller
        // ask for three of eight ports without draining the other five.
        ABSL_RETURN_IF_ERROR(node->Finalize({.wait = true}).Await().status());
        continue;
      }
      outputs.nodes_.emplace(name, std::move(node));
    }
    return outputs;
  }

  /// @return The port, or null when it was omitted.
  [[nodiscard]] std::shared_ptr<AsyncNode> Get(std::string_view name) const {
    const auto found = nodes_.find(name);
    return found == nodes_.end() ? nullptr : found->second;
  }

  /// Writes one value, or does nothing when the port was omitted.
  absl::Status Put(std::string_view name, data::Chunk chunk, bool final) {
    const std::shared_ptr<AsyncNode> node = Get(name);
    if (node == nullptr) {
      return absl::OkStatus();
    }
    if (final) {
      final_.emplace(name);
    }
    return node->PutChunk(std::move(chunk), std::nullopt, final)
        .Await()
        .status();
  }

  /// Writes a port's single value and closes it.
  absl::Status PutOnly(std::string_view name, const nlohmann::json& value) {
    ABSL_ASSIGN_OR_RETURN(data::Chunk chunk, JsonChunk(value));
    ABSL_RETURN_IF_ERROR(Put(name, std::move(chunk), /*final=*/true));
    return Close(name);
  }

  /// Writes one JSON value to a port and leaves it open.
  ///
  /// The pairing for PutOnly, and the reason both exist rather than every caller
  /// spelling out the encode: JsonChunk can fail, and a caller that has to
  /// unpack a StatusOr before it can write tends to write `dump()` instead.
  absl::Status PutJson(std::string_view name, const nlohmann::json& value) {
    ABSL_ASSIGN_OR_RETURN(data::Chunk chunk, JsonChunk(value));
    return Put(name, std::move(chunk), /*final=*/false);
  }

  /// Ends a port and releases it, so Finish() does not do it again.
  absl::Status Close(std::string_view name) {
    const auto found = nodes_.find(name);
    if (found == nodes_.end()) {
      return absl::OkStatus();
    }
    const std::shared_ptr<AsyncNode> node = std::move(found->second);
    nodes_.erase(found);
    // A port whose last value was already marked final has its end established;
    // a null terminator on top of that would claim a second, different one, and
    // the store rejects that rather than guess which was meant.
    const bool terminated = final_.contains(name);
    return CloseNode(node, terminated);
  }

  /// Ends every port still open. Called however the run finished, because a
  /// failed run still has to unblock whoever was reading it.
  absl::Status Finish() {
    absl::Status first = absl::OkStatus();
    for (auto& [name, node] : nodes_) {
      const absl::Status closed = CloseNode(node, final_.contains(name));
      if (first.ok()) {
        first = closed;
      }
    }
    nodes_.clear();
    return first;
  }

  /**
   * Ends a node.
   *
   * @param terminated Whether a chunk marked final has already been written.
   *        When it has, closing must not write a null terminator as well.
   */
  static absl::Status CloseNode(const std::shared_ptr<AsyncNode>& node,
                                bool terminated = false) {
    if (node == nullptr) {
      return absl::OkStatus();
    }
    if (terminated) {
      return node->Close().Await().status();
    }
    return node->Finalize({.wait = true}).Await().status();
  }

 private:
  absl::flat_hash_map<std::string, std::shared_ptr<AsyncNode>> nodes_;
  absl::flat_hash_set<std::string> final_;
};

// ---------------------------------------------------------------------------
// Request options
// ---------------------------------------------------------------------------

/** How a request body reaches the peer. */
enum class BodyMode {
  kBuffer,  ///< Read the port to its end, send with a `content-length`.
  kStream,  ///< Send it as it arrives: HTTP/2 DATA frames, HTTP/1.1 chunked.
};

struct RequestOptions {
  int max_redirects = 5;
  absl::Duration timeout = absl::Minutes(5);
  BodyMode body_mode = BodyMode::kBuffer;
  bool accept_pushes = false;
  bool reuse_connection = true;
  size_t max_body_bytes = 32 * 1024 * 1024;
  std::string user_agent{kDefaultUserAgent};
  HttpHeaders extra_headers;
  std::vector<std::string> omit;
  Http2Options transport;
};

absl::StatusOr<bool> JsonBool(const nlohmann::json& object,
                              std::string_view key, bool fallback) {
  if (!object.contains(key)) {
    return fallback;
  }
  const nlohmann::json& value = object.at(std::string(key));
  if (!value.is_boolean()) {
    return absl::InvalidArgumentError(
        absl::StrCat("options.", key, " must be a boolean"));
  }
  return value.get<bool>();
}

absl::StatusOr<std::int64_t> JsonInt(const nlohmann::json& object,
                                     std::string_view key,
                                     std::int64_t fallback) {
  if (!object.contains(key)) {
    return fallback;
  }
  const nlohmann::json& value = object.at(std::string(key));
  if (!value.is_number_integer()) {
    return absl::InvalidArgumentError(
        absl::StrCat("options.", key, " must be an integer"));
  }
  return value.get<std::int64_t>();
}

absl::StatusOr<std::string> JsonString(const nlohmann::json& object,
                                       std::string_view key,
                                       std::string fallback) {
  if (!object.contains(key)) {
    return fallback;
  }
  const nlohmann::json& value = object.at(std::string(key));
  if (!value.is_string()) {
    return absl::InvalidArgumentError(
        absl::StrCat("options.", key, " must be a string"));
  }
  return value.get<std::string>();
}

absl::StatusOr<RequestOptions> ParseOptions(const nlohmann::json& object) {
  RequestOptions options;
  if (object.is_null()) {
    return options;
  }
  if (!object.is_object()) {
    return absl::InvalidArgumentError("options must be a JSON object");
  }

  ABSL_ASSIGN_OR_RETURN(const std::int64_t redirects,
                        JsonInt(object, "max_redirects", 5));
  if (redirects < 0) {
    return absl::InvalidArgumentError(
        "options.max_redirects must not be negative");
  }
  options.max_redirects = static_cast<int>(redirects);

  if (object.contains("timeout")) {
    const nlohmann::json& value = object.at("timeout");
    if (!value.is_number()) {
      return absl::InvalidArgumentError(
          "options.timeout must be a number of seconds");
    }
    const double seconds = value.get<double>();
    if (seconds <= 0) {
      return absl::InvalidArgumentError("options.timeout must be positive");
    }
    options.timeout = absl::Seconds(seconds);
  }

  ABSL_ASSIGN_OR_RETURN(const std::string body_mode,
                        JsonString(object, "request_body", "buffer"));
  if (body_mode == "buffer") {
    options.body_mode = BodyMode::kBuffer;
  } else if (body_mode == "stream") {
    options.body_mode = BodyMode::kStream;
  } else {
    return absl::InvalidArgumentError(
        "options.request_body must be \"buffer\" or \"stream\"");
  }

  ABSL_ASSIGN_OR_RETURN(options.accept_pushes,
                        JsonBool(object, "accept_pushes", false));
  ABSL_ASSIGN_OR_RETURN(options.reuse_connection,
                        JsonBool(object, "reuse_connection", true));
  ABSL_ASSIGN_OR_RETURN(options.user_agent,
                        JsonString(object, "user_agent",
                                   std::string(kDefaultUserAgent)));

  ABSL_ASSIGN_OR_RETURN(
      const std::int64_t max_body,
      JsonInt(object, "max_body_bytes",
              static_cast<std::int64_t>(options.max_body_bytes)));
  if (max_body <= 0) {
    return absl::InvalidArgumentError(
        "options.max_body_bytes must be positive");
  }
  options.max_body_bytes = static_cast<size_t>(max_body);

  ABSL_ASSIGN_OR_RETURN(const std::string version,
                        JsonString(object, "http_version", "auto"));
  if (version == "auto") {
    options.transport.client_preference =
        Http2Options::ProtocolPreference::kAuto;
  } else if (version == "2") {
    options.transport.client_preference =
        Http2Options::ProtocolPreference::kHttp2;
  } else if (version == "1.1") {
    options.transport.client_preference =
        Http2Options::ProtocolPreference::kHttp11;
  } else {
    return absl::InvalidArgumentError(
        "options.http_version must be \"auto\", \"2\" or \"1.1\"");
  }

  if (object.contains("tls")) {
    const nlohmann::json& tls = object.at("tls");
    if (!tls.is_object()) {
      return absl::InvalidArgumentError("options.tls must be a JSON object");
    }
    ABSL_ASSIGN_OR_RETURN(options.transport.tls.verify_peer,
                          JsonBool(tls, "verify_peer", true));
    ABSL_ASSIGN_OR_RETURN(options.transport.tls.ca_certificate_pem_file,
                          JsonString(tls, "ca_file", ""));
    ABSL_ASSIGN_OR_RETURN(options.transport.tls.certificate_pem_file,
                          JsonString(tls, "certificate_file", ""));
    ABSL_ASSIGN_OR_RETURN(options.transport.tls.key_pem_file,
                          JsonString(tls, "key_file", ""));
  }

  if (object.contains("headers")) {
    const nlohmann::json& headers = object.at("headers");
    if (!headers.is_object()) {
      return absl::InvalidArgumentError(
          "options.headers must be a JSON object");
    }
    for (const auto& [name, value] : headers.items()) {
      std::string folded = absl::AsciiStrToLower(name);
      if (value.is_array()) {
        for (const nlohmann::json& repeated : value) {
          if (!repeated.is_string()) {
            return absl::InvalidArgumentError(
                "options.headers values must be strings or arrays of strings");
          }
          options.extra_headers.emplace_back(folded,
                                            repeated.get<std::string>());
        }
      } else if (value.is_string()) {
        options.extra_headers.emplace_back(std::move(folded),
                                          value.get<std::string>());
      } else {
        return absl::InvalidArgumentError(
            "options.headers values must be strings or arrays of strings");
      }
    }
  }

  if (object.contains("omit")) {
    const nlohmann::json& omit = object.at("omit");
    if (!omit.is_array()) {
      return absl::InvalidArgumentError(
          "options.omit must be an array of output port names");
    }
    for (const nlohmann::json& name : omit) {
      if (!name.is_string()) {
        return absl::InvalidArgumentError(
            "options.omit must be an array of output port names");
      }
      options.omit.push_back(name.get<std::string>());
    }
  }

  options.transport.enable_push = options.accept_pushes;
  options.transport.max_response_body_size = options.max_body_bytes;
  return options;
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

/**
 * The request headers, from the action's own headers plus `options.headers`.
 *
 * An action header that is not one of A11's own is an HTTP request header: that
 * is the mapping, and it is what makes Flow's `with "accept": ...` and
 * `forward headers "authorization"` mean what they look like. `options.headers`
 * comes last so a computed value beats an inherited one, and so a caller can
 * send a literal `x-a11-...` header that would otherwise be filtered.
 */
HttpHeaders RequestHeadersFor(const std::shared_ptr<Action>& action,
                              const RequestOptions& options) {
  HttpHeaders headers;
  std::vector<std::pair<std::string, std::string>> inherited;
  for (const auto& [name, value] : action->Headers()) {
    if (absl::StartsWith(name, kFrameworkHeaderPrefix)) {
      continue;
    }
    inherited.emplace_back(name, value);
  }
  // A ByteMap is unordered; sorting keeps the request byte-for-byte the same
  // from one run to the next, which matters for caches and for tests.
  std::sort(inherited.begin(), inherited.end());
  for (auto& field : inherited) {
    headers.push_back(std::move(field));
  }
  for (const auto& field : options.extra_headers) {
    a11::net::EraseHttpHeader(&headers, field.first);
  }
  for (const auto& field : options.extra_headers) {
    headers.push_back(field);
  }
  a11::net::NormalizeHttpHeaders(&headers);
  if (!options.user_agent.empty() &&
      !GetHttpHeader(headers, "user-agent").has_value()) {
    a11::net::SetHttpHeader(&headers, "user-agent", options.user_agent);
  }
  return headers;
}

/**
 * Header fields as one JSON object, for a caller that wants to look one up.
 *
 * Repeated fields are joined, which is what every high-level HTTP API does and
 * is lossy in exactly one place: `set-cookie`, whose values contain commas, so
 * it is joined with newlines instead. The `fields` port is there for anyone who
 * needs the wire truth.
 */
nlohmann::json HeaderObject(const HttpHeaders& headers) {
  std::vector<std::pair<std::string, std::string>> merged;
  for (const auto& [name, value] : headers) {
    const auto found = std::find_if(
        merged.begin(), merged.end(),
        [&name](const auto& entry) { return entry.first == name; });
    if (found == merged.end()) {
      merged.emplace_back(name, value);
      continue;
    }
    absl::StrAppend(&found->second, name == "set-cookie" ? "\n" : ", ", value);
  }
  nlohmann::json object = nlohmann::json::object();
  for (const auto& [name, value] : merged) {
    object[name] = value;
  }
  return object;
}

/// One `[name, value]` pair per field, for the `fields` port to stream.
std::vector<nlohmann::json> FieldPairs(const HttpHeaders& headers) {
  std::vector<nlohmann::json> fields;
  fields.reserve(headers.size());
  for (const auto& [name, value] : headers) {
    fields.push_back(nlohmann::json::array({name, value}));
  }
  return fields;
}

absl::StatusOr<absl::Time> DeadlineFromAction(
    const std::shared_ptr<Action>& action, absl::Duration timeout) {
  const absl::Time from_timeout = timeout == absl::InfiniteDuration()
                                      ? absl::InfiniteFuture()
                                      : absl::Now() + timeout;
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> raw,
                        action->GetHeader(kDeadlineHeader));
  if (!raw.has_value() || raw->empty()) {
    return from_timeout;
  }
  std::string_view value = *raw;
  const bool nanos = absl::EndsWith(value, "ns");
  if (nanos) {
    value.remove_suffix(2);
  }
  std::int64_t magnitude = 0;
  if (!absl::SimpleAtoi(value, &magnitude) || magnitude < 0) {
    return absl::InvalidArgumentError(
        "x-a11-deadline must be a non-negative base-10 integer of "
        "milliseconds since the epoch, or nanoseconds with an 'ns' suffix");
  }
  const absl::Time absolute =
      nanos ? absl::FromUnixNanos(magnitude) : absl::FromUnixMillis(magnitude);
  // Whichever bound is tighter wins: a caller's deadline is a ceiling, and the
  // action's own timeout is not licence to exceed it.
  return std::min(absolute, from_timeout);
}

// ---------------------------------------------------------------------------
// The exchange
// ---------------------------------------------------------------------------

bool IsRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

/// 301, 302 and 303 become a bodyless GET, as every client does and RFC 9110
/// sanctions; 307 and 308 exist to preserve the method, so they keep it.
bool RedirectRewritesToGet(int status) {
  return status == 301 || status == 302 || status == 303;
}

/** One request's inputs, read before anything is dialled. */
struct Exchange {
  ParsedUrl url;
  std::string method;
  HttpHeaders headers;
  RequestOptions options;
  std::string buffered_body;          ///< Set when body_mode is kBuffer.
  std::shared_ptr<AsyncNode> body;    ///< Set when body_mode is kStream.
  absl::Time deadline = absl::InfiniteFuture();
};

/// Reads the request body port to its end. Only for kBuffer: this is what makes
/// a `content-length` possible, and what makes a redirect replayable.
absl::StatusOr<std::string> DrainBody(const std::shared_ptr<AsyncNode>& node,
                                      size_t limit) {
  std::string body;
  while (true) {
    ABSL_ASSIGN_OR_RETURN(std::optional<data::Chunk> chunk,
                          node->NextChunk().Await());
    if (!chunk.has_value()) {
      break;
    }
    if (chunk->IsNull()) {
      continue;
    }
    if (body.size() + chunk->data.size() > limit) {
      return absl::OutOfRangeError(
          "The request body exceeds options.max_body_bytes");
    }
    body.append(chunk->data);
  }
  return body;
}

absl::StatusOr<Exchange> ReadExchange(const std::shared_ptr<Action>& action) {
  Exchange exchange;

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> options_node,
                        action->GetInput("options"));
  ABSL_ASSIGN_OR_RETURN(std::optional<nlohmann::json> options_json,
                        ReadJson(options_node));
  ABSL_ASSIGN_OR_RETURN(
      exchange.options,
      ParseOptions(options_json.value_or(nlohmann::json::object())));

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> url_node,
                        action->GetInput("url"));
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> url, ReadText(url_node));
  if (!url.has_value() || url->empty()) {
    return absl::InvalidArgumentError("A url is required");
  }
  ABSL_ASSIGN_OR_RETURN(exchange.url, a11::net::ParseUrl(*url));
  if (exchange.url.scheme != "http" && exchange.url.scheme != "https") {
    return absl::InvalidArgumentError(absl::StrCat(
        "An http or https URL is required, got: ", exchange.url.scheme));
  }

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> method_node,
                        action->GetInput("method"));
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> method,
                        ReadText(method_node));
  exchange.method = method.value_or("GET");
  if (exchange.method.empty()) {
    exchange.method = "GET";
  }
  absl::AsciiStrToUpper(&exchange.method);

  exchange.headers = RequestHeadersFor(action, exchange.options);
  ABSL_ASSIGN_OR_RETURN(exchange.deadline,
                        DeadlineFromAction(action, exchange.options.timeout));
  if (exchange.deadline <= absl::Now()) {
    return absl::DeadlineExceededError("The request deadline has already passed");
  }

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> body_node,
                        action->GetInput("request_body"));
  if (exchange.options.body_mode == BodyMode::kStream) {
    exchange.body = body_node;
  } else {
    ABSL_ASSIGN_OR_RETURN(
        exchange.buffered_body,
        DrainBody(body_node, exchange.options.max_body_bytes));
  }
  return exchange;
}

/// One hop's response stream, plus the connection it must not outlive.
struct Attempt {
  HttpConnectionLease lease;
  std::shared_ptr<Http2ResponseStream> response;
  /// The write half, when the body is being streamed rather than buffered.
  std::shared_ptr<Http2DuplexStream> upload;
};

absl::StatusOr<Attempt> Send(const Exchange& exchange, const ParsedUrl& target,
                             const std::string& method,
                             const std::string& body) {
  Http2Options transport = exchange.options.transport;
  transport.tls.enabled = target.secure();

  Attempt attempt;
  ABSL_ASSIGN_OR_RETURN(
      attempt.lease,
      exchange.options.reuse_connection
          ? HttpConnectionPool::Shared()->Acquire(target, transport).Await(
                exchange.deadline)
          : HttpConnectionPool::AcquireUnshared(target, transport)
                .Await(exchange.deadline));

  HttpHeaders headers = exchange.headers;
  if (exchange.body != nullptr) {
    // A streamed body has no length to declare, and the transports frame it
    // themselves (DATA frames or chunked).
    a11::net::EraseHttpHeader(&headers, "content-length");
    ABSL_ASSIGN_OR_RETURN(attempt.upload,
                          attempt.lease.client()->RequestStreamingBody(
                              method, target.target(), headers, target.scheme));
    attempt.response = attempt.upload->response();
    return attempt;
  }
  if (!body.empty() && !GetHttpHeader(headers, "content-length").has_value()) {
    a11::net::SetHttpHeader(&headers, "content-length",
                            std::to_string(body.size()));
  }
  ABSL_ASSIGN_OR_RETURN(attempt.response,
                        attempt.lease.client()->RequestStream(
                            method, target.target(), headers, body,
                            target.scheme));
  return attempt;
}

/// Pumps the request body port into an open request side, chunk by chunk. This
/// is the reason `body` is a stream port: an upload whose length nobody knows.
absl::Status PumpRequestBody(const std::shared_ptr<AsyncNode>& body,
                             const std::shared_ptr<Http2DuplexStream>& upload) {
  while (true) {
    absl::StatusOr<std::optional<data::Chunk>> chunk = body->NextChunk().Await();
    if (!chunk.ok()) {
      (void)upload->Abort(chunk.status());
      return chunk.status();
    }
    if (!chunk->has_value()) {
      break;
    }
    if ((*chunk)->IsNull() || (*chunk)->data.empty()) {
      continue;
    }
    ABSL_RETURN_IF_ERROR(upload->Write((*chunk)->data));
  }
  return upload->Finish();
}

/**
 * Reads pushed responses off @p response until there are no more.
 *
 * A push carries a head *and* a body, and one port cannot interleave several
 * bodies without inventing a framing for them. A11 already has the right answer:
 * each pushed body gets a node of its own, and the record on the `pushes` port
 * carries that node's id. A reader attaches to it -- `node(rec.body)` in Flow --
 * exactly as it would to any other stream.
 */
absl::Status PumpPushes(const std::shared_ptr<Action>& action,
                        const std::shared_ptr<Http2ResponseStream>& response,
                        const std::shared_ptr<AsyncNode>& pushes,
                        absl::Time deadline, size_t limit) {
  const std::shared_ptr<nodes::NodeMap> node_map = action->GetNodeMap();
  if (node_map == nullptr) {
    return absl::FailedPreconditionError(
        "Reading pushed responses needs a node map to put their bodies in");
  }
  int index = 0;
  while (true) {
    absl::StatusOr<std::optional<HttpPushedResponse>> promised =
        response->NextPush().Await(deadline);
    if (!promised.ok() || !promised->has_value()) {
      // A failure here is the associated response's failure, reported there.
      return absl::OkStatus();
    }
    const HttpPushedResponse& push = **promised;

    absl::StatusOr<HttpResponseHead> head =
        push.response->Headers().Await(deadline);
    if (!head.ok()) {
      continue;  // The server promised and then reset it; nothing to report.
    }

    ABSL_ASSIGN_OR_RETURN(
        const std::string body_id,
        Action::MakeNodeId(action->GetId(),
                           absl::StrCat("push-", index++, "-body")));
    ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> body,
                          node_map->Get(body_id));

    nlohmann::json record{
        {"method", push.method},
        {"url", absl::StrCat(push.scheme, "://", push.authority, push.path)},
        {"path", push.path},
        {"status", head->status},
        {"headers", HeaderObject(head->headers)},
        {"request_headers", HeaderObject(push.headers)},
        {"body", body_id}};
    ABSL_ASSIGN_OR_RETURN(data::Chunk head_chunk, JsonChunk(record));
    ABSL_RETURN_IF_ERROR(
        pushes->PutChunk(std::move(head_chunk), std::nullopt, /*final=*/false)
            .Await()
            .status());

    // The head is out, so a reader can attach before the body arrives.
    size_t written = 0;
    while (true) {
      absl::StatusOr<std::optional<std::string>> chunk =
          push.response->Read().Await(deadline);
      if (!chunk.ok()) {
        break;
      }
      if (!chunk->has_value()) {
        break;
      }
      written += (*chunk)->size();
      if (written > limit) {
        (void)push.response->Cancel(absl::OutOfRangeError(
            "A pushed response exceeds options.max_body_bytes"));
        break;
      }
      ABSL_RETURN_IF_ERROR(
          body->PutChunk(BytesChunk(std::move(**chunk)), std::nullopt,
                         /*final=*/false)
              .Await()
              .status());
    }
    ABSL_RETURN_IF_ERROR(Outputs::CloseNode(body));
  }
}

/** The output ports of @c make_http_request, in the order they become readable. */
const std::vector<std::string>& RequestOutputNames() {
  static const std::vector<std::string>* const names =
      new std::vector<std::string>{"status_code", "headers",   "fields",
                                   "body",        "trailers",  "redirects",
                                   "pushes",      "connection"};
  return *names;
}

absl::Status RunRequest(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(Exchange exchange, ReadExchange(action));
  ABSL_ASSIGN_OR_RETURN(
      Outputs outputs,
      Outputs::Open(action, RequestOutputNames(), exchange.options.omit));

  // Whatever happens below, every port this action declared has to end, or a
  // reader is left waiting on one that was never going to be written.
  const absl::Status status = [&]() -> absl::Status {
    ParsedUrl target = exchange.url;
    std::string method = exchange.method;
    std::string body = exchange.buffered_body;
    bool streamed = exchange.body != nullptr;

    Attempt attempt;
    for (int followed = 0;; ++followed) {
      ABSL_ASSIGN_OR_RETURN(attempt, Send(exchange, target, method, body));

      // A streamed body is pumped on its own fiber while the response headers
      // are awaited: a server may well answer -- a 401, a 413 -- before it has
      // taken all of an upload, and waiting for the whole body first would miss
      // that.
      a11::Task upload = a11::ReadyTask();
      if (attempt.upload != nullptr) {
        upload = a11::SubmitTask(
            [body_node = exchange.body, writer = attempt.upload]() {
              return PumpRequestBody(body_node, writer);
            });
      }
      const std::shared_ptr<Http2ResponseStream> response = attempt.response;

      absl::StatusOr<HttpResponseHead> head =
          response->Headers().Await(exchange.deadline);
      if (!head.ok()) {
        (void)upload.Cancel();
        return head.status();
      }

      if (IsRedirect(head->status) && exchange.options.max_redirects > 0) {
        const std::optional<std::string> location =
            GetHttpHeader(head->headers, "location");
        if (location.has_value()) {
          if (followed >= exchange.options.max_redirects) {
            return MakeStatus(
                absl::StatusCode::kResourceExhausted,
                absl::StrCat("Followed ", exchange.options.max_redirects,
                             " redirects without reaching a final response"),
                nlohmann::json{{"url", exchange.url.ToString()},
                               {"last_url", target.ToString()}});
          }
          // The hop chain is a stream of its own: a caller auditing where a URL
          // actually went reads it without the bodies getting in the way.
          ABSL_RETURN_IF_ERROR(
              outputs.PutJson("redirects",
                              nlohmann::json{{"url", target.ToString()},
                                             {"status", head->status},
                                             {"location", *location}}));
          if (attempt.upload != nullptr) {
            (void)attempt.upload->Abort(absl::CancelledError("redirected"));
            (void)upload.Await();
          } else {
            (void)response->Cancel(absl::CancelledError("following redirect"));
          }
          attempt = Attempt();
          ABSL_ASSIGN_OR_RETURN(target,
                                a11::net::ResolveReference(target, *location));
          if (target.scheme != "http" && target.scheme != "https") {
            return absl::InvalidArgumentError(absl::StrCat(
                "A redirect left http(s) for scheme: ", target.scheme));
          }
          if (RedirectRewritesToGet(head->status)) {
            method = "GET";
            body.clear();
            streamed = false;
            exchange.body = nullptr;
          } else if (streamed) {
            // The body was consumed sending the first hop and a node is read
            // once, so there is nothing left to send again. Buffer it
            // (`request_body: "buffer"`) if the URL may redirect.
            return absl::FailedPreconditionError(
                "A streamed request body cannot be replayed to a redirect; use "
                "options.request_body=\"buffer\"");
          }
          continue;
        }
        // A 3xx with no Location is not a redirect anyone can follow: report it
        // as the response it is.
      }

      // Status and headers first, so a caller can decide what to do with the
      // body -- or decide not to read it -- while it is still arriving.
      ABSL_RETURN_IF_ERROR(outputs.PutOnly("status_code", head->status));
      ABSL_RETURN_IF_ERROR(
          outputs.PutOnly("headers", HeaderObject(head->headers)));
      // One field per value, in wire order: a stream, because that is what a
      // header block is, repeats and all.
      for (const nlohmann::json& field : FieldPairs(head->headers)) {
        ABSL_RETURN_IF_ERROR(outputs.PutJson("fields", field));
      }
      ABSL_RETURN_IF_ERROR(outputs.Close("fields"));
      ABSL_RETURN_IF_ERROR(outputs.PutOnly(
          "connection",
          nlohmann::json{
              {"url", target.ToString()},
              {"http_version",
               attempt.lease.multiplexed() ? "2" : "1.1"},
              {"secure", target.secure()},
              {"reused", attempt.lease.reused()}}));
      ABSL_RETURN_IF_ERROR(outputs.Close("redirects"));

      // Pushes are read on their own fiber: they arrive interleaved with the
      // body, and neither should have to wait for the other.
      a11::Task pushed = a11::ReadyTask();
      const std::shared_ptr<AsyncNode> pushes = outputs.Get("pushes");
      if (pushes != nullptr && exchange.options.accept_pushes) {
        pushed = a11::SubmitTask([action, response, pushes,
                                 deadline = exchange.deadline,
                                 limit = exchange.options.max_body_bytes]() {
          return PumpPushes(action, response, pushes, deadline, limit);
        });
      }

      absl::Status body_status = absl::OkStatus();
      size_t received = 0;
      while (true) {
        absl::StatusOr<std::optional<std::string>> chunk =
            response->Read().Await(exchange.deadline);
        if (!chunk.ok()) {
          body_status = chunk.status();
          break;
        }
        if (!chunk->has_value()) {
          break;
        }
        received += (*chunk)->size();
        if (received > exchange.options.max_body_bytes) {
          body_status = absl::OutOfRangeError(
              "The response body exceeds options.max_body_bytes");
          break;
        }
        ABSL_RETURN_IF_ERROR(
            outputs.Put("body", BytesChunk(std::move(**chunk)),
                        /*final=*/false));
      }
      ABSL_RETURN_IF_ERROR(outputs.Close("body"));
      (void)upload.Await();
      (void)pushed.Await();
      ABSL_RETURN_IF_ERROR(outputs.Close("pushes"));
      if (!body_status.ok()) {
        return body_status;
      }

      // Trailers last, because that is where they are: after the body, and the
      // only place a value computed while streaming can be reported from.
      ABSL_ASSIGN_OR_RETURN(const HttpHeaders trailers,
                            response->Trailers().Await(exchange.deadline));
      ABSL_RETURN_IF_ERROR(outputs.PutOnly("trailers", HeaderObject(trailers)));
      return absl::OkStatus();
    }
  }();

  const absl::Status closed = outputs.Finish();
  return status.ok() ? closed : status;
}

// ---------------------------------------------------------------------------
// web-fetch: the same exchange with the protocol turned down
// ---------------------------------------------------------------------------

/** The output ports of @c web-fetch. */
const std::vector<std::string>& FetchOutputNames() {
  static const std::vector<std::string>* const names =
      new std::vector<std::string>{"status_code", "ok",   "headers", "text",
                                   "json",        "body", "items"};
  return *names;
}

/** How the body is decoded into the `items` stream. */
enum class ItemFraming {
  kNone,      ///< Not a shape with items in it.
  kLines,     ///< One JSON value per line (NDJSON / JSON Lines).
  kEvents,    ///< Server-sent events.
  kElements,  ///< The elements of a top-level JSON array.
};

/// Picks the decoding from the response's content type. A caller does not have
/// to say which shape a payload is when the payload already says.
ItemFraming FramingFor(const HttpHeaders& headers) {
  std::string type = GetHttpHeader(headers, "content-type").value_or("");
  type = absl::AsciiStrToLower(type);
  const size_t semicolon = type.find(';');
  if (semicolon != std::string::npos) {
    type = type.substr(0, semicolon);
  }
  type = std::string(absl::StripAsciiWhitespace(type));
  if (type == "text/event-stream") {
    return ItemFraming::kEvents;
  }
  if (type == "application/x-ndjson" || type == "application/jsonl" ||
      type == "application/x-jsonlines" || type == "application/json-seq") {
    return ItemFraming::kLines;
  }
  if (type == "application/json" || absl::EndsWith(type, "+json")) {
    return ItemFraming::kElements;
  }
  return ItemFraming::kNone;
}

/**
 * Decodes a text/event-stream incrementally.
 *
 * The wire format is a handful of `field: value` lines terminated by a blank
 * one. Kept here rather than reached for from the SSE wire stream because that
 * one is a WireStream carrying A11 frames, and this is a decoder over arbitrary
 * bytes -- the same syntax, a different job.
 */
class EventStreamDecoder {
 public:
  /// Feeds bytes, appending each completed event to @p events.
  void Feed(std::string_view data, std::vector<nlohmann::json>* events) {
    buffer_.append(data);
    size_t start = 0;
    while (true) {
      const size_t newline = buffer_.find('\n', start);
      if (newline == std::string::npos) {
        break;
      }
      std::string_view line(buffer_.data() + start, newline - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      start = newline + 1;
      if (line.empty()) {
        Flush(events);
        continue;
      }
      if (line.front() == ':') {
        continue;  // A comment, which is also how a server keeps alive.
      }
      const size_t colon = line.find(':');
      const std::string_view name =
          colon == std::string_view::npos ? line : line.substr(0, colon);
      std::string_view value =
          colon == std::string_view::npos ? std::string_view()
                                          : line.substr(colon + 1);
      if (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
      }
      if (name == "data") {
        if (!data_.empty()) {
          data_.push_back('\n');
        }
        data_.append(value);
      } else if (name == "event") {
        event_.assign(value);
      } else if (name == "id") {
        id_.assign(value);
      } else if (name == "retry") {
        retry_.assign(value);
      }
    }
    buffer_.erase(0, start);
  }

  /// Emits a final event for a stream that ended without a blank line.
  void Finish(std::vector<nlohmann::json>* events) {
    if (!buffer_.empty()) {
      buffer_.push_back('\n');
      const std::string tail = std::move(buffer_);
      buffer_.clear();
      Feed(tail, events);
    }
    Flush(events);
  }

 private:
  void Flush(std::vector<nlohmann::json>* events) {
    if (data_.empty() && event_.empty() && id_.empty()) {
      return;
    }
    nlohmann::json record{{"event", event_.empty() ? "message" : event_},
                          {"data", data_},
                          {"id", id_}};
    // `data` is usually JSON, and a caller that has to parse it again is being
    // made to do the decoding this port exists to have done.
    nlohmann::json parsed = nlohmann::json::parse(data_, nullptr, false);
    if (!parsed.is_discarded()) {
      record["json"] = std::move(parsed);
    }
    if (!retry_.empty()) {
      record["retry"] = retry_;
    }
    events->push_back(std::move(record));
    data_.clear();
    event_.clear();
    id_.clear();
  }

  std::string buffer_;
  std::string data_;
  std::string event_;
  std::string id_;
  std::string retry_;
};

/// Splits a text stream into lines, holding back a partial trailing one.
class LineDecoder {
 public:
  void Feed(std::string_view data, std::vector<std::string>* lines) {
    buffer_.append(data);
    size_t start = 0;
    while (true) {
      const size_t newline = buffer_.find('\n', start);
      if (newline == std::string::npos) {
        break;
      }
      std::string_view line(buffer_.data() + start, newline - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      if (!line.empty()) {
        lines->emplace_back(line);
      }
      start = newline + 1;
    }
    buffer_.erase(0, start);
  }

  void Finish(std::vector<std::string>* lines) {
    std::string_view tail = absl::StripAsciiWhitespace(buffer_);
    if (!tail.empty()) {
      lines->emplace_back(tail);
    }
    buffer_.clear();
  }

 private:
  std::string buffer_;
};

absl::Status RunFetch(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(Exchange exchange, ReadExchange(action));
  ABSL_ASSIGN_OR_RETURN(
      Outputs outputs,
      Outputs::Open(action, FetchOutputNames(), exchange.options.omit));

  const absl::Status status = [&]() -> absl::Status {
    ParsedUrl target = exchange.url;
    std::string method = exchange.method;
    std::string body = exchange.buffered_body;

    Attempt attempt;
    HttpResponseHead head;
    for (int followed = 0;; ++followed) {
      ABSL_ASSIGN_OR_RETURN(attempt, Send(exchange, target, method, body));
      a11::Task upload = a11::ReadyTask();
      if (attempt.upload != nullptr) {
        upload = a11::SubmitTask(
            [body_node = exchange.body, writer = attempt.upload]() {
              return PumpRequestBody(body_node, writer);
            });
      }
      absl::StatusOr<HttpResponseHead> received =
          attempt.response->Headers().Await(exchange.deadline);
      if (!received.ok()) {
        (void)upload.Cancel();
        return received.status();
      }
      if (IsRedirect(received->status) && exchange.options.max_redirects > 0) {
        const std::optional<std::string> location =
            GetHttpHeader(received->headers, "location");
        if (location.has_value() && followed < exchange.options.max_redirects) {
          if (attempt.upload != nullptr) {
            (void)attempt.upload->Abort(absl::CancelledError("redirected"));
            (void)upload.Await();
            return absl::FailedPreconditionError(
                "A streamed request body cannot be replayed to a redirect; use "
                "options.request_body=\"buffer\"");
          }
          (void)attempt.response->Cancel(
              absl::CancelledError("following redirect"));
          ABSL_ASSIGN_OR_RETURN(target,
                                a11::net::ResolveReference(target, *location));
          if (RedirectRewritesToGet(received->status)) {
            method = "GET";
            body.clear();
          }
          continue;
        }
      }
      (void)upload.Await();
      head = *std::move(received);
      break;
    }

    // Unlike make_http_request, a 4xx or 5xx is data here rather than a failure:
    // `ok` is false and the body is still delivered, which is what `fetch()`
    // does and what a caller reading an API's error document needs.
    ABSL_RETURN_IF_ERROR(outputs.PutOnly("status_code", head.status));
    ABSL_RETURN_IF_ERROR(outputs.PutOnly("ok", head.status < 400));
    ABSL_RETURN_IF_ERROR(outputs.PutOnly("headers", HeaderObject(head.headers)));

    const ItemFraming framing = FramingFor(head.headers);
    const bool wants_items = outputs.Get("items") != nullptr;
    const bool wants_whole = outputs.Get("text") != nullptr ||
                             outputs.Get("json") != nullptr ||
                             framing == ItemFraming::kElements;

    EventStreamDecoder events;
    LineDecoder lines;
    std::string whole;
    absl::Status body_status = absl::OkStatus();
    while (true) {
      absl::StatusOr<std::optional<std::string>> chunk =
          attempt.response->Read().Await(exchange.deadline);
      if (!chunk.ok()) {
        body_status = chunk.status();
        break;
      }
      if (!chunk->has_value()) {
        break;
      }
      const std::string& piece = **chunk;
      if (whole.size() + piece.size() > exchange.options.max_body_bytes) {
        body_status = absl::OutOfRangeError(
            "The response body exceeds options.max_body_bytes");
        break;
      }
      ABSL_RETURN_IF_ERROR(outputs.Put("body", BytesChunk(piece),
                                       /*final=*/false));
      if (wants_whole) {
        whole.append(piece);
      }
      // The streaming half of `items`: an event or a line is complete as soon as
      // its terminator arrives, so it goes out then rather than at the end.
      if (wants_items && framing == ItemFraming::kEvents) {
        std::vector<nlohmann::json> decoded;
        events.Feed(piece, &decoded);
        for (const nlohmann::json& record : decoded) {
          ABSL_RETURN_IF_ERROR(outputs.PutJson("items", record));
        }
      } else if (wants_items && framing == ItemFraming::kLines) {
        std::vector<std::string> decoded;
        lines.Feed(piece, &decoded);
        for (const std::string& line : decoded) {
          nlohmann::json value = nlohmann::json::parse(line, nullptr, false);
          ABSL_RETURN_IF_ERROR(outputs.PutJson(
              "items", value.is_discarded() ? nlohmann::json(line)
                                            : std::move(value)));
        }
      }
    }
    ABSL_RETURN_IF_ERROR(outputs.Close("body"));

    if (wants_items && framing == ItemFraming::kEvents) {
      std::vector<nlohmann::json> decoded;
      events.Finish(&decoded);
      for (const nlohmann::json& record : decoded) {
        ABSL_RETURN_IF_ERROR(outputs.PutJson("items", record));
      }
    } else if (wants_items && framing == ItemFraming::kLines) {
      std::vector<std::string> decoded;
      lines.Finish(&decoded);
      for (const std::string& line : decoded) {
        nlohmann::json value = nlohmann::json::parse(line, nullptr, false);
        ABSL_RETURN_IF_ERROR(outputs.PutJson(
            "items", value.is_discarded() ? nlohmann::json(line)
                                          : std::move(value)));
      }
    }

    if (!body_status.ok()) {
      return body_status;
    }

    nlohmann::json parsed = nlohmann::json::parse(whole, nullptr, false);
    ABSL_RETURN_IF_ERROR(outputs.PutOnly("text", whole));
    if (parsed.is_discarded()) {
      // Not JSON. The port closes with nothing in it rather than failing the
      // run: a caller asking for `json` from a page is asking a question, and
      // "it is not JSON" is an answer.
      ABSL_RETURN_IF_ERROR(outputs.Close("json"));
    } else {
      ABSL_RETURN_IF_ERROR(outputs.PutOnly("json", parsed));
    }
    // A whole JSON array is a stream of its own elements -- the shape of nearly
    // every list endpoint, and the reason `items` is not only for NDJSON.
    if (wants_items && framing == ItemFraming::kElements && parsed.is_array()) {
      for (const nlohmann::json& element : parsed) {
        ABSL_RETURN_IF_ERROR(outputs.PutJson("items", element));
      }
    }
    return outputs.Close("items");
  }();

  const absl::Status closed = outputs.Finish();
  return status.ok() ? closed : status;
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

ActionPortSchema Port(std::string name, std::string type, std::string desc,
                      bool required, bool unary) {
  return ActionPortSchema{.name = std::move(name),
                          .type = std::move(type),
                          .description = std::move(desc),
                          .required = required,
                          .unary = unary};
}

/// The type name for a port carrying arbitrary JSON.
std::string JsonType() { return std::string(data::kJsonMimetype); }

void AddSharedHeaders(ActionSchema& schema) {
  schema.headers.emplace(
      std::string(kDeadlineHeader),
      ActionHeaderSchema{
          .name = std::string(kDeadlineHeader),
          .description =
              "Absolute execution deadline: a base-10 count of milliseconds "
              "since the Unix epoch, or nanoseconds with an 'ns' suffix. "
              "Whichever of it and options.timeout is tighter bounds the "
              "request."});
}

void AddRequestInputs(ActionSchema& schema, std::string_view options_help) {
  schema.inputs.emplace(
      "url", Port("url", "string", "Absolute http or https URL to request.",
                  /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "method", Port("method", "string", "Request method; GET when omitted.",
                     /*required=*/false, /*unary=*/true));
  // Not "body": the response body is a port too, and a port name means one node
  // whichever direction it faces, so the two would be the same stream.
  schema.inputs.emplace(
      "request_body",
      Port("request_body", std::string(kOctetStream),
           "Request body, in order. Read to its end and sent with a "
           "content-length by default; with options.request_body=\"stream\" "
           "each chunk is sent as it arrives, which is what an upload of "
           "unknown length needs.",
           /*required=*/false, /*unary=*/false));
  schema.inputs.emplace("options",
                        Port("options", JsonType(), std::string(options_help),
                             /*required=*/false, /*unary=*/true));
}

}  // namespace

ActionSchema MakeHttpRequestSchema() {
  ActionSchema schema;
  schema.name = std::string(kMakeHttpRequestAction);
  schema.description =
      "Make one HTTP request, with every part of the response on a port of its "
      "own: the status and headers as soon as they arrive, the body as it "
      "streams, the trailer section after it, the redirects that were "
      "followed, and any responses the server pushed. A 4xx or 5xx is a "
      "response and is delivered like any other -- the action fails only when "
      "there is no response at all. Action headers that do not begin with "
      "x-a11- are sent as HTTP request headers.";
  AddRequestInputs(
      schema,
      "Request settings, all optional: max_redirects (5), timeout (seconds), "
      "request_body (\"buffer\" | \"stream\"), http_version (\"auto\" | \"2\" "
      "| \"1.1\"), accept_pushes (false), reuse_connection (true), "
      "max_body_bytes, user_agent, headers (an object, merged over the action's "
      "own headers and the way to send an x-a11- one), tls "
      "{verify_peer, ca_file, certificate_file, key_file}, and omit -- output "
      "port names to close immediately rather than write.");

  // Named `status_code` rather than `status` because Flow reads `x.status` as
  // the *outcome* of the step called `x`, whatever ports it declares -- so a
  // port called `status` would be unreachable from a flow.
  schema.outputs.emplace(
      "status_code",
      Port("status_code", "integer",
           "The final response's status code, written before the body so a "
           "caller can act on it while the body is still arriving.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "headers",
      Port("headers", JsonType(),
           "Response header fields as an object, lower-cased. Repeated fields "
           "are joined with ', ' -- with '\\n' for set-cookie, whose values "
           "contain commas. Use `fields` where the exact wire form matters.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "fields",
      Port("fields", JsonType(),
           "Every response header field as a [name, value] pair, in wire order "
           "and with repeats intact.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "body", Port("body", std::string(kOctetStream),
                   "Response body chunks, in order, as they arrive.",
                   /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "trailers",
      Port("trailers", JsonType(),
           "The trailer section that followed the body, as an object; empty "
           "when the peer sent none. Written only once `body` has ended.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "redirects",
      Port("redirects", JsonType(),
           "One {url, status, location} per redirect followed, in order.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "pushes",
      Port("pushes", JsonType(),
           "One record per response the server pushed: {method, url, path, "
           "status, headers, request_headers, body}, where `body` is the id of "
           "a node the pushed body is streamed into -- attach to it to read it. "
           "Needs options.accept_pushes, and the node is reachable where the "
           "action's node map is.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "connection",
      Port("connection", JsonType(),
           "How the exchange was carried: {url (after redirects), "
           "http_version, secure, reused}.",
           /*required=*/false, /*unary=*/true));
  AddSharedHeaders(schema);
  return schema;
}

ActionSchema WebFetchSchema() {
  ActionSchema schema;
  schema.name = std::string(kWebFetchAction);
  schema.description =
      "Fetch a URL and hand back the body the way it is wanted: as text, as "
      "parsed JSON, as bytes, or decoded into a stream of items. A 4xx or 5xx "
      "is reported on `ok` rather than failing, so an error document can still "
      "be read. Action headers that do not begin with x-a11- are sent as HTTP "
      "request headers. Use make_http_request for the protocol itself.";
  AddRequestInputs(
      schema,
      "Request settings, all optional: max_redirects (5), timeout (seconds), "
      "request_body (\"buffer\" | \"stream\"), http_version, headers (an "
      "object), tls {verify_peer, ca_file, certificate_file, key_file}, "
      "max_body_bytes, user_agent, and omit -- output port names to close "
      "immediately rather than write.");

  schema.outputs.emplace(
      "status_code",
      Port("status_code", "integer", "The response's status code.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "ok", Port("ok", "bool", "Whether the status is below 400.",
                 /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "headers", Port("headers", JsonType(),
                      "Response header fields as an object, lower-cased.",
                      /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "text", Port("text", "string", "The whole body as text.",
                   /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "json",
      Port("json", JsonType(),
           "The body parsed as JSON. Closes with nothing when it is not JSON, "
           "rather than failing the fetch.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "body", Port("body", std::string(kOctetStream),
                   "The body as it arrives, for a payload too large to hold.",
                   /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "items",
      Port("items", JsonType(),
           "The body decoded into values, chosen by its content type: one "
           "record per event for text/event-stream, one value per line for "
           "NDJSON, one element for each member of a top-level JSON array. "
           "Empty for anything else. Events and lines are emitted as they "
           "arrive.",
           /*required=*/false, /*unary=*/false));
  AddSharedHeaders(schema);
  return schema;
}

ActionHandler MakeHttpRequestHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() {
      return RunRequest(action);
    });
  };
}

ActionHandler WebFetchHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask(
        [action = std::move(action)]() { return RunFetch(action); });
  };
}

absl::Status RegisterHttpActions(a11::actions::ActionRegistry& registry) {
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kMakeHttpRequestAction),
                                        MakeHttpRequestSchema(),
                                        MakeHttpRequestHandler()));
  return registry.Register(std::string(kWebFetchAction), WebFetchSchema(),
                           WebFetchHandler());
}

}  // namespace a11::sdk::http
