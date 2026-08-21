// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/fs_actions.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "a11/uuid.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/policy.h"
#include "sdk/flow/actions/ports.h"
#include "sdk/flow/actions/stop.h"

namespace a11::sdk::flow {
namespace {

using ::a11::actions::Action;
using ::a11::actions::ActionHandler;
using ::a11::actions::ActionSchema;

namespace fs = std::filesystem;

/// Largest single chunk. Above this a "chunk" is an allocation rather than a
/// unit of streaming, and the value stops behaving like a stream.
constexpr std::int64_t kMaxChunkBytes = 8 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// The status an errno deserves. The mapping matters more than it looks: a
/// flow's `try` reads the code, so ENOENT arriving as `unavailable` would make
/// "no such file" indistinguishable from "the disk went away".
absl::Status ErrnoStatus(int code, std::string_view what,
                         std::string_view path) {
  const std::string message =
      absl::StrCat("cannot ", what, " '", path, "': ", std::strerror(code));
  switch (code) {
    case ENOENT:
      return absl::NotFoundError(message);
    case EACCES:
    case EPERM:
      return absl::PermissionDeniedError(message);
    case EEXIST:
      return absl::AlreadyExistsError(message);
    case EISDIR:
    case ENOTDIR:
    case ENOTEMPTY:
      return absl::FailedPreconditionError(message);
    case ENOSPC:
    case EDQUOT:
    case EFBIG:
    case EMFILE:
    case ENFILE:
      return absl::ResourceExhaustedError(message);
    case EINVAL:
    case ENAMETOOLONG:
      return absl::InvalidArgumentError(message);
    default:
      return absl::UnavailableError(message);
  }
}

absl::Status ErrorCodeStatus(const std::error_code& error,
                            std::string_view what, std::string_view path) {
  return ErrnoStatus(error.value(), what, path);
}

/// A file descriptor that closes itself. Not RAII for elegance: every early
/// return in this file is a `return` out of a fibre, and a leaked descriptor
/// per failed read is a process that stops working after a few thousand of
/// them.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  ~Fd() { Close(); }

