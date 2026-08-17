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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <boost/fiber/fiber.hpp>
#include <boost/fiber/operations.hpp>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/internal/http_transport.h"
#include "a11/net/http_sse_wire_stream.h"
#include "a11/net/signalling.h"
#include "a11/net/webrtc_wire_stream.h"
#include "a11/net/websocket_wire_stream.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/stores/local_chunk_store.h"
#include "bench/harness.h"
#include "thread/boost_primitives.h"
#include "thread/executor.h"

namespace a11::bench {
namespace {

absl::Time Deadline() {
  return absl::Now() + absl::Seconds(30);
}

data::NodeFragment Fragment(std::uint32_t seq, const std::string& payload,
                            bool final = false) {
  return data::NodeFragment{
      .data = data::Chunk{.data = payload}, .seq = seq, .continued = !final};
}

std::string Human(std::int64_t size) {
  if (size >= 1048576)
    return absl::StrCat(size / 1048576, "M");
  if (size >= 1024)
    return absl::StrCat(size / 1024, "K");
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
  //
  // Through `nlohmann::json`, because that is what the C++ registry actually
  // holds: it registers eight types (json plus the seven native A11 values),
  // where the Python registry holds fifty-six (dict, list, str, bytes, int,
  // every pydantic model, ...). That difference is itself worth knowing --
  // Python's per-call `isinstance` scan is seven times longer *by
  // construction*, so the dispatch cost the Python suite measured cannot be
  // reproduced here and is not a C++ problem to fix.
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
      const std::int64_t wire_bytes =
          static_cast<std::int64_t>(encoded->data.size());
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

      const data::Chunk source = *encoded;
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
      if (!encoded.ok())
        continue;
      const std::int64_t wire_bytes =
          static_cast<std::int64_t>(encoded->size());
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

      const data::Bytes source = *encoded;
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
          if (!batch.ok())
            return;
          for (const auto& fragment : *batch) {
            if (fragment.has_value())
              ++drained;
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
            if (store.ok())
              held.push_back(*store);
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
                        if (!store.ok())
                          return;
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
          if (fragment.ok() && fragment->has_value())
            ++seen;
        },
        count, 0, 1, static_cast<std::int64_t>(token.data.size()));
    recorder.Add({.suite = "nodes",
                  .name = "read_one_at_a_time",
                  .metrics = metrics,
                  .params = {{"via", "AsyncNode::NextFragment"}},
                  .note = absl::StrCat(seen, " fragments read")});
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
            if (!store.ok())
              continue;
            auto node = nodes::AsyncNode::Create(*store);
            if (node.ok())
              held.push_back(*node);
          }
        },
        std::vector<std::int64_t>(6, stage), &trail);
    recorder.Add({.suite = "nodes",
                  .name = "node_resident",
                  .metrics = {{"bytes_each", slope}},
                  .note = absl::StrCat("idle node, in-memory store. ", trail)});
  }
}

// ---------------------------------------------------------------------------
// actions: the unit A11 is counted in
// ---------------------------------------------------------------------------

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

actions::ActionHandler EchoHandler() {
  return [](std::shared_ptr<actions::Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      auto input = action->GetInput("input");
      if (!input.ok())
        return input.status();
      auto chunk = (*input)->NextChunk().Await();
      if (!chunk.ok())
        return chunk.status();
      if (!chunk->has_value()) {
        return absl::FailedPreconditionError("echo input ended early");
      }
      auto output = action->GetOutput("output");
      if (!output.ok())
        return output.status();
      return (*output)
          ->PutChunk(std::move(**chunk), std::nullopt, true)
          .Await()
          .status();
    });
  };
}

actions::ActionSchema PortlessSchema() {
  return actions::ActionSchema{.name = "portless"};
}

/// A handler that spawns a fibre and finishes, mirroring a Python coroutine
/// handler: the interesting part is the spawn, not the body.
actions::ActionHandler FiberNoopHandler() {
  return [](std::shared_ptr<actions::Action>) {
    return a11::SubmitTask([]() -> absl::Status { return absl::OkStatus(); });
  };
}

