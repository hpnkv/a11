// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   A11's native benchmarks: the same operations the Python suite measures,
 *   with the Python taken out.
 *
 * The Python suite established that A11's dominant cost is the boundary rather
 * than the core -- one `await` costs ~20us, and a pure-TypeScript client beats
 * the native-backed Python runtime by 20-50x on in-memory work. That is an
 * inference from two data points, and inferences are a poor basis for deciding
 * where to spend a quarter. This binary supplies the third: what the C++ costs
 * on its own.
 *
 * Read a number here as the *ceiling*. If a native operation runs at 3M/s and
 * the Python binding delivers 40k/s, the binding is the work. If the native
 * operation is itself slow, no amount of binding work will help and the fix is
 * underneath. Every benchmark here is named to match its Python counterpart so
 * the two tables line up.
 *
 *   a11_bench                       # everything
 *   a11_bench --suite stores        # one suite
 *   a11_bench --scale 0.1           # a tenth of the iterations
 *   a11_bench --json native.json    # records the Python runner can diff
 */

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <arpa/inet.h>
#include <boost/fiber/fiber.hpp>
#include <boost/fiber/operations.hpp>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>
// The raw-webrtc reference row drives libdatachannel directly, with no A11
// between it and the data channel; a11/net/webrtc_wire_stream.h only
// forward-declares these.
#include <rtc/candidate.hpp>
#include <rtc/configuration.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/description.hpp>
#include <rtc/global.hpp>
#include <rtc/peerconnection.hpp>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/flow/runtime.h"
#include "a11/net/http_sse_wire_stream.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/internal/http_transport.h"
#include "a11/net/signalling.h"
#include "a11/net/webrtc_wire_stream.h"
#include "a11/net/websocket_wire_stream.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/service/service.h"
#include "a11/service/session.h"
#include "a11/stores/local_chunk_store.h"
#include "absl/strings/match.h"
#include "bench/harness.h"
#include "thread/boost_primitives.h"
#include "thread/executor.h"

namespace a11::bench {
namespace {

/// `--only` : run just the rows whose name contains this.
///
/// A suite is the wrong granularity for an optimisation round. Attributing a
/// process-wide counter (`A11_POOL_STATS`) to one operation needs two runs of
/// *one* row at different scales, and a `--suite` run mixes a dozen.
std::string g_only;

bool Wanted(std::string_view name) {
  return g_only.empty() || absl::StrContains(name, g_only);
}

/// An environment knob as an integer, or nullopt when unset or not a number.
///
/// Every tuning variable in this file goes through here rather than through
/// `std::atoi`, which reads a typo as zero -- and a benchmark quietly run with
/// a knob at zero is a measurement of the wrong thing.
std::optional<int> EnvironmentInt(const char* name) {
  const char* setting = std::getenv(name);
  int value = 0;
  if (setting == nullptr || !absl::SimpleAtoi(setting, &value)) {
    return std::nullopt;
  }
  return value;
}

absl::Time Deadline() {
  return absl::Now() + absl::Seconds(30);
}

data::NodeFragment Fragment(std::uint32_t seq, const std::string& payload,
                            bool final = false) {
  return data::NodeFragment{
      .data = data::Chunk{.data = payload}, .seq = seq, .continued = !final};
}

std::string Human(std::int64_t size) {
  if (size >= 1048576) {
    return absl::StrCat(size / 1048576, "M");
  }
  if (size >= 1024) {
    return absl::StrCat(size / 1024, "K");
  }
  return absl::StrCat(size, "B");
}

// ---------------------------------------------------------------------------
// data: the codec floor
// ---------------------------------------------------------------------------

/**
 * Chunk codec and envelope codec, native.
 *
 * The Python numbers said `to_chunk` spends 8.4us of its 8.6us resolving a
 * registration, and that `WireMessage.from_msgpack` decodes 16x slower than it
 * encodes. Both claims are about code that lives here, so both are checkable
 * here -- and if the C++ registry shows the same shape, the fix is in C++ and
 * not in the binding.
 */
void DataSuite(Recorder& recorder, double scale) {
  data::SerializationRegistry& registry = data::GlobalSerializationRegistry();

  // The chunk codec, against the same payload the Python suite uses.
  for (const std::int64_t size : {64, 1024, 65536}) {
    const nlohmann::json body = {
        {"id", "bench"},
        {"body",
         std::string(static_cast<size_t>(std::max<std::int64_t>(size - 24, 1)),
                     'x')}};
    for (const std::string mimetype :
         {"application/json", "application/x-msgpack"}) {
      absl::StatusOr<data::Chunk> encoded = registry.ToChunk(body, mimetype);
      if (!encoded.ok()) {
        std::fprintf(stderr, "  skip to_chunk %s: %s\n", mimetype.c_str(),
                     std::string(encoded.status().message()).c_str());
        continue;
      }
      const auto wire_bytes = static_cast<std::int64_t>(encoded->data.size());
      const std::int64_t iterations =
          Scaled(size <= 1024 ? 200000 : 20000, scale, 100);
      const std::string label =
          mimetype == "application/json" ? "json" : "msgpack";

      recorder.Add({.suite = "data",
                    .name = "to_chunk",
                    .metrics = Throughput(
                        [&](std::int64_t) {
                          auto chunk = registry.ToChunk(body, mimetype);
                          (void)chunk;
                        },
                        iterations, iterations / 10, 1, wire_bytes),
                    .params = {{"repr", label}, {"size", Human(size)}}});

      const data::Chunk& source = *encoded;
      recorder.Add({.suite = "data",
                    .name = "from_chunk",
                    .metrics = Throughput(
                        [&](std::int64_t) {
                          auto value =
                              registry.FromChunk<nlohmann::json>(source);
                          (void)value;
                        },
                        iterations, iterations / 10, 1, wire_bytes),
                    .params = {{"repr", label}, {"size", Human(size)}}});
    }
  }

  // The envelope, against fragments per message. This is the 16x asymmetry the
  // Python suite reported, measured where the code actually is.
  for (const std::int64_t fragments : {1, 8, 64}) {
    for (const std::int64_t size : {64, 4096}) {
      data::WireMessage message;
      for (std::int64_t index = 0; index < fragments; ++index) {
        message.node_fragments.push_back(data::NodeFragment{
            .id = "bench",
            .data = data::Chunk{.data = std::string(static_cast<size_t>(size),
                                                    'x')},
            .seq = static_cast<std::uint32_t>(index),
            .continued = true});
      }
      absl::StatusOr<data::Bytes> encoded = message.ToMsgpack();
      if (!encoded.ok()) {
        continue;
      }
      const auto wire_bytes = static_cast<std::int64_t>(encoded->size());
      const std::int64_t iterations = Scaled(
          std::max<std::int64_t>(
              400000 / (fragments * std::max<std::int64_t>(size / 64, 1)), 200),
          scale, 100);

      recorder.Add({.suite = "data",
                    .name = "wire_to_msgpack",
                    .metrics = Throughput(
                        [&](std::int64_t) {
                          auto bytes = message.ToMsgpack();
                          (void)bytes;
                        },
                        iterations, iterations / 10, fragments, wire_bytes),
                    .params = {{"frags", absl::StrCat(fragments)},
                               {"size", Human(size)}}});

      const data::Bytes& source = *encoded;
      recorder.Add({.suite = "data",
                    .name = "wire_from_msgpack",
                    .metrics = Throughput(
                        [&](std::int64_t) {
                          auto decoded = data::WireMessage::FromMsgpack(source);
                          (void)decoded;
                        },
                        iterations, iterations / 10, fragments, wire_bytes),
                    .params = {{"frags", absl::StrCat(fragments)},
                               {"size", Human(size)}}});
    }
  }

  // Resident bytes per live value, for the same reason the Python suite does
  // it: it bounds how much a store can hold.
  {
    std::vector<data::Chunk> held;
    std::string trail;
    const std::int64_t stage = Scaled(50000, scale, 5000);
    const double slope = MemorySlope(
        [&](std::int64_t count) {
          for (std::int64_t index = 0; index < count; ++index) {
            held.push_back(data::Chunk{.data = std::string(64, 'x')});
          }
        },
        std::vector<std::int64_t>(6, stage), &trail);
    recorder.Add(
        {.suite = "data",
         .name = "chunk_resident",
         .metrics = {{"bytes_each", slope}},
         .params = {{"payload", "64B"}},
         .note = absl::StrCat("64 payload bytes; the rest is the value's own. ",
                              trail)});
  }
}

// ---------------------------------------------------------------------------
// stores: what a node's data costs, with no binding in the way
// ---------------------------------------------------------------------------

void StoresSuite(Recorder& recorder, double scale) {
  const std::string payload(256, 'x');

  {
    auto store = *stores::LocalChunkStore::Create("bench-put");
    const std::int64_t iterations = Scaled(200000, scale, 1000);
    recorder.Add({.suite = "stores",
                  .name = "put",
                  .metrics = Latency(
                      [&](std::int64_t index) {
                        auto seq =
                            store
                                ->Put(Fragment(
                                    static_cast<std::uint32_t>(index), payload))
                                .Await(Deadline());
                        (void)seq;
                      },
                      iterations, iterations / 10),
                  .params = {{"backend", "local"}, {"size", "256B"}}});
  }

  for (const std::int64_t batch : {8, 64, 256}) {
    auto store =
        *stores::LocalChunkStore::Create(absl::StrCat("bench-many-", batch));
    const std::int64_t calls = Scaled(40000 / batch, scale, 20);
    recorder.Add(
        {.suite = "stores",
         .name = "put_many",
         .metrics = Throughput(
             [&](std::int64_t index) {
               std::vector<data::NodeFragment> fragments;
               fragments.reserve(static_cast<size_t>(batch));
               for (std::int64_t offset = 0; offset < batch; ++offset) {
                 fragments.push_back(Fragment(
                     static_cast<std::uint32_t>(index * batch + offset),
                     payload));
               }
               auto seqs =
                   store->PutMany(std::move(fragments)).Await(Deadline());
               (void)seqs;
             },
             calls, 2, batch, batch * 256),
         .params = {{"backend", "local"}, {"batch", absl::StrCat(batch)}}});
  }

  // Reads, and the batched-versus-single question the Python suite found 86x
  // of headroom in.
  const std::int64_t count = Scaled(200000, scale, 1000);
  const auto fill = [&](const std::string& id) {
    auto store = *stores::LocalChunkStore::Create(id);
    for (std::int64_t start = 0; start < count; start += 500) {
      const std::int64_t size = std::min<std::int64_t>(500, count - start);
      std::vector<data::NodeFragment> fragments;
      fragments.reserve(static_cast<size_t>(size));
      for (std::int64_t offset = 0; offset < size; ++offset) {
        fragments.push_back(Fragment(static_cast<std::uint32_t>(start + offset),
                                     payload, start + offset == count - 1));
      }
      auto seqs = store->PutMany(std::move(fragments)).Await(Deadline());
      (void)seqs;
    }
    return store;
  };

  {
    auto store = fill("bench-get");
    recorder.Add({.suite = "stores",
                  .name = "get_by_seq",
                  .metrics = Latency(
                      [&](std::int64_t index) {
                        auto fragment =
                            store
                                ->Get(static_cast<std::uint32_t>(index % count),
                                      Deadline())
                                .Await(Deadline());
                        (void)fragment;
                      },
                      count, count / 10),
                  .params = {{"backend", "local"}}});
  }

  for (const std::int64_t limit : {1, 64}) {
    auto store = fill(absl::StrCat("bench-drain-", limit));
    std::int64_t drained = 0;
    const auto metrics = Throughput(
        [&](std::int64_t) {
          auto batch = store->Next(Deadline(), static_cast<size_t>(limit))
                           .Await(Deadline());
          if (!batch.ok()) {
            return;
          }
          for (const auto& fragment : *batch) {
            if (fragment.has_value()) {
              ++drained;
            }
          }
        },
        count / limit, 0, limit, limit * 256);
    recorder.Add({.suite = "stores",
                  .name = limit == 1 ? "read_one_at_a_time" : "read_batched",
                  .metrics = metrics,
                  .params = {{"backend", "local"},
                             {"via", absl::StrCat("ChunkStore::Next(limit=",
                                                  limit, ")")}},
                  .note = absl::StrCat(drained, " fragments drained")});
  }

  {
    std::vector<std::shared_ptr<stores::LocalChunkStore>> held;
    std::string trail;
    std::int64_t made = 0;
    const std::int64_t stage = Scaled(2000, scale, 200);
    const double slope = MemorySlope(
        [&](std::int64_t n) {
          for (std::int64_t index = 0; index < n; ++index) {
            auto store =
                stores::LocalChunkStore::Create(absl::StrCat("empty-", made++));
            if (store.ok()) {
              held.push_back(*store);
            }
          }
        },
        std::vector<std::int64_t>(6, stage), &trail);
    recorder.Add({.suite = "stores",
                  .name = "empty_store",
                  .metrics = {{"bytes_each", slope}},
                  .params = {{"backend", "local"}},
                  .note = trail});
  }

  {
    auto store = *stores::LocalChunkStore::Create("bench-resident");
    std::uint32_t written = 0;
    std::string trail;
    const std::int64_t stage = Scaled(20000, scale, 2000);
    const double slope = MemorySlope(
        [&](std::int64_t n) {
          for (std::int64_t start = 0; start < n; start += 500) {
            const std::int64_t size = std::min<std::int64_t>(500, n - start);
            std::vector<data::NodeFragment> fragments;
            fragments.reserve(static_cast<size_t>(size));
            for (std::int64_t offset = 0; offset < size; ++offset) {
              fragments.push_back(Fragment(written++, payload));
            }
            auto seqs = store->PutMany(std::move(fragments)).Await(Deadline());
            (void)seqs;
          }
        },
        std::vector<std::int64_t>(6, stage), &trail);
    recorder.Add({.suite = "stores",
                  .name = "stored_fragment",
                  .metrics = {{"bytes_each", slope}},
                  .params = {{"backend", "local"}, {"payload", "256B"}},
                  .note = absl::StrCat(
                      "256 payload bytes; the rest is bookkeeping. ", trail)});
  }
}

// ---------------------------------------------------------------------------
// nodes: the reader and writer above the store
// ---------------------------------------------------------------------------

/**
 * The node layer, native.
 *
 * This is the one that settles the largest open question. The Python suite
 * measured `AsyncNode.next_fragment` at 11.9k/s against `ChunkStore.next(
 * limit=64)` at 1.02M/s and concluded a batched node read was worth 86x. That
 * conclusion assumed the node reader itself is cheap and only the per-await
 * boundary is dear. Measuring both here, with no boundary at all, says whether
 * that is true.
 */
void NodesSuite(Recorder& recorder, double scale) {
  const data::Chunk token{
      .metadata = data::ChunkMetadata{.mimetype = "application/json"},
      .data = R"({"seq":0,"text":"a token"})"};

  {
    const std::int64_t iterations = Scaled(100000, scale, 1000);
    recorder.Add({.suite = "nodes",
                  .name = "node_create",
                  .metrics = Throughput(
                      [&](std::int64_t index) {
                        auto store = stores::LocalChunkStore::Create(
                            absl::StrCat("created-", index));
                        if (!store.ok()) {
                          return;
                        }
                        auto node = nodes::AsyncNode::Create(*store);
                        (void)node;
                      },
                      iterations, iterations / 10)});
  }

  {
    auto store = *stores::LocalChunkStore::Create("bench-write");
    auto node = *nodes::AsyncNode::Create(store);
    const std::int64_t iterations = Scaled(100000, scale, 1000);
    recorder.Add(
        {.suite = "nodes",
         .name = "put",
         .metrics = Latency(
             [&](std::int64_t) {
               auto seq = node->PutChunk(token).Await(Deadline());
               (void)seq;
             },
             iterations, iterations / 10),
         .params = {{"path", "chunk"}, {"stage", "confirmed"}},
         .note = "the native Await is the store confirmation, not admission"});
  }

  {
    const std::int64_t count = Scaled(100000, scale, 1000);
    auto store = *stores::LocalChunkStore::Create("bench-read");
    auto node = *nodes::AsyncNode::Create(store);
    for (std::int64_t index = 0; index < count; ++index) {
      auto seq = node->PutChunk(token, std::nullopt, index == count - 1)
                     .Await(Deadline());
      (void)seq;
    }
    std::int64_t seen = 0;
    const auto metrics = Throughput(
        [&](std::int64_t) {
          auto fragment = node->NextFragment().Await(Deadline());
          if (fragment.ok() && fragment->has_value()) {
            ++seen;
          }
        },
        count, 0, 1, static_cast<std::int64_t>(token.data.size()));
    recorder.Add({.suite = "nodes",
                  .name = "read_one_at_a_time",
                  .metrics = metrics,
                  .params = {{"via", "AsyncNode::NextFragment"}},
                  .note = absl::StrCat(seen, " fragments read")});
  }

  {
    // The node's batched read, which had no native row -- so the only figure
    // for it came through the binding, and there was no way to tell the
    // reader's own per-fragment cost from the crossing's.
    const std::int64_t count = Scaled(100000, scale, 1000);
    auto store = *stores::LocalChunkStore::Create("bench-read-batched");
    auto node = *nodes::AsyncNode::Create(store);
    for (std::int64_t index = 0; index < count; ++index) {
      auto seq = node->PutChunk(token, std::nullopt, index == count - 1)
                     .Await(Deadline());
      (void)seq;
    }
    constexpr size_t kBatch = 64;
    // One *fragment* per iteration, refilling from a batched read when the
    // local buffer runs dry, so `per_op_items` is 1 and the reported
    // rate is fragments per second.
    std::deque<data::NodeFragment> pending;
    std::int64_t seen = 0;
    std::int64_t calls = 0;
    bool ended = false;
    const auto metrics = Throughput(
        [&](std::int64_t) {
          if (pending.empty() && !ended) {
            auto batch = node->NextFragments(kBatch).Await(Deadline());
            ++calls;
            if (!batch.ok()) {
              ended = true;
              return;
            }
            for (auto& fragment : *batch) {
              if (fragment.has_value()) {
                pending.push_back(std::move(*fragment));
              } else {
                ended = true;
              }
            }
          }
          if (!pending.empty()) {
            pending.pop_front();
            ++seen;
          }
        },
        count, 0, 1, static_cast<std::int64_t>(token.data.size()));
    recorder.Add(
        {.suite = "nodes",
         .name = "read_batched",
         .metrics = metrics,
         .params = {{"via", "AsyncNode::NextFragments(64)"}},
         .note = absl::StrCat(seen, " fragments in ", calls, " calls, mean ",
                              calls > 0 ? seen / calls : 0, " per call")});
  }

  {
    std::vector<std::shared_ptr<nodes::AsyncNode>> held;
    std::string trail;
    std::int64_t made = 0;
    const std::int64_t stage = Scaled(2000, scale, 200);
    const double slope = MemorySlope(
        [&](std::int64_t n) {
          for (std::int64_t index = 0; index < n; ++index) {
            auto store = stores::LocalChunkStore::Create(
                absl::StrCat("resident-", made++));
            if (!store.ok()) {
              continue;
            }
            auto node = nodes::AsyncNode::Create(*store);
            if (node.ok()) {
              held.push_back(*node);
            }
          }
        },
        std::vector<std::int64_t>(6, stage), &trail);
    recorder.Add({.suite = "nodes",
                  .name = "node_resident",
                  .metrics = {{"bytes_each", slope}},
                  .note = absl::StrCat("idle node, in-memory store. ", trail)});
  }
}

// `<sys/resource.h>` explicitly: macOS pulls it in transitively through other
// headers, GCC on Linux does not, so leaving it out builds here and fails
// there.
#include <sys/resource.h>

// How much CPU this process has used, across every thread.
double ProcessCpuSeconds() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0.0;
  }
  const auto seconds = [](const timeval& value) {
    return static_cast<double>(value.tv_sec) +
           static_cast<double>(value.tv_usec) / 1e6;
  };
  return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

// ---------------------------------------------------------------------------
// actions: the unit A11 is counted in
// ---------------------------------------------------------------------------

// Short enough that a wedged call stage reports promptly rather than consuming
// the whole run's budget. File scope so the driver lambdas can read it.
constexpr absl::Duration kStageTimeout = absl::Seconds(5);

actions::ActionSchema EchoSchema() {
  return actions::ActionSchema{
      .name = "echo",
      .inputs = {{"input",
                  actions::ActionPortSchema{
                      .name = "input", .type = "application/octet-stream"}}},
      .outputs = {{"output",
                   actions::ActionPortSchema{
                       .name = "output", .type = "application/octet-stream"}}},
  };
}

// Counts handler runs that wrote their reply. A wedge with this equal to the
// number of outstanding calls means the server replied and the client's wakeup
// was lost; below it, the server side is where the work stopped.
std::atomic<std::int64_t> g_echo_handler_replies{0};

actions::ActionHandler EchoHandler() {
  return [](std::shared_ptr<actions::Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      auto input = action->GetInput("input");
      if (!input.ok()) {
        return input.status();
      }
      auto chunk = (*input)->NextChunk().Await();
      if (!chunk.ok()) {
        return chunk.status();
      }
      if (!chunk->has_value()) {
        return absl::FailedPreconditionError("echo input ended early");
      }
      auto output = action->GetOutput("output");
      if (!output.ok()) {
        return output.status();
      }
      const absl::Status written =
          (*output)
              ->PutChunk(std::move(**chunk), std::nullopt, true)
              .Await()
              .status();
      if (written.ok()) {
        g_echo_handler_replies.fetch_add(1, std::memory_order_relaxed);
      }
      return written;
    });
  };
}

