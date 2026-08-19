// Copyright 2026 The A11 Authors.
//
// What the C++ side of the ActionSchema scanner is pinned against. Not a
// translation unit anybody builds: every declaration here is a shape
// `cpp/a11/flow/discover.cc` has to read, including the ones it is meant to read
// badly.
//
// The C++ shape is the hard one, and the honest one to test: a schema is not a
// constructor call with literal arguments but a local variable filled in
// statement by statement, often by helpers, and named by a constant from the
// header beside this file.

#include "schemas.h"

#include "a11/actions/schema.h"

namespace a11::testdata {
namespace {

using actions::ActionPortSchema;
using actions::ActionSchema;

/// The helper every real C++ action uses to write a port, whose arguments run in
/// the order the scanner reads positionally.
ActionPortSchema Port(std::string name, std::string type, std::string desc,
                      bool required, bool unary) {
  return ActionPortSchema{.name = std::move(name),
                          .type = std::move(type),
                          .description = std::move(desc),
                          .required = required,
                          .unary = unary};
}

/// A type written as a call rather than as a literal, which is what leaves a
/// port with no type: the scanner reads literals and does not run anything.
std::string JsonType() { return "application/json"; }

/// A helper that fills in a schema it was handed. The ports it adds are *not*
/// found, because following it would mean following a call across functions --
/// the documented limit, and the reason this file has a port here at all.
void AddSharedInputs(ActionSchema& schema) {
  schema.inputs.emplace(
      "settings", Port("settings", JsonType(), "Request settings, all optional.",
                       /*required=*/false, /*unary=*/true));
}

}  // namespace

ActionSchema AssembledSchema() {
  ActionSchema schema;
  schema.name = std::string(kAssembledAction);
  schema.description =
      "Assembled statement by statement, as every C++ action is, with a "
      "description written as adjacent literals.";
  AddSharedInputs(schema);
  schema.inputs.emplace("url", Port("url", "text/plain", "What to fetch.",
                                    /*required=*/true, /*unary=*/true));
  schema.outputs.emplace(
      "status_code", Port("status_code", "integer",
                          "The response's status code.",
                          /*required=*/false, /*unary=*/true));
  schema.outputs.emplace("body",
                         Port("body", std::string(kOctetStream),
                              "Response body chunks, in order.",
                              /*required=*/false, /*unary=*/false));
  // A type that is a call rather than a literal: the port is found and its type
  // is not, which is the degradation this file exists to pin.
  schema.outputs.emplace("headers",
                         Port("headers", JsonType(),
                              "Response header fields as an object.",
                              /*required=*/false, /*unary=*/true));
  schema.headers.emplace("x-a11-deadline",
                         Port("x-a11-deadline", "text/plain",
                              "Absolute execution deadline.",
                              /*required=*/false, /*unary=*/true));
  return schema;
}

/// A schema whose name is a literal, written inline: the easy C++ case.
ActionSchema InlineSchema() {
  ActionSchema schema;
  schema.name = "cpp-inline";
  schema.description = "Names itself with a literal.";
  schema.outputs.emplace("out", Port("out", "text/plain", "The answer.",
                                     /*required=*/false, /*unary=*/true));
  return schema;
}

/// A schema with no name anywhere: not found, because nothing could look it up.
ActionSchema NamelessSchema() {
  ActionSchema schema;
  schema.description = "Has a description and no name.";
  return schema;
}

/* A mention in a block comment is not a declaration:
   ActionSchema schema; schema.name = "cpp-in-a-comment"; */

}  // namespace a11::testdata
