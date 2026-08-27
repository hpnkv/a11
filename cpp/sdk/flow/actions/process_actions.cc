// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/process_actions.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/policy.h"
#include "sdk/flow/actions/ports.h"
#include "sdk/flow/actions/sandbox.h"
#include "sdk/flow/actions/stop.h"
#include "thread/concurrency.h"

extern char** environ;

namespace a11::sdk::flow {
namespace {

using ::a11::actions::Action;
using ::a11::actions::ActionHandler;
using ::a11::actions::ActionSchema;
using ::a11::nodes::AsyncNode;

/// How long to wait between polls while a child is running. Short enough that a
/// cancellation is acted on promptly, long enough that a silent child costs
/// twenty wakeups a second rather than twenty thousand.
constexpr int kPollMilliseconds = 50;
constexpr std::size_t kPipeChunkBytes = 64 * 1024;

// Writing to the stdin of a child that has exited raises SIGPIPE, whose default
// action is to kill *this* process -- so a flow feeding a program that stops
// reading would take the gateway down with it.
/// Writing to the stdin of a child that has exited raises SIGPIPE, whose
/// default action is to kill *this* process -- so a flow feeding a program that
/// stops reading would take the gateway down with it. Ignored once, on first
/// use, and the write then fails with EPIPE like any other error.
void IgnoreSigpipeOnce() {
  static const bool ignored = []() {
    // sigemptyset is a macro on some platforms, so it is called unqualified.
    struct sigaction previous{};
    if (::sigaction(SIGPIPE, nullptr, &previous) == 0 &&
        previous.sa_handler != SIG_DFL) {
      return true;  // somebody already had an opinion about it; leave it alone
    }
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    return ::sigaction(SIGPIPE, &action, nullptr) == 0;
  }();
  (void)ignored;
}

absl::Status ErrnoStatus(int code, std::string_view what) {
  const std::string message = absl::StrCat(what, ": ", std::strerror(code));
  switch (code) {
    case ENOENT:
      return absl::NotFoundError(message);
    case EACCES:
    case EPERM:
      return absl::PermissionDeniedError(message);
    case EAGAIN:
    case EMFILE:
    case ENFILE:
    case ENOMEM:
      return absl::ResourceExhaustedError(message);
    default:
      return absl::UnavailableError(message);
  }
}

/// The signals a control command may name.
///
/// A closed set on purpose. A flow that can send an arbitrary signal number can
/// send one whose meaning differs between platforms, and none of the signals
/// anybody actually sends are missing here.
std::optional<int> SignalNamed(std::string_view name) {
  const std::string upper = absl::AsciiStrToUpper(
      absl::StartsWith(name, "SIG") ? name.substr(3) : name);
  if (upper == "TERM") {
    return SIGTERM;
  }
  if (upper == "KILL") {
    return SIGKILL;
  }
  if (upper == "INT") {
    return SIGINT;
  }
  if (upper == "HUP") {
    return SIGHUP;
  }
  if (upper == "QUIT") {
    return SIGQUIT;
  }
  if (upper == "USR1") {
    return SIGUSR1;
  }
  if (upper == "USR2") {
    return SIGUSR2;
  }
  if (upper == "CONT") {
    return SIGCONT;
  }
  return std::nullopt;
}

std::string SignalName(int number) {
  switch (number) {
    case SIGTERM:
      return "SIGTERM";
    case SIGKILL:
      return "SIGKILL";
    case SIGINT:
      return "SIGINT";
    case SIGHUP:
      return "SIGHUP";
    case SIGQUIT:
      return "SIGQUIT";
    case SIGSEGV:
      return "SIGSEGV";
    case SIGBUS:
      return "SIGBUS";
    case SIGABRT:
      return "SIGABRT";
    case SIGFPE:
      return "SIGFPE";
    case SIGILL:
      return "SIGILL";
    case SIGPIPE:
      return "SIGPIPE";
    case SIGUSR1:
      return "SIGUSR1";
    case SIGUSR2:
      return "SIGUSR2";
    default:
      return absl::StrCat("signal ", number);
  }
}

/// A pipe whose ends close themselves. Both ends leak on an early return
/// otherwise, and a leaked pipe per failed spawn is a process that eventually
/// cannot spawn anything at all.
class Pipe {
 public:
  Pipe() = default;
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  ~Pipe() {
    CloseRead();
    CloseWrite();
  }

