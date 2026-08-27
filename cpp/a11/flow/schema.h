// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_SCHEMA_H_
#define A11_FLOW_SCHEMA_H_

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "a11/flow/diagnostic.h"
#include "a11/flow/plan.h"

namespace a11::flow {

/// The JSON Schema a `struct` describes.
///
/// **Why both directions exist.** A shape is the same idea as a JSON Schema
/// object, so a flow that declares one should be able to hand it to anything
/// that speaks schemas -- a model's tool definition, an OpenAPI document, a
/// validator somewhere else -- and should be able to accept one back. Neither
/// direction is the "real" one: a shape is what the language reads and a
/// schema is what the world outside it reads, and this is the translation.
///
/// Draft 2020-12, because that is the draft JSON Schema settled on and the one
/// a model's structured-output mode expects. Every shape `struct` reaches,
/// directly or through another, goes into `$defs`, and a field naming one is a
/// `$ref` -- so a shape that names itself round-trips, which a schema inlined
/// by substitution could not.
///
/// **The three types JSON has no word for.** `bytes`, `time` and `duration` go
/// out as strings, with the encoding or format that says how to read them
/// *and* a `x-a11-type` beside it. The format alone is what another reader
/// wants; the extension is what makes coming back lossless, since `{"type":
/// "string", "format": "date-time"}` is also a perfectly good way to say
/// `string`.
nlohmann::json DtoToJsonSchema(const DtoPlan& dto, const Program& program);

/// The shapes a JSON Schema describes, and what would not fit.
struct SchemaImport {
  /// The shape the schema itself is, followed by every `$defs` entry it named.
  ///
  /// In that order because the first is the one the caller asked about; the
  /// rest
  /// are there because a field refers to them and a shape is no use without the
  /// shapes it names.
  std::vector<DtoPlan> dtos;
  /// What did not map, as diagnostics rather than a refusal.
  ///
  /// A schema out in the world uses keywords the language has no spelling for
  /// --
  /// `oneOf`, `additionalProperties`, `$dynamicRef` -- and the useful answer is
  /// the shape it *did* describe plus a list of what was dropped, not nothing
  /// at
  /// all. Nothing here has a source range: there is no Flow text to point at.
  std::vector<Diagnostic> diagnostics;
};

/// The shapes `schema` describes, named `name` where it does not name itself.
///
/// The reverse of [DtoToJsonSchema], and lossless for what that writes.
/// Anything
/// else is a best effort: a keyword with no Flow spelling is reported and the
/// field keeps the type it could be read as.
SchemaImport JsonSchemaToDtos(const nlohmann::json& schema,
                              std::string_view name);

/// One shape as the Flow text that declares it.
///
/// What makes an import useful: the answer is source, so it can be pasted into
/// a file, read, edited and checked in. Written the way the formatter would
/// write it, so `a11 flow fmt` over the result changes nothing.
std::string DtoToFlow(const DtoPlan& dto);

/// The `format` field of the schema envelope.
inline constexpr std::string_view kSchemaFormat = "flow.schema/v1";

/// The extension key that says which Flow type a string really is.
inline constexpr std::string_view kFlowTypeKey = "x-a11-type";

/// The extension key holding a shape's fields in declaration order.
///
/// A JSON object's keys have no order a reader may rely on, and a shape's
/// fields
/// have one -- it is what a reader of the source sees and what
/// [DtoToFlow] writes back. Without this the round trip would quietly
/// alphabetise every shape it touched.
inline constexpr std::string_view kFlowOrderKey = "x-a11-order";

}  // namespace a11::flow

#endif  // A11_FLOW_SCHEMA_H_
