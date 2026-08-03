// Copyright 2026 The A11 Authors.

#include "a11/stores/redis_chunk_store.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/msgpack.h"
#include "a11/data/types.h"
#include "a11/stores/redis_chunk_store_script.h"
#include "redis/client.h"
#include "redis/reply.h"

namespace a11::stores {
namespace {

constexpr size_t kMaximumLuaNextBatch = 1024;
constexpr std::string_view kTombstoneReference = "__tombstone__";

std::string ScriptText() {
  return std::string(internal::kRedisChunkStoreScript);
}

absl::Status ProtocolError(std::string_view message) {
  return absl::DataLossError(
      absl::StrCat("Invalid RedisChunkStore reply: ", message));
}

absl::Status ScriptFailure(std::string_view code, std::string_view message) {
  absl::StatusCode status_code = absl::StatusCode::kUnknown;
  if (code == "CANCELLED")
    status_code = absl::StatusCode::kCancelled;
  else if (code == "UNKNOWN")
    status_code = absl::StatusCode::kUnknown;
  else if (code == "INVALID_ARGUMENT")
    status_code = absl::StatusCode::kInvalidArgument;
  else if (code == "DEADLINE_EXCEEDED")
    status_code = absl::StatusCode::kDeadlineExceeded;
  else if (code == "NOT_FOUND")
    status_code = absl::StatusCode::kNotFound;
  else if (code == "ALREADY_EXISTS")
    status_code = absl::StatusCode::kAlreadyExists;
  else if (code == "PERMISSION_DENIED")
    status_code = absl::StatusCode::kPermissionDenied;
  else if (code == "RESOURCE_EXHAUSTED")
    status_code = absl::StatusCode::kResourceExhausted;
  else if (code == "FAILED_PRECONDITION")
    status_code = absl::StatusCode::kFailedPrecondition;
  else if (code == "ABORTED")
    status_code = absl::StatusCode::kAborted;
  else if (code == "OUT_OF_RANGE")
    status_code = absl::StatusCode::kOutOfRange;
  else if (code == "UNIMPLEMENTED")
    status_code = absl::StatusCode::kUnimplemented;
  else if (code == "INTERNAL")
    status_code = absl::StatusCode::kInternal;
  else if (code == "UNAVAILABLE")
    status_code = absl::StatusCode::kUnavailable;
  else if (code == "DATA_LOSS")
    status_code = absl::StatusCode::kDataLoss;
  else if (code == "UNAUTHENTICATED")
    status_code = absl::StatusCode::kUnauthenticated;
  else
    return ProtocolError(absl::StrCat("unknown script status code ", code));
  return absl::Status(status_code, std::string(message));
}

absl::StatusOr<std::string_view> StringAt(
    const std::vector<redis::Reply>& values, size_t index) {
  if (index >= values.size())
    return ProtocolError("a required array element is missing");
  absl::StatusOr<std::string_view> value = values[index].AsStringView();
  if (!value.ok()) {
    return ProtocolError(
        absl::StrCat("array element ", index, " is not a string"));
  }
  return *value;
}

absl::StatusOr<const std::vector<redis::Reply>*> ResultElements(
    const redis::Reply& reply) {
  absl::StatusOr<const std::vector<redis::Reply>*> elements =
      reply.AsElements();
  if (!elements.ok())
    return ProtocolError("top-level value is not an array");
  if ((*elements)->empty())
    return ProtocolError("top-level array is empty");
  absl::StatusOr<std::string_view> tag = StringAt(**elements, 0);
  if (!tag.ok())
    return tag.status();
  if (*tag != "error")
    return *elements;
  if ((*elements)->size() != 3)
    return ProtocolError("script error does not have three fields");
  absl::StatusOr<std::string_view> code = StringAt(**elements, 1);
  if (!code.ok())
    return code.status();
  absl::StatusOr<std::string_view> message = StringAt(**elements, 2);
  if (!message.ok())
    return message.status();
  return ScriptFailure(*code, *message);
}

template <typename T>
absl::StatusOr<T> ParseUnsigned(std::string_view value,
                                std::string_view field) {
  static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
  if (value.empty())
    return ProtocolError(absl::StrCat(field, " is empty"));
  T parsed = 0;
  const char* first = value.data();
  const char* last = first + value.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return ProtocolError(absl::StrCat(field, " is not an unsigned integer"));
  }
  return parsed;
}

absl::StatusOr<std::optional<std::uint32_t>> ParseOptionalSequence(
    std::string_view value, std::string_view field) {
  if (value.empty())
    return std::optional<std::uint32_t>{};
  absl::StatusOr<std::uint32_t> parsed =
      ParseUnsigned<std::uint32_t>(value, field);
  if (!parsed.ok())
    return parsed.status();
  return std::optional<std::uint32_t>(*parsed);
}

absl::StatusOr<absl::Status> DecodeStoredStatus(std::string_view encoded) {
  absl::StatusOr<absl::Status> decoded = data::UnpackStatus(encoded);
  if (decoded.ok())
    return absl::StatusOr<absl::Status>(std::in_place, std::move(*decoded));
  absl::StatusOr<absl::Status> result;
  result.AssignStatus(absl::DataLossError(
      absl::StrCat("RedisChunkStore contains an invalid terminal status: ",
                   decoded.status().message())));
  return result;
}

absl::StatusOr<data::NodeFragment> DecodeItemFields(
    const std::vector<redis::Reply>& values, size_t offset,
    const std::string& node_id) {
  if (offset > values.size() || values.size() - offset < 4)
    return ProtocolError("chunk item is truncated");
  absl::StatusOr<std::string_view> seq_text = StringAt(values, offset);
  if (!seq_text.ok())
    return seq_text.status();
  absl::StatusOr<std::uint32_t> seq =
      ParseUnsigned<std::uint32_t>(*seq_text, "chunk sequence");
  if (!seq.ok())
    return seq.status();
  absl::StatusOr<std::string_view> storage = StringAt(values, offset + 1);
  if (!storage.ok())
    return storage.status();
  absl::StatusOr<std::string_view> payload = StringAt(values, offset + 2);
  if (!payload.ok())
    return payload.status();
  absl::StatusOr<std::string_view> final_text = StringAt(values, offset + 3);
  if (!final_text.ok())
    return final_text.status();
  absl::StatusOr<std::optional<std::uint32_t>> final_seq =
      ParseOptionalSequence(*final_text, "final sequence");
  if (!final_seq.ok())
    return final_seq.status();
  if (final_seq->has_value() && *seq > **final_seq) {
    return ProtocolError("a stored chunk exceeds the final sequence");
  }

  if (*storage == "s3") {
    return absl::UnimplementedError(absl::StrCat(
        "S3-backed chunk access is not implemented (reference: ", *payload,
        ")"));
  }
  if (*storage != "inline" && *storage != "redis" && *storage != "tombstone") {
    return ProtocolError(absl::StrCat("unknown chunk storage kind ", *storage));
  }

  absl::StatusOr<data::Chunk> chunk = data::Chunk::FromMsgpack(*payload);
  if (!chunk.ok()) {
    return absl::DataLossError(
        absl::StrCat("RedisChunkStore contains an invalid encoded Chunk: ",
                     chunk.status().message()));
  }
  if (*storage == "tombstone" &&
      (chunk->ref != kTombstoneReference || !chunk->data.empty())) {
    return ProtocolError("tombstone payload is not data-free");
  }
  return data::NodeFragment{
      .id = node_id,
      .data = std::move(*chunk),
      .seq = *seq,
      .continued = !final_seq->has_value() || *seq < **final_seq,
  };
}

absl::StatusOr<data::NodeFragment> ParseItemReply(
    const std::vector<redis::Reply>& values, const std::string& node_id) {
  if (values.size() != 5)
    return ProtocolError("item reply does not have five fields");
  absl::StatusOr<std::string_view> tag = StringAt(values, 0);
  if (!tag.ok())
    return tag.status();
  if (*tag != "item")
    return ProtocolError(absl::StrCat("expected item reply, got ", *tag));
  return DecodeItemFields(values, 1, node_id);
}

enum class NextDisposition { kReady, kWait, kEnd, kClosed };

struct NextPage {
  NextDisposition disposition = NextDisposition::kWait;
  std::vector<std::optional<data::NodeFragment>> fragments;
  std::optional<absl::Status> terminal_status;
};

absl::StatusOr<NextPage> ParseNextReply(const std::vector<redis::Reply>& values,
                                        const std::string& node_id) {
  if (values.size() < 4)
    return ProtocolError("next reply is truncated");
  absl::StatusOr<std::string_view> tag = StringAt(values, 0);
  if (!tag.ok())
    return tag.status();
  if (*tag != "next")
    return ProtocolError(absl::StrCat("expected next reply, got ", *tag));
  absl::StatusOr<std::string_view> disposition = StringAt(values, 1);
  if (!disposition.ok())
    return disposition.status();
  absl::StatusOr<std::string_view> detail = StringAt(values, 2);
  if (!detail.ok())
    return detail.status();
  absl::StatusOr<std::string_view> count_text = StringAt(values, 3);
  if (!count_text.ok())
    return count_text.status();
  absl::StatusOr<size_t> count =
      ParseUnsigned<size_t>(*count_text, "next item count");
  if (!count.ok())
    return count.status();
  if (*count > (values.size() - 4) / 4 || values.size() != 4 + *count * 4) {
    return ProtocolError("next item count does not match the reply size");
  }

  NextPage page;
  page.fragments.reserve(*count);
  for (size_t index = 0; index < *count; ++index) {
    absl::StatusOr<data::NodeFragment> fragment =
        DecodeItemFields(values, 4 + index * 4, node_id);
    if (!fragment.ok())
      return fragment.status();
    page.fragments.emplace_back(std::move(*fragment));
  }

  if (*disposition == "ready") {
    page.disposition = NextDisposition::kReady;
  } else if (*disposition == "wait") {
    page.disposition = NextDisposition::kWait;
  } else if (*disposition == "end") {
    page.disposition = NextDisposition::kEnd;
  } else if (*disposition == "closed") {
    page.disposition = NextDisposition::kClosed;
    absl::StatusOr<absl::Status> status = DecodeStoredStatus(*detail);
    if (!status.ok())
      return status.status();
    page.terminal_status = std::move(*status);
  } else if (*disposition == "data_loss") {
    return absl::DataLossError(std::string(*detail));
  } else if (*disposition == "s3") {
    return absl::UnimplementedError(absl::StrCat(
        "S3-backed chunk access is not implemented (reference: ", *detail,
        ")"));
  } else {
    return ProtocolError(
        absl::StrCat("unknown next disposition ", *disposition));
  }
  if (page.disposition != NextDisposition::kClosed && !detail->empty())
    return ProtocolError("non-terminal next reply has unexpected detail");
  return page;
}

absl::Status ExpectTag(const std::vector<redis::Reply>& values,
                       std::string_view expected, size_t expected_size) {
  if (values.size() != expected_size) {
    return ProtocolError(absl::StrCat(expected, " reply has the wrong size"));
  }
  absl::StatusOr<std::string_view> tag = StringAt(values, 0);
  if (!tag.ok())
    return tag.status();
  if (*tag != expected) {
    return ProtocolError(
        absl::StrCat("expected ", expected, " reply, got ", *tag));
  }
  return absl::OkStatus();
}

std::optional<std::string> EnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr)
    return std::nullopt;
  return std::string(value);
}