  [[nodiscard]] bool valid() const { return fd_ >= 0; }
  [[nodiscard]] int get() const { return fd_; }
  void Close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

std::string_view KindOf(const fs::file_status& status) {
  switch (status.type()) {
    case fs::file_type::regular:
      return "file";
    case fs::file_type::directory:
      return "directory";
    case fs::file_type::symlink:
      return "symlink";
    case fs::file_type::not_found:
      return "absent";
    case fs::file_type::fifo:
      return "fifo";
    case fs::file_type::socket:
      return "socket";
    case fs::file_type::block:
    case fs::file_type::character:
      return "device";
    default:
      return "other";
  }
}

/// RFC 3339, the same spelling Flow's `time()` reads back.
std::string FormatFileTime(fs::file_time_type when) {
  // No portable conversion from file_clock before C++20's clock_cast is
  // reliably available across the toolchains this builds on, so the offset
  // between the two clocks is measured once. It is stable to well under the
  // second of resolution anybody wants from a modification time.
  static const auto file_epoch_offset = []() {
    const auto file_now = fs::file_time_type::clock::now();
    const absl::Time wall_now = absl::Now();
    return wall_now - absl::FromChrono(file_now.time_since_epoch());
  }();
  const absl::Time at =
      file_epoch_offset + absl::FromChrono(when.time_since_epoch());
  return absl::FormatTime(absl::RFC3339_full, at, absl::UTCTimeZone());
}

/// One path's metadata, as every action here reports it.
nlohmann::json PathInfo(const fs::path& path, bool follow_symlinks) {
  std::error_code error;
  const fs::file_status status = follow_symlinks
                                     ? fs::status(path, error)
                                     : fs::symlink_status(path, error);
  nlohmann::json info{
      {"path", path.string()},
      {"name", path.filename().string()},
      {"kind", KindOf(status)},
      {"exists", !error && fs::exists(status)},
  };
  if (error) {
    return info;
  }
  if (status.type() == fs::file_type::regular) {
    std::error_code size_error;
    const std::uintmax_t size = fs::file_size(path, size_error);
    info["size"] = size_error ? 0 : static_cast<std::uint64_t>(size);
  }
  std::error_code time_error;
  const fs::file_time_type modified = fs::last_write_time(path, time_error);
  if (!time_error) {
    info["modified"] = FormatFileTime(modified);
  }
  // Octal, because that is how anybody reading a mode reads one.
  info["mode"] = absl::StrFormat(
      "%04o", static_cast<unsigned>(status.permissions()) & 07777U);
  return info;
}

// ---------------------------------------------------------------------------
// Shared option reading
// ---------------------------------------------------------------------------

/// Reads the `options` port, which every action here has and none requires.
absl::StatusOr<Options> ReadOptions(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw,
                        ReadJsonInput(action, "options"));
  if (!raw.has_value()) {
    return Options::Parse(nullptr);
  }
  return Options::Parse(&*raw);
}

/// Resolves the `path` input against the policy. The first thing every handler
/// does, and the only place a path enters this file.
absl::StatusOr<fs::path> ResolveInput(const std::shared_ptr<Action>& action,
                                      const FilesystemPolicy& policy,
                                      std::string_view port, bool for_write) {
  ABSL_ASSIGN_OR_RETURN(const std::string raw,
                        ReadRequiredTextInput(action, port));
  return ResolvePath(policy, raw, for_write);
}

/// Matches a filename against a `*`/`?` pattern.
///
/// Deliberately not a regular expression: a pattern may have come from a model,
/// and `*` and `?` cannot be made to backtrack for a second, where a regular
/// expression can. Iterative rather than recursive for the same reason.
bool WildcardMatch(std::string_view name, std::string_view pattern) {
  std::size_t name_at = 0;
  std::size_t pattern_at = 0;
  std::size_t star = std::string_view::npos;
  std::size_t name_after_star = 0;
  while (name_at < name.size()) {
    if (pattern_at < pattern.size() &&
        (pattern[pattern_at] == '?' || pattern[pattern_at] == name[name_at])) {
      ++name_at;
      ++pattern_at;
    } else if (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
      star = pattern_at++;
      name_after_star = name_at;
    } else if (star != std::string_view::npos) {
      pattern_at = star + 1;
      name_at = ++name_after_star;
    } else {
      return false;
    }
  }
  while (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
    ++pattern_at;
  }
  return pattern_at == pattern.size();
}

bool MatchesAny(std::string_view name,
                const std::vector<std::string>& patterns) {
  if (patterns.empty()) {
    return true;
  }
  for (const std::string& pattern : patterns) {
    if (WildcardMatch(name, pattern)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// read_file
// ---------------------------------------------------------------------------

struct ReadSettings {
  std::int64_t chunk_bytes = static_cast<std::int64_t>(kDefaultChunkBytes);
  std::uint64_t offset = 0;
  std::uint64_t length = 0;  ///< 0 means to the end.
  std::uint64_t max_bytes = 0;
};

absl::StatusOr<ReadSettings> ReadReadSettings(const Options& options,
                                              const FilesystemPolicy& policy) {
  ReadSettings settings;
  ABSL_ASSIGN_OR_RETURN(
      settings.chunk_bytes,
      options.IntInRange("chunk_bytes",
                         static_cast<std::int64_t>(kDefaultChunkBytes), 1,
                         kMaxChunkBytes));
  ABSL_ASSIGN_OR_RETURN(settings.offset, options.Bytes("offset", 0));
  ABSL_ASSIGN_OR_RETURN(settings.length, options.Bytes("length", 0));
  ABSL_ASSIGN_OR_RETURN(settings.max_bytes,
                        options.Bytes("max_bytes", policy.max_read_bytes));
  if (policy.max_read_bytes != 0 &&
      (settings.max_bytes == 0 || settings.max_bytes > policy.max_read_bytes)) {
    // An option may narrow the policy and never widen it.
    settings.max_bytes = policy.max_read_bytes;
  }
  return settings;
}

absl::Status RunReadFile(const std::shared_ptr<Action>& action,
                         const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());
  ABSL_ASSIGN_OR_RETURN(const ReadSettings settings,
                        ReadReadSettings(options, policy));
  ABSL_ASSIGN_OR_RETURN(const fs::path path,
                        ResolveInput(action, policy, "path", /*for_write=*/false));

  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<StopSignal> stop,
                        StopSignal::Create(action));

  const auto finish = [&](const absl::Status& status) -> absl::Status {
    stop->Join();
    if (!status.ok()) {
      outputs.Abort(status).IgnoreError();
      return status;
    }
    return outputs.Finish();
  };

  const Sink info_out = outputs["info"];
  const Sink bytes_out = outputs["bytes"];
  const Sink text_out = outputs["text"];
  const Sink lines_out = outputs["lines"];

  std::error_code error;
  const fs::file_status status =
      policy.follow_symlinks ? fs::status(path, error)
                             : fs::symlink_status(path, error);
  if (error) {
    return finish(ErrorCodeStatus(error, "read", path.string()));
  }
  if (status.type() == fs::file_type::directory) {
    return finish(absl::FailedPreconditionError(absl::StrCat(
        "'", path.string(), "' is a directory; use list_directory")));
  }
  if (status.type() == fs::file_type::symlink && !policy.follow_symlinks) {
    return finish(absl::FailedPreconditionError(
        absl::StrCat("'", path.string(),
                     "' is a symlink and this host does not follow them")));
  }

  // Before a single byte, so a flow can decide on the size while the reading
  // goes on -- the same reason `make_http_request` writes its status first.
  if (const absl::Status written = info_out.PutOnly(PathInfo(path, true));
      !written.ok()) {
    return finish(written);
  }

  Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd.valid()) {
    return finish(ErrnoStatus(errno, "open", path.string()));
  }
  if (settings.offset > 0 &&
      ::lseek(fd.get(), static_cast<off_t>(settings.offset), SEEK_SET) < 0) {
    return finish(ErrnoStatus(errno, "seek in", path.string()));
  }

  const bool wants_text = text_out.present();
  const bool wants_lines = lines_out.present();
  const bool wants_bytes = bytes_out.present();

  // On the heap. A fibre's stack is measured in kilobytes, so a 64 KiB buffer
  // as a local would run off the end of it and into whatever is next.
  std::string buffer(static_cast<std::size_t>(settings.chunk_bytes), '\0');
  std::string whole;      // only when `text` is wanted
  std::string pending;    // the tail of a line that has not ended yet
  std::uint64_t total = 0;

  while (true) {
    // Per chunk rather than per file, so cancelling a read of something large
    // costs about one chunk.
    if (const absl::Status stopped = stop->Check(); !stopped.ok()) {
      return finish(stopped);
    }
    std::size_t want = buffer.size();
    if (settings.length > 0) {
      const std::uint64_t left = settings.length - total;
      if (left == 0) {
        break;
      }
      want = static_cast<std::size_t>(std::min<std::uint64_t>(want, left));
    }
    const ssize_t got = ::read(fd.get(), buffer.data(), want);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return finish(ErrnoStatus(errno, "read", path.string()));
    }
    if (got == 0) {
      break;
    }
    const std::string_view piece(buffer.data(), static_cast<std::size_t>(got));
    total += piece.size();
    if (settings.max_bytes != 0 && total > settings.max_bytes) {
      return finish(absl::ResourceExhaustedError(absl::StrCat(
          "'", path.string(), "' is larger than the ", settings.max_bytes,
          " bytes this read allows")));
    }

    if (wants_bytes) {
      if (const absl::Status written = bytes_out.PutBytes(std::string(piece));
          !written.ok()) {
        return finish(written);
      }
    }
    if (wants_text) {
      whole.append(piece);
    }
    if (wants_lines) {
      pending.append(piece);
      std::size_t start = 0;
      while (true) {
        const std::size_t newline = pending.find('\n', start);
        if (newline == std::string::npos) {
          break;
        }
        std::string_view line(pending.data() + start, newline - start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);  // a file written on Windows is still lines
        }
        if (const absl::Status written = lines_out.PutText(line);
            !written.ok()) {
          return finish(written);
        }
        start = newline + 1;
      }
      pending.erase(0, start);
    }
  }

  if (wants_lines && !pending.empty()) {
    // A last line with no newline after it is still a line.
    std::string_view line(pending);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (const absl::Status written = lines_out.PutText(line, /*final=*/true);
        !written.ok()) {
      return finish(written);
    }
  }
  if (wants_text) {
    if (const absl::Status written = text_out.PutOnlyText(whole);
        !written.ok()) {
      return finish(written);
    }
  }
  return finish(absl::OkStatus());
}

