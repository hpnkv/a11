// Copyright 2026 The A11 Authors.

#include "a11/net/http/download.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <openssl/evp.h>
#include <unistd.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/http/fetch.h"

namespace a11::net {
namespace {

using EvpContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

EvpContext NewSha1Context() {
  EvpContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (context != nullptr &&
      EVP_DigestInit_ex(context.get(), EVP_sha1(), nullptr) != 1) {
    context.reset();
  }
  return context;
}

absl::StatusOr<std::string> FinishSha1(EVP_MD_CTX* absl_nonnull context) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &length) != 1) {
    return absl::InternalError("SHA-1 finalisation failed");
  }
  std::string hex;
  hex.reserve(static_cast<size_t>(length) * 2);
  for (unsigned int index = 0; index < length; ++index) {
    absl::StrAppendFormat(&hex, "%02x", digest[index]);
  }
  return hex;
}

/** A hidden sibling of the destination, so the rename cannot cross a device. */
std::filesystem::path TemporaryFor(const std::filesystem::path& destination) {
  return destination.parent_path() /
         absl::StrCat(".", destination.filename().string(), ".", ::getpid(),
                      ".download");
}

absl::Status EnsureParentDirectory(const std::filesystem::path& destination) {
  const std::filesystem::path parent = destination.parent_path();
  if (parent.empty()) {
    return absl::OkStatus();
  }
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (error && !std::filesystem::is_directory(parent)) {
    return absl::UnavailableError(absl::StrCat(
        "could not create ", parent.string(), ": ", error.message()));
  }
  return absl::OkStatus();
}

bool DigestsMatch(std::string_view expected, std::string_view actual) {
  return absl::AsciiStrToLower(expected) == absl::AsciiStrToLower(actual);
}

absl::StatusOr<std::filesystem::path> RunDownload(std::string url,
                                                  DownloadOptions options) {
  if (options.destination.empty()) {
    return absl::InvalidArgumentError("DownloadOptions.destination is required");
  }
  const std::filesystem::path destination = options.destination;

  if (std::filesystem::exists(destination)) {
    if (options.expected_sha1.empty()) {
      return destination;
    }
    // A digest we cannot read is not a reason to fail: treat it as a miss and
    // re-fetch over the top.
    const absl::StatusOr<std::string> cached = FileSha1(destination);
    if (cached.ok() && DigestsMatch(options.expected_sha1, *cached)) {
      return destination;
    }
  }

  ABSL_RETURN_IF_ERROR(EnsureParentDirectory(destination));
  const std::filesystem::path temporary = TemporaryFor(destination);

  EvpContext digest = NewSha1Context();
  if (digest == nullptr) {
    return absl::InternalError("could not initialise a SHA-1 context");
  }

  std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
  if (!out) {
    return absl::UnavailableError(
        absl::StrCat("could not open ", temporary.string(), " for writing"));
  }

  // One exit point for every failure after the temporary exists, so a partial
  // file is never left in the cache.
  const auto fail = [&temporary, &out](absl::Status status) -> absl::Status {
    out.close();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return status;
  };

  FetchSink sink = [&out, &digest](std::string_view chunk) -> absl::Status {
    if (EVP_DigestUpdate(digest.get(), chunk.data(), chunk.size()) != 1) {
      return absl::InternalError("SHA-1 update failed");
    }
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    if (!out) {
      return absl::UnavailableError("writing the downloaded body failed");
    }
    return absl::OkStatus();
  };

  // The blocking core, not FetchToSink: this already runs on the fiber Download
  // submitted, and nesting a Submit would only spend a second one to block this
  // one against.
  const absl::StatusOr<HttpResponseHead> head = internal::FetchBlocking(
      std::move(url), std::move(options.fetch), std::move(sink),
      std::move(options.on_progress));
  if (!head.ok()) {
    return fail(head.status());
  }
  out.flush();
  out.close();
  if (!out) {
    return fail(absl::UnavailableError(
        absl::StrCat("could not finish writing ", temporary.string())));
  }

  if (!options.expected_sha1.empty()) {
    const absl::StatusOr<std::string> actual = FinishSha1(digest.get());
    if (!actual.ok()) {
      return fail(actual.status());
    }
    if (!DigestsMatch(options.expected_sha1, *actual)) {
      return fail(absl::DataLossError(absl::StrCat(
          "downloaded ", destination.filename().string(),
          " has SHA-1 ", *actual, ", expected ", options.expected_sha1)));
    }
  }

  std::error_code error;
  std::filesystem::rename(temporary, destination, error);
  if (error) {
    return fail(absl::UnavailableError(
        absl::StrCat("could not move the download into place: ",
                     error.message())));
  }
  return destination;
}

}  // namespace

absl::StatusOr<std::string> FileSha1(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("could not open ", path.string(), " to hash it"));
  }
  EvpContext digest = NewSha1Context();
  if (digest == nullptr) {
    return absl::InternalError("could not initialise a SHA-1 context");
  }
  // On the heap: this runs on a fiber whose whole stack is 64 KiB, so a buffer
  // of that size as a local would overflow it into the neighbouring stack.
  std::vector<char> buffer(64 * 1024);
  while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
         in.gcount() > 0) {
    if (EVP_DigestUpdate(digest.get(), buffer.data(),
                         static_cast<size_t>(in.gcount())) != 1) {
      return absl::InternalError("SHA-1 update failed");
    }
    if (in.eof()) {
      break;
    }
  }
  if (in.bad()) {
    return absl::UnavailableError(
        absl::StrCat("reading ", path.string(), " to hash it failed"));
  }
  return FinishSha1(digest.get());
}

a11::Future<std::filesystem::path> Download(std::string url,
                                            DownloadOptions options) {
  return a11::Submit<std::filesystem::path>(
      [url = std::move(url), options = std::move(options)]() mutable
      -> absl::StatusOr<std::filesystem::path> {
        return RunDownload(std::move(url), std::move(options));
      });
}

}  // namespace a11::net
