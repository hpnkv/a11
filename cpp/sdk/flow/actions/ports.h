/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief Output ports, resolved once and closed exactly once.
 *
 * OutputPorts resolves the action schema's ports and closes omitted ports at
 * once. Finish() closes every remaining port, including unused ports, so
 * readers can observe the end of each stream. Resolve a Sink once per port:
 *
 * @code
 *   ABSL_ASSIGN_OR_RETURN(auto outputs, OutputPorts::Open(action, omit));
 *   const Sink bytes = outputs["bytes"];
 *   while (...) {
 *     ABSL_RETURN_IF_ERROR(bytes.PutBytes(std::move(piece)));
 *   }
 *   return outputs.Finish();
 * @endcode
 *
 * Writes to an omitted Sink are no-ops. Use Sink::present() to skip expensive
 * value production. Structured values use JSON by default or MessagePack when
 * `encoding: "msgpack"` is set. Invalid UTF-8 produces `invalid_argument` for
 * JSON; use MessagePack or the raw `bytes` port for arbitrary bytes.
 * ValueChunk() uses a11::DumpJson and a11::PackMsgpack to report conversion
 * errors as statuses. BytesChunk() carries bytes unchanged in either encoding.
 */

#ifndef A11_SDK_FLOW_ACTIONS_PORTS_H_
#define A11_SDK_FLOW_ACTIONS_PORTS_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "sdk/flow/actions/options.h"

namespace a11::nodes {
class AsyncNode;
}  // namespace a11::nodes

namespace a11::sdk::flow {

/** @brief Mimetype for a port carrying opaque bytes. */
inline constexpr std::string_view kOctetStream = "application/octet-stream";
/** @brief Mimetype for a port carrying UTF-8 text. */
inline constexpr std::string_view kTextPlain = data::kTextMimetype;
/** @brief Default size of a streamed byte chunk: one read, one value. */
inline constexpr std::size_t kDefaultChunkBytes = 64 * 1024;

/**
 * @brief How a structured value is written.
 *
 * Chosen per action by `options.encoding`. Structured values use JSON by
 * default; UTF-8 strings use text/plain. Selecting MessagePack applies it to
 * both, including byte strings that are not valid UTF-8.
 */
enum class Encoding {
  kJson,     ///< `application/json`. The default, and readable by everything.
  kMsgpack,  ///< `application/x-msgpack`. Holds arbitrary bytes; no re-encode
             ///< downstream of a `| packb`.
};

/** @brief Reads `options.encoding`, defaulting to JSON. */
absl::StatusOr<Encoding> EncodingFromName(std::string_view name);

/** @brief A chunk carrying @p value in @p encoding, or why it will not fit. */
absl::StatusOr<data::Chunk> ValueChunk(const nlohmann::json& value,
                                       Encoding encoding);
/** @brief A chunk carrying opaque bytes, untouched by either encoding. */
data::Chunk BytesChunk(std::string bytes);

/**
 * @brief A chunk carrying @p value as JSON.
 *
 * Use when no encoding option is available. Other callers use ValueChunk().
 */
absl::StatusOr<data::Chunk> JsonChunk(const nlohmann::json& value);

/**
 * @brief The bytes a chunk means, for a port that carries bytes.
 *
 * Flow may write a string to a byte port as a JSON string. For example,
 * `content: greeting | text` supplies `"hello"`; this function returns `hello`.
 *
 *   * `application/octet-stream`, `text/plain`, anything else -- raw payload.
 *   * `application/json` holding a string -- the string's characters.
 *   * `application/json` holding anything else -- its JSON text.
 *   * `application/x-msgpack` holding a string or binary -- decoded bytes;
 *     anything else -- the original packed bytes.
 */
absl::StatusOr<std::string> BytesOfChunk(const data::Chunk& chunk);

/**
 * @brief Reads a unary input port as JSON.
 *
 * A closed or null port yields nullopt, so an optional input needs no separate
 * test. A payload that is not JSON is taken as a JSON string, which is what
 * makes `path: "/tmp/x"` work whether the writer sent text or JSON.
 */
absl::StatusOr<std::optional<nlohmann::json>> ReadJsonInput(
    const std::shared_ptr<nodes::AsyncNode>& node);
/**
 * @brief Reads a unary input port as text, accepting a JSON string or bytes.
 */
absl::StatusOr<std::optional<std::string>> ReadTextInput(
    const std::shared_ptr<nodes::AsyncNode>& node);
/** @brief Reads a unary input port of @p action as JSON. */
absl::StatusOr<std::optional<nlohmann::json>> ReadJsonInput(
    const std::shared_ptr<actions::Action>& action, std::string_view port);
/** @brief Reads a unary input port of @p action as text. */
absl::StatusOr<std::optional<std::string>> ReadTextInput(
    const std::shared_ptr<actions::Action>& action, std::string_view port);
/**
 * @brief Reads a required unary text input.
 * @return InvalidArgument naming @p port when it carried nothing.
 */
absl::StatusOr<std::string> ReadRequiredTextInput(
    const std::shared_ptr<actions::Action>& action, std::string_view port);

class OutputPorts;

/**
 * @brief A handle to one output port, or to none.
 *
 * Copyable and cheap. Writes to an omitted or closed port are no-ops.
 *
 * A Sink does not own its port: it stays valid for as long as the OutputPorts
 * it came from, and OutputPorts may be moved without invalidating it.
 */
class Sink {
 public:
  Sink() = default;