  absl::Status Open() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      return ErrnoStatus(errno, "cannot create a pipe");
    }
    read_end_ = fds[0];
    write_end_ = fds[1];
    // So that neither end survives an exec in some *other* child spawned
    // concurrently. The ends this child needs are dup2'd, which clears the
    // flag.
    (void)::fcntl(read_end_, F_SETFD, FD_CLOEXEC);
    (void)::fcntl(write_end_, F_SETFD, FD_CLOEXEC);
    return absl::OkStatus();
  }

  [[nodiscard]] int read_end() const { return read_end_; }

  [[nodiscard]] int write_end() const { return write_end_; }

  // Hands the write end to somebody else, who must close it. Ownership moves
  // rather than being shared.
  /// Hands the write end to somebody else, who must close it.
  ///
  /// The child's standard input has to be closed by whoever finishes writing to
  /// it, and nobody else: a program that reads to EOF -- `cat`, `sort`, `wc` --
  /// waits for that close, so closing it late deadlocks and closing it twice
  /// races. Ownership moves rather than being shared.
  int ReleaseWrite() {
    const int fd = write_end_;
    write_end_ = -1;
    return fd;
  }

  void CloseRead() {
    if (read_end_ >= 0) {
      ::close(read_end_);
      read_end_ = -1;
    }
  }

  void CloseWrite() {
    if (write_end_ >= 0) {
      ::close(write_end_);
      write_end_ = -1;
    }
  }

 private:
  int read_end_ = -1;
  int write_end_ = -1;
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct SpawnSettings {
  std::string cwd;
  std::vector<std::pair<std::string, std::string>> environment;
  bool clear_environment = false;
  absl::Duration grace = absl::Seconds(5);
  std::uint64_t max_output_bytes = 0;
};

absl::StatusOr<SpawnSettings> ReadSpawnSettings(const Options& options,
                                                const ProcessPolicy& policy) {
  SpawnSettings settings;
  ABSL_ASSIGN_OR_RETURN(settings.cwd, options.String("cwd", ""));
  ABSL_ASSIGN_OR_RETURN(
      settings.clear_environment,
      options.Bool("clear_environment", !policy.inherit_environment));
  ABSL_ASSIGN_OR_RETURN(settings.grace,
                        options.Duration("grace", absl::Seconds(5)));
  ABSL_ASSIGN_OR_RETURN(settings.max_output_bytes,
                        options.Bytes("max_output_bytes", 0));
  if (!policy.inherit_environment && !settings.clear_environment) {
    // The option may narrow the policy and never widen it.
    settings.clear_environment = true;
  }
  if (options.Has("environment")) {
    const nlohmann::json& value = options.json().at("environment");
    if (!value.is_object()) {
      return absl::InvalidArgumentError(
          "options.environment must be an object of names to values");
    }
    for (const auto& [name, entry] : value.items()) {
      if (!entry.is_string()) {
        return absl::InvalidArgumentError(
            absl::StrCat("options.environment.", name, " must be a string"));
      }
      settings.environment.emplace_back(name, entry.get<std::string>());
    }
  }
  return settings;
}

absl::StatusOr<std::vector<std::string>> ReadArguments(
    const std::shared_ptr<Action>& action) {
  std::vector<std::string> arguments;
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> value,
                        ReadJsonInput(action, "arguments"));
  if (!value.has_value() || value->is_null()) {
    return arguments;
  }
  if (value->is_string()) {
    // Treat this as one argument, not a command line; do not split on spaces.
    // splitting is how a filename with a space in it becomes two arguments and
    // a flow that meant one thing runs another.
    arguments.push_back(value->get<std::string>());
    return arguments;
  }
  if (!value->is_array()) {
    return absl::InvalidArgumentError(
        "arguments must be a list of strings, one per argument");
  }
  arguments.reserve(value->size());
  for (const nlohmann::json& element : *value) {
    if (element.is_string()) {
      arguments.push_back(element.get<std::string>());
    } else if (element.is_number() || element.is_boolean()) {
      arguments.push_back(element.dump());  // `8` for `-j8` is ordinary enough
    } else {
      return absl::InvalidArgumentError(
          "each argument must be a string, a number or a boolean");
    }
  }
  return arguments;
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

/**
 * Everything the child needs, laid out before the fork.
 *
 * This class exists because of what may happen between fork() and exec(): the
 * child of a multi-threaded process holds whatever locks the other threads
 * held at the moment of the fork, so allocating there -- which takes the
 * allocator's lock -- deadlocks the child, occasionally, on a busy machine.
 * Every string and pointer array is therefore built here, in the parent, and
 * the child calls nothing but dup2, chdir, setpgid, execvp and _exit.
 */
class ChildPlan {
 public:
  ChildPlan(const std::string& program,
            const std::vector<std::string>& arguments,
            const SpawnSettings& settings) {
    argument_storage_.reserve(arguments.size() + 1);
    argument_storage_.push_back(program);
    for (const std::string& argument : arguments) {
      argument_storage_.push_back(argument);
    }
    argv_.reserve(argument_storage_.size() + 1);
    for (std::string& argument : argument_storage_) {
      argv_.push_back(argument.data());
    }
    argv_.push_back(nullptr);

    for (const auto& [name, value] : settings.environment) {
      environment_storage_.push_back(absl::StrCat(name, "=", value));
    }
    if (!settings.clear_environment) {
      for (char** entry = environ; entry != nullptr && *entry != nullptr;
           ++entry) {
        const std::string_view existing(*entry);
        const std::size_t equals = existing.find('=');
        const std::string_view name = equals == std::string_view::npos
                                          ? existing
                                          : existing.substr(0, equals);
        bool overridden = false;
        for (const auto& [set_name, unused] : settings.environment) {
          overridden = overridden || set_name == name;
        }
        if (!overridden) {
          environment_storage_.emplace_back(existing);
        }
      }
    }
    envp_.reserve(environment_storage_.size() + 1);
    for (std::string& entry : environment_storage_) {
      envp_.push_back(entry.data());
    }
    envp_.push_back(nullptr);
    cwd_ = settings.cwd;
  }

