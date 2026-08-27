// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief An ::a11::actions::ActionSchema in JSON, which is how one travels.
 *
 * **There is one concept here, in two representations.** An `ActionSchema` is
 * the live object: it holds a `typeinfo`, which is an opaque language handle
 * this layer never dereferences, and `autofills`, which are receiver-owned
 * default *values*. Neither can cross a wire: one is a pointer, and
 * `ActionRegistry::Copy` clears the other. Schemas sent to another process use
 * the textual form defined here.
 *
 * The document is `a11.actions/v1`: a `format` tag and an `actions` array. Each
 * entry is one schema, with `typeinfo` replaced by the port's `json_schema` --
 * the port's `json_schema`, and `autofills` replaced by an `autofilled` flag.
 * Autofill values remain local while their presence is included in the schema.
 *
 * One field in an entry is **not** part of the schema, and cannot be:
 * `runnable`, which says whether the side answering holds a handler. The same
 * schema is runnable here and schema-only there, and that difference is what
 * Flow reads to choose `run` over `call`. It is the registry's annotation on a
 * schema rather than a property of one.
 *
 * **Why one document.** Tool bridges, Flow tooling, editor integrations, and
 * model adapters consume this shared superset of `ActionInfo`, with the same
 * port keys and omit-when-default conventions.
 *
 * The legacy `user_facing` narration flag is absent. Narration uses the
 * reserved log port, which every action has and no schema declares. Readers
 * accept and discard the legacy flag.
 *
 * **What is omitted when it is the default.** `required` and `unary` are
 * written only when true, and a port's `json_schema` only when it says more
 * than
 * `{"type": "object"}` -- which is what an adapter shows a model for a port
 * carrying no schema at all, so writing it out states nothing. Every reader
 * here
 * fills in ::a11::actions::ActionPortSchema's own defaults, so what is absent
 * and what is spelled out mean the same thing.
 *
 * @warning
 *   Those defaults are this document's, not A11's everywhere.
 *   ::a11::flow::catalogue::PortInfo defaults `unary` to `true` and its codec
 *   omits the field when *true* -- the opposite convention, for a different
 *   format. A port entry moved between the two documents without going through
 *   a struct turns every streaming port into a unary one, or the reverse.
 */

#ifndef A11_ACTIONS_DESCRIBE_H_
#define A11_ACTIONS_DESCRIBE_H_

#include <string>
#include <string_view>
#include <vector>

#include <absl/status/statusor.h>
#include <nlohmann/json_fwd.hpp>

#include "a11/actions/schema.h"

namespace a11::actions {

class ActionRegistry;

/** @brief The `format` field of the schema document. */
inline constexpr std::string_view kSchemaDocumentFormat = "a11.actions/v1";

/**
 * @brief Which ports a written schema includes.
 *
 * @c kCallable omits inputs the receiver autofills, because a caller cannot
 * write them: an autofilled input must be empty when the default is applied, so
 * offering it to a model or a flow only invites a call that fails. @c kAll
 * keeps
 * them, flagged, for a reader that is inspecting rather than calling.
 */
enum class PortView {
  kCallable,  ///< What a caller may write. The default.
  kAll,       ///< Everything the schema declares, autofills flagged.
};

/**
 * @brief Which schemas to write, and how much of each.
 *
 * The wire form of @c __list_actions__'s request port, and of the query string
 * on `GET /actions`.
 */
struct SchemaQuery {
  /// Full-match name patterns; empty matches every name.
  std::vector<std::string> names;
  /// Exact names, in addition to @c names; empty adds nothing.
  std::vector<std::string> exact;
  PortView ports = PortView::kCallable;
  /// Include the `__`-prefixed reserved actions, discovery's own included.
  bool include_reserved = false;
  /// Omit actions registered for their schema alone, which live on a peer.
  bool runnable_only = false;
};

/**
 * @brief Parses a @c SchemaQuery from the request port's JSON.
 *
 * An empty or null document is the default request, not an error: asking "what
 * do you serve" with no qualification is the common case.
 */
absl::StatusOr<SchemaQuery> ParseSchemaQuery(std::string_view encoded);

/**
 * @brief Parses a @c SchemaQuery from a URL query string.
 * @param query The part after `?`, without it. May be empty.
 */
absl::StatusOr<SchemaQuery> ParseSchemaQueryString(std::string_view query);

/** @brief Whether @p name matches @p query's name filters. */
bool SchemaQueryAccepts(const SchemaQuery& query, std::string_view name);

/**
 * @brief One schema as an `actions` entry.
 * @param schema The action's interface.
 * @param runnable Whether this side holds a handler for it. The registry's
 *        annotation on the schema, not part of the schema itself.
 * @param ports Which ports to include.
 */
nlohmann::json SchemaToJson(const ActionSchema& schema, bool runnable,
                            PortView ports = PortView::kCallable);

/**
 * @brief A whole document: every schema in @p registry that @p query accepts.
 */
nlohmann::json RegistryToJson(const ActionRegistry& registry,
                              const SchemaQuery& query);

/** @brief RegistryToJson as text, or why it could not be encoded. */
absl::StatusOr<std::string> RegistryToJsonText(const ActionRegistry& registry,
                                               const SchemaQuery& query);

/** @brief One schema as a whole document, for a route that answers with one. */
absl::StatusOr<std::string> SchemaToJsonText(
    const ActionSchema& schema, bool runnable,
    PortView ports = PortView::kCallable);

/**
 * @brief The schema an `actions` entry was written from.
 *
 * The exact inverse of SchemaToJson, for a side that has to *call* what it was
 * told about: a tool bridge registering a reverse-dispatch proxy, or a flow run
 * against a peer registering the peer's actions for their schemas alone. What
 * cannot survive the trip comes back empty: @c typeinfo is a local handle, and
 * receiver-owned autofill defaults remain local.
 *
 * @param entry One entry from a document's `actions` array.
 */
absl::StatusOr<ActionSchema> SchemaFromJson(const nlohmann::json& entry);

/** @brief SchemaFromJson, parsing @p encoded first. */
absl::StatusOr<ActionSchema> SchemaFromJsonText(std::string_view encoded);

/**
 * @brief The `actions` entries of @p document, or why there are none to read.
 *
 * Accepts a bare array too, so a caller need not care whether it was handed a
 * whole document or just its entries.
 */
absl::StatusOr<std::vector<nlohmann::json>> SchemasInDocument(
    const nlohmann::json& document);

}  // namespace a11::actions

#endif  // A11_ACTIONS_DESCRIBE_H_
