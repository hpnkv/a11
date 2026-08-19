// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Schemas describing an action's typed interface and its settings.
 *
 * An a11::actions::ActionSchema is the declarative description of an action:
 * its name and description, its typed input and output ports
 * (a11::actions::ActionPortSchema) and its declared headers
 * (a11::actions::ActionHeaderSchema). a11::actions::ActionSettings captures
 * per-action runtime behaviour (stream binding and post-run cleanup). This
 * header also defines the reserved names/mimetypes A11 uses to carry an
 * action's status on the wire, and helpers to move a status through a
 * ::a11::data::Chunk.
 */

#ifndef A11_ACTIONS_SCHEMA_H_
#define A11_ACTIONS_SCHEMA_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::actions {

/** @brief Mimetype marking a chunk that carries an action status. */
inline constexpr std::string_view kActionStatusMimetype = data::kStatusMimetype;
/** @brief Reserved output port name carrying the action's completion status. */
inline constexpr std::string_view kActionStatusOutput = "__status__";
/** @brief Reserved output port name carrying the remote dispatch status. */
inline constexpr std::string_view kActionDispatchStatusOutput =
    "__dispatch_status__";
/**
 * @brief Reserved output port name carrying the action's log.
 *
 * Every action has one, declared by nobody: it is not in the schema, so it does
 * not appear in an ::a11::data::ActionMessage, in a tool definition, or in the
 * flow catalogue, and a handler that never logs never materialises it. Written
 * through Action::Log, closed with the action's other outputs. See
 * @c a11/actions/log.h.
 */
inline constexpr std::string_view kActionLogOutput = "__log__";
/** @brief Reserved action name used to signal cancellation to a peer. */
inline constexpr std::string_view kCancelActionName = "__cancel__";
/** @brief Header naming the action targeted by a cancel request. */
inline constexpr std::string_view kCancelActionHeader = "__action";
/** @brief Prefix reserved for A11's framework headers. */
inline constexpr std::string_view kActionHeaderPrefix = "x-a11-";

/**
 * @brief Schema for a single input or output port of an action.
 *
 * Describes the port's @c name and payload @c type, whether it is @c required
 * and @c unary (a single value rather than a stream), an optional
 * @c description, and input @c autofills. Before applying autofills the
 * runtime requires that input to be writable and empty; existing data causes
 * the action to fail rather than overriding a receiver-owned default.
 */
struct ActionPortSchema {
  std::string name;         ///< Port name (unique within its direction).
  std::string type;         ///< Payload type name.
  std::string description;  ///< Human-readable description.
  bool required = false;    ///< Whether the port must be supplied.
  bool unary = false;       ///< Whether the port carries a single value.
  /// Input-default fragments; an autofilled input must otherwise be empty.
  std::vector<std::optional<data::NodeFragment>> autofills{};

  /// Opaque, owning language-binding type handle (e.g. a Python type object).
  /// The a11::actions layer stays language-agnostic: it only holds the handle
  /// and never dereferences it. Ownership is real -- the binding that populates
  /// it installs a custom deleter (which reacquires its runtime's lock as
  /// needed), so the referent is kept alive for exactly as long as any copy of
  /// this schema, instead of being a borrowed pointer that can dangle once the
  /// caller drops its own reference. Empty when the port has no bound type.
  std::shared_ptr<void> typeinfo{nullptr};

  /** @brief Validates the port schema. */
  absl::Status Validate() const;
  friend bool operator==(const ActionPortSchema&,
                         const ActionPortSchema&) = default;
};

/**
 * @brief Schema for a single header an action accepts.
 *
 * Declares the header @c name, a @c description, and an optional
 * @c default_value applied when the caller omits it.
 */
struct ActionHeaderSchema {
  std::string name;         ///< Header name.
  std::string description;  ///< Human-readable description.
  std::optional<data::Bytes> default_value{
      std::nullopt};  ///< Value used when omitted.

  /** @brief Validates the header schema. */
  absl::Status Validate() const;
  friend bool operator==(const ActionHeaderSchema&,
                         const ActionHeaderSchema&) = default;
};

/**
 * @brief The full typed interface of an action.
 *
 * Combines the action's @c name and @c description with its @c inputs,
 * @c outputs and @c headers schemas. @c output_to_json_field optionally maps
 * output ports onto fields of a single JSON result document (use
 * ::kWholeJson to map an output to the whole document).
 */
struct ActionSchema {
  static constexpr std::string_view kWholeJson = "$";  ///< Whole-JSON target.

  std::string name;         ///< Action name.
  std::string description;  ///< Human-readable description.
  absl::flat_hash_map<std::string, ActionPortSchema> inputs;  ///< Input ports.
  absl::flat_hash_map<std::string, ActionPortSchema>
      outputs;  ///< Output ports.
  absl::flat_hash_map<std::string, ActionHeaderSchema> headers;  ///< Headers.
  /// Maps output port names to JSON result fields.
  absl::flat_hash_map<std::string, std::string> output_to_json_field;

  /** @brief Validates the schema and all of its ports and headers. */
  absl::Status Validate() const;
  /**
   * @brief Maps an output port onto a JSON result field.
   * @param output_name Output port to map.
   * @param field_name Target field; empty maps to a field named like the port.
   */
  absl::Status MapOutputToJson(std::string output_name,
                               std::string field_name = {});

  friend bool operator==(const ActionSchema&, const ActionSchema&) = default;
};

/**
 * @brief Per-action runtime settings for stream binding and cleanup.
 *
 * Controls whether input/output ports have their streams bound by default and
 * whether the action clears its inputs/outputs once a run completes. Unset
 * optionals defer to the framework default.
 */
struct ActionSettings {
  /// Bind input port streams by default when unset per-port.
  std::optional<bool> bind_streams_on_inputs_by_default;
  /// Bind output port streams by default when unset per-port.
  std::optional<bool> bind_streams_on_outputs_by_default;
  bool clear_inputs_after_run = false;   ///< Release inputs after each run.
  bool clear_outputs_after_run = false;  ///< Release outputs after each run.

  friend bool operator==(const ActionSettings&,
                         const ActionSettings&) = default;
};

/** @brief Encodes a status as a chunk (mimetype ::kActionStatusMimetype). */
absl::StatusOr<data::Chunk> StatusToChunk(const absl::Status& status);
/** @brief Decodes a status previously encoded by StatusToChunk. */
absl::StatusOr<absl::Status> StatusFromChunk(const data::Chunk& chunk);
/**
 * @brief Whether @p chunk carries an encoded action status.
 *
 * The same predicate as ::a11::data::IsStatusChunk, re-exported here because
 * status chunks are an action-protocol concept even though the encoding lives
 * with the wire types.
 */
using data::IsStatusChunk;
/**
 * @brief Whether @p chunk is a status chunk reporting write-half closure.
 *
 * Such a chunk is a node lifecycle marker rather than a value: it says the
 * producer drained the node and closed its writer with that status. See
 * ::a11::data::kCloseAttribute.
 */
using data::IsCloseStatusChunk;

}  // namespace a11::actions

#endif  // A11_ACTIONS_SCHEMA_H_