  /// Replaces the command with whatever entering the sandbox requires. Called
  /// before the fork, like everything else here.
  void WrapIn(const Sandbox& sandbox) {
    const std::string program = argument_storage_.front();
    const std::vector<std::string> rest(argument_storage_.begin() + 1,
                                        argument_storage_.end());
    const std::string wrapped_program = sandbox.WrapProgram(program);
    const std::vector<std::string> wrapped_arguments =
        sandbox.WrapCommand(program, rest);
    if (wrapped_program == program && wrapped_arguments.size() == rest.size()) {
      return;  // nothing to rewrite on this platform
    }
    argument_storage_.clear();
    argument_storage_.push_back(wrapped_program);
    for (const std::string& argument : wrapped_arguments) {
      argument_storage_.push_back(argument);
    }
    argv_.clear();
    argv_.reserve(argument_storage_.size() + 1);
    for (std::string& argument : argument_storage_) {
      argv_.push_back(argument.data());
    }
    argv_.push_back(nullptr);
  }

  [[nodiscard]] char* const* argv() const { return argv_.data(); }

  [[nodiscard]] char** envp() const { return const_cast<char**>(envp_.data()); }

  [[nodiscard]] const char* cwd() const {
    return cwd_.empty() ? nullptr : cwd_.c_str();
  }

  [[nodiscard]] const char* program() const {
    return argument_storage_.front().c_str();
  }