absl::StatusOr<size_t> ParseEnvironmentSize(std::string_view value,
                                            std::string_view name) {
  if (value.empty())
    return absl::InvalidArgumentError(absl::StrCat(name, " is empty"));
  size_t parsed = 0;
  const char* first = value.data();
  const char* last = first + value.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return absl::InvalidArgumentError(
        absl::StrCat(name, " is not a non-negative integer"));
  }
  return parsed;
}

}  // namespace

absl::Status RedisChunkStoreOptions::Validate() const {
  if (absl::StrContains(key_prefix, "{") ||
      absl::StrContains(key_prefix, "}")) {
    return absl::InvalidArgumentError(
        "Redis chunk-store key_prefix must not contain braces because they "
        "would override the Redis Cluster hash tag");
  }
  return absl::OkStatus();
}

absl::StatusOr<RedisChunkStoreOptions>
RedisChunkStoreOptions::FromEnvironment() {
  RedisChunkStoreOptions options;
  if (std::optional<std::string> value =
          EnvironmentValue("A11_REDIS_CHUNK_STORE_KEY_PREFIX")) {
    options.key_prefix = std::move(*value);
  }
  if (std::optional<std::string> value = EnvironmentValue(
          "A11_REDIS_CHUNK_STORE_INLINE_DATA_THRESHOLD_BYTES")) {
    absl::StatusOr<size_t> parsed = ParseEnvironmentSize(
        *value, "A11_REDIS_CHUNK_STORE_INLINE_DATA_THRESHOLD_BYTES");
    if (!parsed.ok())
      return parsed.status();
    options.inline_data_threshold = *parsed;
  }
  absl::Status status = options.Validate();
  if (!status.ok())
    return status;
  return options;
}