// ---------------------------------------------------------------------------
// write_file
// ---------------------------------------------------------------------------

struct WriteSettings {
  bool append = false;
  bool atomic = true;
  bool create_parents = false;
  bool sync = false;
  std::int64_t mode = 0644;
  std::uint64_t max_bytes = 0;
};

absl::StatusOr<WriteSettings> ReadWriteSettings(const Options& options,
                                                const FilesystemPolicy& policy) {
  WriteSettings settings;
  ABSL_ASSIGN_OR_RETURN(settings.append, options.Bool("append", false));
  ABSL_ASSIGN_OR_RETURN(settings.atomic,
                        options.Bool("atomic", !settings.append));
  ABSL_ASSIGN_OR_RETURN(settings.create_parents,
                        options.Bool("create_parents", false));
  ABSL_ASSIGN_OR_RETURN(settings.sync, options.Bool("sync", false));
  ABSL_ASSIGN_OR_RETURN(settings.mode,
                        options.IntInRange("mode", 0644, 0, 07777));
  ABSL_ASSIGN_OR_RETURN(settings.max_bytes,
                        options.Bytes("max_bytes", policy.max_write_bytes));
  if (policy.max_write_bytes != 0 &&
      (settings.max_bytes == 0 || settings.max_bytes > policy.max_write_bytes)) {
    settings.max_bytes = policy.max_write_bytes;
  }
  if (settings.append && settings.atomic) {
    // Not a thing a filesystem offers, and quietly picking one of the two would
    // mean a caller believing in a guarantee it does not have.
    return absl::InvalidArgumentError(
        "options.append and options.atomic cannot both be set: appending "
        "replaces nothing, so there is no rename that would make it atomic");
  }
  return settings;
}

/// The temporary a non-append write goes to. Beside the destination rather than
/// in a temp directory, because a rename across filesystems is a copy and would
/// stop being atomic.
fs::path TemporaryBeside(const fs::path& destination) {
  return destination.parent_path() /
         absl::StrCat(".", destination.filename().string(), ".a11-",
                      a11::NewUuid());
}

