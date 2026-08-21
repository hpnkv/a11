// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's core wire value types: chunks, node fragments and messages.
 *
 * These are the plain data records that A11 moves between endpoints. A
 * ::a11::data::Chunk is a blob of bytes plus a ::a11::data::ChunkMetadata
 * (mimetype and attributes); chunks flow through named nodes as
 * ::a11::data::NodeFragment values (either an inline chunk or a
 * ::a11::data::NodeRef pointing at a slice of another node). Actions are
 * described on the wire by ::a11::data::ActionMessage (a named invocation with
 * input/output ::a11::data::Port lists and headers), and one or more of these,
 * together with node fragments, are batched into a ::a11::data::WireMessage --
 * the unit exchanged across a stream.
 *
 * Every record validates itself, reports an approximate encoded size, prints a
 * debug string, and round-trips through MessagePack via @c ToMsgpack /
 * @c FromMsgpack.
 */

#ifndef A11_DATA_TYPES_H_
#define A11_DATA_TYPES_H_

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

namespace a11::data {

class MsgpackWriter;

/** @brief Raw byte payload; an alias of @c std::string. */
using Bytes = std::string;
/** @brief String-keyed map of byte values (headers, attributes, etc.). */
using ByteMap = absl::flat_hash_map<std::string, Bytes>;

/**
 * @brief Validates an A11 identifier (node/port/action name).
 * @param name Candidate name.
 * @return OK when @p name matches A11's identifier grammar, else an error.
 */
absl::Status ValidateName(std::string_view name);

/**
 * @brief Descriptive metadata attached to a ::a11::data::Chunk.
 *
 * Carries the payload's @c mimetype, an optional @c timestamp, and a bag of
 * free-form @c attributes. The mimetype may embed a serialization type tag
 * (see a11::data::SerializationRegistry).
 */
struct ChunkMetadata {
  std::string mimetype;                   ///< Media type of the chunk payload.
  std::optional<absl::Time> timestamp{};  ///< Optional creation timestamp.
  ByteMap attributes{};                   ///< Free-form key/value attributes.

  /// Estimate memory/wire weight for bounded-buffer accounting.
  [[nodiscard]] size_t ApproxBytes() const;
  /// Return a concise representation suitable for logs and diagnostics.
  [[nodiscard]] std::string DebugString() const;
  /// Validate metadata before it crosses a store or transport boundary.
  absl::Status Validate() const;
  /** @brief Returns attribute @p key, or a NotFound error when absent. */
  absl::StatusOr<std::string> GetAttribute(std::string_view key) const;
  /** @brief Sets attribute @p key to @p value. */
  absl::Status SetAttribute(std::string key, std::string value);

  /** @brief Encodes this metadata as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /**
   * @brief Append this record's fields to an already-open @p writer.
   *
   * What ToMsgpack() is built from, and what a parent record calls through
   * MsgpackWriter::PackRecord so that nesting costs no buffer of its own. Both
   * produce the same bytes; this one avoids the intermediate string.
   *
   * **Assumes the record is already valid.** Validate() recurses through nested
   * records, so validating here as well would re-check every nested id once per
   * ancestor level. ToMsgpack() validates the whole tree once and then calls
   * this; a caller reaching for this directly should do the same, and any
   * caller that got its record from FromMsgpack() already has.
   */
  absl::Status ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const;
  /** @brief Decodes MessagePack @p bytes into a ChunkMetadata. */
  static absl::StatusOr<ChunkMetadata> FromMsgpack(std::string_view bytes);

