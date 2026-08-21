// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/system_actions.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <unistd.h>

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
using ::a11::nodes::AsyncNode;

constexpr int kPollMilliseconds = 50;
/// Largest single draw of randomness. A caller wanting more than this wants a
/// stream cipher, not this action.
constexpr std::int64_t kMaxRandomBytes = 1024 * 1024;

absl::Status ErrnoStatus(int code, std::string_view what) {
  return absl::UnavailableError(absl::StrCat(what, ": ", std::strerror(code)));
}

// ---------------------------------------------------------------------------
// read_stdin
// ---------------------------------------------------------------------------

absl::Status RunReadStdin(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_options,
                        ReadJsonInput(action, "options"));
  ABSL_ASSIGN_OR_RETURN(
      const Options options,
      Options::Parse(raw_options.has_value() ? &*raw_options : nullptr));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());
  ABSL_ASSIGN_OR_RETURN(
      const std::int64_t chunk_bytes,
      options.IntInRange("chunk_bytes",
                         static_cast<std::int64_t>(kDefaultChunkBytes), 1,
                         8 * 1024 * 1024));

  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<StopSignal> stop,
                        StopSignal::Create(action, kControlPort));

  const Sink bytes = outputs["bytes"];
  const Sink lines = outputs["lines"];
  // Written first, because whether this is a terminal decides what a
  // composition should do about prompting -- and it wants to know before it
  // starts waiting for input that may never come.
  if (const absl::Status written = outputs["is_tty"].PutOnly(
          nlohmann::json(::isatty(STDIN_FILENO) == 1));
      !written.ok()) {
    stop->Join();
    outputs.Abort(written).IgnoreError();
    return written;
  }

  std::string buffer(static_cast<std::size_t>(chunk_bytes), '\0');
  std::string pending;
  absl::Status trouble = absl::OkStatus();

  while (trouble.ok()) {
    if (stop->stopped()) {
      break;  // a source asked to finish has finished
    }
    // Polled rather than read straight through, so that a stop is acted on
    // while standard input is quiet -- which, for a terminal, is most of the
    // time.
    struct pollfd waiting{};
    waiting.fd = STDIN_FILENO;
    waiting.events = POLLIN;
    const int ready = ::poll(&waiting, 1, kPollMilliseconds);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      trouble = ErrnoStatus(errno, "cannot wait for standard input");
      break;
    }
    if (ready == 0) {
      continue;
    }
    const ssize_t got = ::read(STDIN_FILENO, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      trouble = ErrnoStatus(errno, "cannot read standard input");
      break;
    }
    if (got == 0) {
      break;  // end of input
    }
    const std::string_view piece(buffer.data(), static_cast<std::size_t>(got));
    if (bytes.present()) {
      trouble = bytes.PutBytes(std::string(piece));
      if (!trouble.ok()) {
        break;
      }
    }
    if (lines.present()) {
      pending.append(piece);
      std::size_t start = 0;
      while (trouble.ok()) {
        const std::size_t newline = pending.find('\n', start);
        if (newline == std::string::npos) {
          break;
        }
        std::string_view line(pending.data() + start, newline - start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }
        trouble = lines.PutText(line);
        start = newline + 1;
      }
      pending.erase(0, start);
    }
  }

  if (trouble.ok() && lines.present() && !pending.empty()) {
    std::string_view line(pending);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    trouble = lines.PutText(line);
  }

  absl::Status exit = stop->ExitStatus();
  stop->Join();
  if (!trouble.ok()) {
    outputs.Abort(trouble).IgnoreError();
    return trouble;
  }
  if (!exit.ok()) {
    outputs.Abort(exit).IgnoreError();
    return exit;
  }
  return outputs.Finish();
}

// ---------------------------------------------------------------------------
// write_stdout / write_stderr
// ---------------------------------------------------------------------------

absl::Status RunWriteStream(const std::shared_ptr<Action>& action, int fd,
                            std::string_view name) {
  // No options port: the only output is a byte count, so there is nothing an
  // encoding or an omission would change.
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OutputPorts::Open(action));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<StopSignal> stop,
                        StopSignal::Create(action));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> content,
                        action->GetInput("content"));

  std::uint64_t written = 0;
  absl::Status trouble = absl::OkStatus();
  while (trouble.ok()) {
    if (const absl::Status stopped = stop->Check(); !stopped.ok()) {
      trouble = stopped;
      break;
    }
    absl::StatusOr<std::optional<data::Chunk>> chunk =
        content->NextChunk().Await();
    if (!chunk.ok()) {
      trouble = chunk.status();
      break;
    }
    if (!chunk->has_value() || (*chunk)->IsNull()) {
      break;
    }
    absl::StatusOr<std::string> bytes = BytesOfChunk(**chunk);
    if (!bytes.ok()) {
      trouble = bytes.status();
      break;
    }
    const std::string& payload = *bytes;
    std::size_t at = 0;
    while (at < payload.size()) {
      const ssize_t put = ::write(fd, payload.data() + at, payload.size() - at);
      if (put < 0) {
        if (errno == EINTR) {
          continue;
        }
        trouble = ErrnoStatus(errno, absl::StrCat("cannot write to ", name));
        break;
      }
      at += static_cast<std::size_t>(put);
    }
    written += at;
  }

  stop->Join();
  if (!trouble.ok()) {
    outputs.Abort(trouble).IgnoreError();
    return trouble;
  }
  ABSL_RETURN_IF_ERROR(
      outputs["bytes_written"].PutOnly(nlohmann::json(written)));
  return outputs.Finish();
}