std::vector<std::string> RedisChunkStoreKeys::ScriptKeys() const {
  return {metadata, stream, sequence_index, arrival_index, blobs, events};
}

absl::StatusOr<std::shared_ptr<RedisChunkStore>> RedisChunkStore::Create(
    std::string node_id, std::shared_ptr<redis::Client> client,
    RedisChunkStoreOptions options) {
  absl::Status node_status = data::ValidateName(node_id);
  if (!node_status.ok())
    return node_status;
  if (client == nullptr)
    return absl::InvalidArgumentError("Redis client must not be null");
  absl::Status options_status = options.Validate();
  if (!options_status.ok())
    return options_status;

  const std::string base =
      absl::StrCat(options.key_prefix, "{chunk-store:", node_id, "}");
  RedisChunkStoreKeys keys{
      .metadata = absl::StrCat(base, ":metadata"),
      .stream = absl::StrCat(base, ":stream"),
      .sequence_index = absl::StrCat(base, ":sequences"),
      .arrival_index = absl::StrCat(base, ":arrivals"),
      .blobs = absl::StrCat(base, ":blobs"),
      .events = absl::StrCat(base, ":events"),
  };
  return std::make_shared<RedisChunkStore>(
      ConstructorToken{}, std::move(node_id), std::move(client),
      std::move(options), std::move(keys));
}