actions::ActionSchema PortlessSchema() {
  return actions::ActionSchema{.name = "portless"};
}

// A schema with `width` inputs and `width` outputs, none of which the handler
// touches.
/// A schema with `width` inputs and `width` outputs, none of which the handler
/// touches.
///
/// This is the shape that prices the Action's *teardown*, which is the only
/// place the width of a schema is paid in full: a handler that writes nothing
/// still leaves every output to be closed and every input to be finalised, and
/// those were once one pool handoff each in series. A caller that reads one
/// output of many is the normal case, not a pathological one -- see the note in
/// Action::CloseUnwrittenOutputs about not materialising untouched outputs.
actions::ActionSchema WideSchema(int width) {
  actions::ActionSchema schema{.name = absl::StrCat("wide-", width)};
  for (int index = 0; index < width; ++index) {
    const std::string in = absl::StrCat("in", index);
    const std::string out = absl::StrCat("out", index);
    schema.inputs.emplace(
        in, actions::ActionPortSchema{.name = in,
                                      .type = "application/octet-stream"});
    schema.outputs.emplace(
        out, actions::ActionPortSchema{.name = out,
                                       .type = "application/octet-stream"});
  }
  return schema;
}

/// A handler that spawns a fibre and finishes, mirroring a Python coroutine
/// handler: the interesting part is the spawn, not the body.
actions::ActionHandler FiberNoopHandler() {
  return [](const std::shared_ptr<actions::Action>&) {
    return a11::SubmitTask([]() -> absl::Status { return absl::OkStatus(); });
  };
}

/// A handler that is already finished when it is handed back. No fibre, no
/// scheduling -- whatever an action costs with this is the lifecycle alone.
actions::ActionHandler InlineNoopHandler() {
  return [](const std::shared_ptr<actions::Action>&) {
    return a11::ReadyTask();
  };
}

/**
 * A whole local action, native, broken down the way the Python probe was.
 *
 * The Python suite put a 1-in/1-out local action at ~500us against 155us here,
 * and the question that split does not answer is how much of the 155us is the
 * binding's absence and how much is the action design. So this measures the
 * same ladder: construction, construction plus run, the whole call with no
 * ports at all, and the same data movement with no action wrapped around it.
 * Read `fiber_round_trip` first -- it is to this suite what
 * `runtime/event_loop_turn` is to the Python one.
 */
