// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/sandbox.h"

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_replace.h>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace a11::sdk::flow {
namespace {

#if defined(__linux__)

// Declared here rather than included from <linux/landlock.h>.
//
// The build images this ships from are older than the headers -- a manylinux
// image's kernel headers predate Landlock by years -- while the kernels it runs
// on are not. Taking the ABI from the header would mean the sandbox silently
// vanishes from a wheel built on an old image and running on a new kernel, which
// is the exact failure this file exists to prevent. The ABI is stable and
// versioned by the kernel itself, so it is written out and probed at runtime.

#ifndef SYS_landlock_create_ruleset
#if defined(__x86_64__)
constexpr long kLandlockCreateRuleset = 444;
constexpr long kLandlockAddRule = 445;
constexpr long kLandlockRestrictSelf = 446;
#elif defined(__aarch64__)
constexpr long kLandlockCreateRuleset = 444;
constexpr long kLandlockAddRule = 445;
constexpr long kLandlockRestrictSelf = 446;
#else
constexpr long kLandlockCreateRuleset = -1;
constexpr long kLandlockAddRule = -1;
constexpr long kLandlockRestrictSelf = -1;
#endif
#else
constexpr long kLandlockCreateRuleset = SYS_landlock_create_ruleset;
constexpr long kLandlockAddRule = SYS_landlock_add_rule;
constexpr long kLandlockRestrictSelf = SYS_landlock_restrict_self;
#endif

constexpr std::uint32_t kCreateRulesetVersion = 1U << 0;

struct LandlockRulesetAttr {
  std::uint64_t handled_access_fs;
  std::uint64_t handled_access_net;
};

struct LandlockPathBeneathAttr {
  std::uint64_t allowed_access;
  std::int32_t parent_fd;
} __attribute__((packed));

// The filesystem rights, ABI 1 through 5. Named rather than included for the
// reason above.
constexpr std::uint64_t kAccessFsExecute = 1ULL << 0;
constexpr std::uint64_t kAccessFsWriteFile = 1ULL << 1;
constexpr std::uint64_t kAccessFsReadFile = 1ULL << 2;
constexpr std::uint64_t kAccessFsReadDir = 1ULL << 3;
constexpr std::uint64_t kAccessFsRemoveDir = 1ULL << 4;
constexpr std::uint64_t kAccessFsRemoveFile = 1ULL << 5;
constexpr std::uint64_t kAccessFsMakeChar = 1ULL << 6;
constexpr std::uint64_t kAccessFsMakeDir = 1ULL << 7;
constexpr std::uint64_t kAccessFsMakeReg = 1ULL << 8;
constexpr std::uint64_t kAccessFsMakeSock = 1ULL << 9;
constexpr std::uint64_t kAccessFsMakeFifo = 1ULL << 10;
constexpr std::uint64_t kAccessFsMakeBlock = 1ULL << 11;
constexpr std::uint64_t kAccessFsMakeSym = 1ULL << 12;
constexpr std::uint64_t kAccessFsRefer = 1ULL << 13;   // ABI 2
constexpr std::uint64_t kAccessFsTruncate = 1ULL << 14;  // ABI 3
constexpr std::uint64_t kAccessFsIoctlDev = 1ULL << 15;  // ABI 5

constexpr std::uint64_t kAccessNetBindTcp = 1ULL << 0;   // ABI 4
constexpr std::uint64_t kAccessNetConnectTcp = 1ULL << 1;

constexpr int kRuleTypePathBeneath = 1;

/// The rights this library asks the kernel to arbitrate, given @p abi.
///
/// Only handled rights are restricted: anything left out of this mask is
/// unaffected by the ruleset, so asking for a right the running kernel does not
/// know about makes the whole call fail. Hence the version gating.
std::uint64_t HandledFilesystemRights(int abi) {
  std::uint64_t handled =
      kAccessFsExecute | kAccessFsWriteFile | kAccessFsReadFile |
      kAccessFsReadDir | kAccessFsRemoveDir | kAccessFsRemoveFile |
      kAccessFsMakeChar | kAccessFsMakeDir | kAccessFsMakeReg |
      kAccessFsMakeSock | kAccessFsMakeFifo | kAccessFsMakeBlock |
      kAccessFsMakeSym;
  if (abi >= 2) {
    handled |= kAccessFsRefer;
  }
  if (abi >= 3) {
    handled |= kAccessFsTruncate;
  }
  if (abi >= 5) {
    handled |= kAccessFsIoctlDev;
  }
  return handled;
}

std::uint64_t ReadRights(int abi) {
  std::uint64_t rights = kAccessFsReadFile | kAccessFsReadDir;
  (void)abi;
  return rights;
}

std::uint64_t WriteRights(int abi) {
  std::uint64_t rights = kAccessFsWriteFile | kAccessFsMakeReg |
                         kAccessFsMakeDir | kAccessFsRemoveFile |
                         kAccessFsRemoveDir | kAccessFsMakeSym |
                         kAccessFsMakeFifo | kAccessFsMakeSock;
  if (abi >= 2) {
    // Without this a rename or a link across two directories in the ruleset is
    // refused, which is what `write_file`'s atomic temp-and-rename does.
    rights |= kAccessFsRefer;
  }
  if (abi >= 3) {
    rights |= kAccessFsTruncate;
  }
  return rights;
}

long CreateRuleset(const LandlockRulesetAttr* attributes, std::size_t size,
                   std::uint32_t flags) {
  if (kLandlockCreateRuleset < 0) {
    errno = ENOSYS;
    return -1;
  }
  return ::syscall(kLandlockCreateRuleset, attributes, size, flags);
}

long AddRule(int ruleset_fd, int rule_type, const void* attributes,
             std::uint32_t flags) {
  return ::syscall(kLandlockAddRule, ruleset_fd, rule_type, attributes, flags);
}

int ProbeAbiVersion() {
  const long version = CreateRuleset(nullptr, 0, kCreateRulesetVersion);
  return version < 0 ? 0 : static_cast<int>(version);
}

#endif  // __linux__

#if defined(__APPLE__)

/// Quotes a path for a Seatbelt profile literal.
///
/// The profile is a small Scheme-like language, and a path is a string literal
/// in it. A path holding a quote or a backslash would otherwise end the literal
/// early and turn the rest of the path into profile source -- which for a
/// path a flow chose is an injection into the very thing meant to contain it.
std::string QuoteForProfile(std::string_view path) {
  return absl::StrCat(
      "\"", absl::StrReplaceAll(path, {{"\\", "\\\\"}, {"\"", "\\\""}}), "\"");
}

#endif  // __APPLE__

}  // namespace

const SandboxAvailability& Availability() {
  static const SandboxAvailability probed = []() {
    SandboxAvailability found;
#if defined(__linux__)
    found.abi_version = ProbeAbiVersion();
    if (found.abi_version <= 0) {
      found.why_not =
          "this kernel has no Landlock (it arrived in 5.13), or it is disabled";
      return found;
    }
    found.kind = SandboxKind::kLandlock;
    found.confines_reads = true;
    found.confines_writes = true;
    // Network rules arrived in ABI 4. Below that a confined child can still
    // open a socket, which is worth saying rather than implying.
    found.confines_network = found.abi_version >= 4;
#elif defined(__APPLE__)
    if (::access("/usr/bin/sandbox-exec", X_OK) != 0) {
      found.why_not = "/usr/bin/sandbox-exec is not there";
      return found;
    }
    found.kind = SandboxKind::kSeatbelt;
    // Reads are *not* confined here. See the comment on SandboxAvailability:
    // the profile imports Apple's own base profile so that a dynamically linked
    // program can start at all, and that base profile permits broad reads. The
    // honest options were to overclaim or to say so.
    found.confines_reads = false;
    found.confines_writes = true;
    found.confines_network = true;
#else
    found.why_not = "no sandbox mechanism is implemented for this platform";
#endif
    return found;
  }();
  return probed;
}

Sandbox::~Sandbox() {
  if (ruleset_fd_ >= 0) {
    ::close(ruleset_fd_);
    ruleset_fd_ = -1;
  }
}

absl::StatusOr<std::shared_ptr<Sandbox>> Sandbox::Prepare(
    const Capabilities& capabilities, std::string_view program) {
  auto sandbox = std::shared_ptr<Sandbox>(new Sandbox());
  const ProcessPolicy& process = capabilities.process;
  const FilesystemPolicy& filesystem = capabilities.filesystem;

  const bool wanted = process.sandbox != SandboxRequest::kNever;
  if (!wanted) {
    sandbox->description_ = "not requested";
    return sandbox;
  }
  // An unrestricted filesystem policy has no roots to confine anything to, and
  // a sandbox that allows everything is a sandbox in name only. Said plainly
  // rather than prepared and reported as active.
  if (filesystem.unrestricted || filesystem.roots.empty()) {
    if (process.sandbox == SandboxRequest::kRequired) {
      return absl::FailedPreconditionError(
          "the process policy requires a sandbox, but the filesystem policy "
          "names no roots to confine the child to; set roots, or set "
          "sandbox: preferred");
    }
    sandbox->description_ = "no roots to confine to";
    return sandbox;
  }

  const SandboxAvailability& available = Availability();
  if (available.kind == SandboxKind::kNone) {
    if (process.sandbox == SandboxRequest::kRequired) {
      // The whole point of `required`: a host that asked for kernel enforcement
      // must not silently get a policy that is only this library's checks.
      return absl::FailedPreconditionError(absl::StrCat(
          "the process policy requires a sandbox and this system cannot "
          "provide one: ",
          available.why_not));
    }
    sandbox->description_ = absl::StrCat("unavailable: ", available.why_not);
    return sandbox;
  }

#if defined(__linux__)
  const int abi = available.abi_version;
  LandlockRulesetAttr attributes{};
  attributes.handled_access_fs = HandledFilesystemRights(abi);
  if (abi >= 4 && !capabilities.network.enabled) {
    attributes.handled_access_net = kAccessNetBindTcp | kAccessNetConnectTcp;
  }
  const std::size_t attribute_size =
      abi >= 4 ? sizeof(attributes) : sizeof(attributes.handled_access_fs);
  const long ruleset = CreateRuleset(&attributes, attribute_size, 0);
  if (ruleset < 0) {
    return absl::UnavailableError(absl::StrCat(
        "cannot create a Landlock ruleset: ", std::strerror(errno)));
  }
  sandbox->ruleset_fd_ = static_cast<int>(ruleset);

  const auto allow = [&sandbox, abi](std::string_view path,
                                     std::uint64_t rights) -> absl::Status {
    const int fd = ::open(std::string(path).c_str(),
                          O_PATH | O_CLOEXEC | O_DIRECTORY);
    // A root that is not there cannot be granted, and is not a reason to refuse
    // to run: the policy already refuses paths under it.
    const int file_fd =
        fd >= 0 ? fd : ::open(std::string(path).c_str(), O_PATH | O_CLOEXEC);
    if (file_fd < 0) {
      return absl::OkStatus();
    }
    LandlockPathBeneathAttr rule{};
    rule.allowed_access = rights & HandledFilesystemRights(abi);
    rule.parent_fd = file_fd;
    const long added =
        AddRule(sandbox->ruleset_fd_, kRuleTypePathBeneath, &rule, 0);
    const int reason = errno;
    ::close(file_fd);
    if (added < 0) {
      return absl::UnavailableError(
          absl::StrCat("cannot allow '", path, "' through Landlock: ",
                       std::strerror(reason)));
    }
    return absl::OkStatus();
  };

  for (const std::string& root : filesystem.roots) {
    std::uint64_t rights = ReadRights(abi);
    if (filesystem.writable) {
      rights |= WriteRights(abi);
    }
    ABSL_RETURN_IF_ERROR(allow(root, rights));
  }
  // The program itself, and the libraries it needs to start at all. Read and
  // execute only: a child that cannot read /usr/lib cannot exec anything, and a
  // sandbox that stops the program from starting is not a useful sandbox.
  for (const std::string_view essential :
       {"/usr", "/lib", "/lib64", "/bin", "/sbin", "/etc/ld.so.cache",
        "/etc/ld.so.preload", "/proc/self", "/dev/null", "/dev/urandom",
        "/dev/zero"}) {
    ABSL_RETURN_IF_ERROR(
        allow(essential, kAccessFsReadFile | kAccessFsReadDir |
                             kAccessFsExecute));
  }
  if (!program.empty() && program.front() == '/') {
    ABSL_RETURN_IF_ERROR(
        allow(program, kAccessFsReadFile | kAccessFsExecute));
  }
  sandbox->kind_ = SandboxKind::kLandlock;
  sandbox->description_ = absl::StrCat(
      "landlock abi ", abi, " over ", filesystem.roots.size(),
      filesystem.roots.size() == 1 ? " root" : " roots",
      filesystem.writable ? " (read/write)" : " (read-only)",
      available.confines_network && !capabilities.network.enabled
          ? ", tcp refused"
          : ", network not confined");
  return sandbox;

#elif defined(__APPLE__)
  // Built before the fork, and passed as argv: sandbox_init() allocates, and
  // the child of a fork in a threaded process must not.
  std::vector<std::string> lines;
  lines.emplace_back("(version 1)");
  // Apple's own base profile, and the reason reads are not confined here.
  //
  // Without it a deny-default profile aborts a dynamically linked program
  // during dyld startup -- allowing /usr/lib, /System and the Cryptexes paths
  // was measured to be insufficient, and what remains is undocumented and
  // version-specific. With it the program starts, and its *reads* are broadly
  // permitted by the import. So writes and the network are genuinely confined
  // and reads are not, which is what Availability() reports.
  lines.emplace_back("(import \"bsd.sb\")");
  lines.emplace_back("(deny default)");
  lines.emplace_back("(allow process-exec)");
  lines.emplace_back("(allow process-fork)");
  lines.emplace_back("(allow signal (target self))");
  // Denied before anything is allowed back, so a root that happens to contain a
  // socket does not become a way out.
  if (!capabilities.network.enabled) {
    lines.emplace_back("(deny network*)");
  } else {
    lines.emplace_back("(allow network-outbound)");
    lines.emplace_back("(allow system-socket)");
  }

  std::vector<std::string> subpaths;
  subpaths.reserve(filesystem.roots.size());
  for (const std::string& root : filesystem.roots) {
    subpaths.push_back(absl::StrCat("(subpath ", QuoteForProfile(root), ")"));
  }
  const std::string roots = absl::StrJoin(subpaths, " ");
  lines.push_back(absl::StrCat("(allow file-read* ", roots, ")"));
  if (filesystem.writable) {
    lines.push_back(absl::StrCat("(allow file-write* ", roots, ")"));
  }
  // Its own output, which is the one thing the child is certainly meant to do.
  lines.emplace_back("(allow file-write* (literal \"/dev/null\"))");
  lines.emplace_back(
      "(allow file-read* file-write* (literal \"/dev/stdout\") "
      "(literal \"/dev/stderr\") (literal \"/dev/stdin\"))");
  if (!program.empty() && program.front() == '/') {
    lines.push_back(absl::StrCat("(allow file-read* (literal ",
                                 QuoteForProfile(program), "))"));
  }
  sandbox->profile_ = absl::StrJoin(lines, "\n");
  sandbox->kind_ = SandboxKind::kSeatbelt;
  sandbox->description_ = absl::StrCat(
      "seatbelt over ", filesystem.roots.size(),
      filesystem.roots.size() == 1 ? " root" : " roots", ": ",
      filesystem.writable ? "writes confined" : "read-only, writes refused",
      capabilities.network.enabled ? ", network allowed" : ", network refused",
      // Said in the description a flow can read, not only in a header comment.
      ", reads NOT confined (macOS)");
  return sandbox;
#else
  return sandbox;
#endif
}

int Sandbox::Apply() const {
#if defined(__linux__)
  if (kind_ != SandboxKind::kLandlock || ruleset_fd_ < 0) {
    return 0;
  }
  // Both of these are bare syscalls against things prepared before the fork,
  // which is what makes this frame safe to be in after one.
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return errno;
  }
  if (::syscall(kLandlockRestrictSelf, ruleset_fd_, 0) != 0) {
    return errno;
  }
  return 0;
#else
  // Elsewhere confinement is applied by what is exec'd, not by this frame.
  return 0;
#endif
}

std::vector<std::string> Sandbox::WrapCommand(
    std::string_view program,
    const std::vector<std::string>& arguments) const {
  if (kind_ != SandboxKind::kSeatbelt) {
    return arguments;
  }
  std::vector<std::string> wrapped;
  wrapped.reserve(arguments.size() + 3);
  wrapped.emplace_back("-p");
  wrapped.push_back(profile_);
  wrapped.emplace_back(program);
  for (const std::string& argument : arguments) {
    wrapped.push_back(argument);
  }
  return wrapped;
}

std::string Sandbox::WrapProgram(std::string_view program) const {
  if (kind_ != SandboxKind::kSeatbelt) {
    return std::string(program);
  }
  return "/usr/bin/sandbox-exec";
}

std::string Sandbox::Describe() const { return description_; }

}  // namespace a11::sdk::flow
