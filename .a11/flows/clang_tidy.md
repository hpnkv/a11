# Run clang-tidy on source paths

Use `clang_tidy.flow` to check an explicit list of C++ source files through
the host's `run_command` action. Flow's `parallel concurrency` map stage
runs one clang-tidy process per source, with at most `max_concurrency`
invocations active. The runtime must support expression-valued parallel limits.
It checks every source even when another process exits unsuccessfully.

## Inputs

- `sources` (required string stream): one source path per value, relative
  to the host working directory or absolute. Checks start as paths arrive,
  without waiting for the stream to close. Paths are shell-quoted; relative
  paths get a `./` prefix so a leading dash is not interpreted as an option.
- `options` (optional `ClangTidyOptions` struct): settings for all sources.
  Omit it or pass `{}` to use the defaults:
  - `max_concurrency` (integer, default `4`): positive maximum number of
    simultaneous clang-tidy processes.
  - `clang_tidy` (string, default `clang-tidy`): executable name on `PATH`
    or an executable path, not a command with extra arguments.
  - `build_dir` (optional string): compilation database directory passed
    as `-p`. When omitted, clang-tidy uses its normal database discovery.

Each path is validated before its command runs. Empty paths and paths
containing NUL fail the flow; earlier valid sources may already be running.
An empty source stream is rejected when it closes.
The host needs Bash and clang-tidy. Configure the compilation database and
`.clang-tidy` for the sources being checked. Each source command has a
600-second timeout; the invoking host may impose a shorter overall deadline.
Shell commands only launch clang-tidy; Flow manages all concurrency.

## Outputs

- `diagnostics` (string stream): output lines buffered per source, emitted
  in input order after that source finishes.
- `result` (object): aggregate completion with an `exit_code` field. It
  is `0` when every invocation succeeds and `1` if any invocation fails.
  Warnings only cause failure when clang-tidy is configured to treat them
  as errors. Host errors and timeouts propagate as action failures.

Example input values for the `clang_tidy` entry flow (the `sources` array
represents separate stream values, not one list-valued item):

```json
{
  "sources": ["cpp/example.cc", "cpp/other.cc"],
  "options": {
    "max_concurrency": 2,
    "clang_tidy": "/opt/llvm/bin/clang-tidy",
    "build_dir": "build/debug"
  }
}
```