 private:
  std::vector<std::string> argument_storage_;
  std::vector<char*> argv_;
  std::vector<std::string> environment_storage_;
  std::vector<char*> envp_;
  std::string cwd_;
};

/**
 * Forks and execs, and reports why it could not.
 *
 * The failure a child cannot return through a status is returned through a
 * pipe: the child writes its errno and `_exit`s, so a program that does not
 * exist arrives here as `not_found` rather than as an exit code of 127 the
 * caller is expected to know how to read.
 */
absl::StatusOr<pid_t> Spawn(const ChildPlan& plan, const Sandbox& sandbox,
                            int stdin_fd, int stdout_fd, int stderr_fd,
                            Pipe& report) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    return ErrnoStatus(errno, "cannot fork");
  }
  if (pid == 0) {
    const int report_fd = report.write_end();
    const auto fail = [report_fd](int reason) {
      // A short write here is not something the child can do anything about,
      // and the parent treats a truncated report as "it failed, unclear why".
      const ssize_t ignored = ::write(report_fd, &reason, sizeof(reason));
      (void)ignored;
      ::_exit(127);
    };
    if (::dup2(stdin_fd, STDIN_FILENO) < 0) {
      fail(errno);
    }
    if (::dup2(stdout_fd, STDOUT_FILENO) < 0) {
      fail(errno);
    }
    if (::dup2(stderr_fd, STDERR_FILENO) < 0) {
      fail(errno);
    }
    if (plan.cwd() != nullptr && ::chdir(plan.cwd()) != 0) {
      fail(errno);
    }
    // Its own process group, so signalling the child does not signal this
    // process, and so a child that spawns children of its own can be stopped as
    // a group rather than leaving orphans behind.
    (void)::setpgid(0, 0);
    // After chdir -- the working directory has to be resolvable before the
    // ruleset takes effect -- and before exec, which is the only order in which
    // the restriction covers the program rather than this frame.
    if (const int reason = sandbox.Apply(); reason != 0) {
      fail(reason);
    }
    // A pointer assignment, which is safe here where an allocation would not
    // be: execvpe is not portable, and execvp reads this.
    environ = plan.envp();
    ::execvp(plan.program(), plan.argv());
    fail(errno);
  }
  return pid;
}

/**
 * Signals the child, whichever group it has managed to join.
 *
 * The child puts itself in its own process group so that signalling it does not
 * signal this process and so its own children go with it. But `setpgid` in the
 * child and `killpg` in the parent race: until the child has run it, its group
 * is *this* process's group, and `killpg(child_pid, ...)` then finds no group
 * of that id and fails with ESRCH. The signal is silently not delivered, and a
 * cancelled `sleep 120` carries on sleeping -- which is how a cancellation that
 * looked instantaneous on an idle machine took ten seconds on a loaded one.
 *
 * Two halves close it: the parent sets the child's group as well (whichever
 * runs first wins, and the loser gets EACCES because the child has already
 * exec'd, which is fine), and a group signal that finds no group falls back to
 * the process. Never `killpg` on this process's own group -- that would be a
 * flow cancelling itself and everything else running here.
 */
void SignalChild(pid_t pid, int signal_number) {
  if (::killpg(pid, signal_number) == 0) {
    return;
  }
  if (errno == ESRCH) {
    // Not in its own group yet: signal the process itself, which is always
    // correct and is merely narrower than intended for a moment.
    (void)::kill(pid, signal_number);
  }
}