/// A handler that is already finished when it is handed back. No fibre, no
/// scheduling -- whatever an action costs with this is the lifecycle alone.
actions::ActionHandler InlineNoopHandler() {
  return [](std::shared_ptr<actions::Action>) {
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
                        if (!created.ok())
                          return;
                        std::shared_ptr<actions::Action> action = *created;
                        if (!action->Run().ok())
                          return;
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
                      if (!created.ok())
                        return;
                      std::shared_ptr<actions::Action> action = *created;
                      auto input = action->GetInput("input", false);
                      if (!input.ok())
                        return;
                      auto put = (*input)
                                     ->PutChunk(data::Chunk{.data = "payload"},
                                                std::nullopt, true)
                                     .Await(Deadline());
                      (void)put;
                      if (!action->Run().ok())
                        return;
                      auto output = action->GetOutput("output", false);
                      if (!output.ok())
                        return;
                      auto chunk = (*output)->NextChunk().Await(Deadline());
                      (void)chunk;
                      auto done = action->Wait().Await(Deadline());
                      (void)done;
                    },
                    iterations, iterations / 10),
                .params = {{"ports", "1in/1out"}}});

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
             if (!store_a.ok() || !store_b.ok())
               return;
             auto first = nodes::AsyncNode::Create(*store_a);
             auto second = nodes::AsyncNode::Create(*store_b);
             if (!first.ok() || !second.ok())
               return;
             auto wrote = (*first)
                              ->PutChunk(data::Chunk{.data = "payload"},
                                         std::nullopt, true)
                              .Await(Deadline());
             (void)wrote;
             auto read = (*first)->NextChunk().Await(Deadline());
             if (!read.ok() || !read->has_value())
               return;
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
  // and only the last is waited for, so no worker parks between them. The gap
  // against pool_post_round_trip is the cost of sleeping and being woken, as
  // opposed to the queue and the signal.
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
  //
  // Each deadline here is earlier than the one before it, so every registration
  // becomes the pool's earliest timer -- the case that has to reach a worker
  // whose park is armed for something later. A pool that answers that by waking
  // every parked worker pays a thread wake per worker per registration, and
  // pays it on the caller's thread, so it shows up here as latency and not
  // merely as load. The deadlines are far enough out that none of them fires
  // during the run: this measures registering a timer, not running one.
  {
    const absl::Time base = absl::Now() + absl::Hours(1);
    std::atomic<std::int64_t> step{0};
    recorder.Add(
        {.suite = "scheduling",
         .name = "post_at_earliest",
         .metrics = Latency(
             [&](std::int64_t) {
               thread::PostAt(base - absl::Microseconds(step.fetch_add(
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
  // worker: the caller is then a fibre the scheduler can park and resume
  // without waking a sleeping thread, which is the difference worth knowing.
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
  //
  // Each of the rows above is a handoff with nothing on the other end, so each
  // of them gets faster the narrower the pool is -- a two-worker pool beats a
  // fourteen-worker one by 7.7x on `pool_post_pipelined`. Tuned against those
  // alone, the right pool has one worker in it, which would be an excellent
  // benchmark result and a useless runtime.
  //
  // This row is the one that punishes that: N fibres that each want a core for
  // a stretch, so its rate is a speedup measurement and it falls off a cliff
  // when the pool cannot run them at once. Anything that concentrates work on
  // fewer workers has to hold this number as well as improve the others.
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
//
// The server side is accepted inside the factory because that is what completes
// the handshake: for WebSocket the accept is what sends the 101 the client's
// Start() is waiting for, so a factory that returned the peer for the caller to
// accept later would deadlock the two against each other.
struct BenchPair {
  std::shared_ptr<net::WireStream> client;
  std::function<void()> close;
};

// The slot is where the factory publishes the accepted peer, so the echo can
// find the endpoint it must reply on. One slot per measurement, owned by it.
using PairFactory = std::function<std::optional<BenchPair>(
    std::weak_ptr<net::WireStream>*, net::OnMessage, net::OnDone)>;

// Counted arrivals, shared with the callback rather than captured by reference.
//
// A promise reassigned on this thread while a pool fibre is resolving the last
// one is a race on the pointer itself, and a callback that outlives the loop it
// was made for is a read of a dead stack frame: both crash inside the fibre
// condition variable, some rows later, with nothing in the trace to say why.
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
  // Accounting is in fragments, not messages, and that is not a detail.
  //
  // Both in-process Sender loops fold whatever is already queued into the
  // message they are about to deliver, so N messages sent can arrive as fewer,
  // larger ones -- and they do, as soon as anything delivers in bursts rather
  // than one at a time (a transport with concurrent outbound requests, say). A
  // window counted in messages then waits for echoes that will never come
  // separately and the row reports a stall that is not one. Fragments survive the
  // fold, so one echo fragment per arriving fragment keeps the credit exact.
  //
  // Replies on the fibre the transport already delivered on.
  //
  // A SubmitTask here would be a pool round trip (2.2us) plus, when the pool's
  // workers are parked, an OS thread wake -- on both sides of every message, and
  // charged to every transport equally. That is a large fraction of the small-
  // message rows and it prices A11's scheduler rather than the wire. The
  // delivery fibre is a legitimate place to send from: it is a fibre, Send takes
  // the endpoint's claim and writes inline, and no callback runs on a caller of
  // Send on any of these transports.
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
  // row. An echo that never arrives is a transport problem, and a benchmark
  // that waits forever for it reports nothing at all -- not even the rows that
  // had already passed.
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
 * pushing back (FINDINGS.md item 7). The peer's one-byte echo per message is the
 * credit that lets the next one go -- cheap enough on the wire (~80 bytes
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
  auto metrics =
      Throughput([&](std::int64_t) { pump(messages); }, 1, 0, messages,
                 messages * size);
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
  // target message size is exactly the size that turns one message into two
  // packets and takes the out-of-order reassembly path instead of the
  // single-packet one. A11_BENCH_WS_SPLIT prices that: it is a wire-format
  // decision shared with the other languages' clients, so the number is worth
  // having before anyone changes the default.
  const char* ws_split = std::getenv("A11_BENCH_WS_SPLIT");
  if (ws_split != nullptr) {
    const size_t split = static_cast<size_t>(std::max(1024, std::atoi(ws_split)));
    options.framing.split_size = split;
  }
  // HTTP/1.1 on both sides. What this row should price is WebSocket framing,
  // not ALPN, and offering h2 to a listener that does not serve it costs the
  // connection rather than just the negotiation.
  options.http2_options.enable_h2 = false;
  options.http2_options.enable_h2c = false;

  const auto shared_server =
      std::make_shared<net::OnMessage>(std::move(on_server));
  const auto shared_done = std::make_shared<net::OnDone>(std::move(on_done));
  // Somebody has to own the accepted stream. The transport does not keep it
  // alive for the handler's sake, and the echo holds only a weak reference so
  // that it is not part of a cycle with the stream it replies on -- so the pair
  // holds it, and lets go in close().
  const auto accepted = std::make_shared<std::shared_ptr<net::WireStream>>();
  absl::StatusOr<std::shared_ptr<net::WebSocketWireServer>> server =
      net::WebSocketWireServer::Create(
          [peer_slot, accepted, shared_server, shared_done](
              std::shared_ptr<net::WebSocketWireStream> stream) -> a11::Task {
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
          absl::StrCat("ws://127.0.0.1:", *port, "/bench"), {},
          client_options);
  if (!client.ok()) {
    std::fprintf(stderr, "  skip websocket: %s\n",
                 std::string(client.status().message()).c_str());
    (*server)->Stop().IgnoreError();
    return std::nullopt;
  }
  const std::shared_ptr<net::WebSocketWireStream> holder = *client;
  const std::shared_ptr<net::WebSocketWireServer> listener = *server;
  return BenchPair{.client = holder,
                   .close = [holder, listener, accepted] {
                     holder->HalfClose().IgnoreError();
                     (void)holder->DrainOutgoingMessages().Await(Deadline());
                     listener->Stop().IgnoreError();
                     accepted->reset();
                   }};
}

// Server-Sent Events: the transport a browser can always reach, and the only one
// whose two directions are different mechanisms -- an SSE response stream
// inbound, and outbound either one HTTP POST per message or a single streamed
// request body. Both outbound modes get a row, because they are the two ends of
// the trade the option exists for: `sse` is what a `fetch()` client can do, and
// `sse-stream` is what a capable backend can.
//
// A11_BENCH_SSE_POSTS sets how many outbound POSTs the `sse` row keeps in flight;
// 1 restores the strictly serialised delivery this used to have.
std::optional<BenchPair> HttpSseVariantPair(
    net::SseOutboundDelivery outbound,
    std::weak_ptr<net::WireStream>* peer_slot, net::OnMessage on_server,
    net::OnDone on_done) {
  net::HttpSseOptions client_options;
  client_options.outbound = outbound;
  if (const char* posts = std::getenv("A11_BENCH_SSE_POSTS");
      posts != nullptr) {
    const int parsed = std::atoi(posts);
    if (parsed > 0) {
      client_options.max_concurrent_posts = static_cast<size_t>(parsed);
    }
  }
  const auto shared_server =
      std::make_shared<net::OnMessage>(std::move(on_server));
  const auto shared_done = std::make_shared<net::OnDone>(std::move(on_done));
  const auto accepted = std::make_shared<std::shared_ptr<net::WireStream>>();
  absl::StatusOr<std::shared_ptr<net::HttpSseServer>> server =
      net::HttpSseServer::Create(
          "127.0.0.1", 0,
          [peer_slot, accepted, shared_server, shared_done](
              std::shared_ptr<net::HttpSseServerWireStream> stream)
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
  const std::shared_ptr<net::HttpSseClientWireStream> holder = *client;
  const std::shared_ptr<net::HttpSseServer> listener = *server;
  return BenchPair{.client = holder,
                   .close = [holder, listener, accepted] {
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
//
// Signalling is local so the row prices the data channel rather than a
// rendezvous server, but everything below it is real: ICE, DTLS, SCTP. The
// accept runs on its own fibre because it does not return until the peer
// connection is up, and the callback it runs in is on the negotiation path.
std::optional<BenchPair> WebRtcPair(std::weak_ptr<net::WireStream>* peer_slot,
                                    net::OnMessage on_server,
                                    net::OnDone on_done) {
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
  const char* split = std::getenv("A11_BENCH_RTC_SPLIT");
  if (split != nullptr) {
    configuration.channel_split_size =
        static_cast<size_t>(std::max(1024, std::atoi(split)));
  }
  absl::StatusOr<std::shared_ptr<net::WebRtcWireServer>> server =
      net::WebRtcWireServer::Create(
          "bench-server", signalling,
          [peer_slot, accepted, shared_server, shared_done](
              std::shared_ptr<net::WebRtcWireStream> stream) -> a11::Task {
            *accepted = stream;
            *peer_slot = stream;
            return a11::SubmitTask([stream, shared_server,
                                    shared_done]() -> absl::Status {
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
  const std::shared_ptr<net::WebRtcWireStream> holder = *client;
  const std::shared_ptr<net::WebRtcWireServer> listener = *server;
  return BenchPair{.client = holder,
                   .close = [holder, listener, accepted, signalling] {
                     holder->HalfClose().IgnoreError();
                     (void)holder->DrainOutgoingMessages().Await(Deadline());
                     listener->Stop().IgnoreError();
                     accepted->reset();
                   }};
}

}  // namespace

// A round trip to the libuv loop and back, with nothing else in it.
//
// Every socket transport crosses this boundary twice per message: once to hand
// the write to the loop thread, once when the loop hands a read back. Pricing it
// on its own is what turns "a WebSocket round trip costs 63us" into a budget,
// because the crossing is the largest thing in that number that is neither the
// kernel nor A11's own scheduling.
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
//
// The floor for every socket transport: two threads, two sockets, one write and
// one read each way, no framing, no fibres, no event loop. Whatever a real
// transport costs above this is A11's; whatever this costs is the kernel's, and
// reading the WebSocket row without it invites attributing one to the other.
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
//
// The counterpart to MeasureLoopbackFloor for the throughput rows, and the
// number the 5 Gbit/s target has to be read against -- it is what loopback plus
// a one-byte-per-message credit costs with no A11 in it at all, so a transport
// cannot be expected above it and the gap below it is A11's.
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
            const ssize_t put =
                ::write(client, payload.data() + offset, payload.size() - offset);
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

void WireSuite(Recorder& recorder, double scale) {
  using Factory = std::optional<BenchPair> (*)(std::weak_ptr<net::WireStream>*,
                                               net::OnMessage, net::OnDone);
  const std::vector<std::pair<std::string, Factory>> transports = {
      {"in-process", InProcessPair},
      {"websocket", WebSocketPair},
      {"sse", HttpSsePair},
      {"sse-stream", HttpSseStreamPair},
      {"webrtc", WebRtcPair}};
  for (const auto& [name, factory] : transports) {
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
  MeasureUvCrossing(recorder, scale);
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
      {"wire", WireSuite},
  };
  return *suites;
}

}  // namespace
}  // namespace a11::bench

int main(int argc, char** argv) {
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
    } else if (flag == "--help" || flag == "-h") {
      std::printf(
          "a11_bench [--suite NAME]... [--scale N] [--json PATH]\n"
          "  Native counterparts to `python -m bench`. Same record shape,\n"
          "  same benchmark names, no Python.\n");
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
    // Report each suite as it finishes rather than everything at the end. A
    // benchmark that crashes takes the process with it, and a run that only
    // reports at the end therefore loses the suites that had already passed --
    // which is how a crash in the last suite hid every wire number there was.
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