void ActionsSuite(Recorder& recorder, double scale) {
  const actions::ActionSchema schema = EchoSchema();
  const actions::ActionHandler handler = EchoHandler();
  const actions::ActionSchema portless = PortlessSchema();
  const std::int64_t iterations = Scaled(20000, scale, 200);

  // The native floor: one hop onto a fibre and back, which is what every
  // Submit in the action path costs at minimum.
  recorder.Add({.suite = "actions",
                .name = "fiber_round_trip",
                .metrics = Latency(
                    [&](std::int64_t) {
                      auto done = a11::SubmitTask([]() -> absl::Status {
                                    return absl::OkStatus();
                                  }).Await(Deadline());
                      (void)done;
                    },
                    iterations, iterations / 10),
                .params = {}});

  recorder.Add({.suite = "actions",
                .name = "action_create",
                .metrics = Latency(
                    [&](std::int64_t index) {
                      auto created = actions::Action::Create(
                          portless, absl::StrCat("create-", index),
                          InlineNoopHandler());
                      (void)created;
                    },
                    iterations, iterations / 10),
                .params = {}});

  for (const auto& [label, noop] :
       std::vector<std::pair<std::string, actions::ActionHandler>>{
           {"inline", InlineNoopHandler()}, {"fiber", FiberNoopHandler()}}) {
    recorder.Add({.suite = "actions",
                  .name = "local_action",
                  .metrics = Latency(
                      [&, noop](std::int64_t index) {
                        auto created = actions::Action::Create(
                            portless, absl::StrCat("portless-", index), noop);
                        if (!created.ok()) {
                          return;
                        }
                        const std::shared_ptr<actions::Action>& action =
                            *created;
                        if (!action->Run().ok()) {
                          return;
                        }
                        auto done = action->Wait().Await(Deadline());
                        (void)done;
                      },
                      iterations, iterations / 10),
                  .params = {{"ports", "0in/0out"}, {"handler", label}}});
  }

  // Awaiting something already finished, to show that the 10us above is the
  // handoff to a worker and not the Future machinery around it.
  recorder.Add({.suite = "actions",
                .name = "ready_future_await",
                .metrics = Latency(
                    [&](std::int64_t) {
                      auto done = a11::ReadyTask().Await(Deadline());
                      (void)done;
                    },
                    iterations, iterations / 10),
                .params = {}});

  recorder.Add({.suite = "actions",
                .name = "local_action",
                .metrics = Latency(
                    [&](std::int64_t index) {
                      auto created = actions::Action::Create(
                          schema, absl::StrCat("local-", index), handler);
                      if (!created.ok()) {
                        return;
                      }
                      const std::shared_ptr<actions::Action>& action = *created;
                      auto input = action->GetInput("input", false);
                      if (!input.ok()) {
                        return;
                      }
                      auto put = (*input)
                                     ->PutChunk(data::Chunk{.data = "payload"},
                                                std::nullopt, true)
                                     .Await(Deadline());
                      (void)put;
                      if (!action->Run().ok()) {
                        return;
                      }
                      auto output = action->GetOutput("output", false);
                      if (!output.ok()) {
                        return;
                      }
                      auto chunk = (*output)->NextChunk().Await(Deadline());
                      (void)chunk;
                      auto done = action->Wait().Await(Deadline());
                      (void)done;
                    },
                    iterations, iterations / 10),
                .params = {{"ports", "1in/1out"}}});

  // Width, which is what prices teardown.
  for (const int width : {8, 32}) {
    const actions::ActionSchema wide = WideSchema(width);
    // The handler *opens* every port and closes none.
    const actions::ActionHandler noop =
        [width](const std::shared_ptr<actions::Action>& action) -> a11::Task {
      for (int index = 0; index < width; ++index) {
        auto in = action->GetInput(absl::StrCat("in", index), false);
        (void)in;
        auto out = action->GetOutput(absl::StrCat("out", index), false);
        (void)out;
      }
      return a11::ReadyTask();
    };
    const std::int64_t wide_iterations =
        std::max<std::int64_t>(1, iterations / (width / 4));
    recorder.Add(
        {.suite = "actions",
         .name = "local_action",
         .metrics = Latency(
             [&](std::int64_t index) {
               auto created = actions::Action::Create(
                   wide, absl::StrCat("wide-", width, "-", index), noop);
               if (!created.ok()) {
                 return;
               }
               const std::shared_ptr<actions::Action>& action = *created;
               if (!action->Run().ok()) {
                 return;
               }
               auto done = action->Wait().Await(Deadline());
               (void)done;
             },
             wide_iterations, wide_iterations / 10),
         .params = {{"ports", absl::StrCat(width, "in/", width, "out")}},
         .note = "handler touches no port; the cost is create plus teardown"});
  }

  // The same data movement, with no action around it: two nodes, a write and a
  // read each way. The gap against local_action[1in/1out] is what the action
  // layer charges for carrying it.
  recorder.Add(
      {.suite = "actions",
       .name = "node_pair_echo",
       .metrics = Latency(
           [&](std::int64_t index) {
             auto store_a =
                 stores::LocalChunkStore::Create(absl::StrCat("np-a-", index));
             auto store_b =
                 stores::LocalChunkStore::Create(absl::StrCat("np-b-", index));
             if (!store_a.ok() || !store_b.ok()) {
               return;
             }
             auto first = nodes::AsyncNode::Create(*store_a);
             auto second = nodes::AsyncNode::Create(*store_b);
             if (!first.ok() || !second.ok()) {
               return;
             }
             auto wrote = (*first)
                              ->PutChunk(data::Chunk{.data = "payload"},
                                         std::nullopt, true)
                              .Await(Deadline());
             (void)wrote;
             auto read = (*first)->NextChunk().Await(Deadline());
             if (!read.ok() || !read->has_value()) {
               return;
             }
             auto echoed = (*second)
                               ->PutChunk(std::move(**read), std::nullopt, true)
                               .Await(Deadline());
             (void)echoed;
             auto back = (*second)->NextChunk().Await(Deadline());
             (void)back;
           },
           iterations, iterations / 10),
       .params = {}});
}

// ---------------------------------------------------------------------------
// scheduling: what A11 pays to move work between contexts
// ---------------------------------------------------------------------------

/**
 * The layers between "switch a fibre" and "Submit and Await".
 *
 * Boost documents a context switch at tens of nanoseconds, and a Submit round
 * trip measures around 8us, so nearly all of it is somewhere other than the
 * switch. This prices each layer in between so the next optimisation aims at
 * the right one: a yield between two fibres already on this thread, a fibre
 * created and joined on this thread, a bare handoff to the worker pool, and the
 * full Submit.
 */
void SchedulingSuite(Recorder& recorder, double scale) {
  const std::int64_t iterations = Scaled(20000, scale, 500);

  // Boost's own number: two switches, no scheduler queue, no thread involved.
  recorder.Add(
      {.suite = "scheduling",
       .name = "fiber_yield",
       .metrics = Latency([&](std::int64_t) { boost::this_fiber::yield(); },
                          iterations, iterations / 10),
       .params = {}});

  // Create, run and join a fibre on this thread's own scheduler.
  recorder.Add({.suite = "scheduling",
                .name = "fiber_local_spawn",
                .metrics = Latency(
                    [&](std::int64_t) {
                      boost::fibers::fiber worker([] {});
                      worker.join();
                    },
                    iterations, iterations / 10),
                .params = {}});

  // A mutex nobody is contending, for scale.
  {
    thread::Mutex mu;
    recorder.Add(
        {.suite = "scheduling",
         .name = "uncontended_mutex",
         .metrics = Latency([&](std::int64_t) { thread::MutexLock lock(&mu); },
                            iterations, iterations / 10),
         .params = {}});
  }

  // One callback through the pool: the queue push, the condvar signal, and the
  // wake of whichever worker thread the OS chooses.
  {
    thread::Mutex mu;
    thread::CondVar cv;
    bool done = false;
    recorder.Add({.suite = "scheduling",
                  .name = "pool_post_round_trip",
                  .metrics = Latency(
                      [&](std::int64_t) {
                        {
                          thread::MutexLock lock(&mu);
                          done = false;
                        }
                        thread::Post([&] {
                          thread::MutexLock lock(&mu);
                          done = true;
                          cv.Signal();
                        });
                        thread::MutexLock lock(&mu);
                        while (!done) {
                          cv.Wait(&mu);
                        }
                      },
                      iterations, iterations / 10),
                  .params = {}});
  }

  // The same handoff with the workers kept awake: posts are issued back to back
  // and only the last is waited for, so no worker parks between them.
  {
    // The counter is atomic and only the last callback touches the mutex: a
    // shared lock across 256 callbacks on 14 workers measures the lock, not the
    // pool.
    thread::Mutex mu;
    thread::CondVar cv;
    std::atomic<std::int64_t> remaining{0};
    const std::int64_t batch = 256;
    recorder.Add(
        {.suite = "scheduling",
         .name = "pool_post_pipelined",
         .metrics = Throughput(
             [&](std::int64_t) {
               remaining.store(batch, std::memory_order_release);
               for (std::int64_t index = 0; index < batch; ++index) {
                 thread::Post([&] {
                   if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                     thread::MutexLock lock(&mu);
                     cv.Signal();
                   }
                 });
               }
               while (remaining.load(std::memory_order_acquire) > 0) {
                 thread::MutexLock lock(&mu);
                 if (remaining.load(std::memory_order_acquire) > 0) {
                   cv.WaitWithTimeout(&mu, absl::Milliseconds(1));
                 }
               }
             },
             std::max<std::int64_t>(iterations / 64, 8), 4, 256),
         .params = {{"batch", "256"}}});
  }

  // Registering a deadline, which is what every request and session timeout in
  // the system does, and what a store read with a timeout does per read.
  {
    const absl::Time base = absl::Now() + absl::Hours(1);
    std::atomic<std::int64_t> step{0};
    recorder.Add({.suite = "scheduling",
                  .name = "post_at_earliest",
                  .metrics = Latency(
                      [&](std::int64_t) {
                        thread::PostAt(
                            base - absl::Microseconds(step.fetch_add(
                                       1, std::memory_order_relaxed)),
                            [] {});
                      },
                      iterations / 4, iterations / 40),
                  .params = {}});
  }

  // The whole thing, for comparison against the rows above.
  recorder.Add({.suite = "scheduling",
                .name = "submit_round_trip",
                .metrics = Latency(
                    [&](std::int64_t) {
                      auto done = a11::SubmitTask([]() -> absl::Status {
                                    return absl::OkStatus();
                                  }).Await(Deadline());
                      (void)done;
                    },
                    iterations, iterations / 10),
                .params = {}});

  // The same Submit, measured from inside a fibre that is already on a pool
  // worker. The caller is then a fibre the scheduler can park and resume
  // without waking a sleeping thread.
  recorder.Add({.suite = "scheduling",
                .name = "submit_round_trip",
                .metrics = Latency(
                    [&](std::int64_t) {
                      auto outer = a11::SubmitTask([]() -> absl::Status {
                                     auto inner =
                                         a11::SubmitTask([]() -> absl::Status {
                                           return absl::OkStatus();
                                         }).Await();
                                     return inner.status();
                                   }).Await(Deadline());
                      (void)outer;
                    },
                    iterations / 2, iterations / 20),
                .params = {{"from", "fiber"}}});

  // Genuine parallel work, and the counterweight to every other row here.
  {
    constexpr int kFibres = 16;
    // Enough work per fibre to be worth a core and to dwarf the handoff, but
    // short enough that the row is not the whole suite's runtime.
    constexpr std::int64_t kBurnIterations = 200000;
    recorder.Add({.suite = "scheduling",
                  .name = "parallel_cpu",
                  .metrics = Throughput(
                      [&](std::int64_t) {
                        std::vector<a11::Task> tasks;
                        tasks.reserve(kFibres);
                        for (int fibre = 0; fibre < kFibres; ++fibre) {
                          tasks.push_back(a11::SubmitTask([]() -> absl::Status {
                            // volatile so the optimiser leaves the work alone.
                            volatile std::int64_t sink = 0;
                            for (std::int64_t step = 0; step < kBurnIterations;
                                 ++step) {
                              sink = sink + step;
                            }
                            return absl::OkStatus();
                          }));
                        }
                        for (a11::Task& task : tasks) {
                          (void)task.Await(Deadline());
                        }
                      },
                      std::max<std::int64_t>(iterations / 256, 4), 2, kFibres),
                  .params = {{"fibres", "16"}}});
  }
}

// ---------------------------------------------------------------------------
// wire: the transport
// ---------------------------------------------------------------------------

/**
 * A message round trip, per transport.
 *
 * The same echo over each transport A11 speaks, so they can be put beside each
 * other: in-process, WebSocket, SSE, and a WebRTC data channel. Two reference
 * rows bracket them -- a bare TCP ping-pong for what the kernel costs, and the
 * in-process pair for what A11 costs with no socket under it -- because without
 * both it is guesswork whether a transport's number is its own overhead or the
 * loopback it is sitting on.
 */
