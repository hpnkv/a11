// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_VALUES_H_
#define A11_FLOW_VALUES_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <absl/types/span.h>

#include "a11/data/types.h"
#include "a11/flow/pattern.h"
#include "a11/flow/plan.h"
#include "a11/flow/syntax.h"

namespace a11::flow {

class Value;

/// A value only the host knows what to do with.
///
/// The language's own values are JSON-ish, plus durations, instants, bytes and
/// chunks -- everything a flow can *write*. But `coerce` validates into types a
/// serialisation registry has been told about, and which types exist is the
/// host's decision: a flow cannot import anything, so `a11.sdk.Interaction` is
/// a pydantic model in the process running the flow and nothing the language
/// can construct. So a coerced value is carried as one of these, and the
/// language only ever asks it the questions an expression can ask: render it,
/// index it, compare it, is it there.
///
/// A host object is immutable and shared: an item fans out to every reader of a
/// stream, and copying a model per reader would be paid for in the host's
/// allocator for nothing.
class HostObject {
 public:
  virtual ~HostObject() = default;

  /// The tag the host knows this type by, for a message.
  [[nodiscard]] virtual std::string Tag() const = 0;
  /// What `as_text` gives: what the value is *on the wire*, not its repr.
  [[nodiscard]] virtual std::string Text() const = 0;
  /// Whether the value counts as true.
  [[nodiscard]] virtual bool Truthy() const = 0;
  /// How many things it holds, where that means anything.
  [[nodiscard]] virtual std::optional<size_t> Size() const = 0;
  /// `value.name`, or a null value when there is no such field. Reading a field
  /// a producer did not send answers nothing rather than failing, which is what
  /// lets a flow say `if not thing.field`.
  [[nodiscard]] virtual Value Field(std::string_view name) const = 0;
  /// `value[key]`.
  [[nodiscard]] virtual Value Element(const Value& key) const = 0;
  /// Whether this is the same value as `other`, by the host's own equality.
  [[nodiscard]] virtual bool Equals(const HostObject& other) const = 0;
};

/// One Flow value.
///
/// Immutable and cheap to copy: a container is held behind a `shared_ptr`, so
/// an item read once and handed to eight readers is one allocation, not eight.
/// Integers are kept apart from doubles because the language's own output
/// depends on it -- `text 3` is `"3"` and `text 3.0` is `"3.0"`, exactly as the
/// Python reference rendered them -- and because a count of values has to be a
/// whole number of them.
class Value {
 public:
  enum class Kind {
    /// A value that is not there: an absent key, an empty stream, a missing
    /// header. One kind for all of them, because a flow asking `if not x`
    /// should not have to know which sort of nothing it got.
    kNull,
    kBool,
    kInteger,
    kDouble,
    kString,
    kBytes,
    kList,
    kObject,
    kDuration,
    kTime,
    /// An undecoded chunk, kept whole so a pipe that only moves values
    /// re-writes the producer's own bytes and mimetype.
    kChunk,
    kHost,
  };

  /// The pairs of an object, in the order they were written.
  ///
  /// A vector rather than a hash map: a flow's objects have a handful of keys,
  /// insertion order is what a value written back out should keep, and a linear
  /// scan over five entries beats hashing them.
  using Pairs = std::vector<std::pair<std::string, Value>>;

  Value() = default;

  static Value Null() { return {}; }

  static Value Bool(bool value);
  static Value Integer(std::int64_t value);
  static Value Double(double value);
  static Value String(std::string value);
  static Value Bytes(data::Bytes value);
  static Value List(std::vector<Value> items);
  static Value Object(Pairs pairs);
  static Value Duration(absl::Duration value);
  static Value Time(absl::Time value);
  static Value Chunk(data::Chunk chunk);
  static Value Host(std::shared_ptr<const HostObject> object);
  /// The value a `Constant` -- a literal, or a folded literal expression -- is.
  static Value Of(const syntax::Constant& constant);

