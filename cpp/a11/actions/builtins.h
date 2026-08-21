// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The actions every A11 peer answers, whatever it was built to do.
 *
 * **Why these are not registrations.** A peer that cannot be asked what it
 * serves has to be told, and telling is what four hand-copied handshakes were
 * for. So discovery cannot be something an application remembers to install: it
 * has to hold for a registry nobody configured, for a registry a service copied
 * per connection, and for a registry an application has since called
 * `Unregister` all over. That rules out being an entry in
 * a11::actions::ActionRegistry -- `Register` replaces and `Unregister` removes --
 * and it rules out a service-level install, because every client builds its own
 * bare registry and would have nothing.
 *
 * What holds instead is a table here, consulted by the registry on a miss. So
 * `ActionRegistry()` is still empty of anything an application put there, while
 * every lookup, every listing, and every Flow `call` resolves these three.
 *
 * The handlers reach their registry through a11::actions::Action::GetRegistry
 * rather than capturing one, which is what keeps this a function-local static
 * and not a cycle from a registry to a handler that owns it.
 */

#ifndef A11_ACTIONS_BUILTINS_H_
#define A11_ACTIONS_BUILTINS_H_

#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"

namespace a11::actions {

/** @brief Lists the actions a peer serves, with their schemas. */
inline constexpr std::string_view kListActionsName = "__list_actions__";
/** @brief Returns one action's schema, or NotFound. */
inline constexpr std::string_view kGetSchemaName = "__get_schema__";
/**
 * @brief Echoes a value, so a caller can tell A11 from anything holding a port.
 *
 * The name and shape a gateway has always used, kept exactly: four languages'
 * clients probe with it, and the point of moving it here is that the probe now
 * works against any A11 service rather than only against the one server that
 * remembered to register it.
 */
inline constexpr std::string_view kPingName = "__ping";

/** @brief A schema and handler pair the registry resolves without holding. */
struct BuiltinAction {
  ActionSchema schema;
  ActionHandler handler;
};

/** @brief Whether @p name is a builtin every registry answers for. */
bool IsBuiltinAction(std::string_view name);

/** @brief The builtin named @p name, or null. */
const BuiltinAction* absl_nullable GetBuiltinAction(std::string_view name);

/** @brief Every builtin's name, sorted. */
const std::vector<std::string>& BuiltinActionNames();

}  // namespace a11::actions

#endif  // A11_ACTIONS_BUILTINS_H_