/// Reads the child's failure report, if it wrote one. Called once the write end
/// is closed in the parent, so a child that exec'd successfully reads as EOF.
std::optional<int> ReadSpawnFailure(int report_fd) {
  int reason = 0;
  const ssize_t got = ::read(report_fd, &reason, sizeof(reason));
  if (got == static_cast<ssize_t>(sizeof(reason))) {
    return reason;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

/// What the poll loop needs to know about one of the child's output streams.
struct OutputStream {
  int fd = -1;
  Sink bytes;
  LineSplitter lines;
  bool open = true;
};

absl::Status RunSpawnProcess(const std::shared_ptr<Action>& action,
                             const CapabilitiesPtr& capabilities) {
  IgnoreSigpipeOnce();
  const ProcessPolicy& policy = capabilities->process;

  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_options,
                        ReadJsonInput(action, "options"));
  ABSL_ASSIGN_OR_RETURN(
      const Options options,
      Options::Parse(raw_options.has_value() ? &*raw_options : nullptr));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());
  ABSL_ASSIGN_OR_RETURN(SpawnSettings settings,
                        ReadSpawnSettings(options, policy));
  ABSL_ASSIGN_OR_RETURN(const std::string requested,
                        ReadRequiredTextInput(action, "program"));
  ABSL_ASSIGN_OR_RETURN(const std::string program,
                        ResolveProgram(policy, requested));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> arguments,
                        ReadArguments(action));
  if (!settings.cwd.empty()) {
    // A working directory is a path, so it answers to the filesystem policy
    // like any other -- otherwise `cwd` would be the way around it.
    ABSL_ASSIGN_OR_RETURN(const std::filesystem::path resolved,
                          ResolvePath(capabilities->filesystem, settings.cwd,
                                      /*for_write=*/false));
    settings.cwd = resolved.string();
  }
  if (policy.max_seconds > 0) {
    const absl::Duration cap = absl::Seconds(policy.max_seconds);
    if (settings.grace > cap) {
      settings.grace = cap;
    }
  }

  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  // The pid is not known until after the fork, and a control command naming a
  // signal may arrive before then. Shared, so the watcher can wait for one.
  auto child_pid = std::make_shared<std::atomic<pid_t>>(-1);
  ABSL_ASSIGN_OR_RETURN(
      const std::shared_ptr<StopSignal> stop,
      StopSignal::Create(
          action, kControlPort,
          [child_pid](const nlohmann::json& command) -> absl::Status {
            if (!command.is_object() || !command.contains("command") ||
                command.at("command") != "signal") {
              return absl::InvalidArgumentError(absl::StrCat(
                  "spawn_process takes {\"command\": \"stop\"} and "
                  "{\"command\": \"signal\", \"signal\": \"TERM\"}; got ",
                  command.dump()));
            }
            const std::string name = command.contains("signal")
                                         ? command.at("signal").dump()
                                         : std::string("\"TERM\"");
            const std::optional<int> number = SignalNamed(
                command.contains("signal") && command.at("signal").is_string()
                    ? command.at("signal").get<std::string>()
                    : "TERM");
            if (!number.has_value()) {
              return absl::InvalidArgumentError(
                  absl::StrCat("no signal called ", name));
            }
            const pid_t pid = child_pid->load(std::memory_order_acquire);
            if (pid <= 0) {
              return absl::FailedPreconditionError(
                  "there is no process to signal yet");
            }
            // The group where there is one, so a child that spawned children
            // of its own does not leave them running.
            SignalChild(pid, *number);
            return absl::OkStatus();
          }));

  const auto give_up = [&](const absl::Status& reason) -> absl::Status {
    stop->Join();
    outputs.Abort(reason).IgnoreError();
    return reason;
  };

  Pipe input;
  Pipe output;
  Pipe errors;
  Pipe report;
  for (Pipe* pipe : {&input, &output, &errors, &report}) {
    if (const absl::Status opened = pipe->Open(); !opened.ok()) {
      return give_up(opened);
    }
  }

  // Prepared before the fork, because everything it needs to allocate has to be
  // allocated before the fork. A policy with `required` fails here when
  // confinement is unavailable.
  absl::StatusOr<std::shared_ptr<Sandbox>> sandbox =
      Sandbox::Prepare(*capabilities, program);
  if (!sandbox.ok()) {
    return give_up(sandbox.status());
  }
  if (const absl::Status written =
          outputs["sandbox"].PutOnlyText((*sandbox)->Describe());
      !written.ok()) {
    return give_up(written);
  }

  ChildPlan plan(program, arguments, settings);
  plan.WrapIn(**sandbox);
  absl::StatusOr<pid_t> spawned =
      Spawn(plan, **sandbox, input.read_end(), output.write_end(),
            errors.write_end(), report);
  if (!spawned.ok()) {
    return give_up(spawned.status());
  }
  // The parent's half of the setpgid race described on SignalChild: whichever
  // of the two runs first wins, and the loser fails harmlessly.
  (void)::setpgid(*spawned, *spawned);
  child_pid->store(*spawned, std::memory_order_release);

  // The parent holds only its own ends. Closing the child's is what makes a
  // read return EOF when the child exits, rather than blocking forever on a
  // descriptor this process is still holding open.
  input.CloseRead();
  output.CloseWrite();
  errors.CloseWrite();
  report.CloseWrite();

  if (const std::optional<int> failure = ReadSpawnFailure(report.read_end());
      failure.has_value()) {
    int discarded = 0;
    (void)::waitpid(*spawned, &discarded, 0);
    return give_up(
        ErrnoStatus(*failure, absl::StrCat("cannot run '", program, "'")));
  }

  // Before any output, so a flow can order something else behind the process
  // having started.
  if (const absl::Status written =
          outputs["pid"].PutOnly(nlohmann::json(*spawned));
      !written.ok()) {
    return give_up(written);
  }

  // stdin is fed by a fibre of its own: feeding it means reading a port, which
  // blocks, and the same fibre cannot also be polling for output. The fibre
  // owns the pipe's write end and closes it when the content stream ends.
  absl::StatusOr<std::shared_ptr<AsyncNode>> content =
      action->GetInput("stdin");
  if (!content.ok()) {
    return give_up(content.status());
  }
  const std::shared_ptr<AsyncNode>& stdin_node = *content;
  a11::Task feeder =
      a11::SubmitTask([node = stdin_node, stdin_fd = input.ReleaseWrite(),
                       stop]() -> absl::Status {
        while (!stop->stopped()) {
          absl::StatusOr<std::optional<data::Chunk>> chunk =
              node->NextChunk().Await();
          if (!chunk.ok() || !chunk->has_value() || (*chunk)->IsNull()) {
            break;
          }
          absl::StatusOr<std::string> bytes = BytesOfChunk(**chunk);
          if (!bytes.ok()) {
            break;
          }
          const std::string& payload = *bytes;
          std::size_t at = 0;
          bool broken = false;
          while (at < payload.size() && !broken) {
            const ssize_t put =
                ::write(stdin_fd, payload.data() + at, payload.size() - at);
            if (put < 0) {
              if (errno == EINTR) {
                continue;
              }
              // EPIPE: the child stopped reading. Its business rather than an
              // error -- `head -1` of a long stream is a correct program.
              broken = true;
              break;
            }
            at += static_cast<std::size_t>(put);
          }
          if (broken) {
            break;
          }
        }
        ::close(stdin_fd);
        return absl::OkStatus();
      });

  OutputStream streams[2] = {
      {output.read_end(), outputs["stdout"],
       LineSplitter(outputs["stdout_lines"])},
      {errors.read_end(), outputs["stderr"],
       LineSplitter(outputs["stderr_lines"])},
  };

  absl::Status trouble = absl::OkStatus();
  std::uint64_t produced = 0;
  bool termed = false;
  absl::Time term_at = absl::InfiniteFuture();
  // On the heap: a fibre's stack is measured in kilobytes, and 64 KiB of local
  // buffer would run off the end of it.
  std::string buffer(kPipeChunkBytes, '\0');

  while ((streams[0].open || streams[1].open) && trouble.ok()) {
    // Give this worker's fibre scheduler a turn. That is a deadlock, and it was
    // one: `SpawnProcessTest.StopsALongRunningProcessAndSaysHow` failed about
    // 40% of the time under load without this yield and not at all with it.
    thread::SleepFor(absl::ZeroDuration());
    if (stop->stopped() && !termed) {
      // Asked nicely first: a program that flushes its output on SIGTERM gets
      // the chance to, and the reading below carries on so that what it flushes
      // is not thrown away.
      termed = true;
      term_at = absl::Now();
      SignalChild(*spawned, SIGTERM);
      // And unblock the feeder, which may be parked waiting for content that
      // is not coming now.
      stdin_node->CancelReader();
    }
    if (termed && absl::Now() - term_at > settings.grace) {
      SignalChild(*spawned, SIGKILL);
      term_at = absl::InfiniteFuture();  // once is enough
    }

    struct pollfd waiting[2] = {};
    int count = 0;
    for (const OutputStream& stream : streams) {
      if (stream.open) {
        waiting[count].fd = stream.fd;
        waiting[count].events = POLLIN;
        ++count;
      }
    }
    const int ready =
        ::poll(waiting, static_cast<nfds_t>(count), kPollMilliseconds);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      trouble = ErrnoStatus(errno, "cannot wait for the process's output");
      break;
    }
    if (ready == 0) {
      continue;  // nothing said anything; round again to re-check the stop
    }

    for (int i = 0; i < count; ++i) {
      if ((waiting[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      OutputStream* stream = nullptr;
      for (OutputStream& candidate : streams) {
        if (candidate.open && candidate.fd == waiting[i].fd) {
          stream = &candidate;
        }
      }
      if (stream == nullptr) {
        continue;
      }
      const ssize_t got = ::read(stream->fd, buffer.data(), buffer.size());
      if (got < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        trouble = ErrnoStatus(errno, "cannot read the process's output");
        break;
      }
      if (got == 0) {
        stream->open = false;  // the child closed it, or exited
        if (const absl::Status flushed = stream->lines.Flush(); !flushed.ok()) {
          trouble = flushed;
        }
        continue;
      }
      const std::string_view piece(buffer.data(),
                                   static_cast<std::size_t>(got));
      produced += piece.size();
      if (settings.max_output_bytes != 0 &&
          produced > settings.max_output_bytes) {
        trouble = absl::ResourceExhaustedError(absl::StrCat(
            "the process produced more than the ", settings.max_output_bytes,
            " bytes of output this call allows"));
        break;
      }
      if (const absl::Status written =
              stream->bytes.PutBytes(std::string(piece));
          !written.ok()) {
        trouble = written;
        break;
      }
      if (const absl::Status written = stream->lines.Feed(piece);
          !written.ok()) {
        trouble = written;
        break;
      }
    }
  }

  if (!trouble.ok()) {
    // Whatever went wrong here, the child is this action's responsibility and
    // must not outlive it.
    SignalChild(*spawned, SIGKILL);
  }

  // The feeder is joined before the wait: both output pipes are at EOF by now,
  // so the child has finished writing, and anything still owed to its standard
  // input is owed to a process that is about to be reaped.
  stdin_node->CancelReader();
  feeder.Await().IgnoreError();

  int wait_status = 0;
  struct rusage usage{};
  const pid_t reaped = ::wait4(*spawned, &wait_status, 0, &usage);
  stop->Join();

  if (!trouble.ok()) {
    outputs.Abort(trouble).IgnoreError();
    return trouble;
  }
  if (reaped < 0) {
    const absl::Status failed =
        ErrnoStatus(errno, "cannot collect the process's exit status");
    outputs.Abort(failed).IgnoreError();
    return failed;
  }

  const bool signalled = WIFSIGNALED(wait_status);
  const int code =
      signalled ? 128 + WTERMSIG(wait_status) : WEXITSTATUS(wait_status);
  ABSL_RETURN_IF_ERROR(outputs["exit_code"].PutOnly(nlohmann::json(code)));
  // Said rather than encoded: "exit code 143" is a thing no caller should have
  // to decode into "it was terminated".
  ABSL_RETURN_IF_ERROR(outputs["signal"].PutOnly(
      signalled ? nlohmann::json(SignalName(WTERMSIG(wait_status)))
                : nlohmann::json()));
  ABSL_RETURN_IF_ERROR(outputs["usage"].PutOnly(nlohmann::json{
      {"user_ms", static_cast<std::int64_t>(usage.ru_utime.tv_sec) * 1000 +
                      usage.ru_utime.tv_usec / 1000},
      {"system_ms", static_cast<std::int64_t>(usage.ru_stime.tv_sec) * 1000 +
                        usage.ru_stime.tv_usec / 1000},
      {"max_rss_bytes", static_cast<std::int64_t>(usage.ru_maxrss)},
      {"output_bytes", produced}}));

  // The terminal values are written even when the run is about to be reported
  // as a failure: a caller diagnosing a deadline wants to know what the program
  // managed to do first.
  absl::Status ended = outputs.Finish();
  if (const absl::Status stopped = stop->Check(); !stopped.ok()) {
    return stopped;
  }
  // A non-zero exit is not a failure of this action. The program ran, and this
  // is what it said -- exactly as a 404 is a response rather than an error.
  return ended;
}

/// One shape, so the policy lives in the handler rather than in an option.
ActionHandler MakeHandler(CapabilitiesPtr capabilities) {
  return
      [capabilities = std::move(capabilities)](std::shared_ptr<Action> action) {
        return a11::SubmitTask(
            [capabilities, action = std::move(action)]() -> absl::Status {
              if (capabilities == nullptr) {
                return absl::FailedPreconditionError(
                    "this action was registered without a policy");
              }
              return RunSpawnProcess(action, capabilities);
            });
      };
}

}  // namespace