namespace {

// A client endpoint whose peer is already accepting, plus how to shut both
// down.
struct BenchPair {
  std::shared_ptr<net::WireStream> client;
  std::function<void()> close;
  // Run after Start() has resolved on both ends, for anything that needs a live
  // connection rather than a configured one.
  std::function<void()> after_start = {};
};

// The slot is where the factory publishes the accepted peer, so the echo can
// find the endpoint it must reply on. One slot per measurement, owned by it.
using PairFactory = std::function<std::optional<BenchPair>(
    std::weak_ptr<net::WireStream>*, net::OnMessage, net::OnDone)>;

// Counted arrivals, shared with the callback rather than captured by reference.
struct Arrivals {
  thread::Mutex mu;
  thread::CondVar cv;
  std::int64_t count ABSL_GUARDED_BY(mu) = 0;
};

a11::Task NothingToDo() {
  return a11::ReadyTask();
}

// An echoing pair with the client started, or nothing if the transport could
// not be brought up. `arrivals` counts the echoes the client has seen.
struct EchoSetup {
  std::optional<BenchPair> pair;
  std::shared_ptr<Arrivals> arrivals;
  // The echo replies on the endpoint published here, and holds it weakly so it
  // is not part of a cycle with the stream it replies on.
  std::shared_ptr<std::weak_ptr<net::WireStream>> peer;
};

// Builds an echoing pair and starts the client. `what` names the measurement in
// the skip line, since a transport that cannot start should abandon its row
// rather than hang the suite.
EchoSetup StartEchoPair(const std::string& transport, const std::string& what,
                        const PairFactory& make_pair) {
  EchoSetup setup;
  setup.arrivals = std::make_shared<Arrivals>();
  setup.peer = std::make_shared<std::weak_ptr<net::WireStream>>();
  // Accounting is in fragments, not messages, and that is not a detail. Replies
  // on the fibre the transport already delivered on.
  const std::shared_ptr<std::weak_ptr<net::WireStream>> peer = setup.peer;
  net::OnMessage on_server =
      [peer](std::optional<data::WireMessage> message) -> a11::Task {
    if (!message.has_value()) {
      return a11::ReadyTask();
    }
    const std::shared_ptr<net::WireStream> endpoint = peer->lock();
    if (endpoint == nullptr) {
      return a11::ReadyTask();
    }
    // One echo fragment per arriving fragment, in one message: the credit stays
    // exact through a fold without paying a second round trip for it.
    data::WireMessage reply;
    reply.node_fragments.reserve(message->node_fragments.size());
    for (size_t index = 0; index < message->node_fragments.size(); ++index) {
      reply.node_fragments.push_back(
          data::NodeFragment{.id = absl::StrCat("bench-", index),
                             .data = data::Chunk{.data = "y"}});
    }
    const absl::Status sent = endpoint->Send(std::move(reply));
    return sent.ok() ? a11::ReadyTask() : a11::FailedTask(sent);
  };

  std::optional<BenchPair> pair =
      make_pair(setup.peer.get(), std::move(on_server), NothingToDo);
  if (!pair.has_value()) {
    return {};
  }

  // Counts on the delivery fibre, for the same reason the echo replies there.
  const std::shared_ptr<Arrivals> arrivals = setup.arrivals;
  net::OnMessage on_client =
      [arrivals](std::optional<data::WireMessage> message) -> a11::Task {
    if (!message.has_value()) {
      return a11::ReadyTask();
    }
    {
      thread::MutexLock lock(&arrivals->mu);
      arrivals->count +=
          static_cast<std::int64_t>(message->node_fragments.size());
    }
    arrivals->cv.SignalAll();
    return a11::ReadyTask();
  };
  const absl::Status started =
      pair->client->Start(std::move(on_client), NothingToDo)
          .Await(Deadline())
          .status();
  if (!started.ok()) {
    std::fprintf(stderr, "  skip %s %s: %s\n", transport.c_str(), what.c_str(),
                 std::string(started.message()).c_str());
    pair->close();
    return {};
  }
  if (pair->after_start) {
    pair->after_start();
  }
  setup.pair = std::move(pair);
  return setup;
}

std::int64_t ArrivalCount(const std::shared_ptr<Arrivals>& arrivals) {
  thread::MutexLock lock(&arrivals->mu);
  return arrivals->count;
}

// Waits until `arrivals` reaches `target`, or gives up after five seconds.
// Returns false on the timeout, which abandons the row.
bool AwaitArrivals(const std::shared_ptr<Arrivals>& arrivals,
                   std::int64_t target) {
  thread::MutexLock lock(&arrivals->mu);
  const absl::Time limit = absl::Now() + absl::Seconds(5);
  while (arrivals->count < target) {
    if (arrivals->cv.WaitWithDeadline(&arrivals->mu, limit)) {
      return false;
    }
  }
  return true;
}

// Builds the pair, starts the client, then times a send-and-wait per iteration.
void MeasureRoundTrip(Recorder& recorder, double scale,
                      const std::string& transport, std::int64_t size,
                      const PairFactory& make_pair) {
  EchoSetup setup = StartEchoPair(transport, "round trip", make_pair);
  if (!setup.pair.has_value()) {
    return;
  }
  std::optional<BenchPair>& pair = setup.pair;
  const std::shared_ptr<Arrivals> arrivals = setup.arrivals;

  const data::WireMessage payload{
      .node_fragments = {data::NodeFragment{
          .id = "bench",
          .data = data::Chunk{
              .data = std::string(static_cast<size_t>(size), 'x')}}}};
  const std::int64_t iterations = Scaled(20000, scale, 200);
  // Every wait is bounded, and the first one that is not answered abandons the
  // row.
  bool stalled = false;
  auto metrics = Latency(
      [&](std::int64_t) {
        if (stalled) {
          return;
        }
        std::int64_t before = 0;
        {
          thread::MutexLock lock(&arrivals->mu);
          before = arrivals->count;
        }
        if (!pair->client->Send(payload).ok()) {
          stalled = true;
          return;
        }
        thread::MutexLock lock(&arrivals->mu);
        const absl::Time limit = absl::Now() + absl::Seconds(5);
        while (arrivals->count == before) {
          if (arrivals->cv.WaitWithDeadline(&arrivals->mu, limit)) {
            stalled = true;
            return;
          }
        }
      },
      iterations, iterations / 10);
  if (stalled) {
    std::fprintf(stderr, "  skip %s round trip at %s: no echo within 5s\n",
                 transport.c_str(), Human(size).c_str());
    pair->close();
    return;
  }
  metrics["mib_per_s"] =
      metrics["ops_per_s"] * static_cast<double>(size) / 1048576.0;
  recorder.Add({.suite = "wire",
                .name = "message_round_trip",
                .metrics = metrics,
                .params = {{"transport", transport}, {"size", Human(size)}}});
  pair->close();
}

/**
 * How many messages a transport carries per second with a pipeline open.
 *
 * The round-trip row divided by its payload is *not* this number. It prices one
 * message at a time, so it reports size/latency -- a figure in which every
 * scheduling cost is charged to the payload and no cost is ever overlapped.
 * Throughput is a different question, and the one a "5 Gbit/s at 64 KiB" target
 * is actually about: with more than one message in flight, the encode of the
 * next can overlap the write of the last, several can merge into one frame, and
 * the loop crossing amortises across the window.
 *
 * The window is what makes it a pipeline and also what keeps it safe: Send has
 * no admission signal, so an unpaced flood aborts the connection rather than
 * pushing back (FINDINGS.md item 7). The peer's one-byte echo per message is
 * the credit that lets the next one go -- cheap enough on the wire (~80 bytes
 * against 64 KiB) that the reverse direction does not distort the rate, and it
 * doubles as the completion signal.
 */
void MeasureStreamThroughput(Recorder& recorder, double scale,
                             const std::string& transport, std::int64_t size,
                             std::int64_t window,
                             const PairFactory& make_pair) {
  EchoSetup setup = StartEchoPair(transport, "throughput", make_pair);
  if (!setup.pair.has_value()) {
    return;
  }
  std::optional<BenchPair>& pair = setup.pair;
  const std::shared_ptr<Arrivals> arrivals = setup.arrivals;

  const data::WireMessage payload{
      .node_fragments = {data::NodeFragment{
          .id = "bench",
          .data = data::Chunk{
              .data = std::string(static_cast<size_t>(size), 'x')}}}};
  // Fewer messages than the latency rows: each one is a whole payload through
  // the transport, and a 64 KiB row at 2 GiB/s still moves gigabytes.
  const std::int64_t messages = Scaled(4000, scale, 200);
  const std::int64_t warmup = std::min<std::int64_t>(messages / 10, 200);

  bool stalled = false;
  // One pass: send `count` messages keeping at most `window` unacknowledged,
  // then wait for the rest of the acknowledgements.
  const auto pump = [&](std::int64_t count) {
    std::int64_t base = 0;
    {
      thread::MutexLock lock(&arrivals->mu);
      base = arrivals->count;
    }
    for (std::int64_t sent = 0; sent < count; ++sent) {
      if (sent - (ArrivalCount(arrivals) - base) >= window &&
          !AwaitArrivals(arrivals, base + sent - window + 1)) {
        stalled = true;
        return;
      }
      if (!pair->client->Send(payload).ok()) {
        stalled = true;
        return;
      }
    }
    if (!AwaitArrivals(arrivals, base + count)) {
      stalled = true;
    }
  };

  if (warmup > 0) {
    pump(warmup);
  }
  auto metrics = Throughput([&](std::int64_t) { pump(messages); }, 1, 0,
                            messages, messages * size);
  if (stalled) {
    // The stream's status distinguishes the two things a stall can be: a
    // transport that failed (and says why) from one that is merely slow.
    const absl::Status status = pair->client->GetStatus();
    std::fprintf(stderr, "  skip %s throughput at %s: no echo within 5s (%s)\n",
                 transport.c_str(), Human(size).c_str(),
                 status.ok() ? "stream still ok"
                             : std::string(status.ToString()).c_str());
    pair->close();
    return;
  }
  // Throughput() reports the rate of its `operation`, which here is one whole
  // pass; the per-message figures are the ones in items_per_s / mib_per_s.
  metrics["ops_per_s"] = metrics["items_per_s"];
  recorder.Add({.suite = "wire",
                .name = "stream_throughput",
                .metrics = metrics,
                .params = {{"transport", transport},
                           {"size", Human(size)},
                           {"window", absl::StrCat(window)}}});
  pair->close();
}

// Both endpoints in this process: no sockets, no framing, no codec beyond the
// envelope itself.
std::optional<BenchPair> InProcessPair(
    std::weak_ptr<net::WireStream>* peer_slot, net::OnMessage on_server,
    net::OnDone on_done) {
  absl::StatusOr<net::InProcessWireStream::Pair> pair =
      net::InProcessWireStream::CreatePair();
  if (!pair.ok()) {
    return std::nullopt;
  }
  const std::shared_ptr<net::InProcessWireStream> client = pair->first;
  const std::shared_ptr<net::InProcessWireStream> server = pair->second;
  *peer_slot = server;
  const absl::Status accepted =
      server->Accept(std::move(on_server), std::move(on_done))
          .Await(Deadline())
          .status();
  if (!accepted.ok()) {
    return std::nullopt;
  }
  return BenchPair{.client = client, .close = [client, server] {
                     client->HalfClose().IgnoreError();
                     (void)client->DrainOutgoingMessages().Await(Deadline());
                   }};
}

// A WebSocket over loopback: a real listener, a real RFC 6455 handshake, and
// channel framing on every message.
std::optional<BenchPair> WebSocketPair(
    std::weak_ptr<net::WireStream>* peer_slot, net::OnMessage on_server,
    net::OnDone on_done) {
  net::WebSocketServerOptions options;
  options.path = "/bench";
  options.bind_address = "127.0.0.1";
  options.port = 0;
  // A11 packetises a message before the channel frames it, and the default
  // split (64 KiB) is just below a 64 KiB payload plus its envelope -- so the
  // target message size is exactly the size that turns one message into two.
  if (const std::optional<int> ws_split = EnvironmentInt("A11_BENCH_WS_SPLIT");
      ws_split.has_value()) {
    options.framing.split_size = static_cast<size_t>(std::max(1024, *ws_split));
  }
  // HTTP/1.1 on both sides. What this row should price is WebSocket framing,
  // not ALPN, and offering h2 to a listener that does not serve it costs the
  // connection rather than just the negotiation.
  options.http2_options.enable_h2 = false;
  options.http2_options.enable_h2c = false;

  const auto shared_server =
      std::make_shared<net::OnMessage>(std::move(on_server));
  const auto shared_done = std::make_shared<net::OnDone>(std::move(on_done));
  // Somebody has to own the accepted stream.
  const auto accepted = std::make_shared<std::shared_ptr<net::WireStream>>();
  absl::StatusOr<std::shared_ptr<net::WebSocketWireServer>> server =
      net::WebSocketWireServer::Create(
          [peer_slot, accepted, shared_server,
           shared_done](const std::shared_ptr<net::WebSocketWireStream>& stream)
              -> a11::Task {
            // Accepting here is what sends the 101.
            *accepted = stream;
            *peer_slot = stream;
            return stream->Accept(*shared_server, *shared_done);
          },
          options);
  if (!server.ok()) {
    std::fprintf(stderr, "  skip websocket: %s\n",
                 std::string(server.status().message()).c_str());
    return std::nullopt;
  }
  const absl::StatusOr<std::uint16_t> port = (*server)->port();
  if (!port.ok()) {
    return std::nullopt;
  }
  net::WebSocketClientOptions client_options;
  client_options.http2_options.enable_h2 = false;
  client_options.http2_options.enable_h2c = false;
  client_options.framing = options.framing;
  absl::StatusOr<std::shared_ptr<net::WebSocketWireStream>> client =
      net::WebSocketWireStream::CreateClient(
          absl::StrCat("ws://127.0.0.1:", *port, "/bench"), {}, client_options);
  if (!client.ok()) {
    std::fprintf(stderr, "  skip websocket: %s\n",
                 std::string(client.status().message()).c_str());
    (*server)->Stop().IgnoreError();
    return std::nullopt;
  }
  const std::shared_ptr<net::WebSocketWireStream>& holder = *client;
  const std::shared_ptr<net::WebSocketWireServer>& listener = *server;
  return BenchPair{.client = holder, .close = [holder, listener, accepted] {
                     holder->HalfClose().IgnoreError();
                     (void)holder->DrainOutgoingMessages().Await(Deadline());
                     listener->Stop().IgnoreError();
                     accepted->reset();
                   }};
}

// Server-Sent Events: the transport a browser can always reach, and the only
// one whose two directions are different mechanisms -- an SSE response stream
// inbound, and outbound either one HTTP POST per message or a single.
std::optional<BenchPair> HttpSseVariantPair(
    net::SseOutboundDelivery outbound,
    std::weak_ptr<net::WireStream>* peer_slot, net::OnMessage on_server,
    net::OnDone on_done) {
  net::HttpSseOptions client_options;
  client_options.outbound = outbound;
  if (const std::optional<int> posts = EnvironmentInt("A11_BENCH_SSE_POSTS");
      posts.value_or(0) > 0) {
    client_options.max_concurrent_posts = static_cast<size_t>(*posts);
  }
  const auto shared_server =
      std::make_shared<net::OnMessage>(std::move(on_server));
  const auto shared_done = std::make_shared<net::OnDone>(std::move(on_done));
  const auto accepted = std::make_shared<std::shared_ptr<net::WireStream>>();
  absl::StatusOr<std::shared_ptr<net::HttpSseServer>> server =
      net::HttpSseServer::Create(
          "127.0.0.1", 0,
          [peer_slot, accepted, shared_server, shared_done](
              const std::shared_ptr<net::HttpSseServerWireStream>& stream)
              -> a11::Task {
            *accepted = stream;
            *peer_slot = stream;
            return stream->Accept(*shared_server, *shared_done);
          });
  if (!server.ok()) {
    std::fprintf(stderr, "  skip sse: %s\n",
                 std::string(server.status().message()).c_str());
    return std::nullopt;
  }
  absl::StatusOr<std::shared_ptr<net::HttpSseClientWireStream>> client =
      net::HttpSseClientWireStream::Create(
          absl::StrCat("http://127.0.0.1:", (*server)->port()),
          std::move(client_options));
  if (!client.ok()) {
    std::fprintf(stderr, "  skip sse: %s\n",
                 std::string(client.status().message()).c_str());
    (*server)->Stop().IgnoreError();
    return std::nullopt;
  }
  const std::shared_ptr<net::HttpSseClientWireStream>& holder = *client;
  const std::shared_ptr<net::HttpSseServer>& listener = *server;
  return BenchPair{.client = holder, .close = [holder, listener, accepted] {
                     holder->HalfClose().IgnoreError();
                     (void)holder->DrainOutgoingMessages().Await(Deadline());
                     listener->Stop().IgnoreError();
                     accepted->reset();
                   }};
}

std::optional<BenchPair> HttpSsePair(std::weak_ptr<net::WireStream>* peer_slot,
                                     net::OnMessage on_server,
                                     net::OnDone on_done) {
  return HttpSseVariantPair(net::SseOutboundDelivery::kPost, peer_slot,
                            std::move(on_server), std::move(on_done));
}

std::optional<BenchPair> HttpSseStreamPair(
    std::weak_ptr<net::WireStream>* peer_slot, net::OnMessage on_server,
    net::OnDone on_done) {
  return HttpSseVariantPair(net::SseOutboundDelivery::kStream, peer_slot,
                            std::move(on_server), std::move(on_done));
}

// WebRTC over an SCTP data channel, negotiated through in-process signalling.
std::optional<BenchPair> WebRtcVariantPair(
    bool set_mtu_live, std::weak_ptr<net::WireStream>* peer_slot,
    net::OnMessage on_server, net::OnDone on_done) {
  const std::shared_ptr<net::SignallingService> signalling =
      net::SignallingService::Create();
  const auto shared_server =
      std::make_shared<net::OnMessage>(std::move(on_server));
  const auto shared_done = std::make_shared<net::OnDone>(std::move(on_done));
  const auto accepted = std::make_shared<std::shared_ptr<net::WireStream>>();
  // A11 fragments a large message before SCTP sees it, so the split size sets
  // how many packets a 64K message becomes and how they stripe across the
  // channels. A11_BENCH_RTC_SPLIT sweeps it.
  net::WebRtcConfiguration configuration;
  if (const std::optional<int> split = EnvironmentInt("A11_BENCH_RTC_SPLIT");
      split.has_value()) {
    configuration.channel_split_size =
        static_cast<size_t>(std::max(1024, *split));
  }
  // The SCTP path MTU, which the raw-webrtc rows establish is worth ~3x at
  // 64 KiB and has a silent cliff above ~4 KiB. Same variable as those rows so
  // one sweep drives both and the A11 and bare columns stay comparable.
  const std::optional<int> mtu = EnvironmentInt("A11_BENCH_RTC_MTU");
  const size_t requested_mtu =
      mtu.value_or(0) > 0 ? static_cast<size_t>(*mtu) : 0;
  // `webrtc` configures the MTU at construction; `webrtc-live-mtu` leaves the
  // configuration at the 1280 default and applies the same value *after* both
  // ends are up.
  if (requested_mtu != 0 && !set_mtu_live) {
    configuration.mtu = requested_mtu;
  }
  absl::StatusOr<std::shared_ptr<net::WebRtcWireServer>> server =
      net::WebRtcWireServer::Create(
          "bench-server", signalling,
          [peer_slot, accepted, shared_server,
           shared_done](const std::shared_ptr<net::WebRtcWireStream>& stream)
              -> a11::Task {
            *accepted = stream;
            *peer_slot = stream;
            return a11::SubmitTask(
                [stream, shared_server, shared_done]() -> absl::Status {
                  return stream->Accept(*shared_server, *shared_done)
                      .Await(Deadline())
                      .status();
                });
          },
          configuration);
  if (!server.ok()) {
    std::fprintf(stderr, "  skip webrtc: %s\n",
                 std::string(server.status().message()).c_str());
    return std::nullopt;
  }
  absl::StatusOr<std::shared_ptr<net::WebRtcWireStream>> client =
      net::WebRtcWireStream::CreateClient("bench-client", "bench-server",
                                          signalling, configuration);
  if (!client.ok()) {
    std::fprintf(stderr, "  skip webrtc: %s\n",
                 std::string(client.status().message()).c_str());
    (*server)->Stop().IgnoreError();
    return std::nullopt;
  }
  const std::shared_ptr<net::WebRtcWireStream>& holder = *client;
  const std::shared_ptr<net::WebRtcWireServer>& listener = *server;
  std::function<void()> after_start;
  if (!set_mtu_live && configuration.path_mtu_discovery && requested_mtu == 0) {
    // Wait for discovery to settle before measuring.
    after_start = [holder] {
      // Waits for the value to stop moving, not for it to become non-zero.
      const absl::Time limit = absl::Now() + absl::Seconds(40);
      size_t settled = 0;
      absl::Time stable_since = absl::Now();
      while (absl::Now() < limit) {
        const size_t now = holder->discovered_path_mtu();
        if (now != settled) {
          settled = now;
          stable_since = absl::Now();
        } else if (settled != 0 &&
                   absl::Now() - stable_since > absl::Seconds(3)) {
          break;
        }
        thread::SleepFor(absl::Milliseconds(20));
      }
      std::fprintf(stderr, "  webrtc: path MTU discovery settled at %zu\n",
                   settled);
    };
  }
  if (set_mtu_live && requested_mtu != 0) {
    after_start = [holder, accepted, requested_mtu] {
      // Both ends: the MTU bounds what a *sender* emits, and the round-trip row
      // has the peer echoing the whole payload back, so setting only the client
      // would leave half the measurement at 1280 and understate the change.
      const absl::Time limit = absl::Now() + absl::Seconds(5);
      bool client_set = false;
      bool peer_set = false;
      while (absl::Now() < limit && !(client_set && peer_set)) {
        if (!client_set) {
          client_set = holder->SetPathMtu(requested_mtu).ok();
        }
        if (!peer_set) {
          const auto peer =
              std::dynamic_pointer_cast<net::WebRtcWireStream>(*accepted);
          peer_set = peer != nullptr && peer->SetPathMtu(requested_mtu).ok();
        }
        if (!(client_set && peer_set)) {
          thread::SleepFor(absl::Milliseconds(5));
        }
      }
      if (!client_set || !peer_set) {
        std::fprintf(stderr,
                     "  webrtc-live-mtu: could not set %zu live (client=%d "
                     "peer=%d) -- row measures the configured MTU\n",
                     requested_mtu, static_cast<int>(client_set),
                     static_cast<int>(peer_set));
      }
    };
  }
  return BenchPair{
      .client = holder,
      .close =
          [holder, listener, accepted, signalling] {
            holder->HalfClose().IgnoreError();
            (void)holder->DrainOutgoingMessages().Await(Deadline());
            listener->Stop().IgnoreError();
            accepted->reset();
          },
      .after_start = std::move(after_start)};
}

std::optional<BenchPair> WebRtcPair(std::weak_ptr<net::WireStream>* peer_slot,
                                    net::OnMessage on_server,
                                    net::OnDone on_done) {
  return WebRtcVariantPair(/*set_mtu_live=*/false, peer_slot,
                           std::move(on_server), std::move(on_done));
}

std::optional<BenchPair> WebRtcLiveMtuPair(
    std::weak_ptr<net::WireStream>* peer_slot, net::OnMessage on_server,
    net::OnDone on_done) {
  return WebRtcVariantPair(/*set_mtu_live=*/true, peer_slot,
                           std::move(on_server), std::move(on_done));
}

}  // namespace