absl::StatusOr<std::shared_ptr<RedisChunkStore>> RedisChunkStore::Create(
    std::string node_id, std::shared_ptr<redis::Client> client) {
  absl::StatusOr<RedisChunkStoreOptions> options =
      RedisChunkStoreOptions::FromEnvironment();
  if (!options.ok())
    return options.status();
  return Create(std::move(node_id), std::move(client), std::move(*options));
}

absl::StatusOr<std::shared_ptr<RedisChunkStore>> RedisChunkStore::Create(
    std::string node_id) {
  absl::StatusOr<std::shared_ptr<redis::Client>> client =
      redis::DefaultClient();
  if (!client.ok())
    return client.status();
  return Create(std::move(node_id), std::move(*client));
}

a11::Future<data::NodeFragment> RedisChunkStore::Read(ReadKind kind,
                                                      std::uint64_t value,
                                                      absl::Time deadline) {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<data::NodeFragment>(
      [client, keys, node_id, kind, value,
       deadline]() -> absl::StatusOr<data::NodeFragment> {
        std::shared_ptr<redis::Subscription> subscription;
        const std::string kind_text =
            kind == ReadKind::kSequence ? "sequence" : "arrival";
        const std::string value_text = std::to_string(value);

        while (true) {
          const std::uint64_t generation =
              subscription == nullptr ? 0 : subscription->generation();
          absl::StatusOr<redis::Reply> reply =
              client
                  ->Eval(ScriptText(), keys.ScriptKeys(),
                         {"lookup", node_id, kind_text, value_text}, deadline)
                  .Await();
          if (!reply.ok())
            return reply.status();
          absl::StatusOr<const std::vector<redis::Reply>*> elements =
              ResultElements(*reply);
          if (!elements.ok())
            return elements.status();
          absl::StatusOr<std::string_view> tag = StringAt(**elements, 0);
          if (!tag.ok())
            return tag.status();
          if (*tag == "item")
            return ParseItemReply(**elements, node_id);
          if (*tag == "closed") {
            absl::Status expected = ExpectTag(**elements, "closed", 2);
            if (!expected.ok())
              return expected;
            absl::StatusOr<std::string_view> encoded = StringAt(**elements, 1);
            if (!encoded.ok())
              return encoded.status();
            absl::StatusOr<absl::Status> terminal =
                DecodeStoredStatus(*encoded);
            if (!terminal.ok())
              return terminal.status();
            if (!terminal->ok())
              return *terminal;
            return absl::NotFoundError(
                kind == ReadKind::kSequence
                    ? absl::StrCat("Chunk store closed without seq ", value)
                    : absl::StrCat("Chunk store closed without arrival order ",
                                   value));
          }
          if (*tag != "wait" || (*elements)->size() != 4)
            return ProtocolError("lookup returned an unexpected disposition");
          absl::StatusOr<std::string_view> revision = StringAt(**elements, 1);
          if (!revision.ok())
            return revision.status();
          absl::StatusOr<std::uint64_t> parsed_revision =
              ParseUnsigned<std::uint64_t>(*revision, "metadata revision");
          if (!parsed_revision.ok())
            return parsed_revision.status();
          absl::StatusOr<std::string_view> echoed_kind =
              StringAt(**elements, 2);
          absl::StatusOr<std::string_view> echoed_value =
              StringAt(**elements, 3);
          if (!echoed_kind.ok())
            return echoed_kind.status();
          if (!echoed_value.ok())
            return echoed_value.status();
          if (*echoed_kind != kind_text || *echoed_value != value_text)
            return ProtocolError(
                "lookup wait reply does not match its request");
          if (subscription == nullptr) {
            absl::StatusOr<std::shared_ptr<redis::Subscription>> subscribed =
                client->Subscribe(keys.events, deadline).Await();
            if (!subscribed.ok())
              return subscribed.status();
            subscription = std::move(*subscribed);
            // Re-read after the subscription acknowledgement. A mutation that
            // happened before this listener existed is then visible without
            // depending on a notification it could not have observed.
            continue;
          }
          absl::StatusOr<std::uint64_t> changed =
              subscription->Wait(generation, deadline).Await();
          if (!changed.ok())
            return changed.status();
        }
      });
}

