// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdk/http/render/web_render.h"

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>

#if defined(__APPLE__) || defined(__linux__)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sdk/http/render/protocol.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"

#endif

namespace a11::sdk::http {

#if !defined(__APPLE__) && !defined(__linux__)

absl::StatusOr<WebRenderResult> RenderWebPage(const WebRenderRequest&) {
  return absl::UnimplementedError(
      "web-render is not implemented on this platform");
}

#else
namespace {

#if defined(__APPLE__)
constexpr char kHelperName[] = "a11-web-render-webkit";
#else
constexpr char kHelperName[] = "a11-web-render-wpe";
#endif
const int kLibraryAnchor = 0;

class FileDescriptor {
 public:
  FileDescriptor() = default;

  explicit FileDescriptor(int value) : value_(value) {}

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept
      : value_(std::exchange(other.value_, -1)) {}

  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      Reset();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  ~FileDescriptor() { Reset(); }

  [[nodiscard]] int get() const { return value_; }

  int Release() { return std::exchange(value_, -1); }

  void Reset(int value = -1) {
    if (value_ >= 0) {
      (void)::close(value_);
    }
    value_ = value;
  }

 private:
  int value_ = -1;
};

struct Pipe {
  FileDescriptor read;
  FileDescriptor write;
};

absl::StatusOr<Pipe> MakePipe() {
  int values[2] = {-1, -1};
  if (::pipe(values) != 0) {
    return absl::InternalError(
        absl::StrCat("cannot create web-render pipe: ", std::strerror(errno)));
  }
  (void)::fcntl(values[0], F_SETFD, FD_CLOEXEC);
  (void)::fcntl(values[1], F_SETFD, FD_CLOEXEC);
  return Pipe{.read = FileDescriptor(values[0]),
              .write = FileDescriptor(values[1])};
}

bool IsExecutable(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::status(path, error);
  return !error && std::filesystem::is_regular_file(status) &&
         ::access(path.c_str(), X_OK) == 0;
}

absl::StatusOr<std::filesystem::path> HelperPath() {
  if (const char* configured = std::getenv("A11_WEB_RENDER_HELPER");
      configured != nullptr && *configured != '\0') {
    const std::filesystem::path path(configured);
    if (!path.is_absolute()) {
      return absl::InvalidArgumentError(
          "A11_WEB_RENDER_HELPER must be an absolute path");
    }
    if (!IsExecutable(path)) {
      return absl::FailedPreconditionError(absl::StrCat(
          "A11_WEB_RENDER_HELPER is not executable: ", path.string()));
    }
    return path;
  }

  Dl_info info{};
  if (::dladdr(&kLibraryAnchor, &info) != 0 && info.dli_fname != nullptr) {
    const std::filesystem::path module(info.dli_fname);
    for (const std::filesystem::path& candidate :
         {module.parent_path() / "libexec" / kHelperName,
          module.parent_path() / kHelperName,
          module.parent_path().parent_path() / "libexec" / "a11" /
              kHelperName}) {
      if (IsExecutable(candidate)) {
        return candidate;
      }
    }
  }
#if defined(__APPLE__)
  return absl::FailedPreconditionError(
      "web-render helper is missing; reinstall or upgrade the A11 package");
#else
  return absl::FailedPreconditionError(
      "web-render requires a11-web-render-wpe and the system pkg-config "
      "modules wpe-webkit-2.0 >= 2.52, wpe-platform-2.0, and "
      "wpe-platform-headless-2.0; install WPEWebKit with WPEPlatform and its "
      "headless display, then build A11 with A11_BUILD_WEB_RENDER_WPE=ON");
#endif
}

void StopChild(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  if (::killpg(pid, SIGTERM) != 0 && errno == ESRCH) {
    (void)::kill(pid, SIGTERM);
  }
  int status = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid || (waited < 0 && errno == ECHILD)) {
      return;
    }
    if (thread::GetPerThreadFiberPtr() != nullptr) {
      thread::SleepFor(absl::Milliseconds(10));
    } else {
      ::usleep(10 * 1000);
    }
  }
  if (::killpg(pid, SIGKILL) != 0 && errno == ESRCH) {
    (void)::kill(pid, SIGKILL);
  }
  (void)::waitpid(pid, &status, 0);
}