absl::Status RunWriteFile(const std::shared_ptr<Action>& action,
                          const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());
  ABSL_ASSIGN_OR_RETURN(const WriteSettings settings,
                        ReadWriteSettings(options, policy));
  ABSL_ASSIGN_OR_RETURN(const fs::path path,
                        ResolveInput(action, policy, "path", /*for_write=*/true));

  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<StopSignal> stop,
                        StopSignal::Create(action));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> content,
                        action->GetInput("content"));

  if (settings.create_parents && !path.parent_path().empty()) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error && !fs::is_directory(path.parent_path())) {
      return ErrorCodeStatus(error, "create the parent directory of",
                             path.string());
    }
  }

  const bool atomic = settings.atomic;
  const fs::path target = atomic ? TemporaryBeside(path) : path;
  int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
  flags |= settings.append ? O_APPEND : (atomic ? O_EXCL : O_TRUNC);
  Fd fd(::open(target.c_str(), flags, static_cast<mode_t>(settings.mode)));
  if (!fd.valid()) {
    return ErrnoStatus(errno, "open", target.string());
  }

  // Every failing path below has to remove the temporary: a cancelled write
  // that left one behind would litter the destination's own directory.
  const auto give_up = [&](const absl::Status& reason) -> absl::Status {
    fd.Close();
    if (atomic) {
      std::error_code ignored;
      fs::remove(target, ignored);
    }
    stop->Join();
    outputs.Abort(reason).IgnoreError();
    return reason;
  };

  std::uint64_t written = 0;
  while (true) {
    if (const absl::Status stopped = stop->Check(); !stopped.ok()) {
      return give_up(stopped);
    }
    absl::StatusOr<std::optional<data::Chunk>> chunk =
        content->NextChunk().Await();
    if (!chunk.ok()) {
      return give_up(chunk.status());
    }
    if (!chunk->has_value() || (*chunk)->IsNull()) {
      break;
    }
    absl::StatusOr<std::string> bytes = BytesOfChunk(**chunk);
    if (!bytes.ok()) {
      return give_up(bytes.status());
    }
    const std::string& payload = *bytes;
    if (payload.empty()) {
      continue;
    }
    if (settings.max_bytes != 0 &&
        written + payload.size() > settings.max_bytes) {
      return give_up(absl::ResourceExhaustedError(
          absl::StrCat("this write allows ", settings.max_bytes,
                       " bytes and the content is longer")));
    }
    std::size_t at = 0;
    while (at < payload.size()) {
      const ssize_t put =
          ::write(fd.get(), payload.data() + at, payload.size() - at);
      if (put < 0) {
        if (errno == EINTR) {
          continue;
        }
        return give_up(ErrnoStatus(errno, "write to", target.string()));
      }
      at += static_cast<std::size_t>(put);
    }
    written += payload.size();
  }

  if (settings.sync && ::fsync(fd.get()) != 0) {
    return give_up(ErrnoStatus(errno, "flush", target.string()));
  }
  fd.Close();

  if (atomic) {
    std::error_code error;
    // Atomic by the filesystem's own guarantee: a reader sees the old contents
    // or the new ones, never a prefix of the new ones.
    fs::rename(target, path, error);
    if (error) {
      std::error_code ignored;
      fs::remove(target, ignored);
      return give_up(ErrorCodeStatus(error, "rename into place", path.string()));
    }
  }

  stop->Join();
  ABSL_RETURN_IF_ERROR(outputs["resolved"].PutOnlyText(path.string()));
  ABSL_RETURN_IF_ERROR(
      outputs["bytes_written"].PutOnly(nlohmann::json(written)));
  ABSL_RETURN_IF_ERROR(outputs["info"].PutOnly(PathInfo(path, true)));
  return outputs.Finish();
}

// ---------------------------------------------------------------------------
// list_directory
// ---------------------------------------------------------------------------

struct ListSettings {
  bool recursive = false;
  std::int64_t max_depth = 0;  ///< 0 means no limit.
  bool hidden = false;
  std::uint64_t max_entries = 0;
  std::vector<std::string> patterns;
  std::vector<std::string> kinds;
};

absl::StatusOr<ListSettings> ReadListSettings(const Options& options,
                                              const FilesystemPolicy& policy) {
  ListSettings settings;
  ABSL_ASSIGN_OR_RETURN(settings.recursive, options.Bool("recursive", false));
  ABSL_ASSIGN_OR_RETURN(settings.max_depth,
                        options.IntInRange("max_depth", 0, 0, 4096));
  ABSL_ASSIGN_OR_RETURN(settings.hidden, options.Bool("hidden", false));
  ABSL_ASSIGN_OR_RETURN(settings.max_entries,
                        options.Bytes("max_entries", policy.max_entries));
  ABSL_ASSIGN_OR_RETURN(settings.patterns, options.StringList("match"));
  ABSL_ASSIGN_OR_RETURN(settings.kinds, options.StringList("kinds"));
  if (policy.max_entries != 0 && (settings.max_entries == 0 ||
                                  settings.max_entries > policy.max_entries)) {
    settings.max_entries = policy.max_entries;
  }
  return settings;
}