// ---------------------------------------------------------------------------
// env_get
// ---------------------------------------------------------------------------

absl::Status RunEnvGet(const std::shared_ptr<Action>& action,
                       const CapabilitiesPtr& capabilities) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_names,
                        ReadJsonInput(action, "names"));
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_options,
                        ReadJsonInput(action, "options"));
  // An environment variable is a byte string too -- a PATH entry can hold a
  // filename that is not valid UTF-8 -- so this action takes the encoding
  // setting like the rest of them.
  ABSL_ASSIGN_OR_RETURN(
      const Options options,
      Options::Parse(raw_options.has_value() ? &*raw_options : nullptr));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  std::vector<std::string> names;
  if (raw_names.has_value() && raw_names->is_string()) {
    names.push_back(raw_names->get<std::string>());
  } else if (raw_names.has_value() && raw_names->is_array()) {
    for (const nlohmann::json& element : *raw_names) {
      if (!element.is_string()) {
        return absl::InvalidArgumentError(
            "names must be a string or a list of strings");
      }
      names.push_back(element.get<std::string>());
    }
  } else {
    // No "give me everything": a flow that can ask for the whole environment
    // can take credentials it was never told about, and one that has to name
    // what it wants is one a reader can audit.
    return absl::InvalidArgumentError(
        "env_get needs the names to read; there is no way to ask for the whole "
        "environment");
  }

  nlohmann::json values = nlohmann::json::object();
  nlohmann::json missing = nlohmann::json::array();
  for (const std::string& name : names) {
    ABSL_RETURN_IF_ERROR(CheckEnvironment(capabilities->environment, name));
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
      missing.push_back(name);
      continue;
    }
    values[name] = std::string(value);
  }
  ABSL_RETURN_IF_ERROR(outputs["values"].PutOnly(values));
  // Said rather than left to be inferred from an absent key: "unset" and "set
  // to the empty string" are different, and a flow acting on the difference
  // should not have to guess which it got.
  ABSL_RETURN_IF_ERROR(outputs["missing"].PutOnly(missing));
  return outputs.Finish();
}

// ---------------------------------------------------------------------------
// random_bytes / new_uuid
// ---------------------------------------------------------------------------

/// Reads @p count bytes from the system's generator.
///
/// /dev/urandom rather than arc4random or getrandom, because it is the one
/// spelling both platforms this builds for agree on, and because it is the same
/// source those two are wrappers around.
absl::StatusOr<std::string> SystemRandom(std::size_t count) {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return ErrnoStatus(errno, "cannot open the system random source");
  }
  std::string bytes(count, '\0');
  std::size_t at = 0;
  while (at < count) {
    const ssize_t got = ::read(fd, bytes.data() + at, count - at);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      absl::Status failed =
          ErrnoStatus(errno, "cannot read the system random source");
      ::close(fd);
      return failed;
    }
    if (got == 0) {
      ::close(fd);
      return absl::UnavailableError(
          "the system random source ended, which should not happen");
    }
    at += static_cast<std::size_t>(got);
  }
  ::close(fd);
  return bytes;
}

