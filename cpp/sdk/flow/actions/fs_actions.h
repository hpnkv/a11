// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The filesystem as Actions: streaming in, streaming out.
 *
 * These actions let Flow programs read and write local files through explicit
 * streaming ports.
 *
 * ### Why streaming is the point rather than a detail
 *
 * `read_file` writes its bytes as it reads them and `write_file` takes its
 * content as a *stream*, so
 *
 * @code{.a11flow}
 *   src = run read_file(path: from)
 *   src.bytes -> dst.content
 *   dst = run write_file(path: to)
 * @endcode
 *
 * moves a file larger than memory at bounded cost. A port write is the
 * backpressure point -- it does not return until the store has taken the chunk
 * -- so the reader is held behind the writer without either of them saying
 * anything about it. A `read_file` that handed back one value could not do this
 * at all, and neither could a `write_file` that took one.
 *
 * `read_file` also writes `info` **before** any bytes, for the same reason
 * `make_http_request` writes `status_code` before the body: it lets a flow
 * decide something while the reading is still going on.
 *
 * @code{.a11flow}
 *   f = run read_file(path: path)
 *   if f.info.size > 100000000 { fail resource_exhausted "too large" }
 * @endcode
 *
 * ### What happens when a run does not finish
 *
 * A cancelled `write_file` must not leave a half file behind, so it writes to a
 * temporary beside the destination and renames it into place at the end --
 * atomically, so a reader sees either the old contents or the new ones and
 * never a prefix. The temporary is removed on every failing path. `append` is
 * the exception and says so: appending atomically is not a thing a filesystem
 * offers, so an append that fails part-way leaves what it had written, and the
 * schema says as much rather than pretending otherwise.
 *
 * Everything reading a stream checks its StopSignal per chunk, not per file, so
 * cancelling a read of a large file takes about as long as one chunk.
 *
 * ### The port-name trap
 *
 * `write_file` takes `path` and reports `resolved`, not `path`, because a port
 * name means one node whichever direction it faces -- an action with an input
 * and an output both called `path` has one port that is both, and a flow
 * writing to it would be reading its own value back.
 *
 * ### Policy
 *
 * Every path goes through ResolvePath() before anything opens it, so a root the
 * host did not name is `permission_denied` however the path was spelled --
 * through `..`, through a symlink, or through both. The handlers here take the
 * policy at registration and there is no option that widens it.
 */

#ifndef A11_SDK_FLOW_ACTIONS_FS_ACTIONS_H_
#define A11_SDK_FLOW_ACTIONS_FS_ACTIONS_H_

#include <string_view>

#include <absl/status/status.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "sdk/flow/actions/policy.h"

namespace a11::sdk::flow {

/** @brief Registered name of the streaming reader. */
inline constexpr std::string_view kReadFileAction = "read_file";
/** @brief Registered name of the streaming writer. */
inline constexpr std::string_view kWriteFileAction = "write_file";
/** @brief Registered name of the directory walker. */
inline constexpr std::string_view kListDirectoryAction = "list_directory";
/** @brief Registered name of the metadata reader. */
inline constexpr std::string_view kStatPathAction = "stat_path";
/** @brief Registered name of the directory maker. */
inline constexpr std::string_view kMakeDirectoryAction = "make_directory";
/** @brief Registered name of the remover. */
inline constexpr std::string_view kRemovePathAction = "remove_path";
/** @brief Registered name of the mover. */
inline constexpr std::string_view kMovePathAction = "move_path";
/** @brief Registered name of the copier. */
inline constexpr std::string_view kCopyPathAction = "copy_path";
/** @brief Registered name of the scratch-space maker. */
inline constexpr std::string_view kMakeTempAction = "make_temp";

/** @brief Schema for @c read_file. */
actions::ActionSchema ReadFileSchema();
/** @brief Schema for @c write_file. */
actions::ActionSchema WriteFileSchema();
/** @brief Schema for @c list_directory. */
actions::ActionSchema ListDirectorySchema();
/** @brief Schema for @c stat_path. */
actions::ActionSchema StatPathSchema();
/** @brief Schema for @c make_directory. */
actions::ActionSchema MakeDirectorySchema();
/** @brief Schema for @c remove_path. */
actions::ActionSchema RemovePathSchema();
/** @brief Schema for @c move_path. */
actions::ActionSchema MovePathSchema();
/** @brief Schema for @c copy_path. */
actions::ActionSchema CopyPathSchema();
/** @brief Schema for @c make_temp. */
actions::ActionSchema MakeTempSchema();

/**
 * @brief Handlers, each closed over the policy it was registered with.
 *
 * The policy is a constructor argument rather than an option because a caller
 * that could pass one would be a caller that could widen it.
 */
actions::ActionHandler ReadFileHandler(CapabilitiesPtr capabilities);
actions::ActionHandler WriteFileHandler(CapabilitiesPtr capabilities);
actions::ActionHandler ListDirectoryHandler(CapabilitiesPtr capabilities);
actions::ActionHandler StatPathHandler(CapabilitiesPtr capabilities);
actions::ActionHandler MakeDirectoryHandler(CapabilitiesPtr capabilities);
actions::ActionHandler RemovePathHandler(CapabilitiesPtr capabilities);
actions::ActionHandler MovePathHandler(CapabilitiesPtr capabilities);
actions::ActionHandler CopyPathHandler(CapabilitiesPtr capabilities);
actions::ActionHandler MakeTempHandler(CapabilitiesPtr capabilities);

/**
 * @brief Registers the reading actions on @p registry.
 *
 * `read_file`, `list_directory` and `stat_path`. Separate from the writing half
 * because that is the line a host most often wants to draw, and drawing it by
 * choosing which function to call is harder to get wrong than drawing it with a
 * flag.
 */
absl::Status RegisterFilesystemReadActions(actions::ActionRegistry& registry,
                                           CapabilitiesPtr capabilities);

/**
 * @brief Registers the writing actions on @p registry.
 *
 * `write_file`, `make_directory`, `remove_path`, `move_path`, `copy_path` and
 * `make_temp`. Fails when the policy is not writable, rather than registering
 * six actions that all refuse: a host that called this meant to allow writing,
 * and a policy that does not is a mistake worth reporting at startup.
 */
absl::Status RegisterFilesystemWriteActions(actions::ActionRegistry& registry,
                                            CapabilitiesPtr capabilities);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_FS_ACTIONS_H_