ActionSchema SpawnProcessSchema() {
  ActionSchema schema;
  schema.name = std::string(kSpawnProcessAction);
  schema.description =
      "Run a program, with its standard output and standard error on separate "
      "ports -- as bytes and as lines, whichever is wanted -- and its pid "
      "written before any of them, so a composition can order something behind "
      "the process having started. A non-zero exit is reported on `exit_code` "
      "rather than failing the action: the program ran, and this is what it "
      "said. Cancelling the action sends SIGTERM, waits options.grace, and "
      "then "
      "sends SIGKILL.";
  schema.inputs.emplace("program",
                        Port("program", "string", "The program to run.",
                             /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "arguments",
      Port(
          "arguments", JsonType(),
          "The arguments, one per element. A single string is one argument and "
          "is not split on spaces -- splitting is how a path with a space in "
          "it becomes two arguments.",
          /*required=*/false, /*unary=*/true));
  schema.inputs.emplace(
      "stdin",
      Port("stdin", kOctetStream,
           "What to write to the program's standard input, in order. Closed "
           "when the stream ends, which is what a program reading to EOF waits "
           "for. A program that stops reading early is not an error.",
           /*required=*/false, /*unary=*/false));
  schema.inputs.emplace(
      "options",
      Port(
          "options", JsonType(),
          "All optional: cwd (checked against the same filesystem policy as "
          "any other path), environment (an object of names to values), "
          "clear_environment, grace (how long SIGTERM is given before SIGKILL, "
          "default 5s), max_output_bytes, and omit -- output port names to "
          "close immediately rather than write.",
          /*required=*/false, /*unary=*/true));
  schema.inputs.emplace(
      std::string(kControlPort),
      Port(
          kControlPort, JsonType(),
          "Control commands: {\"command\": \"stop\"} to wind the process "
          "down, or {\"command\": \"signal\", \"signal\": \"HUP\"} to send one "
          "of TERM, KILL, INT, HUP, QUIT, USR1, USR2 or CONT to the process "
          "group.",
          /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "pid",
      Port("pid", "integer", "The process id, written as soon as there is one.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "stdout", Port("stdout", kOctetStream, "Standard output, as it arrives.",
                     /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "stdout_lines",
      Port("stdout_lines", "string",
           "Standard output, one value per line, without its line ending.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "stderr", Port("stderr", kOctetStream,
                     "Standard error, as it arrives, and never mixed into "
                     "standard output.",
                     /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "stderr_lines",
      Port("stderr_lines", "string",
           "Standard error, one value per line, without its line ending.",
           /*required=*/false, /*unary=*/false));
  // Named `exit_code` rather than `status` because Flow reads `x.status` as the
  // outcome of the step called `x`, whatever ports it declares.
  schema.outputs.emplace(
      "exit_code",
      Port("exit_code", "integer",
           "The exit code, or 128 plus the signal number when it was killed.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "signal",
      Port("signal", "string",
           "The name of the signal that ended it, or nothing when it exited of "
           "its own accord.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "usage",
      Port("usage", JsonType(),
           "{user_ms, system_ms, max_rss_bytes, output_bytes} once it has "
           "finished.",
           /*required=*/false, /*unary=*/true));
  // Reported rather than assumed.
  schema.outputs.emplace(
      "sandbox",
      Port("sandbox", "string",
           "What confinement was applied to the child: a Landlock ruleset on "
           "Linux, a Seatbelt profile on macOS, or why neither was.",
           /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema,
                    "The process is sent SIGTERM once it is reached, then "
                    "SIGKILL after options.grace, and the action fails with "
                    "deadline_exceeded.");
  return schema;
}

ActionHandler SpawnProcessHandler(CapabilitiesPtr capabilities) {
  return MakeHandler(std::move(capabilities));
}

absl::Status RegisterProcessActions(actions::ActionRegistry& registry,
                                    CapabilitiesPtr capabilities) {
  if (capabilities == nullptr) {
    return absl::InvalidArgumentError("a policy is required");
  }
  if (!capabilities->process.enabled) {
    return absl::FailedPreconditionError(
        "the process policy is not enabled, so registering spawn_process would "
        "register an action that always refuses");
  }
  if (!capabilities->process.any_program &&
      capabilities->process.programs.empty()) {
    return absl::InvalidArgumentError(
        "the process policy names no programs and does not allow any, so "
        "nothing could ever be run; set programs, or set any_program");
  }
  return registry.Register(std::string(kSpawnProcessAction),
                           SpawnProcessSchema(),
                           SpawnProcessHandler(std::move(capabilities)));
}

}  // namespace a11::sdk::flow