absl::Status RunListDirectory(const std::shared_ptr<Action>& action,
                              const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());
  ABSL_ASSIGN_OR_RETURN(const ListSettings settings,
                        ReadListSettings(options, policy));
  ABSL_ASSIGN_OR_RETURN(const fs::path root,
                        ResolveInput(action, policy, "path", /*for_write=*/false));

  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<StopSignal> stop,
                        StopSignal::Create(action));

  const auto finish = [&](const absl::Status& status) -> absl::Status {
    stop->Join();
    if (!status.ok()) {
      outputs.Abort(status).IgnoreError();
      return status;
    }
    return outputs.Finish();
  };

  std::error_code error;
  if (!fs::is_directory(root, error) || error) {
    return finish(absl::FailedPreconditionError(
        absl::StrCat("'", root.string(), "' is not a directory")));
  }

  const Sink entries = outputs["entries"];
  std::uint64_t count = 0;
  bool truncated = false;

  const auto options_for_walk = policy.follow_symlinks
                                    ? fs::directory_options::follow_directory_symlink
                                    : fs::directory_options::none;

  // Entries are written as the walk finds them, not gathered first: a tree with
  // a million files in it should cost a flow one entry of memory, and its first
  // entry should arrive before the walk has finished.
  const auto emit = [&](const fs::path& path,
                        int depth) -> absl::StatusOr<bool> {
    const std::string name = path.filename().string();
    if (!settings.hidden && absl::StartsWith(name, ".")) {
      return true;
    }
    if (!MatchesAny(name, settings.patterns)) {
      return true;
    }
    nlohmann::json info = PathInfo(path, policy.follow_symlinks);
    if (!settings.kinds.empty()) {
      const std::string kind = info.value("kind", std::string());
      bool wanted = false;
      for (const std::string& allowed : settings.kinds) {
        wanted = wanted || allowed == kind;
      }
      if (!wanted) {
        return true;
      }
    }
    info["depth"] = depth;
    if (settings.max_entries != 0 && count >= settings.max_entries) {
      truncated = true;
      return false;
    }
    ++count;
    ABSL_RETURN_IF_ERROR(entries.PutValue(info));
    return true;
  };

  if (settings.recursive) {
    fs::recursive_directory_iterator walk(root, options_for_walk, error);
    if (error) {
      return finish(ErrorCodeStatus(error, "list", root.string()));
    }
    const fs::recursive_directory_iterator end;
    while (walk != end) {
      if (const absl::Status stopped = stop->Check(); !stopped.ok()) {
        return finish(stopped);
      }
      if (settings.max_depth != 0 && walk.depth() + 1 >= settings.max_depth) {
        walk.disable_recursion_pending();
      }
      absl::StatusOr<bool> carry_on = emit(walk->path(), walk.depth());
      if (!carry_on.ok()) {
        return finish(carry_on.status());
      }
      if (!*carry_on) {
        break;
      }
      // The incrementing overload that reports rather than throws: a directory
      // that cannot be entered mid-walk is ordinary, and abandoning the whole
      // listing for one of them would be the wrong answer.
      walk.increment(error);
      if (error) {
        break;
      }
    }
  } else {
    fs::directory_iterator walk(root, options_for_walk, error);
    if (error) {
      return finish(ErrorCodeStatus(error, "list", root.string()));
    }
    const fs::directory_iterator end;
    while (walk != end) {
      if (const absl::Status stopped = stop->Check(); !stopped.ok()) {
        return finish(stopped);
      }
      absl::StatusOr<bool> carry_on = emit(walk->path(), 0);
      if (!carry_on.ok()) {
        return finish(carry_on.status());
      }
      if (!*carry_on) {
        break;
      }
      walk.increment(error);
      if (error) {
        break;
      }
    }
  }

  if (const absl::Status written =
          outputs["count"].PutOnly(nlohmann::json(count));
      !written.ok()) {
    return finish(written);
  }
  // Said explicitly rather than left for a caller to infer from a count that
  // happens to equal the limit: a truncated listing that looks complete is how
  // a flow reports a wrong answer confidently.
  if (const absl::Status written =
          outputs["truncated"].PutOnly(nlohmann::json(truncated));
      !written.ok()) {
    return finish(written);
  }
  return finish(absl::OkStatus());
}

// ---------------------------------------------------------------------------
// stat_path
// ---------------------------------------------------------------------------

absl::Status RunStatPath(const std::shared_ptr<Action>& action,
                         const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());
  ABSL_ASSIGN_OR_RETURN(const fs::path path,
                        ResolveInput(action, policy, "path", /*for_write=*/false));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  nlohmann::json info = PathInfo(path, policy.follow_symlinks);
  const bool exists = info.value("exists", false);
  // A path that is not there is an answer rather than a failure: `exists` is
  // what a flow asked for, and failing would make `if not stat.exists` need a
  // `try` around it to be writable at all.
  ABSL_RETURN_IF_ERROR(outputs["exists"].PutOnly(nlohmann::json(exists)));
  ABSL_RETURN_IF_ERROR(outputs["info"].PutOnly(std::move(info)));
  return outputs.Finish();
}

// ---------------------------------------------------------------------------
// make_directory, remove_path, move_path, copy_path, make_temp
// ---------------------------------------------------------------------------

absl::Status RunMakeDirectory(const std::shared_ptr<Action>& action,
                              const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const bool parents, options.Bool("parents", true));
  ABSL_ASSIGN_OR_RETURN(const fs::path path,
                        ResolveInput(action, policy, "path", /*for_write=*/true));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  std::error_code error;
  const bool created = parents ? fs::create_directories(path, error)
                               : fs::create_directory(path, error);
  if (error && !fs::is_directory(path)) {
    return ErrorCodeStatus(error, "create", path.string());
  }
  ABSL_RETURN_IF_ERROR(outputs["resolved"].PutOnlyText(path.string()));
  // False when it was already there, which is the difference between "made it"
  // and "found it" and the only thing a caller cannot see for itself.
  ABSL_RETURN_IF_ERROR(outputs["created"].PutOnly(nlohmann::json(created)));
  return outputs.Finish();
}

absl::Status RunRemovePath(const std::shared_ptr<Action>& action,
                           const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const bool recursive, options.Bool("recursive", false));
  ABSL_ASSIGN_OR_RETURN(const bool missing_ok, options.Bool("missing_ok", true));
  ABSL_ASSIGN_OR_RETURN(const fs::path path,
                        ResolveInput(action, policy, "path", /*for_write=*/true));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  std::error_code error;
  if (!fs::exists(fs::symlink_status(path, error))) {
    if (!missing_ok) {
      return absl::NotFoundError(
          absl::StrCat("'", path.string(), "' is not there to remove"));
    }
    ABSL_RETURN_IF_ERROR(outputs["removed"].PutOnly(nlohmann::json(0)));
    return outputs.Finish();
  }
  if (fs::is_directory(path, error) && !recursive) {
    // Refusing rather than removing: a recursive delete nobody asked for is
    // the single most expensive way for this library to be convenient.
    return absl::FailedPreconditionError(
        absl::StrCat("'", path.string(),
                     "' is a directory; set options.recursive to remove it"));
  }
  const std::uintmax_t removed =
      recursive ? fs::remove_all(path, error)
                : (fs::remove(path, error) ? 1 : 0);
  if (error) {
    return ErrorCodeStatus(error, "remove", path.string());
  }
  ABSL_RETURN_IF_ERROR(outputs["removed"].PutOnly(
      nlohmann::json(static_cast<std::uint64_t>(removed))));
  return outputs.Finish();
}