  friend bool operator==(const ChunkMetadata&, const ChunkMetadata&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ChunkMetadata& value) {
    sink.Append(value.DebugString());
  }
};

/**
 * @brief An in-process value a chunk may carry instead of its encoded bytes.
 *
 * The thing this exists to remove: a value written to a node and read back in
 * the same process was encoded on the way in and decoded on the way out, and
 * both are pure waste when nobody outside the process ever sees the bytes.
 * Decode is the expensive half -- nested-record decode ran at ~130 MiB/s
 * against encode's 2+ GiB/s before ReadBinaryView -- so the local path was
 * paying for a wire format it never used.
 *
 * A chunk carrying one of these has **no bytes at all** until somebody needs
 * them. Chunk::Materialize() is where they come from, and it is called at every
 * boundary where bytes are genuinely required: a store that persists them, a
 * stream that sends them, or a reader that asked for a chunk rather than a
 * value.
 *
 * ### Identity, without RTTI
 *
 * tag() is the serialisation tag -- `a11.sdk.AudioBuffer` -- and it is what a
 * consumer compares before casting. Deliberately *not* `std::type_info`: RTTI
 * identity across the translation units linked into one shared object has
 * already broken here once, as a `bad any cast` from a `std::any` in this same
 * position. The tag table is a compile-time constant that four languages
 * already agree on, so comparing it is both cheaper and sound where comparing
 * type identity is not.
 *
 * ### Immutability
 *
 * The held value must not change after it is put here, which is not a new rule:
 * a mutable value in a ChunkStore was never sound, since a store replays a
 * fragment to every reader and to readers that attach later. What is new is
 * that sharing makes the rule load-bearing rather than academic, so the value
 * is handed out **by copy** on the consuming side -- one copy per consumer that
 * asks, instead of one encode plus one decode per consumer.
 */
class ChunkObject {
 public:
  virtual ~ChunkObject() = default;

  /** @brief The serialisation tag of the held type. */
  [[nodiscard]] virtual std::string_view tag() const = 0;
  /** @brief The mimetype the value would be encoded as. */
  [[nodiscard]] virtual std::string_view mimetype() const = 0;
  /** @brief Encodes the value, for whoever needs bytes after all. */
  [[nodiscard]] virtual absl::StatusOr<Bytes> Encode() const = 0;
  /** @brief A size estimate that does not encode anything. */
  [[nodiscard]] virtual size_t ApproxBytes() const = 0;

  /**
   * @brief The address of the held value.
   *
   * Do not call this. It is dereferenced in exactly one place --
   * a11::data::TryTakeObject, which compares tag() first -- and a cast without
   * that comparison is undefined behaviour with a plausible-looking result.
   */
  [[nodiscard]] virtual const void* address() const = 0;
};

/**
 * @brief A unit of data: bytes plus optional descriptive metadata.
 *
 * A chunk holds its payload in @c data with an optional @c metadata
 * describing it. Instead of inline data a chunk may instead carry a @c ref
 * naming another node whose content it stands in for; such a reference must
 * be resolved before the payload is used.
 *
 * A third possibility exists and is local to one process: an @c object, whose
 * bytes have not been produced because nothing has needed them. See
 * a11::data::ChunkObject, and note the one invariant that matters to everybody
 * else -- **an object-carrying chunk is not empty**, even though @c data is,
 * which is why IsEmpty() consults all three fields. A chunk that read as empty
 * would read as a null stream terminator, and would end a stream that was only
 * getting started.
 */
struct Chunk {
  std::optional<ChunkMetadata> metadata{};  ///< Optional payload metadata.
  std::string ref{};  ///< Node id this chunk references, if not inline.
  Bytes data{};       ///< Inline byte payload.
  /**
   * The value this chunk stands for, when its bytes have not been produced.
   *
   * Never crosses a process boundary: everything that sends, persists or hands
   * out bytes calls Materialize() first, so a peer and a durable store see
   * exactly what they saw before this field existed.
   */
  std::shared_ptr<const ChunkObject> object{};

  /// Estimate memory/wire weight for bounded-buffer accounting.
  [[nodiscard]] size_t ApproxBytes() const;
  /// Return a concise representation suitable for logs and diagnostics.
  [[nodiscard]] std::string DebugString() const;
  /** @brief Returns the metadata mimetype, or empty when unset. */
  [[nodiscard]] std::string GetMimetype() const;
  /** @brief Whether the chunk carries no data, reference or object. */
  [[nodiscard]] bool IsEmpty() const;
  /** @brief Whether the chunk represents an explicit null value. */
  [[nodiscard]] bool IsNull() const;
  /// Validate that payload, reference, and metadata fields are consistent.
  absl::Status Validate() const;

  /** @brief Whether this chunk is carrying a value rather than bytes. */
  [[nodiscard]] bool HasObject() const { return object != nullptr; }
  /**
   * @brief Produces @c data from @c object, if it has not been produced yet.
   *
   * Idempotent, and a no-op on a chunk that already has its bytes. Called at
   * every boundary where bytes are required -- a persisting store, an attached
   * stream, a reader asking for a chunk -- which is what keeps @c object an
   * optimisation rather than a second representation everybody has to know
   * about. The object is released afterwards, so the bytes become the single
   * answer once they exist.
   */
  absl::Status Materialize();