// A round trip to the libuv loop and back, with nothing else in it.
void MeasureUvCrossing(Recorder& recorder, double scale) {
  thread::Mutex mu;
  thread::CondVar cv;
  bool done = false;
  const std::int64_t iterations = Scaled(20000, scale, 500);
  bool broken = false;
  auto metrics = Latency(
      [&](std::int64_t) {
        if (broken) {
          return;
        }
        {
          thread::MutexLock lock(&mu);
          done = false;
        }
        const absl::Status posted =
            net::internal::UvExecutor::Instance().Post([&] {
              thread::MutexLock lock(&mu);
              done = true;
              cv.SignalAll();
            });
        if (!posted.ok()) {
          broken = true;
          return;
        }
        thread::MutexLock lock(&mu);
        const absl::Time limit = absl::Now() + absl::Seconds(5);
        while (!done) {
          if (cv.WaitWithDeadline(&mu, limit)) {
            broken = true;
            return;
          }
        }
      },
      iterations, iterations / 10);
  if (broken) {
    std::fprintf(stderr, "  skip uv_round_trip: the loop did not answer\n");
    return;
  }
  recorder.Add({.suite = "wire",
                .name = "uv_round_trip",
                .metrics = metrics,
                .params = {}});
}

// A bare TCP ping-pong over loopback, with no A11 in it at all.
void MeasureLoopbackFloor(Recorder& recorder, double scale, std::int64_t size) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    return;
  }
  int one = 1;
  (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  auto* bind_address = reinterpret_cast<sockaddr*>(&address);
  if (::bind(listener, bind_address, sizeof(address)) != 0 ||
      ::listen(listener, 1) != 0) {
    ::close(listener);
    return;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
    ::close(listener);
    return;
  }

  // The echo runs on its own thread: read exactly `size` bytes, write them
  // back.
  std::atomic<bool> stop{false};
  std::thread echo([listener, size, &stop] {
    const int peer = ::accept(listener, nullptr, nullptr);
    if (peer < 0) {
      return;
    }
    int nodelay = 1;
    (void)::setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                       sizeof(nodelay));
    std::string buffer(static_cast<size_t>(size), '\0');
    while (!stop.load(std::memory_order_acquire)) {
      size_t filled = 0;
      while (filled < buffer.size()) {
        const ssize_t got =
            ::read(peer, buffer.data() + filled, buffer.size() - filled);
        if (got <= 0) {
          ::close(peer);
          return;
        }
        filled += static_cast<size_t>(got);
      }
      size_t sent = 0;
      while (sent < buffer.size()) {
        const ssize_t put =
            ::write(peer, buffer.data() + sent, buffer.size() - sent);
        if (put <= 0) {
          ::close(peer);
          return;
        }
        sent += static_cast<size_t>(put);
      }
    }
    ::close(peer);
  });

  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  if (client < 0 || ::connect(client, reinterpret_cast<sockaddr*>(&address),
                              sizeof(address)) != 0) {
    stop.store(true, std::memory_order_release);
    ::close(listener);
    echo.join();
    return;
  }
  int nodelay = 1;
  (void)::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                     sizeof(nodelay));

  const std::string payload(static_cast<size_t>(size), 'x');
  std::string inbound(static_cast<size_t>(size), '\0');
  bool broken = false;
  const std::int64_t iterations = Scaled(20000, scale, 200);
  auto metrics = Latency(
      [&](std::int64_t) {
        if (broken) {
          return;
        }
        size_t sent = 0;
        while (sent < payload.size()) {
          const ssize_t put =
              ::write(client, payload.data() + sent, payload.size() - sent);
          if (put <= 0) {
            broken = true;
            return;
          }
          sent += static_cast<size_t>(put);
        }
        size_t filled = 0;
        while (filled < inbound.size()) {
          const ssize_t got =
              ::read(client, inbound.data() + filled, inbound.size() - filled);
          if (got <= 0) {
            broken = true;
            return;
          }
          filled += static_cast<size_t>(got);
        }
      },
      iterations, iterations / 10);
  if (!broken) {
    metrics["mib_per_s"] =
        metrics["ops_per_s"] * static_cast<double>(size) / 1048576.0;
    recorder.Add({.suite = "wire",
                  .name = "message_round_trip",
                  .metrics = metrics,
                  .params = {{"transport", "raw-tcp"}, {"size", Human(size)}}});
  }
  stop.store(true, std::memory_order_release);
  ::shutdown(client, SHUT_RDWR);
  ::close(client);
  ::close(listener);
  echo.join();
}

// The pipelined floor: a bare socket carrying the same windowed stream.
void MeasureLoopbackThroughput(Recorder& recorder, double scale,
                               std::int64_t size, std::int64_t window) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    return;
  }
  int one = 1;
  (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0 ||
      ::listen(listener, 1) != 0) {
    ::close(listener);
    return;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
    ::close(listener);
    return;
  }

  // The peer reads whole messages and acknowledges each with one byte.
  std::atomic<bool> stop{false};
  std::thread echo([listener, size, &stop] {
    const int peer = ::accept(listener, nullptr, nullptr);
    if (peer < 0) {
      return;
    }
    int nodelay = 1;
    (void)::setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                       sizeof(nodelay));
    std::string buffer(static_cast<size_t>(size), '\0');
    while (!stop.load(std::memory_order_acquire)) {
      size_t filled = 0;
      while (filled < buffer.size()) {
        const ssize_t got =
            ::read(peer, buffer.data() + filled, buffer.size() - filled);
        if (got <= 0) {
          ::close(peer);
          return;
        }
        filled += static_cast<size_t>(got);
      }
      const char ack = 'y';
      if (::write(peer, &ack, 1) != 1) {
        ::close(peer);
        return;
      }
    }
    ::close(peer);
  });

  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  if (client < 0 || ::connect(client, reinterpret_cast<sockaddr*>(&address),
                              sizeof(address)) != 0) {
    stop.store(true, std::memory_order_release);
    ::close(listener);
    echo.join();
    return;
  }
  int nodelay = 1;
  (void)::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                     sizeof(nodelay));

  const std::string payload(static_cast<size_t>(size), 'x');
  const std::int64_t messages = Scaled(4000, scale, 200);
  bool broken = false;
  std::int64_t acknowledged = 0;
  // Reads acknowledgements until at least `target` have arrived.
  const auto collect = [&](std::int64_t target) {
    std::array<char, 256> acks{};
    while (acknowledged < target) {
      const ssize_t got = ::read(client, acks.data(), acks.size());
      if (got <= 0) {
        broken = true;
        return;
      }
      acknowledged += got;
    }
  };
  auto metrics = Throughput(
      [&](std::int64_t) {
        for (std::int64_t sent = 0; sent < messages && !broken; ++sent) {
          if (sent - acknowledged >= window) {
            collect(sent - window + 1);
            if (broken) {
              return;
            }
          }
          size_t offset = 0;
          while (offset < payload.size()) {
            const ssize_t put = ::write(client, payload.data() + offset,
                                        payload.size() - offset);
            if (put <= 0) {
              broken = true;
              return;
            }
            offset += static_cast<size_t>(put);
          }
        }
        collect(messages);
      },
      1, 0, messages, messages * size);
  if (!broken) {
    metrics["ops_per_s"] = metrics["items_per_s"];
    recorder.Add({.suite = "wire",
                  .name = "stream_throughput",
                  .metrics = metrics,
                  .params = {{"transport", "raw-tcp"},
                             {"size", Human(size)},
                             {"window", absl::StrCat(window)}}});
  }
  stop.store(true, std::memory_order_release);
  ::shutdown(client, SHUT_RDWR);
  ::close(client);
  ::close(listener);
  echo.join();
}