absl::Status RunMovePath(const std::shared_ptr<Action>& action,
                         const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const bool overwrite, options.Bool("overwrite", false));
  ABSL_ASSIGN_OR_RETURN(const fs::path from,
                        ResolveInput(action, policy, "path", /*for_write=*/true));
  ABSL_ASSIGN_OR_RETURN(const fs::path to,
                        ResolveInput(action, policy, "to", /*for_write=*/true));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  std::error_code error;
  if (!overwrite && fs::exists(fs::symlink_status(to, error))) {
    return absl::AlreadyExistsError(absl::StrCat(
        "'", to.string(), "' is already there; set options.overwrite"));
  }
  fs::rename(from, to, error);
  if (error) {
    // Across filesystems a rename is not available, and copy-then-remove is
    // what the caller meant. Said here rather than silently: the result is no
    // longer atomic, and `copy_path` is where a caller who cares should be.
    return ErrorCodeStatus(
        error, absl::StrCat("move to '", to.string(), "' from"), from.string());
  }
  ABSL_RETURN_IF_ERROR(outputs["resolved"].PutOnlyText(to.string()));
  return outputs.Finish();
}

absl::Status RunCopyPath(const std::shared_ptr<Action>& action,
                         const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const bool recursive, options.Bool("recursive", false));
  ABSL_ASSIGN_OR_RETURN(const bool overwrite, options.Bool("overwrite", false));
  ABSL_ASSIGN_OR_RETURN(const fs::path from,
                        ResolveInput(action, policy, "path", /*for_write=*/false));
  ABSL_ASSIGN_OR_RETURN(const fs::path to,
                        ResolveInput(action, policy, "to", /*for_write=*/true));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  fs::copy_options flags = fs::copy_options::none;
  if (recursive) {
    flags |= fs::copy_options::recursive;
  }
  flags |= overwrite ? fs::copy_options::overwrite_existing
                     : fs::copy_options::skip_existing;
  std::error_code error;
  fs::copy(from, to, flags, error);
  if (error) {
    return ErrorCodeStatus(
        error, absl::StrCat("copy to '", to.string(), "' from"), from.string());
  }
  ABSL_RETURN_IF_ERROR(outputs["resolved"].PutOnlyText(to.string()));
  return outputs.Finish();
}

absl::Status RunMakeTemp(const std::shared_ptr<Action>& action,
                         const CapabilitiesPtr& capabilities) {
  const FilesystemPolicy& policy = capabilities->filesystem;
  ABSL_ASSIGN_OR_RETURN(const Options options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(const std::string prefix, options.String("prefix", "a11-"));
  ABSL_ASSIGN_OR_RETURN(const std::string suffix, options.String("suffix", ""));
  ABSL_ASSIGN_OR_RETURN(const bool directory, options.Bool("directory", true));
  ABSL_ASSIGN_OR_RETURN(const std::string inside, options.String("in", ""));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  std::error_code error;
  fs::path parent;
  if (inside.empty()) {
    // A policy with roots has no business handing out a path in the system
    // temp directory: the first root is the scratch space the host meant.
    if (policy.unrestricted || policy.roots.empty()) {
      parent = fs::temp_directory_path(error);
      if (error) {
        return ErrorCodeStatus(error, "find a temporary directory for", "");
      }
    } else {
      parent = fs::path(policy.roots.front());
    }
  } else {
    ABSL_ASSIGN_OR_RETURN(parent,
                          ResolvePath(policy, inside, /*for_write=*/true));
  }
  ABSL_ASSIGN_OR_RETURN(
      const fs::path path,
      ResolvePath(policy,
                  (parent / absl::StrCat(prefix, a11::NewUuid(), suffix))
                      .string(),
                  /*for_write=*/true));

  if (directory) {
    if (!fs::create_directory(path, error) || error) {
      return ErrorCodeStatus(error, "create", path.string());
    }
  } else {
    // O_EXCL, so a name that somehow already exists is an error rather than a
    // file somebody else is using.
    Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!fd.valid()) {
      return ErrnoStatus(errno, "create", path.string());
    }
  }
  ABSL_RETURN_IF_ERROR(outputs["path"].PutOnlyText(path.string()));
  return outputs.Finish();
}

// ---------------------------------------------------------------------------
// Handler plumbing
// ---------------------------------------------------------------------------

using PolicyRun = absl::Status (*)(const std::shared_ptr<Action>&,
                                   const CapabilitiesPtr&);

/// One shape for all nine: submit onto a fibre, run, report. The handler holds
/// the policy, which is what makes it a capability rather than a setting.
ActionHandler Handler(PolicyRun run, CapabilitiesPtr capabilities) {
  return [run, capabilities = std::move(capabilities)](
             std::shared_ptr<Action> action) {
    return a11::SubmitTask([run, capabilities,
                            action = std::move(action)]() -> absl::Status {
      if (capabilities == nullptr) {
        return absl::FailedPreconditionError(
            "this action was registered without a policy");
      }
      return run(action, capabilities);
    });
  };
}