  [[nodiscard]] Kind kind() const { return kind_; }

  [[nodiscard]] bool IsNull() const { return kind_ == Kind::kNull; }

  [[nodiscard]] bool IsNumber() const {
    return kind_ == Kind::kInteger || kind_ == Kind::kDouble;
  }

  [[nodiscard]] bool IsTimelike() const {
    return kind_ == Kind::kDuration || kind_ == Kind::kTime;
  }

  /// Whether the value is one the language itself can take apart: a string or a
  /// byte string, which index and slice but are not containers of values.
  [[nodiscard]] bool IsTextlike() const {
    return kind_ == Kind::kString || kind_ == Kind::kBytes;
  }

  [[nodiscard]] bool boolean() const { return boolean_; }

  [[nodiscard]] std::int64_t integer() const { return integer_; }

  [[nodiscard]] double number() const { return number_; }

  [[nodiscard]] const std::string& text() const { return *text_; }

  [[nodiscard]] const std::vector<Value>& items() const { return *items_; }

  [[nodiscard]] const Pairs& pairs() const { return *pairs_; }

  [[nodiscard]] absl::Duration duration() const { return duration_; }

  [[nodiscard]] absl::Time time() const { return time_; }

  [[nodiscard]] const data::Chunk& chunk() const { return *chunk_; }

  [[nodiscard]] const HostObject& host() const { return *host_; }

  [[nodiscard]] const std::shared_ptr<const HostObject>& host_object() const {
    return host_;
  }

  /// The value at `key` of an object, or `nullptr`.
  [[nodiscard]] const Value* absl_nullable Get(std::string_view key) const;

  /// Whether the two values are equal, as `==` in a flow decides it.
  friend bool operator==(const Value& left, const Value& right);

 private:
  Kind kind_ = Kind::kNull;
  bool boolean_ = false;
  std::int64_t integer_ = 0;
  double number_ = 0.0;
  absl::Duration duration_;
  absl::Time time_;
  std::shared_ptr<const std::string> text_;
  std::shared_ptr<const std::vector<Value>> items_;
  std::shared_ptr<const Pairs> pairs_;
  std::shared_ptr<const data::Chunk> chunk_;
  std::shared_ptr<const HostObject> host_;
};

/// What the process running a flow knows and the language does not.
///
/// Three questions, all of them about types a serialisation registry has been
/// told about: make a value one of them, read one out of a chunk, and write one
/// into a chunk. The Python bindings answer them against the Python registry --
/// which is where a pydantic model actually lives -- and the standalone tool
/// answers them with the C++ registry, so the same runtime drives both without
/// either being the special case.
class HostBridge {
 public:
  virtual ~HostBridge() = default;

  /// Make `value` a value of the type `tag` names.
  ///
  /// `tag` is a dotted registry tag or a quoted mimetype: the built-in names
  /// are the language's own and are coerced before this is reached.
  virtual absl::StatusOr<Value> Coerce(std::string_view tag,
                                       const Value& value) = 0;

  /// The value `chunk` holds.
  virtual absl::StatusOr<Value> FromChunk(const data::Chunk& chunk) = 0;

  /// A chunk holding `value`, in `mimetype` when one is asked for.
  virtual absl::StatusOr<data::Chunk> ToChunk(const Value& value,
                                              std::string_view mimetype) = 0;

  /// The values a batch of chunks holds, in order, each with its own outcome.
  ///
  /// **Why a batch is its own question.** Crossing into a host is not free per
  /// crossing: the Python bridge takes the GIL, which a flow's fiber competes
  /// for with the interpreter thread that dispatched it, and that -- not the
  /// decoding -- is what a value through a stage costs. A pipeline usually has
  /// several values in hand at once, so it asks once for all of them.
  ///
  /// Per-value statuses rather than one for the batch, because the values
  /// before a bad one are still values: a reader that would have seen three of
  /// five and then a failure sees exactly that.
  ///
  /// The default asks one at a time, so a host need not implement it.
  virtual std::vector<absl::StatusOr<Value>> FromChunks(
      absl::Span<const data::Chunk* const> chunks) {
    std::vector<absl::StatusOr<Value>> values;
    values.reserve(chunks.size());
    for (const data::Chunk* chunk : chunks) {
      {
        values.push_back(FromChunk(*chunk));
      }
    }
    return values;
  }