// The reference row the WebRTC diagnosis has been missing: a bare
// libdatachannel data channel with no A11 in it at all.
void MeasureRawWebRtc(Recorder& recorder, double scale, std::int64_t size,
                      bool pipelined, std::int64_t window) {
  rtc::Configuration configuration;
  // Loopback only: a STUN round trip would price the internet, not the stack.
  configuration.maxMessageSize = 4 * 1024 * 1024;
  // The three knobs that decide how a message becomes packets, swept here
  // rather than on the A11 row because this row has no A11 in it: whatever they
  // are worth here is what they are worth at all.
  const auto env_size = [](const char* name) -> std::optional<size_t> {
    const std::optional<int> parsed = EnvironmentInt(name);
    return parsed.value_or(0) > 0
               ? std::optional<size_t>(static_cast<size_t>(*parsed))
               : std::nullopt;
  };
  if (const std::optional<size_t> mtu = env_size("A11_BENCH_RTC_MTU")) {
    configuration.mtu = *mtu;
  }
  const std::optional<size_t> burst = env_size("A11_BENCH_RTC_MAXBURST");
  const std::optional<size_t> cwnd = env_size("A11_BENCH_RTC_CWND");
  if (burst.has_value() || cwnd.has_value()) {
    try {
      rtc::SctpSettings settings;
      settings.recvBufferSize = 32 * 1024 * 1024;
      settings.sendBufferSize = 32 * 1024 * 1024;
      settings.maxBurst = burst;
      settings.initialCongestionWindow = cwnd;
      rtc::SetSctpSettings(settings);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "  raw-webrtc: sctp settings: %s\n", error.what());
    }
  }

  struct Sync {
    thread::Mutex mu;
    thread::CondVar cv;
    std::int64_t arrivals = 0;
    bool open = false;
    bool peer_open = false;
    bool failed = false;
  };

  const auto sync = std::make_shared<Sync>();
  const auto peer_channel =
      std::make_shared<std::shared_ptr<rtc::DataChannel>>();

  std::shared_ptr<rtc::PeerConnection> client;
  std::shared_ptr<rtc::PeerConnection> server;
  std::shared_ptr<rtc::DataChannel> channel;
  try {
    client = std::make_shared<rtc::PeerConnection>(configuration);
    server = std::make_shared<rtc::PeerConnection>(configuration);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "  skip raw-webrtc: %s\n", error.what());
    return;
  }

  // Signalling is a direct call in both directions to isolate the data path.
  // The `webrtc` rows also use in-process signalling.
  std::weak_ptr<rtc::PeerConnection> weak_server = server;
  std::weak_ptr<rtc::PeerConnection> weak_client = client;
  client->onLocalDescription([weak_server](rtc::Description description) {
    if (auto peer = weak_server.lock(); peer != nullptr) {
      peer->setRemoteDescription(std::move(description));
    }
  });
  client->onLocalCandidate([weak_server](rtc::Candidate candidate) {
    if (auto peer = weak_server.lock(); peer != nullptr) {
      peer->addRemoteCandidate(std::move(candidate));
    }
  });
  server->onLocalDescription([weak_client](rtc::Description description) {
    if (auto peer = weak_client.lock(); peer != nullptr) {
      peer->setRemoteDescription(std::move(description));
    }
  });
  server->onLocalCandidate([weak_client](rtc::Candidate candidate) {
    if (auto peer = weak_client.lock(); peer != nullptr) {
      peer->addRemoteCandidate(std::move(candidate));
    }
  });

  // The echo half, on whatever thread libdatachannel delivers on -- the same
  // place A11's own callback runs, so the comparison keeps that term.
  server->onDataChannel([sync, peer_channel, pipelined](
                            const std::shared_ptr<rtc::DataChannel>& accepted) {
    *peer_channel = accepted;
    std::weak_ptr<rtc::DataChannel> weak = accepted;
    accepted->onMessage([weak, pipelined](rtc::message_variant message) {
      std::shared_ptr<rtc::DataChannel> reply = weak.lock();
      if (reply == nullptr) {
        return;
      }
      try {
        if (pipelined) {
          // One byte of credit per arriving message, exactly as the A11
          // throughput row's peer sends, so the reverse direction does not
          // distort the rate.
          (void)reply->send(rtc::binary{std::byte{'y'}});
        } else if (const rtc::binary* binary =
                       std::get_if<rtc::binary>(&message);
                   binary != nullptr) {
          (void)reply->send(*binary);
        }
      } catch (...) {}
    });
    thread::MutexLock lock(&sync->mu);
    sync->peer_open = true;
    sync->cv.SignalAll();
  });

  try {
    channel = client->createDataChannel("raw-bench");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "  skip raw-webrtc: %s\n", error.what());
    return;
  }
  channel->onOpen([sync] {
    thread::MutexLock lock(&sync->mu);
    sync->open = true;
    sync->cv.SignalAll();
  });
  channel->onClosed([sync] {
    thread::MutexLock lock(&sync->mu);
    sync->failed = true;
    sync->cv.SignalAll();
  });
  channel->onMessage([sync](const rtc::message_variant&) {
    thread::MutexLock lock(&sync->mu);
    ++sync->arrivals;
    sync->cv.SignalAll();
  });

  const auto close = [&] {
    try {
      channel->resetCallbacks();
      if (*peer_channel != nullptr) {
        (*peer_channel)->resetCallbacks();
      }
      client->resetCallbacks();
      server->resetCallbacks();
      client->close();
      server->close();
    } catch (...) {}
  };

  {
    thread::MutexLock lock(&sync->mu);
    const absl::Time limit = absl::Now() + absl::Seconds(20);
    while (!sync->open || !sync->peer_open) {
      if (sync->failed || sync->cv.WaitWithDeadline(&sync->mu, limit)) {
        std::fprintf(stderr, "  skip raw-webrtc: channel did not open\n");
        close();
        return;
      }
    }
  }

  const rtc::binary payload(static_cast<size_t>(size), std::byte{'x'});
  bool stalled = false;
  const auto await_arrivals = [&](std::int64_t target) {
    thread::MutexLock lock(&sync->mu);
    const absl::Time limit = absl::Now() + absl::Seconds(5);
    while (sync->arrivals < target) {
      if (sync->cv.WaitWithDeadline(&sync->mu, limit)) {
        return false;
      }
    }
    return true;
  };
  const auto arrival_count = [&] {
    thread::MutexLock lock(&sync->mu);
    return sync->arrivals;
  };

  if (!pipelined) {
    const std::int64_t iterations = Scaled(20000, scale, 200);
    auto metrics = Latency(
        [&](std::int64_t) {
          if (stalled) {
            return;
          }
          const std::int64_t before = arrival_count();
          try {
            (void)channel->send(payload);
          } catch (...) {
            stalled = true;
            return;
          }
          if (!await_arrivals(before + 1)) {
            stalled = true;
          }
        },
        iterations, iterations / 10);
    if (!stalled) {
      metrics["mib_per_s"] =
          metrics["ops_per_s"] * static_cast<double>(size) / 1048576.0;
      recorder.Add(
          {.suite = "wire",
           .name = "message_round_trip",
           .metrics = metrics,
           .params = {{"transport", "raw-webrtc"}, {"size", Human(size)}}});
    } else {
      std::fprintf(stderr, "  skip raw-webrtc round trip at %s: no echo\n",
                   Human(size).c_str());
    }
    close();
    return;
  }

  const std::int64_t messages = Scaled(4000, scale, 200);
  const std::int64_t warmup = std::min<std::int64_t>(messages / 10, 200);
  const auto pump = [&](std::int64_t count) {
    const std::int64_t base = arrival_count();
    for (std::int64_t sent = 0; sent < count; ++sent) {
      if (sent - (arrival_count() - base) >= window &&
          !await_arrivals(base + sent - window + 1)) {
        stalled = true;
        return;
      }
      try {
        (void)channel->send(payload);
      } catch (...) {
        stalled = true;
        return;
      }
    }
    if (!await_arrivals(base + count)) {
      stalled = true;
    }
  };
  if (warmup > 0) {
    pump(warmup);
  }
  auto metrics = Throughput([&](std::int64_t) { pump(messages); }, 1, 0,
                            messages, messages * size);
  if (!stalled) {
    metrics["ops_per_s"] = metrics["items_per_s"];
    recorder.Add({.suite = "wire",
                  .name = "stream_throughput",
                  .metrics = metrics,
                  .params = {{"transport", "raw-webrtc"},
                             {"size", Human(size)},
                             {"window", absl::StrCat(window)}}});
  } else {
    std::fprintf(stderr, "  skip raw-webrtc throughput at %s: no credit\n",
                 Human(size).c_str());
  }
  close();
}

void WireSuite(Recorder& recorder, double scale) {
  using Factory = std::optional<BenchPair> (*)(std::weak_ptr<net::WireStream>*,
                                               net::OnMessage, net::OnDone);
  const std::vector<std::pair<std::string, Factory>> transports = {
      {"in-process", InProcessPair}, {"websocket", WebSocketPair},
      {"sse", HttpSsePair},          {"sse-stream", HttpSseStreamPair},
      {"webrtc", WebRtcPair},        {"webrtc-live-mtu", WebRtcLiveMtuPair}};
  // A11_BENCH_TRANSPORTS restricts the row set to a comma-separated subset.
  // Iterating on one transport otherwise pays for all five, and the ones that
  // are not being changed are the slowest.
  const char* only = std::getenv("A11_BENCH_TRANSPORTS");
  const std::vector<std::string> selected =
      only == nullptr ? std::vector<std::string>{}
                      : absl::StrSplit(only, ',', absl::SkipEmpty());
  for (const auto& [name, factory] : transports) {
    if (!selected.empty() &&
        std::find(selected.begin(), selected.end(), name) == selected.end()) {
      continue;
    }
    for (const std::int64_t size : {64, 4096, 65536}) {
      MeasureRoundTrip(recorder, scale, name, size, factory);
    }
    // 32 in flight: enough to overlap encode, write and loop crossing without
    // reaching the 64-message inbound reassembly bound that aborts a stream.
    for (const std::int64_t size : {64, 65536}) {
      MeasureStreamThroughput(recorder, scale, name, size, 32, factory);
    }
  }
  for (const std::int64_t size : {64, 4096, 65536}) {
    MeasureLoopbackFloor(recorder, scale, size);
  }
  for (const std::int64_t size : {64, 65536}) {
    MeasureLoopbackThroughput(recorder, scale, size, 32);
  }
  if (selected.empty() || std::find(selected.begin(), selected.end(),
                                    "raw-webrtc") != selected.end()) {
    for (const std::int64_t size : {64, 4096, 65536}) {
      MeasureRawWebRtc(recorder, scale, size, /*pipelined=*/false, 0);
    }
    for (const std::int64_t size : {64, 65536}) {
      MeasureRawWebRtc(recorder, scale, size, /*pipelined=*/true, 32);
    }
  }
  MeasureUvCrossing(recorder, scale);
}

// ---------------------------------------------------------------------------
// server: a native service under a client population
// ---------------------------------------------------------------------------