  /** @brief Whether there is a port behind this handle. */
  [[nodiscard]] bool present() const;

  /**
   * @brief Writes one chunk.
   * @param chunk The value to write.
   * @param final Whether this ends the stream. A second final write fails.
   */
  absl::Status Put(data::Chunk chunk, bool final = false) const;
  /** @brief Writes one structured value, in this run's encoding. */
  absl::Status PutValue(const nlohmann::json& value, bool final = false) const;
  /**
   * @brief Writes one text value.
   *
   * Text uses text/plain by default. MessagePack remains available when the
   * caller selects it explicitly, including for byte strings that are not
   * valid UTF-8.
   */
  absl::Status PutText(std::string_view text, bool final = false) const;
  /** @brief Writes one opaque-byte value, untouched by the encoding. */
  absl::Status PutBytes(std::string bytes, bool final = false) const;

  /** @brief Writes a unary port's only value and ends it. */
  absl::Status PutOnly(const nlohmann::json& value) const;
  /** @brief Writes a unary port's only text value and ends it. */
  absl::Status PutOnlyText(std::string_view text) const;

  /**
   * @brief Ends this port before OutputPorts::Finish().
   *
   * Use when a reader needs a completed port while the action continues.
   * Idempotent; the handle becomes absent afterwards.
   */
  absl::Status Close() const;

 private:
  friend class OutputPorts;

  /// One port's state. Held by stable pointer so a Sink outlives any growth or
  /// move of the owning OutputPorts.
  struct Entry {
    std::string name;
    std::shared_ptr<nodes::AsyncNode> node;
    Encoding encoding = Encoding::kJson;
    bool final = false;
    bool closed = false;
  };

  explicit Sink(Entry* entry) : entry_(entry) {}

  Entry* entry_ = nullptr;
};

/**
 * @brief The output ports of one action run: opened once, ended exactly once.
 *
 * Open() resolves every port the action's schema declares, closing the ones
 * the caller omitted immediately. Finish() ends the rest, and has to be
 * reached however the run finished -- a failed run still has to unblock
 * whoever was reading it.
 */
class OutputPorts {
 public:
  OutputPorts() = default;
  OutputPorts(const OutputPorts&) = delete;
  OutputPorts& operator=(const OutputPorts&) = delete;
  OutputPorts(OutputPorts&&) = default;
  OutputPorts& operator=(OutputPorts&&) = default;

