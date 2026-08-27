// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_INSPECT_H_
#define A11_FLOW_INSPECT_H_

#include <string_view>
#include <vector>

#include "a11/flow/diagnostic.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"

namespace a11::flow {

/// Everything a flow does that it probably did not mean to.
///
/// Not errors: every one of these compiles and runs. They are the things a
/// reader would point at -- a `try` whose failure nothing looks at, a `| drop
/// 3` after a `| collect` that left one value to drop from, an `out` port
/// nothing writes, a header declared under an alias nobody uses. Each is one
/// of the published codes, so an editor can switch one off and a CI job can
/// gate on some and not others.
///
/// The findings and the families are the ones the IntelliJ plugin's Kotlin
/// analysis
/// works out today; this is where they come from now, so there is one
/// implementation
/// of the judgement rather than one per editor.
///
/// Takes the resolved program because that is where the facts are: the resolver
/// walked every reference and counted what read what, and re-deriving that here
/// would be a second name resolver. The parse result is for the sequences,
/// which are
/// a property of the text rather than of the names.
std::vector<Diagnostic> Inspect(std::string_view source,
                                const ParseResult& parsed,
                                const ResolveResult& resolved);

}  // namespace a11::flow

#endif  // A11_FLOW_INSPECT_H_