// What a C++ server costs per request, and whether it uses more than one core.
void ServerSuite(Recorder& recorder, double scale) {
  for (const int clients : {1, 4, 16, 64, 256, 1024}) {
    const std::int64_t per_client =
        Scaled(std::max<std::int64_t>(8, 512 / clients), scale, 4);

    auto registry = std::make_shared<actions::ActionRegistry>();
    if (!registry->Register(EchoSchema().name, EchoSchema(), EchoHandler())
             .ok()) {
      continue;
    }
    absl::StatusOr<std::shared_ptr<service::Service>> service =
        service::Service::Create(registry);
    if (!service.ok()) {
      std::fprintf(stderr, "  skip server: %s\n",
                   service.status().ToString().c_str());
      continue;
    }

    // Every accepted connection becomes a session on the one service, which is
    // the shape a real server has: one process, one registry, many peers.
    const std::shared_ptr<service::Service>& serving = *service;
    net::WebSocketServerOptions options;
    options.path = "/bench";
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.http2_options.enable_h2 = false;
    options.http2_options.enable_h2c = false;
    absl::StatusOr<std::shared_ptr<net::WebSocketWireServer>> server =
        net::WebSocketWireServer::Create(
            [serving](
                std::shared_ptr<net::WebSocketWireStream> stream) -> a11::Task {
              // Reported, not discarded.
              absl::StatusOr<std::shared_ptr<service::Session>> accepted =
                  serving->StartStreamHandler(std::move(stream));
              if (!accepted.ok()) {
                std::fprintf(stderr, "  server: accept failed: %s\n",
                             accepted.status().ToString().c_str());
              }
              return a11::ReadyTask();
            },
            options);
    if (!server.ok()) {
      std::fprintf(stderr, "  skip server: %s\n",
                   server.status().ToString().c_str());
      continue;
    }
    const absl::StatusOr<std::uint16_t> port = (*server)->port();
    if (!port.ok()) {
      continue;
    }

    std::fprintf(stderr, "  server[%d clients]: listening on %u\n", clients,
                 static_cast<unsigned>(*port));
    std::vector<std::shared_ptr<service::Session>> sessions;
    std::vector<std::shared_ptr<net::WireStream>> streams;
    bool ready = true;
    for (int index = 0; index < clients && ready; ++index) {
      net::WebSocketClientOptions client_options;
      client_options.http2_options.enable_h2 = false;
      client_options.http2_options.enable_h2c = false;
      absl::StatusOr<std::shared_ptr<net::WebSocketWireStream>> stream =
          net::WebSocketWireStream::CreateClient(
              absl::StrCat("ws://127.0.0.1:", *port, "/bench"),
              net::WireStreamOptions{}, client_options);
      if (!stream.ok()) {
        std::fprintf(stderr, "  server: connect %d failed: %s\n", index,
                     stream.status().ToString().c_str());
        ready = false;
        break;
      }
      absl::StatusOr<std::shared_ptr<service::Session>> session =
          service::Session::Create();
      if (!session.ok()) {
        ready = false;
        break;
      }
      // AddStream returns a StatusOr<Task>: the accept itself can fail before
      // there is anything to await.
      absl::StatusOr<a11::Task> added =
          (*session)->AddStream(*stream, service::StreamMode::kStart);
      if (!added.ok()) {
        std::fprintf(stderr, "  server: add_stream %d rejected: %s\n", index,
                     added.status().ToString().c_str());
        ready = false;
        break;
      }
      if (const absl::Status started = added->Await(Deadline()).status();
          !started.ok()) {
        std::fprintf(stderr, "  server: add_stream %d did not start: %s\n",
                     index, started.ToString().c_str());
        ready = false;
        break;
      }
      std::fprintf(stderr, "  server: client %d connected\n", index);
      sessions.push_back(*session);
      streams.push_back(*stream);
    }
    if (!ready || sessions.empty()) {
      std::fprintf(stderr, "  skip server[%d clients]: could not connect\n",
                   clients);
      (void)(*server)->Stop();
      continue;
    }

    // One fibre per client, each calling echo in a loop: concurrency is what
    // makes the cores-busy column mean anything.
    const double cpu_before = ProcessCpuSeconds();
    const absl::Time started = absl::Now();
    g_echo_handler_replies.store(0, std::memory_order_relaxed);
    std::atomic<std::int64_t> completed{0};
    // When the last successful round-trip landed.
    std::atomic<std::int64_t> last_completion_unix_nanos{0};
    std::vector<a11::Task> drivers;
    drivers.reserve(sessions.size());
    for (size_t index = 0; index < sessions.size(); ++index) {
      drivers.push_back(a11::SubmitTask(
          [&sessions, &streams, &completed, &last_completion_unix_nanos, index,
           per_client]() -> absl::Status {
            for (std::int64_t round = 0; round < per_client; ++round) {
              absl::StatusOr<std::shared_ptr<actions::Action>> call =
                  actions::Action::Create(EchoSchema(), /*action_id=*/"");
              if (!call.ok()) {
                return call.status();
              }
              // The id must be empty so each call gets a generated one.
              if (!(*call)->BindNodeMap(sessions[index]->GetNodeMap()).ok() ||
                  !(*call)->BindSession(sessions[index]).ok() ||
                  !(*call)->BindStream(streams[index]).ok()) {
                return absl::InternalError("could not bind the call");
              }
              // Each stage gets a deadline shorter than the driver's own, so
              // that a wedge is reported as the stage that wedged.
              const absl::Time stage_deadline = absl::Now() + kStageTimeout;
              if (const absl::Status dispatched =
                      (*call)->Call().Await(stage_deadline).status();
                  !dispatched.ok()) {
                return {dispatched.code(),
                        absl::StrCat("stage=call ", dispatched.message())};
              }
              absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> input =
                  (*call)->GetInput("input");
              if (!input.ok()) {
                return input.status();
              }
              // The mimetype is not decoration here.
              if (!(*input)
                       ->PutChunk(
                           data::Chunk{.metadata =
                                           data::ChunkMetadata{
                                               .mimetype = "application/"
                                                           "octet-stream"},
                                       .data = "ping"},
                           std::nullopt, true)
                       .Await(absl::Now() + kStageTimeout)
                       .ok()) {
                return absl::InternalError("stage=put-input timed out");
              }
              // A11_BENCH_READ_DELAY_MS delays the read of the output so the
              // reply is already delivered before anyone asks for it.
              if (const std::optional<int> delay =
                      EnvironmentInt("A11_BENCH_READ_DELAY_MS");
                  delay.has_value()) {
                thread::SleepFor(absl::Milliseconds(*delay));
              }
              absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> output =
                  (*call)->GetOutput("output");
              if (!output.ok()) {
                return output.status();
              }
              // One read, no retries.
              const absl::StatusOr<std::optional<data::Chunk>> replied =
                  (*output)->NextChunk().Await(absl::Now() + kStageTimeout);
              if (!replied.ok() || !replied->has_value()) {
                return {
                    replied.ok() ? absl::StatusCode::kDataLoss
                                 : replied.status().code(),
                    absl::StrCat("stage=read-output ",
                                 replied.ok() ? "the reply ended before a chunk"
                                              : replied.status().message())};
              }
              if (const absl::Status finished =
                      (*call)
                          ->Wait()
                          .Await(absl::Now() + kStageTimeout)
                          .status();
                  !finished.ok()) {
                return {finished.code(),
                        absl::StrCat("stage=wait ", finished.message())};
              }
              completed.fetch_add(1, std::memory_order_relaxed);
              const std::int64_t now_nanos = absl::ToUnixNanos(absl::Now());
              std::int64_t seen =
                  last_completion_unix_nanos.load(std::memory_order_relaxed);
              while (now_nanos > seen &&
                     !last_completion_unix_nanos.compare_exchange_weak(
                         seen, now_nanos, std::memory_order_relaxed)) {}
            }
            return absl::OkStatus();
          },
          thread::TreeOptions{.stack_size = 512 * 1024}));
    }
    // Reported, not discarded -- for the third time in this suite's short life.
    std::vector<std::string> driver_errors;
    for (const a11::Task& driver : drivers) {
      if (const absl::Status result = driver.Await(Deadline()).status();
          !result.ok()) {
        std::string text = result.ToString();
        if (std::find(driver_errors.begin(), driver_errors.end(), text) ==
            driver_errors.end()) {
          driver_errors.push_back(std::move(text));
        }
      }
    }
    for (const std::string& text : driver_errors) {
      std::fprintf(
          stderr,
          "  server[%d clients]: driver failed: %s"
          " (client round-trips=%lld, server replies written=%lld)\n",
          clients, text.c_str(),
          static_cast<long long>(completed.load(std::memory_order_relaxed)),
          static_cast<long long>(
              g_echo_handler_replies.load(std::memory_order_relaxed)));
    }
    // Printed so a fibre or scheduling census can be divided by it.
    std::fprintf(
        stderr, "  server[%d clients]: completed=%lld\n", clients,
        static_cast<long long>(completed.load(std::memory_order_relaxed)));
    const std::int64_t last_nanos =
        last_completion_unix_nanos.load(std::memory_order_relaxed);
    const absl::Duration elapsed =
        last_nanos > 0 ? absl::FromUnixNanos(last_nanos) - started
                       : absl::Now() - started;
    const double cpu = ProcessCpuSeconds() - cpu_before;
    const std::int64_t done = completed.load(std::memory_order_relaxed);

    for (const auto& stream : streams) {
      stream->Abort(absl::CancelledError("benchmark over")).IgnoreError();
    }
    (void)serving->Abort(absl::CancelledError("benchmark over"));
    (void)(*server)->Stop();

    const double seconds = absl::ToDoubleSeconds(elapsed);
    if (done == 0 || seconds <= 0.0) {
      continue;
    }
    recorder.Add(
        {.suite = "server",
         .name = "native_echo",
         .metrics =
             {
                 {"ops_per_s", static_cast<double>(done) / seconds},
                 {"cpu_us_per_op", cpu * 1e6 / static_cast<double>(done)},
                 {"cores_busy", cpu / seconds},
             },
         .params = {{"clients", absl::StrCat(clients)}},
         .note = "one native service, one connection per client; cpu covers "
                 "both ends because they share this process"});
  }
}

// ---------------------------------------------------------------------------
// flow: the language's runtime, with no host bridge in the way
// ---------------------------------------------------------------------------

/**
 * The same flows `bench/suites/flow.py` runs, with the Python taken out.
 *
 * The Python suite can say what a flow costs and not what the *runtime* costs:
 * every value crossing a stage is read and written through the [HostBridge],
 * which on that path is an interpreter call under the GIL. This suite runs the
 * identical sources against the native bridge, so the difference between the
 * two tables is the bridge and everything left is the runtime.
 *
 * Names match the Python suite's on purpose (`flow_run`, `pipe_values`), and
 * the rows the Python suite does not have -- per-stage cost, a chain of pipes,
 * a loop with a call per pass -- are the ones that answer whether a pipeline
 * paces itself or runs item by item.
 */
/// `echo`, as the Python flow suite declares it: one unary input, one output.
actions::ActionSchema FlowEchoSchema() {
  return actions::ActionSchema{
      .name = "echo",
      .inputs = {{"text",
                  actions::ActionPortSchema{
                      .name = "text", .type = "text/plain", .unary = true}}},
      .outputs = {{"out", actions::ActionPortSchema{.name = "out",
                                                    .type = "text/plain"}}},
  };
}

actions::ActionHandler FlowEchoHandler() {
  return actions::MakeAsyncActionHandler(
      [](const std::shared_ptr<actions::Action>& action) -> absl::Status {
        ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> input,
                              action->GetInput("text"));
        ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> output,
                              action->GetOutput("out"));
        ABSL_ASSIGN_OR_RETURN(const std::optional<data::Chunk> chunk,
                              input->NextChunk().Await());
        data::Chunk reply = chunk.has_value()
                                ? *chunk
                                : data::Chunk{.metadata = data::ChunkMetadata{
                                                  .mimetype = "text/plain"}};
        return output->Finalize(std::move(reply), {.wait = true})
            .Await()
            .status();
      });
}

std::shared_ptr<actions::ActionRegistry> FlowRegistry() {
  auto registry = std::make_shared<actions::ActionRegistry>();
  (void)registry->Register("echo", FlowEchoSchema(), FlowEchoHandler());
  return registry;
}

data::Chunk TextChunk(std::string text) {
  return data::Chunk{.metadata = data::ChunkMetadata{.mimetype = "text/plain"},
                     .data = std::move(text)};
}

/// One run of one flow: feed every input, drain every output, wait.
///
/// Runs the complete invocation used by `flow_lang.invoke`, making the result
/// comparable with the Python benchmark.
struct FlowRun {
  absl::Status status;
  std::int64_t values_out = 0;
};

FlowRun RunOneFlow(
    const std::shared_ptr<flow::CompiledProgram>& program,
    const std::string& name,
    const std::shared_ptr<actions::ActionRegistry>& registry,
    const std::map<std::string, std::vector<std::string>>& inputs,
    std::int64_t index, bool prefilled = false) {
  FlowRun ran;
  const flow::ResolvedFlow* found = program->Flow(name);
  if (found == nullptr) {
    ran.status = absl::NotFoundError(name);
    return ran;
  }
  absl::StatusOr<actions::ActionSchema> schema = flow::FlowSchema(found->plan);
  if (!schema.ok()) {
    ran.status = schema.status();
    return ran;
  }
  absl::StatusOr<actions::ActionHandler> handler =
      flow::MakeHandler(program, name);
  if (!handler.ok()) {
    ran.status = handler.status();
    return ran;
  }
  absl::StatusOr<std::shared_ptr<nodes::NodeMap>> map =
      nodes::NodeMap::Create();
  if (!map.ok()) {
    ran.status = map.status();
    return ran;
  }
  absl::StatusOr<std::shared_ptr<actions::Action>> action =
      actions::Action::Create(*schema, absl::StrCat(name, "-", index), *handler,
                              *map, nullptr, nullptr, registry);
  if (!action.ok()) {
    ran.status = action.status();
    return ran;
  }
  std::map<std::string, std::shared_ptr<nodes::AsyncNode>> outputs;
  for (const auto& [port, unused] : schema->outputs) {
    absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node =
        (*action)->GetOutput(port, false);
    if (!node.ok()) {
      ran.status = node.status();
      return ran;
    }
    outputs[port] = *node;
  }
  // Either the flow starts and is then fed value by value -- a live producer,
  // which is what every other row measures -- or it is handed a stream that is
  // already there: a node an earlier step filled, a stored conversation.
  const auto feed = [&]() -> absl::Status {
    for (const auto& [port, unused] : schema->inputs) {
      ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> node,
                            (*action)->GetInput(port, false));
      const auto given = inputs.find(port);
      if (given != inputs.end()) {
        for (const std::string& value : given->second) {
          ABSL_RETURN_IF_ERROR(
              node->PutChunk(TextChunk(value)).Await().status());
        }
      }
      ABSL_RETURN_IF_ERROR(node->Finalize({.wait = true}).Await().status());
    }
    return absl::OkStatus();
  };
  if (prefilled) {
    ran.status = feed();
    if (!ran.status.ok()) {
      return ran;
    }
  }
  ran.status = (*action)->Run().status();
  if (!ran.status.ok()) {
    return ran;
  }
  if (!prefilled) {
    ran.status = feed();
    if (!ran.status.ok()) {
      return ran;
    }
  }
  // Drained as the caller would: an output nobody reads stalls its writer.
  for (auto& [port, node] : outputs) {
    while (true) {
      absl::StatusOr<std::optional<data::Chunk>> chunk =
          node->NextChunk().Await(Deadline());
      if (!chunk.ok()) {
        ran.status = chunk.status();
        break;
      }
      if (!chunk->has_value()) {
        break;
      }
      if (!(*chunk)->IsNull()) {
        ++ran.values_out;
      }
    }
  }
  if (ran.status.ok()) {
    ran.status = (*action)->Wait(absl::Seconds(30)).Await(Deadline()).status();
  }
  return ran;
}