a11::Future<data::NodeFragment> RedisChunkStore::Get(std::uint32_t seq,
                                                     absl::Time deadline) {
  return Read(ReadKind::kSequence, seq, deadline);
}

a11::Future<data::NodeFragment> RedisChunkStore::GetByArrivalOrder(
    std::uint64_t arrival_order, absl::Time deadline) {
  return Read(ReadKind::kArrivalOrder, arrival_order, deadline);
}

a11::Future<std::vector<std::optional<data::NodeFragment>>>
RedisChunkStore::Next(absl::Time deadline, size_t limit) {
  if (limit == 0) {
    return a11::FailedFuture<std::vector<std::optional<data::NodeFragment>>>(
        absl::InvalidArgumentError("limit must be positive"));
  }
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<std::vector<std::optional<data::NodeFragment>>>(
      [client, keys, node_id, deadline, limit]()
          -> absl::StatusOr<std::vector<std::optional<data::NodeFragment>>> {
        std::shared_ptr<redis::Subscription> subscription;
        std::vector<std::optional<data::NodeFragment>> fragments;
        fragments.reserve(std::min(limit, kMaximumLuaNextBatch) + 1);

        while (true) {
          const std::uint64_t generation =
              subscription == nullptr ? 0 : subscription->generation();
          const size_t remaining = limit - fragments.size();
          const size_t page_limit = std::min(remaining, kMaximumLuaNextBatch);
          absl::StatusOr<redis::Reply> reply =
              client
                  ->Eval(ScriptText(), keys.ScriptKeys(),
                         {"next", node_id, std::to_string(page_limit)},
                         deadline)
                  .Await();
          if (!reply.ok()) {
            if (!fragments.empty())
              return fragments;
            return reply.status();
          }
          absl::StatusOr<const std::vector<redis::Reply>*> elements =
              ResultElements(*reply);
          if (!elements.ok())
            return elements.status();
          absl::StatusOr<NextPage> page = ParseNextReply(**elements, node_id);
          if (!page.ok())
            return page.status();
          for (std::optional<data::NodeFragment>& fragment : page->fragments)
            fragments.push_back(std::move(fragment));

          if (page->disposition == NextDisposition::kReady)
            return fragments;
          if (page->disposition == NextDisposition::kEnd) {
            fragments.emplace_back(std::nullopt);
            return fragments;
          }
          if (page->disposition == NextDisposition::kClosed) {
            if (!page->terminal_status.has_value())
              return ProtocolError("closed next reply has no status");
            if (page->terminal_status->ok()) {
              fragments.emplace_back(std::nullopt);
              return fragments;
            }
            if (!fragments.empty())
              return fragments;
            return *page->terminal_status;
          }

          if (subscription == nullptr) {
            absl::StatusOr<std::shared_ptr<redis::Subscription>> subscribed =
                client->Subscribe(keys.events, deadline).Await();
            if (!subscribed.ok()) {
              if (!fragments.empty())
                return fragments;
              return subscribed.status();
            }
            subscription = std::move(*subscribed);
            // Re-run the atomic claim after acknowledgement so a write or
            // close racing subscription setup cannot be missed.
            continue;
          }
          absl::StatusOr<std::uint64_t> changed =
              subscription->Wait(generation, deadline).Await();
          if (!changed.ok()) {
            if (!fragments.empty())
              return fragments;
            return changed.status();
          }
        }
      });
}

a11::Future<std::uint32_t> RedisChunkStore::Put(data::NodeFragment fragment) {
  std::vector<data::NodeFragment> fragments;
  fragments.push_back(std::move(fragment));
  a11::Future<std::vector<std::uint32_t>> batch = PutMany(std::move(fragments));
  a11::Promise<std::uint32_t> promise;
  a11::Future<std::uint32_t> result = promise.future();
  batch.OnReady(
      [promise = std::move(promise)](
          const absl::StatusOr<std::vector<std::uint32_t>>& seqs) mutable {
        if (!seqs.ok()) {
          promise.SetStatus(seqs.status()).IgnoreError();
        } else if (seqs->size() != 1) {
          promise
              .SetStatus(absl::DataLossError(
                  "Redis PutMany did not return exactly one sequence"))
              .IgnoreError();
        } else {
          promise.SetValue(seqs->front()).IgnoreError();
        }
      });
  return result;
}