absl::Status RunRandomBytes(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_options,
                        ReadJsonInput(action, "options"));
  ABSL_ASSIGN_OR_RETURN(
      const Options options,
      Options::Parse(raw_options.has_value() ? &*raw_options : nullptr));
  ABSL_ASSIGN_OR_RETURN(const std::int64_t count,
                        options.IntInRange("count", 32, 1, kMaxRandomBytes));
  // `format`, not `encoding`: every action in this library takes an `encoding`
  // meaning json-or-msgpack, and two settings of one name on one port would be
  // a caller writing one and getting the other.
  ABSL_ASSIGN_OR_RETURN(
      const std::string format,
      options.Enum("format", "hex", {"hex", "base64", "base64url", "raw"}));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));
  ABSL_ASSIGN_OR_RETURN(std::string drawn,
                        SystemRandom(static_cast<std::size_t>(count)));

  if (format == "raw") {
    ABSL_RETURN_IF_ERROR(
        outputs["bytes"].Put(BytesChunk(std::move(drawn)), /*final=*/true));
    ABSL_RETURN_IF_ERROR(outputs["bytes"].Close());
    ABSL_RETURN_IF_ERROR(outputs["text"].PutOnly(nlohmann::json()));
    return outputs.Finish();
  }
  std::string encoded;
  if (format == "hex") {
    encoded = absl::BytesToHexString(drawn);
  } else if (format == "base64") {
    encoded = absl::Base64Escape(drawn);
  } else {
    encoded = absl::WebSafeBase64Escape(drawn);
  }
  ABSL_RETURN_IF_ERROR(
      outputs["bytes"].Put(BytesChunk(std::move(drawn)), /*final=*/true));
  ABSL_RETURN_IF_ERROR(outputs["bytes"].Close());
  ABSL_RETURN_IF_ERROR(outputs["text"].PutOnlyText(encoded));
  return outputs.Finish();
}

absl::Status RunNewUuid(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_options,
                        ReadJsonInput(action, "options"));
  ABSL_ASSIGN_OR_RETURN(
      const Options options,
      Options::Parse(raw_options.has_value() ? &*raw_options : nullptr));
  ABSL_ASSIGN_OR_RETURN(const std::int64_t count,
                        options.IntInRange("count", 1, 1, 100000));
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));

  const Sink ids = outputs["ids"];
  for (std::int64_t i = 0; i < count; ++i) {
    ABSL_RETURN_IF_ERROR(ids.PutText(a11::NewUuid(), i + 1 == count));
  }
  return outputs.Finish();
}

// ---------------------------------------------------------------------------
// Handler plumbing
// ---------------------------------------------------------------------------

ActionHandler Plain(absl::Status (*run)(const std::shared_ptr<Action>&)) {
  return [run](std::shared_ptr<Action> action) {
    return a11::SubmitTask(
        [run, action = std::move(action)]() { return run(action); });
  };
}

std::string_view kOmitHelp() {
  return SharedOptionsHelp();
}

}  // namespace

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