  /**
   * @brief Resolves every output port of @p action, closing those in @p
   * omitted.
   * @param action The running action, whose schema names the ports.
   * @param omitted Port names to close immediately. Unknown names are errors.
   * @param encoding How structured values are written.
   */
  static absl::StatusOr<OutputPorts> Open(
      const std::shared_ptr<actions::Action>& action,
      const std::vector<std::string>& omitted = {},
      Encoding encoding = Encoding::kJson);

  /**
   * @brief Returns a handle to port @p name.
   *
   * Unknown names are recorded as handler errors and reported by Finish().
   */
  Sink operator[](std::string_view name);

  /** @brief The names of the ports this action declares, for diagnostics. */
  [[nodiscard]] std::vector<std::string> Names() const;

  /** @brief Ends every port still open, and reports the first failure. */
  absl::Status Finish();

  /**
   * @brief Ends every port still open with @p reason.
   *
   * Readers receive @p reason when an action fails after writing part of a
   * stream.
   */
  absl::Status Abort(const absl::Status& reason);

 private:
  friend class Sink;

  static absl::Status CloseEntry(Sink::Entry& entry);

  std::vector<std::unique_ptr<Sink::Entry>> entries_;
  absl::Status misuse_ = absl::OkStatus();
};

/**
 * @brief Opens an action's outputs from its `options`, honouring both settings.
 *
 * Reads `omit` and `encoding` from @p options for the action's output ports.
 */
absl::StatusOr<OutputPorts> OpenOutputs(
    const std::shared_ptr<actions::Action>& action, const Options& options);

/** @brief Reads `options.encoding`. */
absl::StatusOr<Encoding> EncodingOption(const Options& options);

/**
 * @brief The `options` sentence every schema in this library ends with.
 *
 * Provides common help text for `omit` and `encoding`.
 */
std::string_view SharedOptionsHelp();

/**
 * @brief Declares one port, the way every schema in this library wants it.
 *
 * Names the port flags explicitly in action schemas.
 */
actions::ActionPortSchema Port(std::string_view name, std::string_view type,
                               std::string_view description, bool required,
                               bool unary);

/** @brief The type name for a port carrying arbitrary JSON. */
std::string JsonType();

/**
 * @brief Declares the deadline header every action in this library honours.
 *
 * What honouring it means differs by shape and each schema says so: a source
 * (`ticker`, `watch_path`) stops gracefully, and an action with one answer to
 * give (`read_file`) fails with `deadline_exceeded` rather than hand back half
 * of it. See a11::sdk::flow::StopSignal.
 */
void AddDeadlineHeader(actions::ActionSchema& schema, std::string_view effect);

/// Splits a byte stream into lines across chunk boundaries, for a `*_lines`
/// port. Holds the tail of a line that has not ended yet.
class LineSplitter {
 public:
  explicit LineSplitter(Sink sink) : sink_(sink) {}

  absl::Status Feed(std::string_view piece) {
    if (!sink_.present()) {
      return absl::OkStatus();
    }
    pending_.append(piece);
    std::size_t start = 0;
    while (true) {
      const std::size_t newline = pending_.find('\n', start);
      if (newline == std::string::npos) {
        break;
      }
      ABSL_RETURN_IF_ERROR(
          Emit(std::string_view(pending_.data() + start, newline - start)));
      start = newline + 1;
    }
    pending_.erase(0, start);
    return absl::OkStatus();
  }

  /// A last line with no newline after it is still a line.
  ///
  /// @param final Ends the port on the last line before the action finishes.
  absl::Status Flush(bool final = false) {
    if (!sink_.present() || pending_.empty()) {
      return absl::OkStatus();
    }
    const absl::Status written = Emit(pending_, final);
    pending_.clear();
    return written;
  }

 private:
  absl::Status Emit(std::string_view line, bool final = false) {
    // A file or a pipe written on Windows is still lines.
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    return sink_.PutText(line, final);
  }

  Sink sink_;
  std::string pending_;
};

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_PORTS_H_