  /** @brief Encodes this chunk as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /**
   * @brief Append this record's fields to an already-open @p writer.
   *
   * What ToMsgpack() is built from, and what a parent record calls through
   * MsgpackWriter::PackRecord so that nesting costs no buffer of its own. Both
   * produce the same bytes; this one avoids the intermediate string.
   *
   * **Assumes the record is already valid.** Validate() recurses through nested
   * records, so validating here as well would re-check every nested id once per
   * ancestor level. ToMsgpack() validates the whole tree once and then calls
   * this; a caller reaching for this directly should do the same, and any
   * caller that got its record from FromMsgpack() already has.
   */
  absl::Status ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const;
  /** @brief Decodes MessagePack @p bytes into a Chunk. */
  static absl::StatusOr<Chunk> FromMsgpack(std::string_view bytes);

  friend bool operator==(const Chunk&, const Chunk&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Chunk& value) {
    sink.Append(value.DebugString());
  }
};

/** @brief Mimetype marking a chunk whose payload is a packed status. */
inline constexpr std::string_view kStatusMimetype = "application/x-a11-status";
/**
 * @brief Metadata attribute marking a status chunk as a closure marker.
 *
 * A plain status chunk carries a value (an action's outcome) or a failure to
 * apply to a node. One carrying this attribute instead reports that the
 * producer has drained the node and closed its write half with that status; it
 * holds no application value and is never stored.
 */
inline constexpr std::string_view kCloseAttribute = "a11-close";

/**
 * @brief Encodes @p status as a chunk of mimetype ::kStatusMimetype.
 * @param status The status to pack.
 * @param closing Whether to mark the chunk as a closure marker
 *   (::kCloseAttribute), meaning the producer closed the node's write half.
 */
absl::StatusOr<Chunk> MakeStatusChunk(const absl::Status& status,
                                      bool closing = false);
/** @brief Whether @p chunk carries a packed status. */
bool IsStatusChunk(const Chunk& chunk);
/** @brief Whether @p chunk is a status chunk marking write-half closure. */
bool IsCloseStatusChunk(const Chunk& chunk);
/** @brief Decodes a status previously encoded by MakeStatusChunk. */
absl::StatusOr<absl::Status> StatusFromStatusChunk(const Chunk& chunk);

/**
 * @brief A reference to a (slice of a) logical node, in lieu of inline data.
 *
 * Lets a fragment point at content held by another node -- optionally a
 * window of it -- rather than copying the bytes.
 */
struct NodeRef {
  std::string id;            ///< Id of the referenced node.
  std::uint32_t offset = 0;  ///< Byte offset into the referenced node.
  // 2^32 is a valid length for a full logical node and therefore requires a
  // wider representation than offset and sequence numbers.
  std::optional<std::uint64_t> length;  ///< Optional length of the window.

  /// Estimate memory/wire weight for bounded-buffer accounting.
  [[nodiscard]] size_t ApproxBytes() const;
  /// Return a concise representation suitable for logs and diagnostics.
  [[nodiscard]] std::string DebugString() const;
  /// Validate the referenced node id and requested slice.
  absl::Status Validate() const;

  /** @brief Encodes this reference as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /**
   * @brief Append this record's fields to an already-open @p writer.
   *
   * What ToMsgpack() is built from, and what a parent record calls through
   * MsgpackWriter::PackRecord so that nesting costs no buffer of its own. Both
   * produce the same bytes; this one avoids the intermediate string.
   *
   * **Assumes the record is already valid.** Validate() recurses through nested
   * records, so validating here as well would re-check every nested id once per
   * ancestor level. ToMsgpack() validates the whole tree once and then calls
   * this; a caller reaching for this directly should do the same, and any
   * caller that got its record from FromMsgpack() already has.
   */
  absl::Status ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const;
  /** @brief Decodes MessagePack @p bytes into a NodeRef. */
  static absl::StatusOr<NodeRef> FromMsgpack(std::string_view bytes);