/// A flow that threads one value through `steps` sequential `run`s.
std::string ChainSource(int steps) {
  std::string source =
      absl::StrCat("flow chain", steps, " {\n", "  in text: string required\n",
                   "  out result: string\n");
  std::string previous = "text";
  for (int index = 0; index < steps; ++index) {
    absl::StrAppend(&source, "  s", index, " = run echo(text: ", previous,
                    ")\n");
    previous = absl::StrCat("s", index, ".out");
  }
  absl::StrAppend(&source, "  ", previous, " -> result\n}\n");
  return source;
}

/// `stages` chained per-value stages in one pipeline.
///
/// The question is whether stage *n* costs what stage 1 costs. Each stage is
/// its own ref in the plan, which the runtime gives its own producer and its
/// own bounded queue, so a pipeline should pace itself: the second value can be
/// in stage 1 while the first is in stage 2. If the per-stage cost is flat, it
/// does; if the whole pipeline costs one stage times the number of stages, it
/// does not.
std::string StageSource(int stages) {
  std::string source = absl::StrCat("flow staged", stages, " {\n",
                                    "  in items: string stream required\n",
                                    "  out result: string stream\n  items");
  for (int index = 0; index < stages; ++index) {
    absl::StrAppend(&source, " | map it");
  }
  absl::StrAppend(&source, " -> result\n}\n");
  return source;
}

void FlowSuite(Recorder& recorder, double scale) {
  const std::shared_ptr<actions::ActionRegistry> registry = FlowRegistry();

  // The baseline the flow rows are charged against: the same action, called
  // directly, with the same feed-and-drain the flow pays for.
  const std::int64_t calls = Scaled(400, scale, 20);
  const actions::ActionSchema echo = FlowEchoSchema();
  const actions::ActionHandler echo_handler = FlowEchoHandler();
  if (Wanted("action_direct")) {
    recorder.Add(
        {.suite = "flow",
         .name = "action_direct",
         .metrics = Latency(
             [&](std::int64_t index) {
               absl::StatusOr<std::shared_ptr<actions::Action>> action =
                   actions::Action::Create(echo, absl::StrCat("echo-", index),
                                           echo_handler);
               if (!action.ok()) {
                 return;
               }
               absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> out =
                   (*action)->GetOutput("out", false);
               if (!out.ok() || !(*action)->Run().ok()) {
                 return;
               }
               absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> in =
                   (*action)->GetInput("text", false);
               if (!in.ok()) {
                 return;
               }
               (void)(*in)
                   ->Finalize(TextChunk("payload"), {.wait = true})
                   .Await();
               (void)(*out)->NextChunk().Await(Deadline());
               (void)(*out)->NextChunk().Await(Deadline());
               (void)(*action)->Wait(absl::Seconds(30)).Await(Deadline());
             },
             calls, calls / 10),
         .params = {{"steps", "0"}},
         .note =
             "no flow involved -- the baseline the flow is charged against"});
  }

  for (const int steps : {1, 2, 8}) {
    if (!Wanted("flow_run")) {
      break;
    }
    absl::StatusOr<std::shared_ptr<flow::CompiledProgram>> program =
        flow::CompiledProgram::Compile(ChainSource(steps), "bench.flow");
    if (!program.ok()) {
      continue;
    }
    const std::string name = absl::StrCat("chain", steps);
    std::map<std::string, double> metrics = Latency(
        [&](std::int64_t index) {
          const FlowRun ran = RunOneFlow(*program, name, registry,
                                         {{"text", {"payload"}}}, index);
          (void)ran;
        },
        calls, calls / 10);
    metrics["steps_per_s"] = metrics["ops_per_s"] * steps;
    recorder.Add({.suite = "flow",
                  .name = "flow_run",
                  .metrics = std::move(metrics),
                  .params = {{"steps", absl::StrCat(steps)}}});
  }

  // Values through `|`. The same counts the Python suite uses, so the two rows
  // divide into the bridge's share of a value and the runtime's.
  absl::StatusOr<std::shared_ptr<flow::CompiledProgram>> piped =
      flow::CompiledProgram::Compile(StageSource(1), "bench.flow");
  if (piped.ok() && Wanted("pipe_values")) {
    for (const int count : {16, 256, 4096}) {
      std::vector<std::string> values;
      values.reserve(static_cast<size_t>(count));
      for (int index = 0; index < count; ++index) {
        values.push_back(absl::StrCat("value-", index));
      }
      const std::int64_t iterations = Scaled(count <= 256 ? 120 : 20, scale, 4);
      std::int64_t out = 0;
      std::map<std::string, double> metrics = Throughput(
          [&](std::int64_t index) {
            const FlowRun ran = RunOneFlow(*piped, "staged1", registry,
                                           {{"items", values}}, index);
            out = ran.values_out;
          },
          iterations, iterations / 10, count);
      metrics["items_per_s"] = metrics["ops_per_s"] * count;
      metrics["us_per_value"] = metrics["ns_per_op"] / 1000.0 / count;
      recorder.Add({.suite = "flow",
                    .name = "pipe_values",
                    .metrics = std::move(metrics),
                    .params = {{"values", absl::StrCat(count)}},
                    .note = absl::StrCat(out, " values out per run")});
    }
  }

  // The same pipeline over a stream that is already there.
  if (piped.ok() && Wanted("pipe_prefilled")) {
    constexpr int kValues = 256;
    std::vector<std::string> values;
    values.reserve(kValues);
    for (int index = 0; index < kValues; ++index) {
      values.push_back(absl::StrCat("value-", index));
    }
    const std::int64_t iterations = Scaled(120, scale, 4);
    std::map<std::string, double> metrics = Throughput(
        [&](std::int64_t index) {
          const FlowRun ran =
              RunOneFlow(*piped, "staged1", registry, {{"items", values}},
                         index, /*prefilled=*/true);
          (void)ran;
        },
        iterations, iterations / 10, kValues);
    metrics["items_per_s"] = metrics["ops_per_s"] * kValues;
    metrics["us_per_value"] = metrics["ns_per_op"] / 1000.0 / kValues;
    recorder.Add({.suite = "flow",
                  .name = "pipe_prefilled",
                  .metrics = std::move(metrics),
                  .params = {{"values", absl::StrCat(kValues)}},
                  .note = "the input written and closed before the flow starts,"
                          " so a stage sees several values at once"});
  }

  // Per-stage cost, which is what says whether stages pace themselves.
  for (const int stages : {1, 2, 4, 8}) {
    if (!Wanted("pipe_stages")) {
      break;
    }
    absl::StatusOr<std::shared_ptr<flow::CompiledProgram>> program =
        flow::CompiledProgram::Compile(StageSource(stages), "bench.flow");
    if (!program.ok()) {
      continue;
    }
    constexpr int kValues = 256;
    std::vector<std::string> values;
    values.reserve(kValues);
    for (int index = 0; index < kValues; ++index) {
      values.push_back(absl::StrCat("value-", index));
    }
    const std::int64_t iterations = Scaled(60, scale, 4);
    std::map<std::string, double> metrics = Throughput(
        [&](std::int64_t index) {
          const FlowRun ran =
              RunOneFlow(*program, absl::StrCat("staged", stages), registry,
                         {{"items", values}}, index);
          (void)ran;
        },
        iterations, iterations / 10, kValues);
    metrics["us_per_value"] = metrics["ns_per_op"] / 1000.0 / kValues;
    metrics["us_per_value_per_stage"] = metrics["us_per_value"] / stages;
    recorder.Add({.suite = "flow",
                  .name = "pipe_stages",
                  .metrics = std::move(metrics),
                  .params = {{"stages", absl::StrCat(stages)},
                             {"values", absl::StrCat(kValues)}}});
  }

  // A loop with a call per pass: the shape of a real composition, and the one
  // place `parallel` decides whether the passes overlap.
  for (const int parallel : {1, 4, 16}) {
    if (!Wanted("for_each_call")) {
      break;
    }
    const std::string source = absl::StrCat(
        "flow fanned", parallel, " {\n", "  in items: string stream required\n",
        "  out result: string stream\n", "  for item in items parallel ",
        parallel, " {\n", "    r = run echo(text: item)\n",
        "    r.out -> result\n", "  }\n}\n");
    absl::StatusOr<std::shared_ptr<flow::CompiledProgram>> program =
        flow::CompiledProgram::Compile(source, "bench.flow");
    if (!program.ok()) {
      continue;
    }
    constexpr int kValues = 32;
    std::vector<std::string> values;
    values.reserve(kValues);
    for (int index = 0; index < kValues; ++index) {
      values.push_back(absl::StrCat("value-", index));
    }
    const std::int64_t iterations = Scaled(40, scale, 4);
    std::map<std::string, double> metrics = Throughput(
        [&](std::int64_t index) {
          const FlowRun ran =
              RunOneFlow(*program, absl::StrCat("fanned", parallel), registry,
                         {{"items", values}}, index);
          (void)ran;
        },
        iterations, iterations / 10, kValues);
    metrics["us_per_pass"] = metrics["ns_per_op"] / 1000.0 / kValues;
    recorder.Add({.suite = "flow",
                  .name = "for_each_call",
                  .metrics = std::move(metrics),
                  .params = {{"parallel", absl::StrCat(parallel)},
                             {"values", absl::StrCat(kValues)}},
                  .note = "one action call per pass"});
  }

  // Compilation, for the same document the Python suite compiles.
  const std::int64_t compiles = Scaled(2000, scale, 50);
  const std::string chain = ChainSource(2);
  if (Wanted("compile_source")) {
    recorder.Add(
        {.suite = "flow",
         .name = "compile_source",
         .metrics = Throughput(
             [&](std::int64_t) {
               absl::StatusOr<std::shared_ptr<flow::CompiledProgram>> program =
                   flow::CompiledProgram::Compile(chain, "bench");
               (void)program;
             },
             compiles, compiles / 10, 1,
             static_cast<std::int64_t>(chain.size())),
         .params = {{"doc", "chain2"}, {"bytes", absl::StrCat(chain.size())}}});
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

using SuiteFn = std::function<void(Recorder&, double)>;

const std::vector<std::pair<std::string, SuiteFn>>& Suites() {
  static const auto* suites = new std::vector<std::pair<std::string, SuiteFn>>{
      {"data", DataSuite},
      {"stores", StoresSuite},
      {"nodes", NodesSuite},
      {"actions", ActionsSuite},
      {"scheduling", SchedulingSuite},
      {"flow", FlowSuite},
      {"wire", WireSuite},
      {"server", ServerSuite},
  };
  return *suites;
}

}  // namespace
}  // namespace a11::bench

int main(int argc, char** argv) {
  // Ignore SIGPIPE, or the whole run dies inside the `wire` suite on Linux.
  std::signal(SIGPIPE, SIG_IGN);

  double scale = 1.0;
  std::string json_path;
  std::vector<std::string> chosen;

  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    const bool has_value = index + 1 < argc;
    if (flag == "--scale" && has_value) {
      scale = std::stod(argv[++index]);
    } else if (flag == "--json" && has_value) {
      json_path = argv[++index];
    } else if (flag == "--suite" && has_value) {
      chosen.emplace_back(argv[++index]);
    } else if (flag == "--only" && has_value) {
      a11::bench::g_only = argv[++index];
    } else if (flag == "--help" || flag == "-h") {
      std::printf(
          "a11_bench [--suite NAME]... [--only SUBSTRING] [--scale N]"
          " [--json PATH]\n"
          "  Native counterparts to `python -m bench`. Same record shape,\n"
          "  same benchmark names, no Python.\n"
          "  --only runs the rows whose name contains SUBSTRING, which is "
          "what\n"
          "  attributing a process-wide counter to one operation needs.\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
      return 2;
    }
  }

  a11::bench::Recorder recorder;
  std::printf("a11 bench (native): scale=%g\n", scale);
  for (const auto& [name, run] : a11::bench::Suites()) {
    if (!chosen.empty() &&
        std::find(chosen.begin(), chosen.end(), name) == chosen.end()) {
      continue;
    }
    std::printf("=== %s ===\n", name.c_str());
    std::fflush(stdout);
    run(recorder, scale);
    // Report each suite as it finishes rather than everything at the end.
    recorder.PrintTable(name);
    std::fflush(stdout);
    if (!json_path.empty() && !recorder.WriteJson(json_path)) {
      std::fprintf(stderr, "could not write %s\n", json_path.c_str());
      return 1;
    }
  }

  if (!json_path.empty()) {
    std::printf("\nwrote %s\n", json_path.c_str());
  }
  return 0;
}
