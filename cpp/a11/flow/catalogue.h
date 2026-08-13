// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_CATALOGUE_H_
#define A11_FLOW_CATALOGUE_H_

#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <nlohmann/json_fwd.hpp>

#include "a11/flow/plan.h"

namespace a11::flow {

/// What the language knows about the world it runs in.
///
/// **Why this exists.** The tools know the *language* exhaustively and the world
/// it runs in not at all: `run make_http_request(` offers no ports, hovering an
/// action name says "identifier", and a flow that names `a11.sdk.AudioBuffer`
/// gets no more help than one that names `a11.sdk.Typo`. The tools cannot fix
/// that by importing a registry, because `a11::flow_lang` links nothing but
/// Abseil and nlohmann on purpose -- so what the world contains arrives as
/// *data*.
///
/// **Where the data comes from.** A snapshot generated from the live registries
/// (`scripts/generate_flow_catalogue.py` -> `testdata/flow/catalogue.json`) is
/// embedded, so the standalone `a11-flow` is useful with nothing configured. A
/// frontend that has a *live* registry -- `a11 flow serve` running inside a
/// process that imported the SDK, an IDE that knows which registry an inline
/// flow is attached to -- passes its own with each request or once per session,
/// and that is merged over the snapshot. Neither is privileged: the snapshot is
/// a default, and a name given twice takes the later description.
namespace catalogue {

/// One port of an action, as a tool needs to show it.
struct PortInfo {
  std::string name;
  /// The type as the action's schema spells it: a Python type name, a
  /// serialisation tag, or a mimetype.
  std::string type;
  std::string description;
  bool required = false;
  /// Whether the port carries one value rather than a stream.
  bool unary = true;
};

/// One action a flow may `run` or `call`.
struct ActionInfo {
  std::string name;
  std::string description;
  std::vector<PortInfo> inputs;
  std::vector<PortInfo> outputs;
  /// Header names the action declares, with what each is for.
  std::vector<PortInfo> headers;

  const PortInfo* absl_nullable Port(std::string_view name,
                                     syntax::PortDirection direction) const;
  std::vector<std::string> PortNames(syntax::PortDirection direction) const;
};

/// One type the host knows, by the tag a flow writes it as.
///
/// A [DtoPlan] rather than a shape of its own: a `struct` and a registered type are
/// the same idea to everything that consumes this -- completion of its fields,
/// a hover that lists them, a schema made from it -- and one shape means the
/// tools have one code path rather than two.
struct TypeInfo {
  std::string tag;
  DtoPlan shape;
};

/// Everything the tools know about the world, in one place.
class Catalogue {
 public:
  /// The embedded snapshot: what a standalone tool knows with nothing
  /// configured.
  static const Catalogue& Builtin();

  /// The catalogue `value` describes, in the shape [ToJson] writes.
  ///
  /// Never fails: a field of the wrong type is skipped, because this arrives
  /// from a frontend that may be older or newer than the tool reading it and
  /// refusing the lot over one bad entry helps nobody.
  static Catalogue FromJson(const nlohmann::json& value);

  /// This catalogue with `other` laid over it.
  ///
  /// A name in `other` replaces the entry of that name; a name only here stays.
  /// That is what "a live registry extends the snapshot" means, and it is why
  /// merging is a whole-entry replace rather than a field-by-field merge: half a
  /// description from each side would be a third thing that is true of neither.
  Catalogue MergedWith(const Catalogue& other) const;

  const ActionInfo* absl_nullable Action(std::string_view name) const;
  const TypeInfo* absl_nullable Type(std::string_view tag) const;

  const std::vector<ActionInfo>& actions() const { return actions_; }
  const std::vector<TypeInfo>& types() const { return types_; }

  bool Empty() const { return actions_.empty() && types_.empty(); }

  nlohmann::json ToJson() const;

 private:
  std::vector<ActionInfo> actions_;
  std::vector<TypeInfo> types_;
};

/// The `format` field of the catalogue envelope.
inline constexpr std::string_view kCatalogueFormat = "flow.catalogue/v1";

}  // namespace catalogue
}  // namespace a11::flow

#endif  // A11_FLOW_CATALOGUE_H_
