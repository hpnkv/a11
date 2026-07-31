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
  std::string mimetype;             ///< Media type of the chunk payload.
  std::optional<absl::Time> timestamp{};  ///< Optional creation timestamp.
  ByteMap attributes{};             ///< Free-form key/value attributes.

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;
  /** @brief Returns attribute @p key, or a NotFound error when absent. */
  absl::StatusOr<std::string> GetAttribute(std::string_view key) const;
  /** @brief Sets attribute @p key to @p value. */
  absl::Status SetAttribute(std::string key, std::string value);

  /** @brief Encodes this metadata as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /** @brief Decodes MessagePack @p bytes into a ChunkMetadata. */
  static absl::StatusOr<ChunkMetadata> FromMsgpack(std::string_view bytes);

  friend bool operator==(const ChunkMetadata&, const ChunkMetadata&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ChunkMetadata& value) {
    sink.Append(value.DebugString());
  }
};

/**
 * @brief A unit of data: bytes plus optional descriptive metadata.
 *
 * A chunk holds its payload in @c data with an optional @c metadata
 * describing it. Instead of inline data a chunk may instead carry a @c ref
 * naming another node whose content it stands in for; such a reference must
 * be resolved before the payload is used.
 */
struct Chunk {
  std::optional<ChunkMetadata> metadata{};  ///< Optional payload metadata.
  std::string ref{};   ///< Node id this chunk references, if not inline.
  Bytes data{};        ///< Inline byte payload.

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  /** @brief Returns the metadata mimetype, or empty when unset. */
  [[nodiscard]] std::string GetMimetype() const;
  /** @brief Whether the chunk carries neither data nor a reference. */
  [[nodiscard]] bool IsEmpty() const;
  /** @brief Whether the chunk represents an explicit null value. */
  [[nodiscard]] bool IsNull() const;
  absl::Status Validate() const;

  /** @brief Encodes this chunk as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
  /** @brief Decodes MessagePack @p bytes into a Chunk. */
  static absl::StatusOr<Chunk> FromMsgpack(std::string_view bytes);

  friend bool operator==(const Chunk&, const Chunk&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Chunk& value) {
    sink.Append(value.DebugString());
  }
};

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

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  /** @brief Encodes this reference as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
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
  std::optional<std::uint32_t> seq;  ///< Ordering sequence number.
  bool continued = false;  ///< Whether further fragments follow this one.

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
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

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  /** @brief Encodes this port as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
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
  std::string id;             ///< Unique id of this action instance.
  std::string name;           ///< Registered action name.
  std::vector<Port> inputs{};   ///< Input port -> node bindings.
  std::vector<Port> outputs{};  ///< Output port -> node bindings.
  ByteMap headers{};          ///< Per-call headers.

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  /** @brief Encodes this message as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
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

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  /** @brief Encodes this message as MessagePack bytes. */
  absl::StatusOr<Bytes> ToMsgpack() const;
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
