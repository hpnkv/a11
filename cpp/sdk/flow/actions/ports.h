// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Output ports, resolved once and closed exactly once.
 *
 * Every action in the Flow standard library declares more output ports than
 * most callers read, for the reason @c make_http_request does: a port per
 * concern is what lets a caller act on the status while the body is still
 * arriving. That shape has one obligation attached to it -- **every declared
 * port has to be ended, whether or not it was used**, because a reader waiting
 * on a port nothing was ever written to waits forever -- and one optimisation
 * that follows from it: a port named in @c options.omit is closed up front, so
 * asking for three of eight ports does not mean draining the other five.
 *
 * Getting that right per action is a hang when it goes wrong rather than an
 * error, so it is written once here instead. a11::sdk::flow::OutputPorts owns
 * the ports and ends whatever is still open however the run finished;
 * a11::sdk::flow::Sink is a handle to one of them, resolved outside the loop
 * that writes to it.
 *
 * ### Why Sink exists
 *
 * A streaming action writes thousands of chunks to the same port. Looking the
 * port up by name per chunk would put a string hash on the hot path for no
 * reason, so a handler resolves each port once:
 *
 * @code
 *   ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OutputPorts::Open(action, omit));
 *   const Sink bytes = outputs["bytes"];
 *   while (...) {
 *     ABSL_RETURN_IF_ERROR(bytes.PutBytes(std::move(piece)));
 *   }
 *   return outputs.Finish();
 * @endcode
 *
 * An omitted port yields an absent Sink whose writes are cheap no-ops, so the
 * loop above needs no test of its own -- and where producing the value is
 * itself expensive, Sink::present() says whether to bother.
 *
 * Open() takes the ports from the action's own schema rather than from a list
 * the handler repeats, because the two drifting apart is exactly the bug this
 * class exists to make impossible.
 *
 * ### What a port's chunks look like, and why the encoding is a choice
 *
 * A structured value goes out as JSON by default and as MessagePack when the
 * caller says `encoding: "msgpack"`. That is not only a speed setting, and the
 * reason is worth stating plainly because it decides whether some perfectly
 * ordinary files can be read at all:
 *
 * **JSON is defined over text, and the filesystem is not.** A path, a line of a
 * file, and a program's output are byte strings; nothing stops one of them
 * holding a byte sequence that is not valid UTF-8, and such a value has no JSON
 * spelling. So `read_file`'s `lines` and `list_directory`'s `entries` can carry
 * something JSON cannot represent, and the honest options are to fail, to
 * corrupt it silently, or to offer an encoding that can hold it. This library
 * does the first and the third: a value that will not fit its encoding is an
 * `invalid_argument` naming both ways out -- `encoding: "msgpack"`, which holds
 * arbitrary bytes, or the `bytes` port, which is exact by construction.
 *
 * Silently replacing the offending bytes was the option not taken. A flow that
 * copies a file should not quietly change it.
 *
 * Every conversion here goes through a11::DumpJson and a11::PackMsgpack rather
 * than through nlohmann directly, because nlohmann reports these failures by
 * throwing and this library is compiled without exceptions -- where its
 * fallback is `std::abort()`. A non-UTF-8 filename must be a status, not a
 * crash, and routing it through the codec is what makes that true.
 *
 * BytesChunk() needs none of this: a file really is bytes, and an
 * @c application/octet-stream port carries them untouched in either encoding.
 * None of the three registers a type, because a directory entry really is a map
 * -- which keeps the cross-language tag table out of the standard library.
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
#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/data/types.h"
#include "sdk/flow/actions/options.h"

namespace a11::nodes {
class AsyncNode;
}  // namespace a11::nodes