absl::Status WaitForChild(pid_t pid, absl::Time deadline) {
  int status = 0;
  while (true) {
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return absl::OkStatus();
      }
      return absl::UnavailableError("web-render helper exited unsuccessfully");
    }
    if (waited < 0) {
      return absl::UnavailableError(absl::StrCat(
          "cannot wait for web-render helper: ", std::strerror(errno)));
    }
    if (absl::Now() >= deadline) {
      return absl::DeadlineExceededError(
          "web-render helper did not exit before its deadline");
    }
    if (thread::GetPerThreadFiberPtr() != nullptr) {
      thread::SleepFor(absl::Milliseconds(10));
    } else {
      ::usleep(10 * 1000);
    }
  }
}

absl::Status ErrorStatus(const nlohmann::json& message) {
  const std::string text =
      message.value("message", std::string("web-render helper failed"));
  const std::string code = message.value("code", std::string("INTERNAL"));
  if (code == "INVALID_ARGUMENT") {
    return absl::InvalidArgumentError(text);
  }
  if (code == "DEADLINE_EXCEEDED") {
    return absl::DeadlineExceededError(text);
  }
  if (code == "RESOURCE_EXHAUSTED") {
    return absl::ResourceExhaustedError(text);
  }
  if (code == "UNIMPLEMENTED") {
    return absl::UnimplementedError(text);
  }
  if (code == "UNAVAILABLE") {
    return absl::UnavailableError(text);
  }
  if (code == "FAILED_PRECONDITION") {
    return absl::FailedPreconditionError(text);
  }
  return absl::InternalError(text);
}

absl::StatusOr<nlohmann::json> NextControl(int fd, absl::Time deadline) {
  ABSL_ASSIGN_OR_RETURN(render_protocol::Frame frame,
                        render_protocol::ReadFrame(fd, deadline));
  return render_protocol::ParseControl(frame);
}