ActionSchema ReadStdinSchema() {
  ActionSchema schema;
  schema.name = std::string(kReadStdinAction);
  schema.description =
      "Read this process's standard input, as bytes and as lines. What makes a "
      "composition a filter something else can pipe into: the reads are paced "
      "by whoever is reading the output, so a slow consumer is felt upstream "
      "rather than buffered. `is_tty` is written first, before any waiting, so "
      "a composition can tell an interactive run from a piped one.";
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           absl::StrCat("Optional: chunk_bytes (65536), and ", kOmitHelp()),
           /*required=*/false, /*unary=*/true));
  schema.inputs.emplace(
      std::string(kControlPort),
      Port(kControlPort, JsonType(),
           R"(Control commands; a {"command": "stop"} stops reading.)",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "bytes", Port("bytes", kOctetStream, "Standard input, as it arrives.",
                    /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "lines",
      Port("lines", "string",
           "Standard input, one value per line, without its line ending.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "is_tty",
      Port("is_tty", "bool",
           "Whether standard input is a terminal, written before any reading.",
           /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "Reading ends gracefully once it is reached.");
  return schema;
}

namespace {

ActionSchema WriteStreamSchema(std::string_view name, std::string_view which,
                               std::string_view why) {
  ActionSchema schema;
  schema.name = std::string(name);
  schema.description = absl::StrCat(
      "Write a stream to this process's ", which, ". ", why,
      " The write is the backpressure point, so whoever is producing the "
      "content is held behind the terminal or the pipe rather than buffered "
      "ahead of it.");
  schema.inputs.emplace(
      "content", Port("content", kOctetStream, "What to write, in order.",
                      /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "bytes_written",
      Port("bytes_written", "integer", "How many bytes were written.",
           /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The write fails once it is reached.");
  return schema;
}

}  // namespace

ActionSchema WriteStdoutSchema() {
  return WriteStreamSchema(
      kWriteStdoutAction, "standard output",
      "The other half of what makes a composition a filter.");
}

ActionSchema WriteStderrSchema() {
  return WriteStreamSchema(
      kWriteStderrAction, "standard error",
      "Kept separate from standard output for the reason the operating system "
      "keeps them separate: what a composition says about its work should not "
      "land in the middle of the work's own output.");
}

ActionSchema EnvGetSchema() {
  ActionSchema schema;
  schema.name = std::string(kEnvGetAction);
  schema.description =
      "Read named environment variables, and only the ones this host allows. "
      "There is deliberately no way to ask for the whole environment: a "
      "composition that names what it wants is one a reader can audit, and one "
      "that asks for everything can take credentials it was never told about.";
  schema.inputs.emplace(
      "names",
      Port("names", JsonType(),
           "The variable names to read, as a string or a list of strings.",
           /*required=*/true, /*unary=*/true));
  schema.inputs.emplace("options",
                        Port("options", JsonType(),
                             absl::StrCat("Optional: ", SharedOptionsHelp()),
                             /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "values", Port("values", JsonType(),
                     "An object of the names that were set to their values.",
                     /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "missing",
      Port("missing", JsonType(),
           "The names that were not set at all -- which is a different thing "
           "from being set to an empty string.",
           /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

ActionSchema RandomBytesSchema() {
  ActionSchema schema;
  schema.name = std::string(kRandomBytesAction);
  schema.description =
      "Draw bytes from the system's random source. Flow has no random number "
      "generator and should not have one -- a composition whose output depends "
      "only on its input is one whose failures reproduce -- so nondeterminism "
      "lives here, where it is visible in the source and refusable by not "
      "registering it. These bytes are fit for a token or a nonce, which the "
      "ones new_uuid uses are not.";
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           "Optional: count (32 bytes) and format (hex, base64, base64url or "
           "raw -- raw writes only `bytes`). Named `format` because `encoding` "
           "already means json-or-msgpack on every action here.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace("bytes",
                         Port("bytes", kOctetStream, "The bytes themselves.",
                              /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "text", Port("text", "string",
                   "The bytes in the requested format, or nothing when raw.",
                   /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

ActionSchema NewUuidSchema() {
  ActionSchema schema;
  schema.name = std::string(kNewUuidAction);
  schema.description =
      "Make identifiers -- A11's ordinary kind, which are unique but not "
      "unguessable. Use random_bytes where the value has to be a secret.";
  schema.inputs.emplace("options",
                        Port("options", JsonType(), "Optional: count (1).",
                             /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "ids", Port("ids", "string", "The identifiers, one per value.",
                  /*required=*/false, /*unary=*/false));
  AddDeadlineHeader(schema, "The call fails once it is reached.");
  return schema;
}

// ---------------------------------------------------------------------------
// Handlers and registration
// ---------------------------------------------------------------------------

ActionHandler ReadStdinHandler() {
  return Plain(&RunReadStdin);
}

ActionHandler WriteStdoutHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() {
      return RunWriteStream(action, STDOUT_FILENO, "standard output");
    });
  };
}

ActionHandler WriteStderrHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() {
      return RunWriteStream(action, STDERR_FILENO, "standard error");
    });
  };
}

ActionHandler EnvGetHandler(CapabilitiesPtr capabilities) {
  return
      [capabilities = std::move(capabilities)](std::shared_ptr<Action> action) {
        return a11::SubmitTask(
            [capabilities, action = std::move(action)]() -> absl::Status {
              if (capabilities == nullptr) {
                return absl::FailedPreconditionError(
                    "this action was registered without a policy");
              }
              return RunEnvGet(action, capabilities);
            });
      };
}

ActionHandler RandomBytesHandler() {
  return Plain(&RunRandomBytes);
}

ActionHandler NewUuidHandler() {
  return Plain(&RunNewUuid);
}

absl::Status RegisterStandardStreamActions(actions::ActionRegistry& registry) {
  ABSL_RETURN_IF_ERROR(registry.Register(
      std::string(kReadStdinAction), ReadStdinSchema(), ReadStdinHandler()));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kWriteStdoutAction),
                                         WriteStdoutSchema(),
                                         WriteStdoutHandler()));
  return registry.Register(std::string(kWriteStderrAction), WriteStderrSchema(),
                           WriteStderrHandler());
}

absl::Status RegisterRandomActions(actions::ActionRegistry& registry) {
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kRandomBytesAction),
                                         RandomBytesSchema(),
                                         RandomBytesHandler()));
  return registry.Register(std::string(kNewUuidAction), NewUuidSchema(),
                           NewUuidHandler());
}

absl::Status RegisterEnvironmentActions(actions::ActionRegistry& registry,
                                        CapabilitiesPtr capabilities) {
  if (capabilities == nullptr) {
    return absl::InvalidArgumentError("a policy is required");
  }
  if (!capabilities->environment.any_name &&
      capabilities->environment.names.empty()) {
    return absl::InvalidArgumentError(
        "the environment policy names no variables and does not allow any, so "
        "env_get could only ever refuse");
  }
  return registry.Register(std::string(kEnvGetAction), EnvGetSchema(),
                           EnvGetHandler(std::move(capabilities)));
}

}  // namespace a11::sdk::flow
