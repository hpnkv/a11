// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/policy.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>
#include <absl/strings/strip.h>

namespace a11::sdk::flow {
namespace {

/// Every std::filesystem call here takes an error_code: the throwing overloads
/// are the default, and this library is compiled without exceptions.
absl::StatusOr<std::filesystem::path> Canonical(
    const std::filesystem::path& path) {
  std::error_code error;
  // Weakly, because a path being written to does not exist yet. It resolves
  // symlinks and `..` across the part that does exist, which is where an escape
  // out of a root would be hiding.
  std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
  if (error) {
    return absl::InvalidArgumentError(
        absl::StrCat("cannot resolve '", path.string(), "': ", error.message()));
  }
  return resolved;
}

/// Whether @p path is @p root or below it, on resolved paths only.
///
/// Compared component by component rather than by string prefix, because
/// `/srv/data-2` starts with `/srv/data` and is not inside it.
bool IsWithin(const std::filesystem::path& path,
              const std::filesystem::path& root) {
  auto path_part = path.begin();
  auto root_part = root.begin();
  for (; root_part != root.end(); ++root_part, ++path_part) {
    if (path_part == path.end() || *path_part != *root_part) {
      return false;
    }
  }
  return true;
}

/// Matches a host against a pattern in which `*` stands for one label:
/// `*.example.com` matches `api.example.com` and not `example.com`.
bool MatchesHostPattern(std::string_view host, std::string_view pattern) {
  if (pattern == "*") {
    return true;
  }
  const std::vector<std::string_view> host_labels = absl::StrSplit(host, '.');
  const std::vector<std::string_view> pattern_labels =
      absl::StrSplit(pattern, '.');
  if (host_labels.size() != pattern_labels.size()) {
    return false;
  }
  for (std::size_t i = 0; i < host_labels.size(); ++i) {
    if (pattern_labels[i] == "*") {
      continue;
    }
    if (!absl::EqualsIgnoreCase(host_labels[i], pattern_labels[i])) {
      return false;
    }
  }
  return true;
}

/// The four IPv4 octets of @p address, or nullopt when it is not one.
std::optional<std::array<std::uint32_t, 4>> Ipv4Octets(
    std::string_view address) {
  const std::vector<std::string_view> parts = absl::StrSplit(address, '.');
  if (parts.size() != 4) {
    return std::nullopt;
  }
  std::array<std::uint32_t, 4> octets{};
  for (std::size_t i = 0; i < 4; ++i) {
    std::uint32_t value = 0;
    if (!absl::SimpleAtoi(parts[i], &value) || value > 255) {
      return std::nullopt;
    }
    octets[i] = value;
  }
  return octets;
}

std::string StrippedIpv6(std::string_view address) {
  std::string_view stripped = address;
  if (absl::ConsumePrefix(&stripped, "[")) {
    (void)absl::ConsumeSuffix(&stripped, "]");
  }
  // A scope id (`fe80::1%en0`) is not part of the address for these purposes.
  const std::size_t scope = stripped.find('%');
  return absl::AsciiStrToLower(
      scope == std::string_view::npos ? stripped : stripped.substr(0, scope));
}

}  // namespace

absl::StatusOr<std::filesystem::path> ResolvePath(
    const FilesystemPolicy& policy, std::string_view path, bool for_write) {
  if (path.empty()) {
    return absl::InvalidArgumentError("a path is required");
  }
  if (for_write && !policy.writable) {
    return absl::PermissionDeniedError(
        "this host registered the filesystem actions read-only");
  }
  ABSL_ASSIGN_OR_RETURN(const std::filesystem::path resolved,
                        Canonical(std::filesystem::path(path)));
  if (policy.unrestricted) {
    return resolved;
  }
  if (policy.roots.empty()) {
    return absl::PermissionDeniedError(
        "this host registered the filesystem actions with no readable roots");
  }
  for (const std::string& root : policy.roots) {
    absl::StatusOr<std::filesystem::path> canonical_root =
        Canonical(std::filesystem::path(root));
    if (!canonical_root.ok()) {
      continue;  // A root that does not resolve cannot contain anything.
    }
    if (IsWithin(resolved, *canonical_root)) {
      return resolved;
    }
  }
  // Naming the roots and not what is outside them: which paths exist elsewhere
  // is not something a caller should learn from an error message.
  return absl::PermissionDeniedError(
      absl::StrCat("'", path, "' is outside every allowed root (",
                   absl::StrJoin(policy.roots, ", "), ")"));
}

absl::StatusOr<std::string> ResolveProgram(const ProcessPolicy& policy,
                                           std::string_view program) {
  if (program.empty()) {
    return absl::InvalidArgumentError("a program is required");
  }
  if (!policy.enabled) {
    return absl::PermissionDeniedError(
        "this host did not register the process actions");
  }
  if (policy.any_program) {
    return std::string(program);
  }
  for (const std::string& allowed : policy.programs) {
    // Either spelling matches: a policy naming `/bin/ls` accepts `ls`, and one
    // naming `ls` accepts an absolute path ending in it. Anything looser would
    // let `../../bin/sh` through under the name of something else.
    if (allowed == program ||
        std::filesystem::path(allowed).filename() ==
            std::filesystem::path(program).filename()) {
      return allowed;
    }
  }
  return absl::PermissionDeniedError(absl::StrCat(
      "'", program, "' is not one of the programs this host allows (",
      absl::StrJoin(policy.programs, ", "), ")"));
}

absl::Status CheckEnvironment(const EnvironmentPolicy& policy,
                              std::string_view name) {
  if (policy.any_name) {
    return absl::OkStatus();
  }
  for (const std::string& allowed : policy.names) {
    if (allowed == name) {
      return absl::OkStatus();
    }
  }
  return absl::PermissionDeniedError(absl::StrCat(
      "'", name, "' is not one of the environment variables this host exposes"));
}

absl::Status CheckHost(const NetworkPolicy& policy, std::string_view host) {
  if (host.empty()) {
    return absl::InvalidArgumentError("a host is required");
  }
  if (!policy.enabled) {
    return absl::PermissionDeniedError(
        "this host did not register the network actions");
  }
  if (!policy.any_host) {
    const bool matched = std::any_of(
        policy.hosts.begin(), policy.hosts.end(),
        [host](const std::string& pattern) {
          return MatchesHostPattern(host, pattern);
        });
    if (!matched) {
      return absl::PermissionDeniedError(absl::StrCat(
          "'", host, "' is not one of the hosts this host allows (",
          absl::StrJoin(policy.hosts, ", "), ")"));
    }
  }
  // A name may still resolve to an address the policy refuses; that is checked
  // once it is known. What can be decided from the spelling is decided now.
  return CheckAddress(policy, host);
}

absl::Status CheckAddress(const NetworkPolicy& policy,
                          std::string_view address) {
  const auto refuse = [address](std::string_view kind) {
    return absl::PermissionDeniedError(absl::StrCat(
        "'", address, "' is ", kind,
        ", which this host did not allow. A flow that can reach these can "
        "reach services that trust the network they are on."));
  };

  if (const std::optional<std::array<std::uint32_t, 4>> octets =
          Ipv4Octets(address);
      octets.has_value()) {
    const auto& parts = *octets;
    if (parts[0] == 127 && !policy.allow_loopback) {
      return refuse("a loopback address");
    }
    if (parts[0] == 169 && parts[1] == 254 && !policy.allow_link_local) {
      // 169.254.169.254 is a cloud instance's credential endpoint. This is the
      // single most valuable address to an attacker holding a flow that fetches.
      return refuse("a link-local address");
    }
    if (!policy.allow_private) {
      const bool private_range =
          parts[0] == 10 || (parts[0] == 172 && parts[1] >= 16 &&
                             parts[1] <= 31) ||
          (parts[0] == 192 && parts[1] == 168);
      if (private_range) {
        return refuse("a private address");
      }
    }
    return absl::OkStatus();
  }

  const std::string lowered = StrippedIpv6(address);
  if (lowered == "localhost" && !policy.allow_loopback) {
    return refuse("a loopback name");
  }
  if ((lowered == "::1" || lowered == "::ffff:127.0.0.1") &&
      !policy.allow_loopback) {
    return refuse("a loopback address");
  }
  if (absl::StartsWith(lowered, "fe80:") && !policy.allow_link_local) {
    return refuse("a link-local address");
  }
  // Unique local addresses, fc00::/7: the IPv6 spelling of a private range.
  if ((absl::StartsWith(lowered, "fc") || absl::StartsWith(lowered, "fd")) &&
      lowered.find(':') != std::string::npos && !policy.allow_private) {
    return refuse("a private address");
  }
  return absl::OkStatus();
}

absl::Status CheckListen(const NetworkPolicy& policy, std::uint32_t port) {
  if (!policy.may_listen) {
    return absl::PermissionDeniedError(
        "this host did not register the listening actions");
  }
  if (port > 65535) {
    return absl::InvalidArgumentError(
        absl::StrCat("port must be below 65536, got ", port));
  }
  if (policy.listen_ports.empty()) {
    return absl::OkStatus();
  }
  // Port 0 asks the operating system to choose, so it is checked against the
  // list only when the host named an explicit set -- in which case "any port"
  // is exactly what was refused.
  if (std::find(policy.listen_ports.begin(), policy.listen_ports.end(), port) !=
      policy.listen_ports.end()) {
    return absl::OkStatus();
  }
  return absl::PermissionDeniedError(absl::StrCat(
      "port ", port, " is not one of the ports this host allows (",
      absl::StrJoin(policy.listen_ports, ", "), ")"));
}

}  // namespace a11::sdk::flow
