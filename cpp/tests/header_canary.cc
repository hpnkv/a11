// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Every installed A11 header, compiled with exceptions disabled.
 *
 * A11's own libraries are built `-fno-exceptions` (see the exception policy
 * block in cpp/CMakeLists.txt), but its headers are also compiled by
 * *consumers*, whose build may go either way. This translation unit pins the
 * direction that is easy to break: a `try` or a `throw` added to a public
 * header compiles fine in a build with exceptions on and fails here, which is a
 * build error in the commit that introduced it rather than a puzzle for whoever
 * next flips a library.
 *
 * The other direction -- that the headers still work *with* exceptions -- is
 * covered by every other test in this directory, which are compiled normally.
 *
 * Add new public headers here. There is no automatic globbing on purpose: a
 * header that nobody added is a header nobody promised to keep compiling.
 */

#include "a11/actions/action.h"
#include "a11/actions/log.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/callback_scheduler.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/concurrency/inline_pump.h"
#include "a11/data/json.h"
#include "a11/data/msgpack.h"
#include "a11/data/serial_tags.h"
#include "a11/data/serializable.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/exception_guard.h"
#include "a11/json_codec.h"
#include "a11/net/byte_chunking.h"
#include "a11/net/channel_wire_stream.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/wire_stream.h"
#include "a11/net/wire_stream_with_recv.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/obs/span.h"
#include "a11/obs/tracer.h"
#include "a11/percent.h"
#include "a11/service/service.h"
#include "a11/service/session.h"
#include "a11/status.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/local_chunk_store.h"
#include "a11/time.h"
#include "a11/utf8.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

// Compiled, not run: the assertion is that the above parse and instantiate with
// -fno-exceptions. A symbol keeps the archive non-empty on every toolchain.
namespace a11::internal {

bool A11HeadersCompileWithoutExceptions() {
  return true;
}

}  // namespace a11::internal