/// The two settings every action in this library shares, worded once in
/// ports.cc so thirty descriptions cannot drift apart.
std::string_view kOmitHelp() { return SharedOptionsHelp(); }

}  // namespace

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

ActionSchema ReadFileSchema() {
  ActionSchema schema;
  schema.name = std::string(kReadFileAction);
  schema.description =
      "Read a file, with each way of wanting it on a port of its own: the bytes "
      "as they arrive, the whole thing as text, one value per line, and the "
      "metadata -- which is written before any content, so a composition can "
      "act on the size while the reading is still going on. Reading streams, so "
      "a file larger than memory costs a chunk at a time.";
  schema.inputs.emplace(
      "path", Port("path", "string", "Path of the file to read.",
                   /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           absl::StrCat("All optional: chunk_bytes (65536), offset, length, "
                        "max_bytes, and ",
                        kOmitHelp()),
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "info",
      Port("info", JsonType(),
           "{path, name, kind, exists, size, modified, mode}, written before "
           "any content.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "bytes", Port("bytes", kOctetStream,
                    "The contents, in order, as they are read.",
                    /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "text", Port("text", "string",
                   "The whole file as one text value. Held in memory, so "
                   "`bytes` or `lines` is what a large file wants.",
                   /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "lines", Port("lines", "string",
                    "One value per line, without its line ending. A trailing "
                    "line with no newline after it is still a line.",
                    /*required=*/false, /*unary=*/false));
  AddDeadlineHeader(schema,
                    "The read fails with deadline_exceeded once it is reached, "
                    "because a partial file is not a shorter file.");
  return schema;
}

ActionSchema WriteFileSchema() {
  ActionSchema schema;
  schema.name = std::string(kWriteFileAction);
  schema.description =
      "Write a file from a stream, so that what is written need never be held "
      "in one piece. Atomic by default: the content goes to a temporary beside "
      "the destination and is renamed into place at the end, so a reader sees "
      "either the old contents or the new ones and a cancelled write leaves "
      "neither a partial file nor a temporary. Appending cannot be atomic and "
      "says so.";
  schema.inputs.emplace(
      "path", Port("path", "string", "Path of the file to write.",
                   /*required=*/true, /*unary=*/true));
  // Not "bytes": an input and an output of the same name are one node, so a
  // port called `bytes` in both directions would be a stream writing to itself.
  schema.inputs.emplace(
      "content",
      Port("content", kOctetStream,
           "The contents, in order. Read to its end; the write is the "
           "backpressure point, so a producer is held behind the disk rather "
           "than buffered ahead of it.",
           /*required=*/false, /*unary=*/false));
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           absl::StrCat("All optional: append (false), atomic (true unless "
                        "appending), create_parents (false), sync (false), "
                        "mode (0644), max_bytes, and ",
                        kOmitHelp()),
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "resolved",
      Port("resolved", "string",
           "The absolute path that was written. Named `resolved` rather than "
           "`path` because a port name is one node whichever way it faces.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "bytes_written",
      Port("bytes_written", "integer", "How many bytes were written.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "info", Port("info", JsonType(), "The metadata of the finished file.",
                   /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema,
                    "The write fails once it is reached, and leaves the "
                    "destination as it was.");
  return schema;
}

ActionSchema ListDirectorySchema() {
  ActionSchema schema;
  schema.name = std::string(kListDirectoryAction);
  schema.description =
      "List a directory, one entry at a time as the walk finds them. A tree "
      "with a million files in it costs a composition one entry of memory, and "
      "its first entry arrives before the walk has finished. `truncated` says "
      "whether a limit cut the listing short, because a partial listing that "
      "looks complete is worse than no listing.";
  schema.inputs.emplace(
      "path", Port("path", "string", "Directory to list.",
                   /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           absl::StrCat("All optional: recursive (false), max_depth, hidden "
                        "(false -- whether to report dot files), match (one or "
                        "more `*`/`?` patterns matched against the name), kinds "
                        "(file, directory, symlink, ...), max_entries, and ",
                        kOmitHelp()),
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "entries",
      Port("entries", JsonType(),
           "One {path, name, kind, exists, size, modified, mode, depth} per "
           "entry, as the walk reaches it.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "count", Port("count", "integer", "How many entries were reported.",
                    /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "truncated",
      Port("truncated", "bool",
           "Whether a limit stopped the listing before the walk finished.",
           /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The listing fails once it is reached.");
  return schema;
}

ActionSchema StatPathSchema() {
  ActionSchema schema;
  schema.name = std::string(kStatPathAction);
  schema.description =
      "Read one path's metadata. A path that is not there is an answer -- "
      "`exists` is false -- rather than a failure, so a composition can ask "
      "without wrapping the question in a `try`.";
  schema.inputs.emplace(
      "path", Port("path", "string", "Path to look at.",
                   /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "options", Port("options", JsonType(), absl::StrCat("Optional: ", kOmitHelp()),
                      /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "info",
      Port("info", JsonType(),
           "{path, name, kind, exists, size, modified, mode}.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "exists", Port("exists", "bool", "Whether anything is there.",
                     /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The lookup fails once it is reached.");
  return schema;
}

ActionSchema MakeDirectorySchema() {
  ActionSchema schema;
  schema.name = std::string(kMakeDirectoryAction);
  schema.description =
      "Create a directory, and by default its parents. Finding it already "
      "there is a success with `created` false, because a composition that "
      "wants a directory to exist has got what it wanted.";
  schema.inputs.emplace(
      "path", Port("path", "string", "Directory to create.",
                   /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(), "Optional: parents (true).",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "resolved", Port("resolved", "string", "The absolute path.",
                       /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "created",
      Port("created", "bool",
           "Whether this call made it, as opposed to finding it there.",
           /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

ActionSchema RemovePathSchema() {
  ActionSchema schema;
  schema.name = std::string(kRemovePathAction);
  schema.description =
      "Remove a path. A directory is refused unless options.recursive says "
      "otherwise -- a recursive delete nobody asked for is the most expensive "
      "way for this library to be convenient. A path that is not there is a "
      "success by default, since the composition wanted it gone.";
  schema.inputs.emplace(
      "path", Port("path", "string", "Path to remove.",
                   /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           "Optional: recursive (false), missing_ok (true).",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "removed", Port("removed", "integer", "How many entries were removed.",
                      /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

ActionSchema MovePathSchema() {
  ActionSchema schema;
  schema.name = std::string(kMovePathAction);
  schema.description =
      "Rename a path. Atomic within one filesystem and refused across two, "
      "where it would silently become a copy followed by a delete -- use "
      "copy_path and remove_path when that is what is meant.";
  schema.inputs.emplace(
      "path", Port("path", "string", "Path to move.",
                   /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "to", Port("to", "string", "Where to move it.",
                 /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "options", Port("options", JsonType(), "Optional: overwrite (false).",
                      /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "resolved", Port("resolved", "string", "The absolute destination.",
                       /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

ActionSchema CopyPathSchema() {
  ActionSchema schema;
  schema.name = std::string(kCopyPathAction);
  schema.description =
      "Copy a file or, with options.recursive, a tree. For a copy whose "
      "progress a composition wants to watch, read_file into write_file gives "
      "the same result one chunk at a time.";
  schema.inputs.emplace(
      "path", Port("path", "string", "What to copy.",
                   /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "to", Port("to", "string", "Where to copy it.",
                 /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           "Optional: recursive (false), overwrite (false).",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "resolved", Port("resolved", "string", "The absolute destination.",
                       /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

ActionSchema MakeTempSchema() {
  ActionSchema schema;
  schema.name = std::string(kMakeTempAction);
  schema.description =
      "Make a scratch path nothing else is using, and hand back its name. What "
      "keeps a composition from inventing names in /tmp and colliding with "
      "another run of itself. Where the host restricted the filesystem to a "
      "set of roots, the scratch space is inside the first of them rather than "
      "in the system temporary directory.";
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           "Optional: directory (true -- a directory rather than a file), "
           "prefix (\"a11-\"), suffix, in (which directory to make it in).",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "path", Port("path", "string", "The path that was made.",
                   /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

// ---------------------------------------------------------------------------
// Handlers and registration
// ---------------------------------------------------------------------------

ActionHandler ReadFileHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunReadFile, std::move(capabilities));
}
ActionHandler WriteFileHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunWriteFile, std::move(capabilities));
}
ActionHandler ListDirectoryHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunListDirectory, std::move(capabilities));
}
ActionHandler StatPathHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunStatPath, std::move(capabilities));
}
ActionHandler MakeDirectoryHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunMakeDirectory, std::move(capabilities));
}
ActionHandler RemovePathHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunRemovePath, std::move(capabilities));
}
ActionHandler MovePathHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunMovePath, std::move(capabilities));
}
ActionHandler CopyPathHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunCopyPath, std::move(capabilities));
}
ActionHandler MakeTempHandler(CapabilitiesPtr capabilities) {
  return Handler(&RunMakeTemp, std::move(capabilities));
}

absl::Status RegisterFilesystemReadActions(actions::ActionRegistry& registry,
                                           CapabilitiesPtr capabilities) {
  if (capabilities == nullptr) {
    return absl::InvalidArgumentError("a policy is required");
  }
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kReadFileAction),
                                         ReadFileSchema(),
                                         ReadFileHandler(capabilities)));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kListDirectoryAction),
                                         ListDirectorySchema(),
                                         ListDirectoryHandler(capabilities)));
  return registry.Register(std::string(kStatPathAction), StatPathSchema(),
                           StatPathHandler(std::move(capabilities)));
}

absl::Status RegisterFilesystemWriteActions(actions::ActionRegistry& registry,
                                            CapabilitiesPtr capabilities) {
  if (capabilities == nullptr) {
    return absl::InvalidArgumentError("a policy is required");
  }
  if (!capabilities->filesystem.writable) {
    // Registering six actions that all refuse would be a host believing it
    // allowed writing. Better to say so where the mistake was made.
    return absl::FailedPreconditionError(
        "the filesystem policy is not writable, so registering the writing "
        "actions would register six actions that all refuse");
  }
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kWriteFileAction),
                                         WriteFileSchema(),
                                         WriteFileHandler(capabilities)));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kMakeDirectoryAction),
                                         MakeDirectorySchema(),
                                         MakeDirectoryHandler(capabilities)));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kRemovePathAction),
                                         RemovePathSchema(),
                                         RemovePathHandler(capabilities)));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kMovePathAction),
                                         MovePathSchema(),
                                         MovePathHandler(capabilities)));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kCopyPathAction),
                                         CopyPathSchema(),
                                         CopyPathHandler(capabilities)));
  return registry.Register(std::string(kMakeTempAction), MakeTempSchema(),
                           MakeTempHandler(std::move(capabilities)));
}

}  // namespace a11::sdk::flow