a11::Future<std::vector<std::uint32_t>> RedisChunkStore::PutMany(
    std::vector<data::NodeFragment> fragments) {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const RedisChunkStoreOptions options = options_;
  const std::string node_id = node_id_;
  return a11::Submit<std::vector<std::uint32_t>>(
      [client, keys, options, node_id,
       fragments = std::move(
           fragments)]() mutable -> absl::StatusOr<std::vector<std::uint32_t>> {
        constexpr size_t kArgumentsPerFragment = 5;
        constexpr size_t kCommandOverhead = 16;
        const size_t maximum_arguments =
            static_cast<size_t>(std::numeric_limits<int>::max());
        if (fragments.size() >
            (maximum_arguments - kCommandOverhead) / kArgumentsPerFragment) {
          return absl::ResourceExhaustedError(
              "Redis PutMany contains too many fragments");
        }

        bool any_explicit = false;
        bool all_explicit = true;
        absl::flat_hash_set<std::uint32_t> explicit_sequences;
        std::vector<std::string> arguments;
        arguments.reserve(3 + fragments.size() * kArgumentsPerFragment);
        arguments.push_back("put");
        arguments.push_back(node_id);
        arguments.push_back(std::to_string(fragments.size()));

        for (const data::NodeFragment& fragment : fragments) {
          absl::Status validation = fragment.Validate();
          if (!validation.ok())
            return validation;
          any_explicit = any_explicit || fragment.seq.has_value();
          all_explicit = all_explicit && fragment.seq.has_value();
          if (fragment.seq.has_value() &&
              !explicit_sequences.insert(*fragment.seq).second) {
            return absl::InvalidArgumentError(absl::StrCat(
                "Explicit seq ", *fragment.seq, " occurs more than once"));
          }
          if (!std::holds_alternative<data::Chunk>(fragment.data)) {
            return absl::UnimplementedError(
                "RedisChunkStore supports Chunk payloads, not NodeRef");
          }
        }
        if (any_explicit != all_explicit) {
          return absl::InvalidArgumentError(
              "Sequence numbers must be set on every fragment or none");
        }

        for (const data::NodeFragment& fragment : fragments) {
          const data::Chunk& chunk = std::get<data::Chunk>(fragment.data);
          absl::StatusOr<data::Bytes> encoded = chunk.ToMsgpack();
          if (!encoded.ok())
            return encoded.status();
          data::Chunk tombstone{
              .metadata = chunk.metadata,
              .ref = std::string(kTombstoneReference),
              .data = {},
          };
          absl::StatusOr<data::Bytes> encoded_tombstone = tombstone.ToMsgpack();
          if (!encoded_tombstone.ok())
            return encoded_tombstone.status();

          arguments.push_back(fragment.seq.has_value()
                                  ? std::to_string(*fragment.seq)
                                  : std::string());
          arguments.push_back(fragment.continued ? "0" : "1");
          arguments.push_back(chunk.data.size() > options.inline_data_threshold
                                  ? "redis"
                                  : "inline");
          arguments.push_back(std::move(*encoded));
          arguments.push_back(std::move(*encoded_tombstone));
        }

        absl::StatusOr<redis::Reply> reply =
            client->Eval(ScriptText(), keys.ScriptKeys(), std::move(arguments))
                .Await();
        if (!reply.ok())
          return reply.status();
        absl::StatusOr<const std::vector<redis::Reply>*> elements =
            ResultElements(*reply);
        if (!elements.ok())
          return elements.status();
        if ((*elements)->size() != fragments.size() + 1)
          return ProtocolError("put reply has the wrong sequence count");
        absl::StatusOr<std::string_view> tag = StringAt(**elements, 0);
        if (!tag.ok())
          return tag.status();
        if (*tag != "ok")
          return ProtocolError(absl::StrCat("expected put reply, got ", *tag));
        std::vector<std::uint32_t> assigned;
        assigned.reserve(fragments.size());
        for (size_t index = 1; index < (*elements)->size(); ++index) {
          absl::StatusOr<std::string_view> value = StringAt(**elements, index);
          if (!value.ok())
            return value.status();
          absl::StatusOr<std::uint32_t> seq =
              ParseUnsigned<std::uint32_t>(*value, "assigned sequence");
          if (!seq.ok())
            return seq.status();
          assigned.push_back(*seq);
        }
        return assigned;
      });
}

a11::Future<data::NodeFragment> RedisChunkStore::ClearData(std::uint32_t seq) {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<data::NodeFragment>(
      [client, keys, node_id, seq]() -> absl::StatusOr<data::NodeFragment> {
        absl::StatusOr<redis::Reply> reply =
            client
                ->Eval(ScriptText(), keys.ScriptKeys(),
                       {"clear", node_id, std::to_string(seq)})
                .Await();
        if (!reply.ok())
          return reply.status();
        absl::StatusOr<const std::vector<redis::Reply>*> elements =
            ResultElements(*reply);
        if (!elements.ok())
          return elements.status();
        return ParseItemReply(**elements, node_id);
      });
}