  friend bool operator==(const NodeRef&, const NodeRef&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const NodeRef& value) {
    sink.Append(value.DebugString());
  }
};

/**
 * @brief One piece of a node's stream: an inline chunk or a node reference.
 *
 * Fragments are how data flows through a named node. Each targets a node by
 * @c id and carries either a Chunk or a NodeRef. @c seq orders fragments
 * within the node and @c continued marks that more fragments follow (the
 * stream is not yet complete).
 */
struct NodeFragment {
  std::string id;  ///< Id of the node this fragment belongs to.
  std::variant<Chunk, NodeRef> data = Chunk{};  ///< Inline chunk or reference.
  std::optional<std::uint32_t> seq;             ///< Ordering sequence number.
  bool continued = false;  ///< Whether further fragments follow this one.

  /// Estimate memory/wire weight for session and transport limits.
  [[nodiscard]] size_t ApproxBytes() const;
  /// Return a concise representation suitable for logs and diagnostics.
  [[nodiscard]] std::string DebugString() const;
  /// Validate the node id, payload, sequence, and continuation marker.
  absl::Status Validate() const;
  /** @brief Returns the held Chunk, or an error when it holds a NodeRef. */
  absl::StatusOr<Chunk* absl_nonnull> GetChunk();
  /** @brief Returns the held Chunk, or an error when it holds a NodeRef. */
  absl::StatusOr<const Chunk* absl_nonnull> GetChunk() const;
  /** @brief Returns the held NodeRef, or an error when it holds a Chunk. */
  absl::StatusOr<NodeRef* absl_nonnull> GetNodeRef();
  /** @brief Returns the held NodeRef, or an error when it holds a Chunk. */
  absl::StatusOr<const NodeRef* absl_nonnull> GetNodeRef() const;

  /** @brief Encodes this fragment as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /**
   * @brief Append this record's fields to an already-open @p writer.
   *
   * What ToMsgpack() is built from, and what a parent record calls through
   * MsgpackWriter::PackRecord so that nesting costs no buffer of its own. Both
   * produce the same bytes; this one avoids the intermediate string.
   *
   * **Assumes the record is already valid.** Validate() recurses through nested
   * records, so validating here as well would re-check every nested id once per
   * ancestor level. ToMsgpack() validates the whole tree once and then calls
   * this; a caller reaching for this directly should do the same, and any
   * caller that got its record from FromMsgpack() already has.
   */
  absl::Status ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const;
  /** @brief Decodes MessagePack @p bytes into a NodeFragment. */
  static absl::StatusOr<NodeFragment> FromMsgpack(std::string_view bytes);

  friend bool operator==(const NodeFragment&, const NodeFragment&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const NodeFragment& value) {
    sink.Append(value.DebugString());
  }
};

/**
 * @brief Binds an action's port @c name to the concrete node @c id serving it.
 *
 * Ports appear in a ::a11::data::ActionMessage to tell the peer which node
 * backs each of the action's named inputs and outputs.
 */
struct Port {
  std::string name;  ///< Schema-defined port name.
  std::string id;    ///< Id of the node backing this port.

  /// Estimate memory/wire weight for session and transport limits.
  [[nodiscard]] size_t ApproxBytes() const;
  /// Return a concise representation suitable for logs and diagnostics.
  [[nodiscard]] std::string DebugString() const;
  /// Validate the port name and node id used to bind an action.
  absl::Status Validate() const;

  /** @brief Encodes this port as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /**
   * @brief Append this record's fields to an already-open @p writer.
   *
   * What ToMsgpack() is built from, and what a parent record calls through
   * MsgpackWriter::PackRecord so that nesting costs no buffer of its own. Both
   * produce the same bytes; this one avoids the intermediate string.
   *
   * **Assumes the record is already valid.** Validate() recurses through nested
   * records, so validating here as well would re-check every nested id once per
   * ancestor level. ToMsgpack() validates the whole tree once and then calls
   * this; a caller reaching for this directly should do the same, and any
   * caller that got its record from FromMsgpack() already has.
   */
  absl::Status ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const;
  /** @brief Decodes MessagePack @p bytes into a Port. */
  static absl::StatusOr<Port> FromMsgpack(std::string_view bytes);