  /// Chunks holding `values`, in order, each with its own outcome.
  ///
  /// The writing counterpart of [FromChunks]; the same reasoning applies.
  virtual std::vector<absl::StatusOr<data::Chunk>> ToChunks(
      absl::Span<const Value* const> values, std::string_view mimetype) {
    std::vector<absl::StatusOr<data::Chunk>> chunks;
    chunks.reserve(values.size());
    for (const Value* value : values) {
      chunks.push_back(ToChunk(*value, mimetype));
    }
    return chunks;
  }

  // How this host would rather hold a value of a shape the *language* declared.
  /// How this host would rather hold a value of a shape the *language*
  /// declared.
  ///
  /// Called with a value that has already been validated against `shape` --
  /// defaults filled, bounds checked, nested shapes coerced -- so this is about
  /// presentation and nothing else. The default is to hand it back unchanged,
  /// which is what a host with no opinion wants; the Python bindings build the
  /// pydantic model the shape describes and wrap an instance of it, so a value
  /// on a `struct`-typed port arrives in Python as a real model.
  ///
  /// Validation stays on this side of the boundary on purpose: there is one
  /// implementation of what a shape means, and a host that disagreed with it
  /// would be a second, quieter one.
  /// `program` is there because a shape is not much use without the shapes it
  /// names: a host building a type from `shape` needs the ones its fields refer
  /// to, and looking them up again on the other side of the boundary would be a
  /// second trip for data this side already has.
  virtual absl::StatusOr<Value> Adopt(const DtoPlan& shape,
                                      const Program& program,
                                      const Value& value) {
    (void)shape;
    (void)program;
    return value;
  }
};

/// A bridge over A11's own C++ serialisation registry.
///
/// What the standalone tool and the C++ tests run with. A tag the C++ registry
/// does not know is an error in the same words the Python reference used, which
/// reports the type as unavailable because its defining module is not loaded.
std::unique_ptr<HostBridge> NativeHostBridge();

// --- Reading values ----------------------------------------------------------

/// Take `key` out of `value`: a mapping key, an index, or a field.
///
/// Answers null when it is not there, because a flow reading a field a producer
/// did not send should be able to say `if not thing.field` rather than fall
/// over.
Value Lookup(const Value& value, const Value& key);

/// Whether `value` counts as true, as an `if` and a `where` decide it.
bool Truthy(const Value& value);

/// Where two values sit relative to one another: -1, 0 or 1.
///
/// What `<` and `>` mean in an expression, and so what `| sort` means: two
/// instants or two durations compare as themselves, a number as a number, and
/// everything else as text, so nothing dies on `"3" < 5`.
int Order(const Value& left, const Value& right);

/// `left + right`, as an expression means it.
///
/// For `| sum`, `| avg` and `| fold`, which add values the way `+` does:
/// numbers as numbers, durations as durations, an instant and a duration as the
/// shifted instant, and text as text.
absl::StatusOr<Value> Add(const Value& left, const Value& right);

/// `value` as text, the way the `text` stage and builtin render it.
std::string AsText(const Value& value);

/// `value` as a number, or zero when there is nothing to read.
///
/// Integral where the value was integral, so `%d` of a count is the count.
Value AsNumber(const Value& value);

/// The fields a pattern pulls out of `subject`, or null where it does not fit.
///
/// One implementation for both senses of `match`: the stage drops a value this
/// answers null for, and the function hands the null on. A record when every
/// hole is named, and a list when they are not -- `{}` is read by position, so
/// `it[0]` is what a positional pattern gives.
///
/// See [pattern::Compile] for the language. A pattern that does not compile is
/// an `invalid_argument` naming what is wrong with it, because a pattern is a
/// literal almost every time and a silent no-match would hide a typo in it.
absl::StatusOr<Value> MatchPattern(std::string_view pattern,
                                   std::string_view subject);

/// The same, against a pattern already compiled.
///
/// What a stage uses: the pattern is written once in the source and the stream
/// may be ten thousand values, so compiling it per value would be paying for
/// the same scan over and over.
Value MatchCompiled(const pattern::Pattern& pattern, std::string_view subject);

/// `AsNumber` as a double, for the places that only want the magnitude.
double AsDouble(const Value& value);

/// Text parsed as JSON; anything already decoded is left alone.
Value AsJson(const Value& value);

/// The first `size` of a value: characters, bytes, elements or pairs.
Value Truncate(const Value& value, std::int64_t size);

/// A value's own JSON text, as `json.dumps(sort_keys=True)` writes it.
///
/// The Python reference rendered a container with `json.dumps`, and a flow's
/// output can be a rendered object, so the spelling is part of the contract:
/// sorted keys, `", "` and `": "` between things, non-ASCII escaped.
std::string JsonText(const Value& value);

// --- Times and durations -----------------------------------------------------

/// A duration as a number of seconds, for every duration there is.
///
/// Infinite ones answer an infinity rather than failing: a timeout that never
/// fires is a duration a flow can hold, and so is the negative one two instants
/// subtracted the wrong way round give.
double DurationSeconds(absl::Duration value);

/// A duration of `total` seconds, negative ones included.
///
/// `absl::Seconds` is fine with a negative, but A11's own `Duration.seconds`
/// reads one as *infinite*, and this is the language's spelling: a number
/// beside a duration is a length, so `-30` is thirty seconds the other way.
absl::Duration SecondsDuration(double total);

/// A duration from the way the language writes one, or `nullopt`.
///
/// The source's spelling and the formatter's, both ways round: `30s`, `250ms`,
/// `1m30s`, `forever`, and a bare number of seconds. A duration a flow put on a
/// port comes back as text often enough -- through a header, a JSON field, a
/// model's answer -- that reading it back has to be as ordinary as writing it.
std::optional<absl::Duration> ParseDuration(std::string_view text);

/// A duration from a duration, from written text, or from seconds.
absl::Duration AsDuration(const Value& value);

/// An instant from an instant, from RFC 3339 text, or from epoch seconds.
absl::Time AsTime(const Value& value);

/// An instant from RFC 3339 text, as `TimeText` writes it.
std::optional<absl::Time> ParseTime(std::string_view text);

/// A duration as text: `1m30s` by default, or one unit when `spec` names one.
std::string DurationText(absl::Duration value, std::string_view spec = {});

/// An instant as text: RFC 3339 in UTC, a `strftime` pattern, or `epoch`.
std::string TimeText(absl::Time value, std::string_view spec = {});

// --- Formatting --------------------------------------------------------------

/// `format` with each `%` conversion replaced by one of `arguments`.
///
/// printf's syntax, because a format string is a thing people already know how
/// to read, and because it is *only* a format string: no attribute access, no
/// indexing, nothing a template can reach through. A flow's templates can come
/// from a model, so that matters more here than the convenience of a richer
/// template language would.
///
/// `%(SPEC)s` applies a spec to the value first -- a duration unit, a strftime
/// pattern, `epoch` -- and a conversion with no value behind it is left as it
/// was written, because a visible `%3$s` in the output is easier to diagnose
/// than a flow that died formatting a log line.
std::string Strformat(const Value& format, absl::Span<const Value> arguments);

// --- Evaluation --------------------------------------------------------------

/// What coercion needs besides the value and the type.
///
/// Two things, and they are different in kind: `bridge` is the *host* -- which
/// types have been registered where the flow runs -- and `shapes` is the
/// *program* -- which `struct`s this text declared. A shape wins over a
/// registry tag of the same name, which is why both are here and consulted in
/// that order.
struct CoerceContext {
  HostBridge* absl_nullable bridge = nullptr;
  const Program* absl_nullable shapes = nullptr;
};

/// What an expression is evaluated against.
///
/// `bound` is the first value of each stream the expression mentions, keyed by
/// the syntax node that mentioned it. Keying on the node rather than rewriting
/// the tree is what lets the AST stay immutable and shared: the Python
/// reference replaced each resolved name with a `RefValue` node, which a tree
/// handed across a language boundary cannot do.
struct EvalContext {
  const absl::flat_hash_map<const syntax::Node*, Value>* absl_nullable bound =
      nullptr;
  /// The value a `where`/`map`/`group` stage is looking at.
  Value it;
  bool has_it = false;
  HostBridge* absl_nullable bridge = nullptr;
  /// The shapes the program declared, for a cast written inside an expression.
  const Program* absl_nullable shapes = nullptr;