a11::Future<std::uint32_t> RedisChunkStore::GetSeqForArrivalOrder(
    std::uint64_t arrival_order) {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<std::uint32_t>(
      [client, keys, node_id,
       arrival_order]() -> absl::StatusOr<std::uint32_t> {
        absl::StatusOr<redis::Reply> reply =
            client
                ->Eval(ScriptText(), keys.ScriptKeys(),
                       {"arrival_seq", node_id, std::to_string(arrival_order)})
                .Await();
        if (!reply.ok())
          return reply.status();
        absl::StatusOr<const std::vector<redis::Reply>*> elements =
            ResultElements(*reply);
        if (!elements.ok())
          return elements.status();
        absl::Status expected = ExpectTag(**elements, "value", 2);
        if (!expected.ok())
          return expected;
        absl::StatusOr<std::string_view> value = StringAt(**elements, 1);
        if (!value.ok())
          return value.status();
        return ParseUnsigned<std::uint32_t>(*value, "arrival sequence");
      });
}

a11::Future<std::optional<std::uint32_t>> RedisChunkStore::GetFinalSeq() {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<std::optional<std::uint32_t>>(
      [client, keys,
       node_id]() -> absl::StatusOr<std::optional<std::uint32_t>> {
        absl::StatusOr<redis::Reply> reply =
            client->Eval(ScriptText(), keys.ScriptKeys(), {"final", node_id})
                .Await();
        if (!reply.ok())
          return reply.status();
        absl::StatusOr<const std::vector<redis::Reply>*> elements =
            ResultElements(*reply);
        if (!elements.ok())
          return elements.status();
        absl::Status expected = ExpectTag(**elements, "optional", 2);
        if (!expected.ok())
          return expected;
        absl::StatusOr<std::string_view> value = StringAt(**elements, 1);
        if (!value.ok())
          return value.status();
        return ParseOptionalSequence(*value, "final sequence");
      });
}

a11::Future<absl::Status> RedisChunkStore::CloseWritesWithStatus(
    absl::Status status, bool return_status_if_already_closed) {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<absl::Status>([client, keys, node_id,
                                    status = std::move(status),
                                    return_status_if_already_closed]() mutable
                                       -> absl::StatusOr<absl::Status> {
    absl::StatusOr<std::string> encoded = data::PackStatus(status);
    if (!encoded.ok()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(encoded.status());
      return result;
    }
    absl::StatusOr<redis::Reply> reply =
        client
            ->Eval(ScriptText(), keys.ScriptKeys(),
                   {"close", node_id, std::move(*encoded),
                    return_status_if_already_closed ? "1" : "0"})
            .Await();
    if (!reply.ok()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(reply.status());
      return result;
    }
    absl::StatusOr<const std::vector<redis::Reply>*> elements =
        ResultElements(*reply);
    if (!elements.ok()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(elements.status());
      return result;
    }
    absl::Status expected = ExpectTag(**elements, "status", 2);
    if (!expected.ok()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(std::move(expected));
      return result;
    }
    absl::StatusOr<std::string_view> stored = StringAt(**elements, 1);
    if (!stored.ok()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(stored.status());
      return result;
    }
    return DecodeStoredStatus(*stored);
  });
}

a11::Future<size_t> RedisChunkStore::Size() {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<size_t>(
      [client, keys, node_id]() -> absl::StatusOr<size_t> {
        absl::StatusOr<redis::Reply> reply =
            client->Eval(ScriptText(), keys.ScriptKeys(), {"size", node_id})
                .Await();
        if (!reply.ok())
          return reply.status();
        absl::StatusOr<const std::vector<redis::Reply>*> elements =
            ResultElements(*reply);
        if (!elements.ok())
          return elements.status();
        absl::Status expected = ExpectTag(**elements, "value", 2);
        if (!expected.ok())
          return expected;
        absl::StatusOr<std::string_view> value = StringAt(**elements, 1);
        if (!value.ok())
          return value.status();
        return ParseUnsigned<size_t>(*value, "chunk store size");
      });
}

absl::StatusOr<std::string> RedisChunkStore::GetId() const {
  return node_id_;
}

