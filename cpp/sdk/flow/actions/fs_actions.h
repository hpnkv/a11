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
 * @brief Read and write local files through Flow actions.
 *
 * `read_file` emits bytes as it reads; `write_file` consumes a byte stream.
 *
 * @code{.a11flow}
 *   src = run read_file(path: from)
 *   src.bytes -> dst.content
 *   dst = run write_file(path: to)
 * @endcode
 *
 * Port writes wait for the store to accept each chunk. The pipeline uses
 * bounded memory for files allowed by the host policy.
 *
 * `read_file` emits `info` before any bytes, so a flow can inspect metadata
 * while the content is still streaming.
 *
 * @code{.a11flow}
 *   f = run read_file(path: path)
 *   if f.info.size > 100000000 { fail resource_exhausted "too large" }
 * @endcode
 *
 * ### Incomplete writes
 *
 * `write_file` writes to a temporary beside the destination and renames it on
 * success. Failure removes the temporary. An interrupted `append` retains the
 * bytes already appended.
 *
 * Stream readers check their StopSignal after each chunk.
 *
 * ### Port names
 *
 * `write_file` accepts `path` and reports the destination as `resolved`.
 * Input and output ports with the same name share one node.
 *
 * ### Policy
 *
 * Every path passes through ResolvePath() before opening. Paths outside the
 * configured roots return `permission_denied`, including symlink traversals.
 * Handlers capture the policy at registration.
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
 * The handler captures its policy at registration.
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
 * Registers `read_file`, `list_directory` and `stat_path`.
 */
absl::Status RegisterFilesystemReadActions(actions::ActionRegistry& registry,
                                           CapabilitiesPtr capabilities);

/**
 * @brief Registers the writing actions on @p registry.
 *
 * Registers `write_file`, `make_directory`, `remove_path`, `move_path`,
 * `copy_path` and `make_temp`. A read-only policy returns an error.
 */
absl::Status RegisterFilesystemWriteActions(actions::ActionRegistry& registry,
                                            CapabilitiesPtr capabilities);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_FS_ACTIONS_H_