  [[nodiscard]] CoerceContext Coercion() const { return {bridge, shapes}; }
};

/// Evaluate one expression.
///
/// Fails for unsupported operations such as an unknown type or adding two
/// instants. Other invalid values use the language defaults: incompatible
/// comparisons use text, a missing key is null, and an unreadable number is
/// zero.
absl::StatusOr<Value> Evaluate(const syntax::Node& node,
                               const EvalContext& context);

/// Make `value` a value of the type `type` names.
///
/// A built-in name coerces the way the matching builtin does; a shape the
/// program declared is **validated** against, field by field; a tag or a
/// mimetype goes to the host, which is the only place that knows what has been
/// registered.
absl::StatusOr<Value> Coerce(const Value& value,
                             const syntax::TypeExpression& type,
                             const CoerceContext& context);

// Make `value` a value of `shape`: fill its defaults, check its bounds, and
// coerce every field to the type the shape gives it.
/// Make `value` a value of `shape`: fill its defaults, check its bounds, and
/// coerce every field to the type the shape gives it.
///
/// **The one implementation of what a shape means.** The resolver checks what
/// it can before anything runs -- a key the shape does not have, a constant of
/// the wrong kind -- and this checks the rest, when a value actually arrives. A
/// failure names the field it was about, by path (`parent.tags[2]`), because a
/// flow's data comes from somewhere else and "invalid" without a path is a
/// message nobody can act on.
///
/// A key the shape does not have is **dropped**, not refused: extra data is how
/// `{...it, ..}` is useful, and a producer that sends more than a reader
/// declared has done nothing wrong. Writing such a key out by hand is a
/// different thing and the resolver says so.
absl::StatusOr<Value> CoerceShape(const DtoPlan& shape, const Value& value,
                                  const CoerceContext& context);

/// Call one of the language's fixed functions.
///
/// Public because the `strformat` stage is the one-value shorthand for the
/// builtin and must not become a second implementation of it.
absl::StatusOr<Value> CallBuiltin(std::string_view name,
                                  absl::Span<const Value> arguments,
                                  HostBridge* absl_nullable bridge);

// --- Statuses as data --------------------------------------------------------

/// A status as the record a flow sees when it looks at an outcome.
///
/// `{"ok": .., "code": "NOT_FOUND", "number": 5, "message": ..}` -- data, so a
/// flow can branch on it, put it on an output, or raise it again, without any
/// of the language knowing what a status is.
Value StatusRecord(const absl::Status& status);

/// The status a record like the one above describes.
absl::Status StatusOfRecord(const Value& record);

/// The canonical code `value` names, by name or by number, or `nullopt`.
///
/// Either case of a canonical name, and any number Abseil defines a code for,
/// which is what lets a flow re-raise a status it was handed without knowing
/// how it was spelled.
std::optional<absl::StatusCode> StatusCodeOf(const Value& value);

}  // namespace a11::flow

#endif  // A11_FLOW_VALUES_H_