  friend bool operator==(const Port&, const Port&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Port& value) {
    sink.Append(value.DebugString());
  }
};

/**
 * @brief The wire description of an action invocation.
 *
 * Names the action (@c name) and the specific instance (@c id), lists the
 * ::a11::data::Port bindings for its @c inputs and @c outputs, and carries
 * per-call @c headers. This is what a peer sends to dispatch an action and
 * what an a11::actions::Action produces to describe itself.
 */
struct ActionMessage {
  std::string id;               ///< Unique id of this action instance.
  std::string name;             ///< Registered action name.
  std::vector<Port> inputs{};   ///< Input port -> node bindings.
  std::vector<Port> outputs{};  ///< Output port -> node bindings.
  ByteMap headers{};            ///< Per-call headers.

  /// Estimate memory/wire weight for session and transport limits.
  [[nodiscard]] size_t ApproxBytes() const;
  /// Return a concise representation suitable for logs and diagnostics.
  [[nodiscard]] std::string DebugString() const;
  /// Validate the invocation id, action name, ports, and headers.
  absl::Status Validate() const;

  /** @brief Encodes this message as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /**
   * @brief Append this record's fields to an already-open @p writer.
   *
   * What ToMsgpack() is built from, and what a parent record calls through
   * MsgpackWriter::PackRecord so that nesting costs no buffer of its own. Both
   * produce the same bytes; this one avoids the intermediate string.
   *
   * **Assumes the record is already valid.** Validate() recurses through nested
   * records, so validating here as well would re-check every nested id once per
   * ancestor level. ToMsgpack() validates the whole tree once and then calls
   * this; a caller reaching for this directly should do the same, and any
   * caller that got its record from FromMsgpack() already has.
   */
  absl::Status ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const;
  /** @brief Decodes MessagePack @p bytes into an ActionMessage. */
  static absl::StatusOr<ActionMessage> FromMsgpack(std::string_view bytes);

  friend bool operator==(const ActionMessage&, const ActionMessage&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ActionMessage& value) {
    sink.Append(value.DebugString());
  }
};

/**
 * @brief The top-level frame exchanged between two A11 endpoints.
 *
 * Batches any number of ::a11::data::NodeFragment payloads and
 * ::a11::data::ActionMessage invocations together with connection-level
 * @c headers into a single unit sent across a stream. A half-close message
 * (see MakeHalfCloseMessage) is a WireMessage that signals the sender is done
 * writing.
 */
struct WireMessage {
  static constexpr std::uint32_t kVersion = 1;  ///< Wire format version.

  std::vector<NodeFragment> node_fragments{};  ///< Streamed data fragments.
  std::vector<ActionMessage> actions{};        ///< Action invocations.
  ByteMap headers{};                           ///< Message-level headers.

  /// Estimate memory/wire weight before admitting this message to a buffer.
  [[nodiscard]] size_t ApproxBytes() const;
  /// Return a concise representation suitable for logs and diagnostics.
  [[nodiscard]] std::string DebugString() const;
  /// Validate every contained fragment, action, and header.
  absl::Status Validate() const;

  /** @brief Encodes this message as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /**
   * @brief Append this record's fields to an already-open @p writer.
   *
   * What ToMsgpack() is built from, and what a parent record calls through
   * MsgpackWriter::PackRecord so that nesting costs no buffer of its own. Both
   * produce the same bytes; this one avoids the intermediate string.
   *
   * **Assumes the record is already valid.** Validate() recurses through nested
   * records, so validating here as well would re-check every nested id once per
   * ancestor level. ToMsgpack() validates the whole tree once and then calls
   * this; a caller reaching for this directly should do the same, and any
   * caller that got its record from FromMsgpack() already has.
   */
  absl::Status ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const;
  /** @brief Decodes MessagePack @p bytes into a WireMessage. */
  static absl::StatusOr<WireMessage> FromMsgpack(std::string_view bytes);

  friend bool operator==(const WireMessage&, const WireMessage&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const WireMessage& value) {
    sink.Append(value.DebugString());
  }
};

/** @brief Whether @p message is a transport half-close signal. */
bool IsHalfCloseMessage(const WireMessage& message);
/** @brief Builds a half-close WireMessage carrying optional @p trailers. */
WireMessage MakeHalfCloseMessage(ByteMap trailers = {});

/**
 * @brief The encoded size of an empty WireMessage.
 *
 * Computed from the same encoder rather than hard-coded, so it stays correct
 * if the wire format changes.
 */
size_t EmptyWireMessageSize();

}  // namespace a11::data

#endif  // A11_DATA_TYPES_H_