a11::Task RedisChunkStore::Initialize() {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<a11::Unit>([client, keys,
                                 node_id]() -> absl::StatusOr<a11::Unit> {
    absl::StatusOr<redis::Reply> reply =
        client->Eval(ScriptText(), keys.ScriptKeys(), {"initialize", node_id})
            .Await();
    if (!reply.ok())
      return reply.status();
    absl::StatusOr<const std::vector<redis::Reply>*> elements =
        ResultElements(*reply);
    if (!elements.ok())
      return elements.status();
    absl::Status expected = ExpectTag(**elements, "ok", 1);
    if (!expected.ok())
      return expected;
    return a11::Unit{};
  });
}

a11::Future<RedisChunkStoreMetadata> RedisChunkStore::GetMetadata() {
  const std::shared_ptr<redis::Client> client = client_;
  const RedisChunkStoreKeys keys = keys_;
  const std::string node_id = node_id_;
  return a11::Submit<RedisChunkStoreMetadata>(
      [client, keys, node_id]() -> absl::StatusOr<RedisChunkStoreMetadata> {
        absl::StatusOr<redis::Reply> reply =
            client->Eval(ScriptText(), keys.ScriptKeys(), {"metadata", node_id})
                .Await();
        if (!reply.ok())
          return reply.status();
        absl::StatusOr<const std::vector<redis::Reply>*> elements =
            ResultElements(*reply);
        if (!elements.ok())
          return elements.status();
        absl::Status expected = ExpectTag(**elements, "metadata", 10);
        if (!expected.ok())
          return expected;

        absl::StatusOr<std::string_view> id = StringAt(**elements, 1);
        absl::StatusOr<std::string_view> closed_text = StringAt(**elements, 2);
        absl::StatusOr<std::string_view> status_text = StringAt(**elements, 3);
        absl::StatusOr<std::string_view> final_text = StringAt(**elements, 4);
        absl::StatusOr<std::string_view> size_text = StringAt(**elements, 5);
        absl::StatusOr<std::string_view> put_text = StringAt(**elements, 6);
        absl::StatusOr<std::string_view> cursor_text = StringAt(**elements, 7);
        absl::StatusOr<std::string_view> max_text = StringAt(**elements, 8);
        absl::StatusOr<std::string_view> revision_text =
            StringAt(**elements, 9);
        if (!id.ok())
          return id.status();
        if (!closed_text.ok())
          return closed_text.status();
        if (!status_text.ok())
          return status_text.status();
        if (!final_text.ok())
          return final_text.status();
        if (!size_text.ok())
          return size_text.status();
        if (!put_text.ok())
          return put_text.status();
        if (!cursor_text.ok())
          return cursor_text.status();
        if (!max_text.ok())
          return max_text.status();
        if (!revision_text.ok())
          return revision_text.status();
        if (*id != node_id)
          return ProtocolError("metadata belongs to a different node");
        if (*closed_text != "0" && *closed_text != "1")
          return ProtocolError("metadata closed flag is not boolean");

        absl::StatusOr<std::optional<std::uint32_t>> final_seq =
            ParseOptionalSequence(*final_text, "final sequence");
        if (!final_seq.ok())
          return final_seq.status();
        absl::StatusOr<size_t> size =
            ParseUnsigned<size_t>(*size_text, "chunk store size");
        if (!size.ok())
          return size.status();
        absl::StatusOr<std::uint64_t> put_count =
            ParseUnsigned<std::uint64_t>(*put_text, "put count");
        if (!put_count.ok())
          return put_count.status();
        absl::StatusOr<std::uint64_t> next_cursor =
            ParseUnsigned<std::uint64_t>(*cursor_text, "next cursor");
        if (!next_cursor.ok())
          return next_cursor.status();
        absl::StatusOr<std::optional<std::uint32_t>> max_seq =
            ParseOptionalSequence(*max_text, "maximum sequence");
        if (!max_seq.ok())
          return max_seq.status();
        absl::StatusOr<std::uint64_t> revision =
            ParseUnsigned<std::uint64_t>(*revision_text, "metadata revision");
        if (!revision.ok())
          return revision.status();

        std::optional<absl::Status> terminal;
        const bool closed = *closed_text == "1";
        if (closed) {
          absl::StatusOr<absl::Status> decoded =
              DecodeStoredStatus(*status_text);
          if (!decoded.ok())
            return decoded.status();
          terminal = std::move(*decoded);
        } else if (!status_text->empty()) {
          return ProtocolError("open metadata unexpectedly contains a status");
        }

        return RedisChunkStoreMetadata{
            .id = std::string(*id),
            .closed = closed,
            .status = std::move(terminal),
            .final_seq = *final_seq,
            .size = *size,
            .total_chunks_put = *put_count,
            .next_cursor = *next_cursor,
            .max_seq = *max_seq,
            .revision = *revision,
        };
      });
}

}  // namespace a11::stores
