// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Downloading a URL to a verified file in a local cache.
 *
 * The shape every model-fetching caller needs: a file that is either absent or
 * complete and correct, never half-written, and cheap to ask for again. A
 * download streams to a temporary sibling, hashes as it goes, and is renamed
 * into place only once the digest matches -- so a crash, a full disk, or two
 * processes racing for the same model leaves the cache usable rather than
 * poisoned.
 */

#ifndef A11_NET_HTTP_DOWNLOAD_H_
#define A11_NET_HTTP_DOWNLOAD_H_

#include <filesystem>
#include <string>

#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "a11/net/http/fetch.h"

namespace a11::net {

/** @brief Where a download goes, and what proves it arrived intact. */
struct DownloadOptions {
  std::filesystem::path destination;  ///< Final path; parents are created.
  /**
   * Expected SHA-1 as hex, or empty to skip verification. When set, it is also
   * what decides whether an existing @c destination counts as a cache hit, so a
   * file truncated by an earlier crash is re-fetched rather than trusted.
   */
  std::string expected_sha1;
  OnFetchProgress on_progress;  ///< Optional progress callback.
  FetchOptions fetch;           ///< Request settings; see FetchOptions.
};

/**
 * @brief Computes the SHA-1 of a file, as lowercase hex.
 *
 * Exposed because a caller checking a cache it did not populate wants the same
 * digest this module verifies with.
 *
 * @param path File to hash.
 * @return The hex digest, or NotFound / a read error status.
 */
absl::StatusOr<std::string> FileSha1(const std::filesystem::path& path);

/**
 * @brief Downloads @p url to `options.destination`, verified and atomically.
 *
 * Returns immediately when the destination already exists and either no digest
 * was given or its digest matches. Otherwise streams the body to a temporary
 * sibling, verifies, and renames.
 *
 * @param url Absolute `http`/`https` URL.
 * @param options Destination, expected digest, and request settings.
 * @return An awaitable resolving to the destination path, or a status:
 *         InvalidArgument for no destination, DataLoss when the digest does not
 *         match (the temporary is removed), or whatever Fetch reports. A failed
 *         download never leaves the temporary behind.
 */
a11::Future<std::filesystem::path> Download(std::string url,
                                            DownloadOptions options);

}  // namespace a11::net

#endif  // A11_NET_HTTP_DOWNLOAD_H_