absl::StatusOr<std::string> ReadArtifact(int fd, absl::Time deadline,
                                         size_t limit) {
  std::string bytes;
  while (true) {
    ABSL_ASSIGN_OR_RETURN(render_protocol::Frame frame,
                          render_protocol::ReadFrame(fd, deadline));
    if (frame.kind == render_protocol::FrameKind::kData) {
      if (bytes.size() + frame.payload.size() > limit) {
        return absl::ResourceExhaustedError(
            "web-render helper output exceeds its configured limit");
      }
      bytes.append(frame.payload);
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(nlohmann::json control,
                          render_protocol::ParseControl(frame));
    if (!control.is_object() || !control.contains("type")) {
      return absl::InvalidArgumentError(
          "web-render helper sent an invalid artifact frame");
    }
    if (control.at("type") == "artifact_end") {
      return bytes;
    }
    if (control.at("type") == "error") {
      return ErrorStatus(control);
    }
    return absl::InvalidArgumentError(
        "web-render helper ended an artifact incorrectly");
  }
}

absl::StatusOr<WebRenderResult> ExchangeWithHelper(
    const WebRenderRequest& request, int input_fd, int output_fd) {
  absl::StatusOr<nlohmann::json> hello_result =
      NextControl(output_fd, request.deadline);
  if (!hello_result.ok()) {
#if defined(__linux__)
    return absl::FailedPreconditionError(absl::StrCat(
        "a11-web-render-wpe did not start; verify that wpe-webkit-2.0 >= "
        "2.52, wpe-platform-2.0, and wpe-platform-headless-2.0 are "
        "installed and visible to the dynamic loader: ",
        hello_result.status().message()));
#else
    return hello_result.status();
#endif
  }
  nlohmann::json hello = std::move(*hello_result);
  if (!hello.is_object() || hello.value("type", "") != "hello" ||
      hello.value("protocol", "") != render_protocol::kVersion) {
    return absl::FailedPreconditionError(
        "web-render helper uses an incompatible protocol");
  }

  nlohmann::json headers = nlohmann::json::array();
  for (const auto& [name, value] : request.headers) {
    headers.push_back(nlohmann::json::array({name, value}));
  }
  const std::int64_t timeout_ms = std::max<std::int64_t>(
      1, absl::ToInt64Milliseconds(request.deadline - absl::Now()));
  const nlohmann::json command{
      {"type", "render"},
      {"url", request.url},
      {"headers", std::move(headers)},
      {"user_agent", request.user_agent},
      {"timeout_ms", timeout_ms},
      {"max_redirects", request.max_redirects},
      {"max_body_bytes", request.max_body_bytes},
      {"include_image", request.include_image},
      {"image_screen_heights", request.image_screen_heights},
      {"max_image_bytes", request.max_image_bytes}};
  ABSL_RETURN_IF_ERROR(render_protocol::WriteControl(input_fd, command));

  WebRenderResult result;
  bool got_response = false;
  bool got_html = false;
  bool got_text = false;
  while (true) {
    ABSL_ASSIGN_OR_RETURN(nlohmann::json control,
                          NextControl(output_fd, request.deadline));
    if (!control.is_object()) {
      return absl::InvalidArgumentError(
          "web-render helper sent a non-object control frame");
    }
    const std::string type = control.value("type", std::string());
    if (type == "error") {
      return ErrorStatus(control);
    }
    if (type == "response") {
      if (got_response) {
        return absl::InvalidArgumentError(
            "web-render helper sent two response frames");
      }
      got_response = true;
      result.final_url = control.value("final_url", request.url);
      result.status_code = control.value("status_code", 0);
      if (control.contains("headers") && control.at("headers").is_array()) {
        for (const nlohmann::json& field : control.at("headers")) {
          if (field.is_array() && field.size() == 2 && field[0].is_string() &&
              field[1].is_string()) {
            result.headers.emplace_back(field[0].get<std::string>(),
                                        field[1].get<std::string>());
          }
        }
      }
      if (request.response_observer != nullptr) {
        WebRenderResponse response{.final_url = result.final_url,
                                   .status_code = result.status_code,
                                   .headers = result.headers};
        ABSL_RETURN_IF_ERROR(request.response_observer->OnResponse(response));
      }
      continue;
    }
    if (type == "artifact_begin") {
      if (!got_response) {
        return absl::InvalidArgumentError(
            "web-render helper sent an artifact before its response");
      }
      const std::string role = control.value("role", std::string());
      if (role == "html" && !got_html) {
        ABSL_ASSIGN_OR_RETURN(
            result.html,
            ReadArtifact(output_fd, request.deadline, request.max_body_bytes));
        got_html = true;
      } else if (role == "text" && !got_text) {
        ABSL_ASSIGN_OR_RETURN(
            result.text,
            ReadArtifact(output_fd, request.deadline, request.max_body_bytes));
        got_text = true;
      } else if (role == "snapshot" && request.include_image &&
                 result.image_png.empty()) {
        ABSL_ASSIGN_OR_RETURN(
            result.image_png,
            ReadArtifact(output_fd, request.deadline, request.max_image_bytes));
        result.image_width = control.value("width", 0);
        result.image_height = control.value("height", 0);
        result.image_tile_count = control.value("tile_count", 0);
      } else {
        return absl::InvalidArgumentError(
            "web-render helper sent an unexpected artifact");
      }
      continue;
    }
    if (type == "complete") {
      if (!got_response || !got_html || !got_text ||
          (request.include_image && result.image_png.empty())) {
        return absl::InvalidArgumentError(
            "web-render helper completed with missing outputs");
      }
      return result;
    }
    return absl::InvalidArgumentError(
        "web-render helper sent an unknown control frame");
  }
}

}  // namespace

absl::StatusOr<WebRenderResult> RenderWebPage(const WebRenderRequest& request) {
  ABSL_ASSIGN_OR_RETURN(const std::filesystem::path helper, HelperPath());
  ABSL_ASSIGN_OR_RETURN(Pipe input, MakePipe());
  ABSL_ASSIGN_OR_RETURN(Pipe output, MakePipe());

  const pid_t pid = ::fork();
  if (pid < 0) {
    return absl::InternalError(
        absl::StrCat("cannot start web-render helper: ", std::strerror(errno)));
  }
  if (pid == 0) {
    (void)::setpgid(0, 0);
    if (::dup2(input.read.get(), STDIN_FILENO) < 0 ||
        ::dup2(output.write.get(), STDOUT_FILENO) < 0) {
      ::_exit(126);
    }
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      (void)::dup2(null_fd, STDERR_FILENO);
    }
    ::execl(helper.c_str(), helper.c_str(), nullptr);
    ::_exit(127);
  }
  (void)::setpgid(pid, pid);
  input.read.Reset();
  output.write.Reset();

  absl::StatusOr<WebRenderResult> result =
      ExchangeWithHelper(request, input.write.get(), output.read.get());
  input.write.Reset();
  output.read.Reset();
  if (!result.ok()) {
    StopChild(pid);
    return result.status();
  }
  const absl::Status child_status = WaitForChild(pid, request.deadline);
  if (!child_status.ok()) {
    StopChild(pid);
    return child_status;
  }
  return result;
}

#endif

}  // namespace a11::sdk::http