namespace a11::sdk::flow {

/** @brief Mimetype for a port carrying opaque bytes. */
inline constexpr std::string_view kOctetStream = "application/octet-stream";
/** @brief Default size of a streamed byte chunk: one read, one value. */
inline constexpr std::size_t kDefaultChunkBytes = 64 * 1024;

/**
 * @brief How a structured value is written.
 *
 * Chosen per action by `options.encoding`, and applied to every port that
 * carries a value rather than bytes. MessagePack is not merely the faster of
 * the two: it is the one that can hold a byte string, which is what a path and
 * a line of a file actually are.
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
 * For the handful of places that have no encoding to hand. Prefer ValueChunk().
 */
absl::StatusOr<data::Chunk> JsonChunk(const nlohmann::json& value);

/**
 * @brief The bytes a chunk means, for a port that carries bytes.
 *
 * The reverse of what ReadJsonInput does for a value port, and needed for the
 * same reason: a writer chose an encoding and a byte port should honour what
 * was *meant* rather than what the encoding happens to spell.
 *
 * The case that forces it: Flow's runtime encodes a string value as JSON when
 * it writes it to a port, whatever the port's declared type -- so a flow that
 * says `content: greeting | text` hands `write_stdout` the seven bytes
 * `"hello"` **with the quotes**, and a program that printed those would be
 * wrong in a way its author could not see from the source. So:
 *
 *   * `application/octet-stream`, `text/plain`, anything else -- the payload,
 *     untouched. Bytes are bytes.
 *   * `application/json` holding a string -- the string's characters. This is
 *     the case above.
 *   * `application/json` holding anything else -- its JSON text, which is the
 *     only reading of "write this object to a file" that means anything.
 *   * `application/x-msgpack` holding a string or a binary -- those bytes;
 *     anything else, the packed bytes as they arrived, since a caller writing
 *     packed records means the packed records.
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
/** @brief Reads a unary input port as text, accepting a JSON string or bytes. */
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
 * Copyable and cheap. Every write is a no-op on an absent handle -- the port
 * was omitted by the caller, or already closed -- so a handler writes
 * unconditionally and lets the caller's `omit` decide what that costs.
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
   * @param final Whether this is the port's last value. Establishes the
   *        logical end of the stream, which is why a second final write to the
   *        same port is rejected by the store rather than guessed at.
   */
  absl::Status Put(data::Chunk chunk, bool final = false) const;
  /** @brief Writes one structured value, in this run's encoding. */
  absl::Status PutValue(const nlohmann::json& value, bool final = false) const;
  /**
   * @brief Writes one text value.
   *
   * Text that is not valid UTF-8 is an error under JSON and ordinary under
   * MessagePack, where it goes out as a byte string. See the file comment: this
   * is the case a path or a line of a file can genuinely be in.
   */
  absl::Status PutText(std::string_view text, bool final = false) const;
  /** @brief Writes one opaque-byte value, untouched by the encoding. */
  absl::Status PutBytes(std::string bytes, bool final = false) const;

  /** @brief Writes a unary port's only value and ends it. */
  absl::Status PutOnly(const nlohmann::json& value) const;
  /** @brief Writes a unary port's only text value and ends it. */
  absl::Status PutOnlyText(std::string_view text) const;

  /**
   * @brief Ends this port now, rather than at OutputPorts::Finish().
   *
   * Worth doing where a port is finished long before the action is -- an
   * `info` or `columns` port whose reader is waiting to get on with something
   * -- and harmless otherwise. Idempotent; the handle goes absent afterwards.
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
   * @brief Resolves every output port of @p action, closing those in @p omitted.
   * @param action The running action, whose schema names the ports.
   * @param omitted Port names the caller asked not to be written. A name that
   *        is not a port is reported rather than ignored: a caller that
   *        misspelled one would otherwise wait on a port nobody omitted.
   * @param encoding How structured values are written.
   */
  static absl::StatusOr<OutputPorts> Open(
      const std::shared_ptr<actions::Action>& action,
      const std::vector<std::string>& omitted = {},
      Encoding encoding = Encoding::kJson);

  /**
   * @brief Returns a handle to port @p name.
   *
   * A name that is not one of this action's ports is a bug in the handler
   * rather than a caller's mistake, so it is recorded and surfaced by Finish()
   * instead of being silently ignored -- and instead of aborting a process
   * that has a perfectly good way to report an error.
   */
  Sink operator[](std::string_view name);

  /** @brief The names of the ports this action declares, for diagnostics. */
  [[nodiscard]] std::vector<std::string> Names() const;

  /** @brief Ends every port still open, and reports the first failure. */
  absl::Status Finish();

  /**
   * @brief Ends every port still open with @p reason.
   *
   * A reader of a stream that stopped early needs to know it stopped early;
   * closing normally would tell it the file simply ended here. Called for a run
   * that failed, so that a half-written body is a failure at the reader rather
   * than a short one.
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
 * What every handler in this library calls: reads `omit` and `encoding` from one
 * place, so neither can be honoured by some actions and forgotten by others.
 */
absl::StatusOr<OutputPorts> OpenOutputs(
    const std::shared_ptr<actions::Action>& action, const Options& options);

/** @brief Reads `options.encoding`. */
absl::StatusOr<Encoding> EncodingOption(const Options& options);

/**
 * @brief The `options` sentence every schema in this library ends with.
 *
 * One string, so the wording of the two settings every action shares cannot
 * drift between thirty descriptions.
 */
std::string_view SharedOptionsHelp();

/**
 * @brief Declares one port, the way every schema in this library wants it.
 *
 * A free function rather than aggregate initialisation at each site because
 * the flags read as noise in a list of thirty ports, and because `unary` is
 * the one people get backwards.
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

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_PORTS_H_
