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

#include "a11/flow/vocabulary.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>

namespace a11::flow::vocabulary {
namespace {

// Stages in the order a listing reads best: what shortens a stream, what
// reshapes each value, what reduces it to one, what encodes it.
constexpr std::array kStageOrder = {
    std::string_view("read_with"),  std::string_view("ref"),
    std::string_view("take while"), std::string_view("take until"),
    std::string_view("first"),      std::string_view("last"),
    std::string_view("drop"),       std::string_view("truncate"),
    std::string_view("batch"),      std::string_view("window"),
    std::string_view("context"),    std::string_view("flatten"),
    std::string_view("group"),      std::string_view("sort"),
    std::string_view("where"),      std::string_view("map"),
    std::string_view("scan"),       std::string_view("match"),
    std::string_view("distinct"),   std::string_view("then"),
    std::string_view("log"),        std::string_view("logf"),
    std::string_view("mime"),       std::string_view("strformat"),
    std::string_view("chunk"),      std::string_view("collect"),
    std::string_view("consume"),    std::string_view("count"),
    std::string_view("sum"),        std::string_view("min"),
    std::string_view("max"),        std::string_view("avg"),
    std::string_view("fold"),       std::string_view("join"),
    std::string_view("text"),       std::string_view("json"),
    std::string_view("packb"),      std::string_view("timeout"),
    std::string_view("pace"),
};

const absl::flat_hash_map<std::string_view, StageArgument>& StageTable() {
  static const auto* table =
      new absl::flat_hash_map<std::string_view, StageArgument>{
          {"read_with", StageArgument::kExpression},
          {"ref", StageArgument::kOptionalExpression},
          {"take while", StageArgument::kExpression},
          {"take until", StageArgument::kExpression},
          {"first", StageArgument::kNumber},
          {"last", StageArgument::kNumber},
          {"drop", StageArgument::kNumber},
          {"truncate", StageArgument::kNumber},
          {"batch", StageArgument::kNumber},
          {"window", StageArgument::kNumber},
          {"chunk", StageArgument::kNumber},
          {"where", StageArgument::kExpression},
          {"context", StageArgument::kExpression},
          {"map", StageArgument::kExpression},
          {"group", StageArgument::kExpression},
          {"join", StageArgument::kOptionalString},
          {"strformat", StageArgument::kString},
          {"match", StageArgument::kString},
          {"mime", StageArgument::kString},
          {"then", StageArgument::kStream},
          {"log", StageArgument::kLog},
          {"logf", StageArgument::kLogFormat},
          {"collect", StageArgument::kNone},
          {"consume", StageArgument::kNone},
          {"count", StageArgument::kNone},
          {"distinct", StageArgument::kNone},
          {"flatten", StageArgument::kNone},
          {"sum", StageArgument::kOptionalExpression},
          {"min", StageArgument::kOptionalExpression},
          {"max", StageArgument::kOptionalExpression},
          {"avg", StageArgument::kOptionalExpression},
          {"sort", StageArgument::kSortKey},
          {"fold", StageArgument::kFold},
          {"scan", StageArgument::kFold},
          {"timeout", StageArgument::kDuration},
          {"pace", StageArgument::kDuration},
          {"text", StageArgument::kNone},
          {"json", StageArgument::kNone},
          {"packb", StageArgument::kNone},
      };
  return *table;
}

// What each stage does, as reference.
const absl::flat_hash_map<std::string_view, WordDoc>& StageDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"read_with",
       {"Read a node through an independent configured cursor.",
        "a reader option record",
        "The record overlays the reader defaults for this pipeline. Other "
        "subscriptions keep their own cursor and options. `offset` counts "
        "fragments, so a separate reader can inspect a later portion without "
        "advancing an earlier reader.",
        "page.text | read_with {offset: 2} -> selected\n\n"
        R"(Read a fragment window without changing other readers.
flow inspect-articles {
  in articles: string stream required
  out selected: string stream
  articles | read_with {offset: 1, max_chunks_to_read: 2} -> selected
})"}},
      {"ref",
       {"Write a reference to a direct source node.",
        "optionally a fragment window record",
        "The destination reads the source's retained fragments when needed. "
        "The referenced source must remain accessible to the reader.",
        "page.text | ref -> archive\n\n"
        R"(Forward a reference to retained findings for later reading.
flow retain-findings {
  in findings: string stream required
  out archive: string stream
  findings | ref -> archive
})"}},
      {"take while",
       {"Forwards the prefix for which the condition stays true.",
        "a condition with 'it' bound",
        "Evaluates one item at a time. The first false item and all later "
        "items "
        "are excluded. Ends this subscription without cancelling its producer. "
        "Predicate failures propagate unless the stage uses 'try'.",
        "lines | take while std::lower(it) != \"/exit\" -> work.input\n\n"
        R"(Stop forwarding rows at the first blank row.
flow valid-prefix {
  in rows: string stream required
  out valid: string stream
  rows | take while std::len(it) > 0 -> valid
})"}},
      {"take until",
       {"Forwards the prefix before the condition first becomes true.",
        "a condition with 'it' bound",
        "Evaluates one item at a time. The first true item and all later items "
        "are excluded. Ends this subscription without cancelling its producer. "
        "A tolerated predicate failure drops that item and keeps looking.",
        "lines | take until std::lower(it) == \"/exit\" -> work.input\n\n"
        R"(A sentinel ends the exposed event stream without being forwarded.
flow active-events {
  in events: string stream required
  out active: string stream
  events | take until it == "STOP" -> active
})"}},
      {"first",
       {"Emit at most the first `n` values.",
        "a scalar integer expression, evaluated once",
        "Stops this subscription after `n` values. The producer continues "
        "unless its own control port or cancellation mechanism stops it. "
        "`first 1` yields a stream with at most one value. `last` waits for "
        "the source to finish before selecting values.",
        "hits | first 3 -> shown\n\n"
        R"(Sort scores, retain the two leaders, and emit their names.
flow top-scores {
  out leaders: string stream
  [{name: "Ada", points: 91}, {name: "Bo", points: 98}, {name: "Cy", points: 86}]
    | flatten | sort by it.points desc | first 2 | map it.name -> leaders
})"}},
      {"last",
       {"Emit the final `n` values.",
        "a scalar integer expression, evaluated once",
        "Reads upstream to completion and retains at most `n` values. Output "
        "begins after the source ends. Use `first` for a prefix that can be "
        "emitted while upstream is still producing values.",
        "lines | last 20 -> tail\n\n"
        R"(Filter errors before selecting the last two.
flow trailing-errors {
  out recent: string stream
  ["ok", "error: disk", "ok", "error: network"]
    | flatten | where std::startswith(it, "error:") | last 2 -> recent
})"}},
      {"drop",
       {"Discard the first `n` values in this pipeline.",
        "a scalar integer expression, evaluated once",
        "Other readers retain their positions. `skip n port` advances the "
        "shared port for every reader.",
        "rows | drop 1 -> body\n\n"
        R"(Skip the first CSV row while preserving the data rows.
flow without-header {
  out rows: string stream
  ["name,age", "Ada,31", "Bo,29"] | flatten | drop 1 -> rows
})"}},
      {"truncate",
       {"Limit the size of each value.",
        "a scalar integer expression, evaluated once",
        "Retains at most `n` characters of text, bytes of a byte string, "
        "items of a list or keys of a record. Other types pass through. The "
        "number of stream values is unchanged.",
        "page.text | truncate 4000 -> brief.pages\n\n"
        R"(Each description is shortened independently before it is streamed.
flow bounded-previews {
  out previews: string stream
  ["A long description", "A short note"] | flatten
    | truncate 12 -> previews
})"}},
      {"batch",
       {"Emit consecutive groups of at most `n` values as lists.",
        "a scalar integer expression, evaluated once",
        "Each group is one output value. The final group may contain fewer "
        "than `n` values. `window` emits overlapping groups instead.",
        "samples | batch 100 -> frames\n\n"
        R"(Group five samples into consecutive batches of at most two.
flow minute-buckets {
  out buckets: list stream
  [10, 20, 15, 30, 25] | flatten | batch 2 -> buckets
})"}},
      {"window",
       {"Emit overlapping windows of `n` values.",
        "a scalar integer expression, evaluated once",
        "The first output follows the `n`th input. Each subsequent output "
        "replaces the oldest item. Memory is bounded by `n` values. Use "
        "`batch` for disjoint groups.",
        R"(lines | window 3 | map std::join(it, "\n") -> paragraphs

Compute a two-sample total as the window advances.
flow rolling-sums {
  out totals: int stream
  [80, 100, 120, 90] | flatten | window 2
    | map (it[0] + it[1]) -> totals
})"}},
      {"context",
       {"Matching excerpts extended by nearby matches.",
        "predicate before COUNT after COUNT [numbered [base EXPR]]",
        "Keeps preceding items, then emits a list after COUNT consecutive "
        "nonmatches. A match in that trailing window resets the count. EOF "
        "flushes a shorter excerpt. Counts are nonnegative signed 32-bit "
        "integers evaluated once outside it. Numbered items are {index, "
        "value}; "
        "indices count all source items from base (default 1). Base evaluates "
        "once and must be an integer. Storage grows with the active excerpt; "
        "bound endless sources upstream. Predicate failures propagate; try "
        "context treats them as nonmatches and reports them.",
        "lines | context std::contains(it, \"error\") before 3 after 5 "
        "numbered -> excerpts\n\n"
        R"(Retain one line before and two lines after an error marker.
flow log-context {
  out excerpts: list stream
  ["ready", "connected", "ERROR", "retrying", "done"]
    | flatten | context it == "ERROR" before 1 after 2 -> excerpts
})"}},
      {"group",
       {"Collect values until a predicate closes the group.",
        "an expression, with `it` bound to the value in hand",
        "The value satisfying the predicate is included in its group. A final "
        "incomplete group is emitted at end of stream. Use `batch` for fixed "
        "group sizes.",
        "words | group it.final -> sentences\n\n"
        R"(Close each paragraph at a final marker and count its entries.
flow paragraph-sizes {
  out sizes: int stream
  [{text: "A", final: false}, {text: "B", final: true}, {text: "C", final: true}]
    | flatten | group it.final | map std::len(it) -> sizes
})"}},
      {"where",
       {"Filter values by a predicate.",
        "an expression, with `it` bound to the value in hand",
        "Retained values preserve their bytes and metadata. `where` also "
        "accepts the infix form without `|`. `map` changes each retained "
        "value; `where` retains its original representation.",
        "hits | where it.score > 0.5 -> kept\n\n"
        R"(Keep expensive orders and project only their IDs.
flow expensive-orders {
  out ids: string stream
  [{id: "A", price: 25}, {id: "B", price: 5}]
    | flatten | where it.price >= 20 | map it.id -> ids
})"}},
      {"map",
       {"Transform each value with an expression.",
        "an expression, with `it` bound to the value in hand",
        "`map Shape{...}` and `map it as Shape` declare the output type for "
        "downstream field completion and validation. Other expressions "
        "produce values without a declared output type. Use `where` to remove "
        "values before mapping; a `map` expression runs once per input.",
        "hits | map Source{url: it.url} -> sources\n\n"
        R"(Parse log records, retain warnings, and emit their messages.
flow warning-lines {
  out messages: string stream
  ["INFO: ready", "WARN: cache low", "WARN: disk full"]
    | flatten | match "{level:word}: {message:line}"
    | where it.level == "WARN" | map it.message -> messages
})"}},
      {"log",
       {"Logs each value and passes it on unchanged.",
        "an optional level and expression with `it` bound",
        "Without an expression, logs the input value. The flow log requires "
        "no declared port and no reader.",
        "hits | log warning it.error | collect -> problems"}},
      {"logf",
       {"Logs a formatted line per value and passes the value on unchanged.",
        "optionally a level, a format, and the values to fill it with",
        "Uses the `strformat` conversion rules. `it` refers to the current "
        "value. Logging does not change the output stream.",
        "pages | logf \"fetched %s\" it.url -> fetched\n\n"
        R"(Log each URL while forwarding the original values.
flow logged-downloads {
  out urls: string stream
  ["/a", "/b"] | flatten
    | logf info "downloading %s" it -> urls
})"}},
      {"match",
       {"Extract named fields from matching stream values.", "a pattern",
        "Literal text matches itself. Whitespace runs match flexible spaces "
        "or tabs. `{name}` captures text; typed captures accept `int`, "
        "`number`, `bool`, `word`, `line`, `rest`, `duration`, `time`, and "
        "`json`. The pattern searches within each value. Unmatched values "
        "are dropped. A literal pattern exposes captured field names to "
        "completion. `std::match` returns null for one unmatched value.",
        "lines | match \"{level:word}: {message:line}\" | map it.level -> "
        "levels\n\n"
        R"(Parse log records, retain warnings, and emit their messages.
flow warning-lines {
  out messages: string stream
  ["INFO: ready", "WARN: cache low", "WARN: disk full"]
    | flatten | match "{level:word}: {message:line}"
    | where it.level == "WARN" | map it.message -> messages
})"}},
      {"distinct",
       {"Emit each distinct text representation once.", "",
        "Values are compared as text, including records and lists. Retains "
        "every representation seen until the stream ends. For an unbounded "
        "source, the set of previously seen values can grow without limit.",
        "urls | distinct -> once_each\n\n"
        R"(Remove repeated domains in first-seen order.
flow unique-domains {
  out domains: string stream
  ["a.example", "b.example", "a.example"]
    | flatten | distinct -> domains
})"}},
      {"then",
       {"Concatenate two streams in source order.", "another stream",
        "Reads the left stream to completion before reading the right. "
        "May be written without the leading `|`. Flow statements start "
        "concurrently, so statement position does not order independent "
        "writes; `then` orders values at this read.",
        "history then asked -> prompt\n\n"
        R"(Emit instructions before requests even when sources run concurrently.
flow ordered-prompt {
  in instructions: string stream required
  in request: string stream required
  out prompt: string stream
  instructions then request -> prompt
})"}},
      {"mime",
       {"Filter values by their stored media type.",
        "a media type, optionally ending in `*`",
        "Matches chunk metadata without decoding the payload.",
        "parts | mime \"text/*\" -> readable"}},
      {"strformat",
       {"Formats each value into a string.", "a format string",
        "Equivalent to `| map strformat(\"...\", it)`. Supports `%s`, numeric "
        "conversions such as `%d` and `%.2f`, and `%%` for a literal percent "
        "sign. Use `std::strformat` to format multiple arguments.",
        "elapsed | strformat \"took %s\" -> log\n\n"
        R"(Prefix each state with a stable label.
flow formatted-states {
  out states: string stream
  ["ready", "busy"] | flatten | strformat "state=%s" -> states
})"}},
      {"chunk",
       {"Split text or bytes into values of at most `n` bytes.",
        "a scalar integer size in bytes, evaluated once",
        "Text splits only at character boundaries. Other value types pass "
        "through unchanged.",
        "body | chunk 65536 -> upload.parts\n\n"
        R"(Split a long string into bounded text fragments.
flow utf8-parts {
  out parts: string stream
  ["abcdefghij"] | flatten | chunk 4 -> parts
})"}},
      {"collect",
       {"Collect the complete stream into one list.", "",
        "Reads upstream to completion and emits exactly one list, including "
        "an empty list for an empty stream. Subsequent stages receive one "
        "value. `join` produces one string instead; `count` produces one "
        "integer without retaining the values.",
        "hits | collect -> all\n\n"
        R"(Collect text fragments, then join the resulting list.
flow page-summary {
  out summary: string
  ["one", "two", "three"] | flatten | collect
    | map std::join(it, ", ") -> summary
})"}},
      {"consume",
       {"Exactly one complete value, checked before it is yielded.", "",
        "An empty stream or a second item fails with FAILED_PRECONDITION. "
        "Waits for normal termination after the sole value; upstream errors "
        "retain their status. A null value counts as an item.",
        "result | consume -> answer\n\n"
        R"(Require exactly one accepted result.
flow sole-result {
  out answer: string
  ["accepted"] | flatten | consume -> answer
})"}},
      {"count",
       {"Count stream values.", "",
        "Reads upstream to completion and emits one integer. Does not decode "
        "value payloads. `collect` retains the values when downstream work "
        "needs them.",
        "pages | count -> how_many\n\n"
        R"(Count emitted records without collecting their contents.
flow counted {
  out count: int
  ["one", "two", "three"] | flatten | count -> count
})"}},
      {"sum",
       {"Sum stream values or projected fields.",
        "an optional expression over each value",
        "Reads upstream to completion and emits one value. Numbers and "
        "durations retain their respective types. An empty stream sums to "
        "zero. `sum it.price` is equivalent to `map it.price | sum`.",
        "orders | sum it.price -> revenue\n\n"
        R"(Project each order price and sum the revenue.
flow revenue {
  out total: int
  [{price: 19}, {price: 8}, {price: 13}]
    | flatten | sum it.price -> total
})"}},
      {"min",
       {"Return the minimum stream value or projected field.",
        "an optional expression over each value",
        "Uses the `<` comparison. Emits no value for an empty stream.",
        "samples | min it.latency -> fastest\n\n"
        R"(Two reductions expose the shortest and longest samples.
flow duration-range {
  out shortest: int
  out longest: int
  [8, 2, 5] | flatten | min -> shortest
  [8, 2, 5] | flatten | max -> longest
})"}},
      {"max",
       {"Return the maximum stream value or projected field.",
        "an optional expression over each value",
        "Uses the `>` comparison. Emits no value for an empty stream.",
        "samples | max it.latency -> slowest\n\n"
        R"(Two reductions expose the shortest and longest samples.
flow duration-range {
  out shortest: int
  out longest: int
  [8, 2, 5] | flatten | min -> shortest
  [8, 2, 5] | flatten | max -> longest
})"}},
      {"avg",
       {"Compute the mean of stream values or projected fields.",
        "an optional expression over each value",
        "Reads upstream to completion. Emits no value for an empty stream. "
        "Duration inputs produce a duration mean.",
        "runs | avg it.elapsed -> typical\n\n"
        R"(Three latency samples reduce to one mean.
flow average-latency {
  out typical: number
  [120, 80, 100] | flatten | avg -> typical
})"}},
      {"fold",
       {"Reduce the stream to one accumulated value.",
        "an initial literal, an accumulator name and an expression",
        "In `fold 0 as total, total + it`, `total` is the previous result and "
        "`it` is the current input. Emits the final accumulator after the "
        "stream ends. `scan` emits each intermediate accumulator instead. "
        "Both read values in source order.",
        "orders | fold 0 as total, total + it.price -> revenue\n\n"
        R"(Accumulate invoice line amounts into one final total.
flow invoice-total {
  out total: int
  [19, 8, 13] | flatten
    | fold 0 as subtotal, subtotal + it -> total
})"}},
      {"scan",
       {"Emit each intermediate accumulator value.",
        "an initial literal, an accumulator name and an expression",
        "Uses the same accumulation form as `fold` and emits one result per "
        "input value. State may be a record. The stage retains only the "
        "current accumulator. `fold` emits only the final state. A downstream "
        "`first` stops this subscription before later updates are computed.",
        "lines | scan 0 as n, n + 1 -> numbered\n\n"
        R"(Emit the balance after each delta, including a withdrawal.
flow running-total {
  out balances: int stream
  [15, -4, 20] | flatten
    | scan 0 as balance, balance + it -> balances
})"}},
      {"sort",
       {"Sort stream values.", "an optional `by` expression and `desc`",
        "Reads upstream to completion before emitting. `by` selects the "
        "comparison key; `desc` reverses the order. Uses `<` comparison. "
        "A following `first` limits the result, but does not make sorting "
        "incremental.",
        "hits | sort by it.score desc | first 10 -> best\n\n"
        R"(Sort scores, retain the two leaders, and emit their names.
flow top-scores {
  out leaders: string stream
  [{name: "Ada", points: 91}, {name: "Bo", points: 98}, {name: "Cy", points: 86}]
    | flatten | sort by it.points desc | first 2 | map it.name -> leaders
})"}},
      {"flatten",
       {"Emit list elements as separate stream values.", "",
        "Preserves element order. Non-list values pass through as one value.",
        "pages | map it.lines | flatten -> lines\n\n"
        R"(Flatten a list of lists into individual lines.
flow nested-lines {
  out lines: string stream
  [["alpha"], ["beta", "gamma"]]
    | flatten | flatten -> lines
})"}},
      {"timeout",
       {"Fail when the interval between stream values exceeds a limit.",
        "a duration",
        "The timer resets after each value. Expiration yields "
        "`deadline_exceeded`. `wait ... timeout` bounds completion of a "
        "subject.",
        "tokens | timeout 30s -> answer\n\n"
        R"(A feed fails when the gap between messages exceeds two seconds.
flow bounded-feed {
  in messages: string stream required
  out safe: string stream
  messages | timeout 2s -> safe
})"}},
      {"pace",
       {"Limit emission to one value per interval.",
        "the least time between two values",
        "No values are dropped. Backpressure delays upstream production when "
        "buffers fill.",
        "requests | pace 100ms -> to_api\n\n"
        R"(Space requests by at least 100 ms while retaining every request.
flow paced-feed {
  in requests: string stream required
  out paced: string stream
  requests | pace 100ms -> paced
})"}},
      {"join",
       {"Join stream values into one string.", "an optional separator string",
        "Reads upstream to completion and renders each value as text. "
        "`std::join` operates on a list value. `collect | map std::join(it, "
        "separator)` retains the intermediate list; the stage does not.",
        "words | join \", \" -> sentence\n\n"
        R"(Turn three tokens into a single status line.
flow joined {
  out sentence: string
  ["retries", "exceeded", "limit"] | flatten | join " " -> sentence
})"}},
      {"text",
       {"Render each stream value as text.", "",
        "Preserves the number of values. `std::text` renders one value.",
        "numbers | text -> labels"}},
      {"json",
       {"Encode each stream value as JSON.", "",
        "Values containing bytes cannot be represented as JSON. The "
        "compiler reports a known incompatible type before execution. "
        "`packb` supports bytes.",
        "records | json -> payload"}},
      {"packb",
       {"Encode stream values as MessagePack.", "",
        "Already packed values pass through with their metadata intact. "
        "MessagePack supports byte strings.",
        "clips | packb -> transcribe.audio"}},
  };
  return *table;
}

// The fixed function set, as reference. Same house style as [StageDocs], and
// the same rule about accuracy: `Call` in `values.cc` is the implementation.
const absl::flat_hash_map<std::string_view, WordDoc>& BuiltinDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"ref",
       {"Make a typed reference to a direct node or port.",
        "a node source and optional destination, window, or credentials",
        "The source stays unread; a later value read follows its retained "
        "fragments.",
        "ref(page.text, for: archive, offset: 2, length: 5)"}},
      {"integer",
       {"Convert a value to an integer.", "one value",
        "Conversion fails when the value cannot be represented as an integer. "
        "`number` accepts decimal text and returns zero for non-numeric text; "
        "`integer` is appropriate when invalid input must fail.",
        "integer(\"12\")\n\n"
        R"(Read a retry header with a fallback, then require an integer.
flow parsed-header {
  out retries: int
  header = {retries: "3"}
  std::integer(std::get(header, "retries", "0")) -> retries
})"}},
      {"int",
       {"Convert a value to an integer.", "one value",
        "Alias of `integer`. Fractional values are rejected.", "int(\"12\")"}},
      {"string",
       {"Render a value as text.", "one value",
        "See `text` for rendering rules.", "string(12)"}},
      {"bytes",
       {"Convert text to UTF-8 bytes or retain a byte string.", "one value",
        "Non-text values are rendered before encoding.", "bytes(\"hello\")"}},
      {"object",
       {"Require an object value with string keys.", "one value",
        "Conversion fails for values without object structure.",
        "object({\"answer\": 42})"}},
      {"list",
       {"Convert a value to a list; a scalar becomes one item.", "one value",
        "An existing list retains its elements. Scalars produce a singleton "
        "list.",
        "list([1, 2])"}},
      {"status",
       {"Convert a status record while preserving its code, message and "
        "details.",
        "one value",
        "Accepts a status value or a compatible record. Details retain their "
        "structured representation.",
        "std::status({\"code\": \"not_found\", \"message\": \"missing\"})"}},
      {"null",
       {"Require an explicit null value.", "one value",
        "An absent option is distinct from a present null value.",
        "null(null)"}},
      {"parse_duration",
       {"Parse duration text or fail.", "one text value",
        "Accepts the same units as duration(), including compound units and "
        "bare seconds. Invalid text fails instead of falling back to zero. "
        "`duration` performs the permissive conversion; `seconds` converts "
        "a duration to a number.",
        "std::parse_duration(\"1m30s\")\n\n"
        R"(Parse a strict duration budget and expose its numeric seconds.
flow deadline-budget {
  out seconds: number
  budget = std::parse_duration("1m30s")
  std::seconds(budget) -> seconds
})"}},
      {"parse_time",
       {"Parse time text or fail.", "one text value",
        "Accepts RFC 3339 instants and UTC calendar dates. Invalid text fails "
        "instead of falling back to the epoch.",
        "std::parse_time(\"2026-10-30T12:00:00Z\")"}},
      {"boolean",
       {"Convert a value to bool using Flow truth testing.", "one value",
        "Empty strings, lists and records, zero, false and null are false.",
        "std::boolean(1)"}},
      {"array",
       {"Convert a value to a list; a scalar becomes one item.", "one value",
        "Alias of `list`; an existing list retains its elements.",
        "std::array(1)"}},
      {"any",
       {"Retain a value without imposing a payload type.", "one value",
        "No conversion or type validation is applied.", "std::any(null)"}},
      {"json",
       {"Convert a value to its JSON representation.", "one value",
        "Bytes require a binary representation such as `packb`.",
        "std::json({answer: 42})"}},
      {"option",
       {"Validate an explicit presence and payload record.", "one value",
        "Validate an explicit presence and payload record.",
        "std::option({has_value: true, value: 42})"}},
      {"len",
       {"Return a value's length.", "one value",
        "Counts characters in text, bytes in a byte string, elements in a "
        "list and keys in a record. Other values return zero.",
        "if std::len(report) < 12 { fail invalid_argument \"too short\" }"}},
      {"lower",
       {"Convert ASCII letters to lowercase.", "one value",
        "Renders the value as text. Non-ASCII characters are unchanged. Use "
        "`trim` first when leading or trailing whitespace is insignificant.",
        "std::lower(header.value) == \"application/json\"\n\n"
        R"(Trim surrounding whitespace before applying ASCII lowercase.
flow normalized-label {
  out label: string
  raw = "  Critical  "
  std::lower(std::trim(raw)) -> label
})"}},
      {"upper",
       {"Convert ASCII letters to uppercase.", "one value",
        "Renders the value as text. Non-ASCII characters are unchanged.",
        "std::upper(code) -> shouted"}},
      {"trim",
       {"Remove leading and trailing whitespace from text.", "one value",
        "Removes spaces, tabs and line breaks. Internal whitespace remains. "
        "`lower` and `upper` change ASCII case without trimming.",
        "std::trim(page.title) -> title\n\n"
        R"(Trim surrounding whitespace before applying ASCII lowercase.
flow normalized-label {
  out label: string
  raw = "  Critical  "
  std::lower(std::trim(raw)) -> label
})"}},
      {"text",
       {"A value as text.", "one value",
        "Strings pass through. Numbers, booleans, durations and instants use "
        "their Flow text forms. Records and lists render as JSON. The `text` "
        "stage applies this conversion to every stream value.",
        "std::text(count) -> label"}},
      {"number",
       {"Convert a value to a number.", "one value",
        "Integer text yields an integer; decimal text yields a floating-point "
        "number. Booleans become 0 or 1, durations become seconds, and times "
        "become Unix seconds. Non-numeric text yields zero.",
        "number(header.retries) > 3"}},
      {"bool",
       {"Apply Flow truth testing.", "one value",
        "Null, `false`, 0, an empty string, an empty list and an empty record "
        "are false. Other values are true. `if` uses the same rule.",
        "bool(page.body) -> anything_there"}},
      {"keys",
       {"The keys of a record, sorted, as a list of strings.", "one record",
        "Ordering is stable. Non-record values yield an empty list. "
        "`values` returns record values in the same sorted-key order.",
        "std::keys(headers) | join \", \" -> named\n\n"
        R"(List record keys in deterministic order for display.
flow names-of-fields {
  out names: string
  record = {team: "ops", owner: "Ada"}
  std::join(std::keys(record), ", ") -> names
})"}},
      {"values",
       {"The values of a record, in the order their keys sort.", "one value",
        "Lists pass through unchanged. Record values follow sorted key order.",
        "std::values(scores) | collect -> all"}},
      {"get",
       {"Read a field or indexed item with a fallback.",
        "the value, the key, and what to use instead",
        "Record keys are strings; list indices are integers and may count "
        "backward from the end. Missing or null results use the fallback. "
        "`default` also substitutes empty strings, lists and records.",
        R"(std::get(page.meta, "title", "untitled") -> title

Treat an empty title as absent after reading optional metadata.
flow fallback-title {
  out title: string
  metadata = {title: ""}
  std::default(std::get(metadata, "title", ""), "Untitled") -> title
})"}},
      {"join",
       {"Join list elements into one string.", "the list, and a separator",
        "Renders each element as text. A non-list value is rendered as one "
        "string. The `join` stage operates on a stream. After `collect`, "
        "`std::join(it, separator)` receives the complete list as one value.",
        "std::join(std::keys(headers), \", \") -> named\n\n"
        R"(Collect text fragments, then join the resulting list.
flow page-summary {
  out summary: string
  ["one", "two", "three"] | flatten | collect
    | map std::join(it, ", ") -> summary
})"}},
      {"split",
       {"Split text into a list.", "the text, and a separator",
        "Without a separator, splits on whitespace runs and omits empty "
        "fields. For comma-separated text this function does not implement "
        "CSV quoting or escaping.",
        "std::split(line, \",\") -> fields\n\n"
        R"(Extract the first field from simple comma-delimited rows.
flow csv-first-field {
  out names: string stream
  ["Ada,ops", "Bo,infra"] | flatten
    | map std::split(it, ",")[0] -> names
})"}},
      {"merge",
       {"Merge records.", "two or more records",
        "Later arguments overwrite duplicate keys. Non-record arguments are "
        "ignored, including null. Use `get` to read an optional field from "
        "the resulting record.",
        "std::merge(defaults, page.meta) -> settings\n\n"
        R"(Overlay request options on defaults before reading the endpoint.
flow sanitized-options {
  out endpoint: string
  defaults = {endpoint: "https://api.example", timeout: "5s"}
  overrides = {timeout: "2s"}
  options = std::merge(defaults, overrides)
  std::get(options, "endpoint", "") -> endpoint
})"}},
      {"contains",
       {"Test whether a value contains a member.", "a container and a member",
        "Matches a text substring, a list element, or a record key. Other "
        "container types return false.",
        "std::contains(page.text, \"error\")"}},
      {"startswith",
       {"Test whether text begins with a prefix.",
        "a value and one prefix or a list of prefixes",
        "Renders the value as text. A list succeeds when any prefix matches.",
        R"(std::startswith(part.mime, ["text/", "application/json"]))"}},
      {"endswith",
       {"Test whether text ends with a suffix.",
        "a value and one suffix or a list of suffixes",
        "Renders the value as text. A list succeeds when any suffix matches.",
        R"(std::endswith(piece, [".", "?", "!"]))"}},
      {"match",
       {"Extract named fields from text or return null.",
        "a pattern and a text value",
        "Uses the same pattern syntax as the `match` stage. `{name}` "
        "captures text; `{name:int}` converts the capture to an integer. "
        "Literal patterns expose captured fields to completion. The `match` "
        "stage drops unmatched stream values; this function returns null for "
        "one unmatched value.",
        "who = std::match(\"name={name}\", line)\n\n"
        R"(Extract a typed status code from a request log line.
flow named-capture {
  out code: int
  result = std::match("status={code:int}", "GET /health status=503")
  result.code -> code
})"}},
      {"replace",
       {"Replace every occurrence of a text substring.",
        "a value, a search string and a replacement string",
        "Search text is literal; `.` and `*` have no pattern meaning. An "
        "empty search string leaves the value unchanged.",
        R"(std::replace(path, "\\", "/") -> posix)"}},
      {"slice",
       {"Extract a range from text, bytes, or a list.",
        "a value, start index and optional end index",
        "Negative indices count from the end. An omitted end index includes "
        "the remaining elements. `truncate` applies a size limit to every "
        "value in a stream; `slice` selects a range from one value.",
        "std::slice(page.text, 0, 200) -> preview\n\n"
        R"(Take a fixed prefix from each title.
flow title-previews {
  out previews: string stream
  ["Distributed systems", "Observability"] | flatten
    | map std::slice(it, 0, 11) -> previews
})"}},
      {"default",
       {"Substitute a value when the first is empty.",
        "the value, and what to use instead",
        "Null and empty strings, lists and records use the substitute. Zero "
        "and false remain unchanged. `get` only substitutes a missing or null "
        "field; compose them when an empty field also needs a fallback.",
        "std::default(header.locale, \"en\") -> locale\n\n"
        R"(Treat an empty title as absent after reading optional metadata.
flow fallback-title {
  out title: string
  metadata = {title: ""}
  std::default(std::get(metadata, "title", ""), "Untitled") -> title
})"}},
      {"to_chunk",
       {"Encode a value as a chunk with a media type.",
        "a value and media type",
        "The media type may name a registered value type, as in "
        "`\"application/json;type=a11.sdk.Interaction\"`.",
        R"(std::to_chunk({"type": "text", "text": said}, "application/json"))"}},
      {"from_chunk",
       {"Decode a chunk's value.", "one chunk or decoded value",
        "A value that is already decoded passes through unchanged.",
        "std::from_chunk(it) -> decoded"}},
      {"strformat",
       {"A string with values written into it.",
        "a format string, and the values it names",
        "printf's conversions: `%s` for anything, `%d` and `%.2f` for numbers, "
        "`%x` for hexadecimal, `%%` for a literal per cent. `%2$s` picks an "
        "argument by position, and a duration or an instant may carry its own "
        "format in parentheses, as in `%(%H:%M)s`. A conversion with no "
        "argument for it is left in the output as written, because a visible "
        "`%3$s` is easier to diagnose than a flow that died formatting a log "
        "line. The `strformat` stage formats each stream value with one "
        "template; this function accepts multiple arguments.",
        "std::strformat(\"%s took %s\", name, elapsed) -> line\n\n"
        R"(Format two counters into a compact report line.
flow formatted-report {
  out report: string
  requests = 35
  failures = 2
  std::strformat("%d requests; %d failed", requests, failures) -> report
})"}},
      {"b64encode",
       {"A value as base64 text.", "one value",
        "Produces padded text suitable for a JSON field or header.",
        "std::b64encode(page.bytes) -> encoded"}},
      {"b64decode",
       {"Decode base64 text to bytes.", "one value",
        "Padding is optional. Invalid input fails with `invalid_argument`. "
        "Use `utf8decode` to interpret decoded UTF-8 bytes as text.",
        "std::b64decode(header.token) -> raw"}},
      {"b64urlencode",
       {"Encode a value with the URL-safe base64 alphabet.", "one value",
        "Uses `-` and `_` in place of `+` and `/`. `b64urldecode` reverses "
        "this encoding; `utf8encode` makes a text-to-bytes conversion "
        "explicit.",
        "std::b64urlencode(claims) -> segment\n\n"
        R"(Encode a text label as a URL-safe base64 segment.
flow encoded-label {
  out encoded: string
  ["project/blue"] | flatten
    | map std::b64urlencode(std::utf8encode(it)) -> encoded
})"}},
      {"b64urldecode",
       {"Decode URL-safe base64 text to bytes.", "one value",
        "Padding is optional. The result is bytes; `utf8decode` validates "
        "and converts UTF-8 text.",
        "std::b64urldecode(segment) -> claims\n\n"
        R"(Decode a URL-safe base64 segment and validate its UTF-8 payload.
flow decoded-label {
  out label: string
  ["cHJvamVjdC9ibHVl"] | flatten
    | map std::utf8decode(std::b64urldecode(it)) -> label
})"}},
      {"utf8encode",
       {"Text encoded as UTF-8 bytes.", "one value",
        "Produces bytes suitable for an application/octet-stream input. The "
        "textual representation of a non-text value is encoded. "
        "`utf8decode` validates bytes before interpreting them as text.",
        "std::utf8encode(request.text) -> body\n\n"
        R"(Encode a text label as a URL-safe base64 segment.
flow encoded-label {
  out encoded: string
  ["project/blue"] | flatten
    | map std::b64urlencode(std::utf8encode(it)) -> encoded
})"}},
      {"utf8decode",
       {"UTF-8 bytes decoded as text.", "one value",
        "Produces text suitable for a text/plain input. Malformed UTF-8 ends "
        "the flow with invalid_argument instead of replacing bytes. "
        "`b64decode` and `b64urldecode` also produce bytes.",
        "std::utf8decode(response.body) -> std::text\n\n"
        R"(Decode a URL-safe base64 segment and validate its UTF-8 payload.
flow decoded-label {
  out label: string
  ["cHJvamVjdC9ibHVl"] | flatten
    | map std::utf8decode(std::b64urldecode(it)) -> label
})"}},
      {"now",
       {"Return the current instant.", "",
        "Each call reads the clock separately. Bind the result once when "
        "several expressions must use the same instant. Flow statements start "
        "concurrently; statement order alone does not make one timestamp "
        "follow another operation. Use `after` or `wait` for that dependency.",
        "started = std::now()\n\n"
        R"(Bind one clock reading and reuse it in two outputs.
flow stamped {
  out started: string
  out line: string
  clock = std::now()
  std::text(clock) -> started
  std::strformat("started at %s", clock) -> line
})"}},
      {"duration",
       {"Convert a value to a duration.", "one value",
        "Accepts unit suffixes such as `ms`, `s`, and `h`, including compound "
        "forms such as `1m30s`. A bare number represents seconds.",
        "std::duration(header.budget) -> budget"}},
      {"time",
       {"Convert a value to an instant.", "one value",
        "Accepts RFC 3339 timestamps and calendar dates. A missing time zone "
        "is interpreted as UTC.",
        "std::time(page.published) -> published"}},
      {"seconds",
       {"Convert a duration to numeric seconds.", "one duration",
        "Returns a floating-point number, preserving subsecond values. "
        "`parse_duration` validates text before conversion.",
        "std::seconds(std::now() - started) -> elapsed\n\n"
        R"(Parse a strict duration budget and expose its numeric seconds.
flow deadline-budget {
  out seconds: number
  budget = std::parse_duration("1m30s")
  std::seconds(budget) -> seconds
})"}},
  };
  return *table;
}

// --- the rest of the language -----------------------------------------------
// Everything below documents the forms an editor

constexpr WordDoc kStreamDoc = {
    "Marks a port as multi-valued.", "",
    "A port without `stream` accepts at most one value; a second value fails "
    "with `invalid_argument`. Unary ports can be shared between readers.",
    "out briefs: string stream \"One instruction per investigation.\""};

constexpr WordDoc kRequiredDoc = {
    "Requires a port value or struct field.", "",
    "A missing required port fails before execution. A missing required "
    "struct field fails validation. Optional ports may be empty; `try take` "
    "reports absence through `has_value`.",
    "in topic: string required \"What to research.\""};

// The two forms written as two words. Each is documented under its full
// spelling, which is how the ordered lists carry it, and under each word on its
// own, which is what a hover on one of them asks for.
constexpr WordDoc kOneOfDoc = {
    "Constrains a field to listed literals.", "a list of literals",
    "Values outside the list fail struct validation. The alternatives are "
    "published in the field schema for completion.",
    R"(role: string one of ["user", "assistant"])"};

constexpr WordDoc kForwardHeadersDoc = {
    "Forwards selected incoming headers to a step.",
    "one or more names, `*` matching a family",
    "`x-a11-` headers propagate automatically. This clause forwards other "
    "headers, including wildcard families. Explicit `with` values take "
    "precedence.",
    R"(llm = run ask_model(..) forward headers "x-tenant", "x-trace-*")"};

constexpr WordDoc kNodesDoc = {
    "Declares a local node map.", "a name, and optionally a block",
    "Nodes created in the block and steps using `via` retain streams in the "
    "local map. The compiler reports maps with no member nodes or steps.",
    "nodes fetched { for hit in search.hits { .. } }"};

const absl::flat_hash_map<std::string_view, WordDoc>& StatementDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"run",
       {"Starts a locally registered action.",
        "an action, its arguments in parentheses, and any modifiers",
        "Requires a local handler or a flow declared in the same file. The "
        "step's nodes stay local. A bound step exposes outputs through "
        "`step.port` and its outcome through `status step`. Pipeline form "
        "requires one unbound input and one output.",
        "search = run web-search(query: question, limit: 3)"}},
      {"call",
       {"Dispatches an action to the connected peer.",
        "an action, its arguments in parentheses, and any modifiers",
        "The peer resolves the action. Its nodes cross the transport unless a "
        "`nodes` block or `via` places them locally. Pipeline form requires a "
        "known interface with one unbound input and one output.",
        "said = call text-upper(text: words)"}},
      {"try",
       {"Contains a step or block failure.", "a `run`, a `call`, or a block",
        "A failure leaves the step's output ports empty and remains available "
        "through `wait step` or `status step`. The compiler reports an unread "
        "`try` status.",
        "page = try run web-fetch(url: hit.url)"}},
      {"take",
       {"Reads the next item from a stream.", "a stream",
        "Successive reads use source order. End of stream fails; 'try take' "
        "returns an absent option. Upstream failures still propagate.",
        "value = take rows"}},
      {"skip",
       {"Drops values from the front of a shared stream.",
        "a positive count and a port or node",
        "`skip n port` removes the port's first `n` values for every reader, "
        "such as removing a shared header row. Several statements naming one "
        "port add up. Unbound call outputs are drained automatically. Use "
        "`| drop n` to trim one pipeline only.",
        "skip 1 rows"}},
      {"wait",
       {"Waits for a step, node, port or barrier to finish.",
        "a subject, and optionally `timeout`",
        "Binding the result captures a status record. `wait all` forms a "
        "barrier over preceding work in the current scope and accepts no "
        "modifiers or value binding. A repeated wait for the same subject in "
        "one body is diagnosed.",
        "done = wait llm timeout 30s"}},
      {"close",
       {"Finish a node and drain its remaining values.", "a node",
        "Marks the node final, closes its writer, and drains remaining values. "
        "A bound name receives the completion status. Use `abort` for a failed "
        "node.",
        "ended = close findings"}},
      {"abort",
       {"End a node with a failure status.",
        "a node and an optional code and message",
        "Readers receive the failure status. The code and message use the "
        "same form as `fail`. Data-dependent arguments synchronize the "
        "statement; constant arguments require `if`, a loop body, or `after`.",
        "if not status page.ok { abort findings unavailable \"gone\" }"}},
      {"cancel",
       {"Request cancellation of a step.", "a step",
        "The request does not wait for completion. Use `if`, a loop body, or "
        "`after` to control when it executes. Cancelling a step already "
        "awaited in the same body is diagnosed.",
        "if code == 429 { cancel fetch }"}},
      {"fail",
       {"End the flow with a status.", "a code or status and optional message",
        "Accepts a canonical code name, its numeric value, or a status "
        "record. Data-dependent arguments synchronize the statement; "
        "constant arguments require `if`, a loop body, or `after`.",
        "fail invalid_argument \"the topic is empty\""}},
      {"log",
       {"Writes a line to the flow's log.",
        "optionally a level, and what to log",
        "The log requires no declared port or manual drain, and is created "
        "only when used. Reads its argument before logging. A data-dependent "
        "argument synchronizes it; constant-only arguments need an `if`, "
        "a loop body, or an `after`.",
        "log \"searching\" after plan"}},
      {"logf",
       {"Writes a formatted line to the flow's log.",
        "optionally a level, a format, and the values to fill it with",
        "The format is `strformat`'s. Reads its arguments before logging. "
        "Data-dependent arguments synchronize it; constant-only arguments "
        "need an `if`, a loop body, or an `after`.",
        "logf \"found %s results\" count after search"}},
      {"if",
       {"Runs a conditional scope, or binds a present option payload.",
        "a condition or name = option, a block, and optionally else",
        "Boolean conditions use ordinary truthiness. Optional binding "
        "evaluates "
        "its option once and binds the payload only in the success branch. "
        "Presence includes null, false, zero and empty values. EOF is absence; "
        "upstream errors and cancellation remain failures. The binding is "
        "branch-local and cannot shadow an enclosing name. Else-if "
        "initializers run only when earlier branches were not selected.",
        "if row = try take rows { row.name -> names }"}},
      {"for",
       {"Execute a block for each source value.",
        "one or more names, a stream or list literal, and optionally "
        "`parallel`",
        "Multiple names destructure tuple values, including `zip` output. "
        "A stream of lists supplies one list per pass; a bare list literal "
        "supplies its elements. Use `flatten` to expand list values from a "
        "stream. `until` and `while` can stop iteration early. A bound loop "
        "name exposes its completion status.",
        "for hit in search.hits parallel 2 { .. }"}},
      {"repeat",
       {"Repeat a block with state carried between passes.",
        "a name, its initial value, and an optional `max` count",
        "`name <- source` sets the next pass's state. Requires an `until` or "
        "`while` condition, a `max` count, or both. A bound loop name exposes "
        "its completion status.",
        "repeat asked = question max 4 { .. }"}},
      {"until",
       {"Ends a `repeat` or a `for` when a condition holds.", "a condition",
        "Evaluated after each pass. In a `for`, stops reading the source "
        "without cancelling its producer. `while` continues while its "
        "condition holds.",
        "until std::len(answer) > 0"}},
      {"while",
       {"Keeps a `repeat` or a `for` going while a condition holds.",
        "a condition",
        "Evaluated after each pass, so the body executes at least once.",
        "while not found"}},
      {"nodes", kNodesDoc},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& DeclarationDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"mod",
       {"Declares a lexical module.", "a name and a declaration block",
        "Modules are private by default. The exported attribute exposes the "
        "module path; its public members need their own exports.",
        "mod tools exported { flow work exported {} }"}},
      {"export",
       {"Publishes an accessible existing symbol.",
        "a qualified symbol and optional as alias",
        "The public alias retains the source identity and implementation. "
        "Exporting a module exposes its public view.",
        "export tools::work as search"}},
      {"declare",
       {"Declares a callable interface without an implementation.",
        "flow name { interface }",
        "Contains descriptions, headers, inputs and outputs. Implementations "
        "are resolved before execution.",
        "declare flow render { in data: object }"}},
      {"exported",
       {"Makes a module declaration visible to importers.",
        "after a declaration name",
        "Unattributed declarations remain private to their module.",
        "flow render exported { }"}},
      {"import",
       {"Binds a source module to a lexical namespace.",
        "a source string and 'as name'",
        "Supported at module level and directly inside implemented flows. "
        "Imports resolve before execution.",
        "import \"./render.flow\" as rendering"}},
      {"from",
       {"Imports selected public symbols into the current lexical scope.",
        "a source string, 'import', and names with optional 'as' aliases",
        "A same-name callable declaration constrains the imported symbol. "
        "Use aliases to keep distinct symbols under separate names.",
        "from \"./render.flow\" import render, Report as RenderReport"}},
      {"flow",
       {"Declares an executable composition of actions.", "a name, and a block",
        "The declared name is callable from a peer or another flow. `in` and "
        "`out` ports form its public action interface; intermediate values "
        "remain internal.",
        "flow answer-from-the-web { .. }"}},
      {"struct",
       {"Declares a record type with named fields.",
        "a name, and a block of fields",
        "Fields may declare types, defaults and constraints. A local struct "
        "takes precedence over a serial tag of the same name. Unused structs "
        "are diagnosed.",
        "struct Finding { url: string required }"}},
      {"describe",
       {"Documents a flow or struct.", "a string",
        "The text appears in action and type descriptions. Triple-quoted "
        "strings preserve line breaks and source indentation.",
        "describe \"Search, read the best hits, and answer from them.\""}},
      {"in",
       {"Declares an input port.",
        "a name, a type, any of `stream`/`required`, and a description",
        "A port without `stream` accepts one value. The description is "
        "published with the action schema.",
        "in question: string required \"What to find out.\""}},
      {"out",
       {"Declares an output port.",
        "a name, a type, any of `stream`/`required`, and a description",
        "Output values are visible to the caller. The compiler diagnoses "
        "declared outputs with no writer. Use `stream` when the port emits "
        "multiple values.",
        "out answer: string \"The answer, as it is written.\""}},
      {"header",
       {"Bind an incoming header to a Flow name.",
        "a header name, `as` alias, and optional `default`",
        "The alias can be read in expressions. `x-a11-` headers propagate to "
        "steps automatically. An unused alias is diagnosed.",
        R"(header "x-a11-locale" as locale default "en")"}},
      {"as",
       {"Bind an alias or convert a value to a named type.",
        "an alias, or a type",
        "`header ... as name` binds a header value. `value as Type` "
        "validates and converts a value to `Type`; it can be used where "
        "`Type{...}` conflicts with block syntax.",
        R"({...it, "role": "user"} as a11::sdk::Interaction)"}},
      {"default",
       {"Supply a missing header or struct field value.", "a literal",
        "Header defaults are evaluated before the flow body begins. Struct "
        "field defaults apply during value validation.",
        R"(header "x-a11-locale" as locale default "en")"}},
      // `stream` and `required` are in [OrderedDeclarations] because a port
      // declaration is where they are offered, but they are documented in
      // [PortModifierDocs] rather than here:
      {"node",
       {"Create a flow-owned node.",
        "optionally an id, and optionally `in` a map",
        "Several statements may write to the node. Readers receive values "
        "as they arrive. `n.id` exposes its ID to external callers.",
        "findings = node()"}},
      {"nodes", kNodesDoc},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& ClauseDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"else",
       {"Execute a block when the preceding `if` condition is false.",
        "a block",
        "Statements outside the conditional block remain independent.",
        "if ok { page.text -> body } else { \"\" -> body }"}},
      {"parallel",
       {"Bound concurrent loop passes or stage evaluations.",
        "a positive integer expression",
        "The count is a maximum; a pass waiting for its child step occupies "
        "one slot. Parallel stages preserve input order unless `unordered` "
        "is set. The count is evaluated once at activation and must be a "
        "positive signed 32-bit integer.",
        "for hit in search.hits parallel 2 { .. }"}},
      {"unordered",
       {"Lets a parallel stage publish its values as they finish.", "",
        "A parallel stage normally restores input order before publishing. "
        "This modifier publishes completion order and requires a consumer "
        "that does not depend on ordering.",
        "urls | map fetch(it) parallel 8 unordered -> bodies"}},
      {"by",
       {"Select the comparison key for `sort`.",
        "an expression with `it` bound",
        "Without `by`, `sort` compares the input values directly.",
        "hits | sort by it.score desc -> ranked"}},
      {"before",
       {"Preceding items included by a context stage.", "a count expression",
        "Evaluated once outside it; zero keeps no preceding items. Context "
        "also requires an after count for following items.",
        "lines | context it == \"error\" before 3 after 5 -> excerpts"}},
      {"numbered",
       {"Includes source sequence numbers in context excerpts.", "[base EXPR]",
        "Each item becomes {index, value}. Indices count every source item, "
        "including skipped items, from 1 unless base is supplied.",
        "items | context it.failed before 2 after 3 numbered -> excerpts"}},
      {"base",
       {"Starting source index for numbered context items.",
        "an integer expression",
        "Evaluated once outside it. Default 1; zero and negative bases are "
        "accepted. Index overflow fails the stage.",
        "items | context it.failed before 2 after 3 numbered base 0 -> "
        "excerpts"}},
      {"desc",
       {"Sort in descending order.", "",
        "Uses the same ordering as `>`. Ascending is the default.",
        "hits | sort by it.score desc -> ranked"}},
      {"into",
       {"Send `try` stage failures to a destination.", "a node or output port",
        "Each failure is emitted as a status record. Without `into`, "
        "failures are logged at warning level and the input value is dropped.",
        "docs | try map parse(it) into bad -> good"}},
      {"max",
       {"Limit the number of `repeat` passes.", "a count",
        "A `repeat` requires `max`, `until`, or `while`. If both a count and "
        "condition are present, the first reached ends the loop.",
        "repeat asked = question max 4 { .. }"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& ModifierDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"tee",
       {"Gives each reader an independent output cursor.", "",
        "Each reader receives the full output stream. Values remain buffered "
        "until the slowest reader advances.",
        "page = run web-fetch(url: url) tee"}},
      {"via",
       {"Places a step's nodes in a named node map.", "a map name",
        "The step's streams remain in the named local map instead of being "
        "replicated across the caller transport. Unused maps are diagnosed. "
        "See `nodes`.",
        "page = call web-fetch(url: url) via fetched"}},
      {"deadline",
       {"Bounds completion by an absolute time.", "a std::time expression",
        "Evaluated once when the scope activates and dependencies are ready. "
        "An 'after' clause delays evaluation of the whole expression.",
        "run fetch() deadline std::now() + 30s after prepared"}},
      {"timeout",
       {"Bounds a step's execution duration.", "a duration",
        "Expiration gives the step `deadline_exceeded`. A `try` step keeps "
        "that status available to its caller. A negative duration removes "
        "the bound.",
        "llm = run ask_model(interactions: asked, config: {}) timeout 90s"}},
      {"after",
       {"Delay a statement until named subjects finish.",
        "one or more steps, ports or nodes",
        "Argument evaluation also waits. A named loop can be a subject. "
        "The prefix form `after a, b { ... }` gates the entire block, "
        "including nested action activation.",
        "std::strformat(\"[done] %s\", brief) -> user_log after done"}},
      {"with",
       {"Sets a header on a call or run step.",
        "one or more names and their values",
        "Header values are expressions. Inherited `x-a11-` headers reach "
        "steps automatically. An explicit `with` value overrides a header "
        "forwarded by `forward headers`.",
        "llm = run ask_model(..) with \"x-a11-provider\": provider"}},
      {"id",
       {"Set the node ID used by a step.", "an expression",
        "An external session can use this ID to read or write the node while "
        "the flow executes.",
        "sink = run collect-audio(frames: mic.audio) id session"}},
      {"forward headers", kForwardHeadersDoc},
      {"forward", kForwardHeadersDoc},
      {"headers", kForwardHeadersDoc},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& SourceDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"status",
       {"Read a subject's completion status as a record.", "a subject",
        "Contains `ok`, `code`, `number`, `message`, and `details`. Reading "
        "it waits for the subject to finish. `wait` can bind the same status.",
        "if not status fetch.ok { \"\" -> body }"}},
      {"zip",
       {"Combine streams into a stream of tuples.", "two or more streams",
        "Each tuple has one position per source. A completed source "
        "contributes null until all sources finish. A source failure ends "
        "the combined stream with that status.",
        "for url, body in std::zip(urls, bodies) { .. }"}},
      {"interleave",
       {"Merge values from several streams by arrival.", "two or more streams",
        "Preserves order within each source. A completed source contributes "
        "no further values; a source failure ends the merged stream.",
        "std::interleave(tokens, progress) -> events"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& PortModifierDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"stream", kStreamDoc},
      {"required", kRequiredDoc},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& FieldModifierDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"required", kRequiredDoc},
      {"unique",
       {"Require distinct elements in a list field.", "",
        "Checked during struct validation. Element order remains unchanged.",
        "tags: std::list[string] unique"}},
      {"matching",
       {"Require a field's text to match a regular expression.",
        "a string literal pattern",
        "The pattern is fixed in the declaration and checked during struct "
        "validation.",
        "url: string matching \"^https?://\""}},
      {"one of", kOneOfDoc},
      {"one", kOneOfDoc},
      {"of", kOneOfDoc},
      {"default",
       {"Supply a field value when the input record omits it.", "a literal",
        "Applied during struct validation. A field with a default cannot "
        "also be `required`.",
        "limit: int default 10"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& TypeDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"string",
       {"UTF-8 text.", "", "Serialized as `text/plain`. `text` is an alias.",
        "in question: string required\nout answer: string stream"}},
      {"text",
       {"Alias of `string`.", "", "Serialized as `text/plain`.",
        "out answer: std::text"}},
      {"number",
       {"Integer or floating-point number.", "",
        "Integral values retain their integer representation. Use `int` to "
        "exclude fractional values.",
        "in temperature: number"}},
      {"integer",
       {"Alias of `int`.", "", "Fractional values fail type validation.",
        "in limit: integer"}},
      {"int",
       {"Integer value.", "",
        "Fractional values fail type validation. `integer` is an alias.",
        "in limit: int"}},
      {"bool",
       {"Boolean value.", "",
        "`boolean` is an alias. Conditional expressions use truth testing; "
        "`bool()` applies that conversion explicitly.",
        "in verbose: bool"}},
      {"boolean",
       {"Alias of `bool`.", "", "Accepts `true` and `false`.",
        "in verbose: std::boolean"}},
      {"status",
       {"An operation outcome carried as data.", "",
        "Carries canonical code, number, ok, message and structured details. "
        "Status values preserve errors as data through typed initialization.",
        "std::status outcome = status work"}},
      {"option",
       {"An optional value with explicit presence.", "a payload type",
        "The has_value flag distinguishes EOF from a present null. "
        "Upstream errors remain failures when using try take.",
        "std::option[string] next = try take words"}},
      {"stream",
       {"A stream capability with a payload type.", "a payload type",
        "Names streaming data without consuming it. A stream is distinct "
        "from one value read by take.",
        "std::stream[string] words = source"}},
      {"null",
       {"An explicit null value.", "",
        "Null is a value and remains distinct from an absent option.",
        "null value = null"}},
      {"duration",
       {"Duration value.", "",
        "Supports `ns`, `us`, `ms`, `s`, `m` and `h`, including compound "
        "literals such as `1m30s`. JSON transport uses a tagged string.",
        "in budget: std::duration\ntimeout 1m30s"}},
      {"time",
       {"Absolute time value.", "",
        "Parses RFC 3339. Flow writes instants in UTC; JSON transport uses a "
        "tagged string.",
        "out published: std::time"}},
      {"object",
       {"Record with string keys.", "",
        "`object[T]` constrains mapped values to `T`. `json` is an alias. "
        "Use `struct` to declare named fields with individual types and "
        "constraints.",
        "out settings: object"}},
      {"json",
       {"JSON-compatible record or list.", "",
        "Alias of `object`. Values containing bytes require `packb` for "
        "serialization.",
        "out result: std::json"}},
      {"list",
       {"Ordered collection held as one value.", "",
        "`list[T]` and `T[]` constrain element type. A `list[string]` port "
        "carries one list; a `string stream` port carries many values. "
        "`std::list` is equivalent.",
        "in frames: list[a11::NodeFragment] stream"}},
      {"array",
       {"Alias of `list`.", "",
        "Accepts an element type parameter, as in `array[string]`.",
        "in tags: std::array[string]"}},
      {"bytes",
       {"Raw bytes.", "",
        "Serialized as `application/octet-stream`. Values containing bytes "
        "require `packb` for structured serialization.",
        "out audio: bytes stream"}},
      {"any",
       {"Unconstrained value.", "",
        "Skips payload type validation. Named types, structs and quoted media "
        "types provide a checked interface.",
        "in payload: std::any"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& ConstantDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"true",
       {"Boolean true literal.", "",
        "Accepted by `bool` ports and conditional expressions.",
        "accept_pushes: bool default true"}},
      {"false",
       {"Boolean false literal.", "",
        "Other values can also test false, including zero and empty "
        "collections. `default()` retains false because it is present.",
        "reuse_connection: bool default false"}},
      {"null",
       {"Null literal.", "",
        "Missing fields, unmatched `match` patterns and exhausted `zip` "
        "sources yield null. Null tests false and remains distinct from an "
        "absent option.",
        "if part.title == null { \"untitled\" -> title }"}},
      {"it",
       {"Current pipeline value.", "",
        "Available in stage expressions that bind an input value, including "
        "`where`, `map` and `group`. Fields and indices use normal access "
        "syntax.",
        "hits | where it.ok | map it.url\nrows | map it[0] -> first_columns"}},
      {"_",
       {"Discard destination.", "",
        "Consumes the pipeline output after all stages execute. `_` cannot "
        "be bound or read as a variable. Use `skip` to remove values from a "
        "shared source before its readers consume them.",
        "items | log -> _"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& OperatorWordDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"and",
       {"Logical conjunction.", "two conditions",
        "Operands use Flow truth testing. Returns the left operand when it "
        "tests false; otherwise evaluates and returns the right operand.",
        "if code >= 200 and code < 300 { .. }"}},
      {"or",
       {"Default an option by presence or a value by truthiness.",
        "two expressions",
        "Evaluates the right side only when the left is absent or false-like. "
        "An option tests presence and unwraps its payload, including null, "
        "false, zero and empty values. Ordinary values test truthiness. "
        "Upstream failures propagate.",
        "Settings settings = try take options or Settings{}"}},
      {"not",
       {"Logical negation.", "a condition",
        "Applies Flow truth testing to the operand. An unbound optional value "
        "tests false.",
        "lines | where not std::startswith(it, \"FINALLY:\")"}},
  };
  return *table;
}

// The codes are Abseil's, and so are these meanings.
const absl::flat_hash_map<std::string_view, WordDoc>& LogLevelDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"debug",
       {"Diagnostic log level.", "",
        "Use for implementation details that are usually omitted from user "
        "facing progress output. The levels in increasing severity are "
        "`debug`, `info`, `warning`, `error`, and `critical`. `logf` formats "
        "one line per stream value; `log` records the value directly.",
        "log debug it after step"}},
      {"info",
       {"Informational log level.", "",
        "Default for `log` without an explicit level. Suitable for progress "
        "messages and narration. The other levels are `debug` for diagnostics, "
        "`warning` for recoverable conditions, and `error` or `critical` for "
        "failures. `log` and `logf` leave the flow outcome unchanged.",
        "log info \"searching\" after plan"}},
      {"warning",
       {"Warning log level.", "",
        "Records recoverable conditions such as retries, partial results and "
        "corrected inputs. `debug` and `info` describe routine activity; "
        "`error` and `critical` describe failed work. `fail` changes the flow "
        "status; logging alone does not.",
        "log warning \"retrying\" after fetch"}},
      {"error",
       {"Error log level.", "",
        "Records failed steps or discarded values. Logging does not change the "
        "flow outcome; use `fail` to end execution with a status. `critical` "
        "reports a more severe condition; `warning` records recoverable work. "
        "`info` and `debug` cover routine progress and diagnostics.",
        "log error status fetch.message after fetch"}},
      {"critical",
       {"Critical log level.", "",
        "A11 reports `critical` and `error` at error severity. Neither level "
        "aborts execution. `warning` records recoverable conditions; `info` "
        "and `debug` cover routine progress and diagnostics. Use `fail` to "
        "return an unsuccessful status.",
        "log critical \"the index is gone\" after check"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& StatusCodeDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"ok",
       {"Operation succeeded.", "",
        "The only successful status code. `status step.ok` also tests success.",
        "if status fetch.code == \"OK\" { .. }"}},
      {"cancelled",
       {"Operation was cancelled.", "",
        "A caller cancellation or `cancel` request can produce this code.",
        "fail cancelled \"the caller went away\""}},
      {"unknown",
       {"Failure with no more specific code.", "",
        "Use a specific canonical code when the condition can be classified.",
        "fail unknown \"provider returned no cause\""}},
      {"invalid_argument",
       {"Request has invalid input.", "",
        "Includes wrong port types and excess values on a unary port. The "
        "request must change before retrying.",
        "fail invalid_argument \"the topic is empty\""}},
      {"deadline_exceeded",
       {"Execution deadline expired.", "",
        "Produced by `timeout` and the `x-a11-deadline` header. The operation "
        "may have partially executed.",
        "fail deadline_exceeded \"model response timed out\""}},
      {"not_found",
       {"Requested resource does not exist.", "",
        "Use `out_of_range` for a position beyond an existing sequence.",
        "fail not_found \"page is absent\""}},
      {"already_exists",
       {"Resource already exists.", "",
        "Applies to a creation request whose target is present.",
        "fail already_exists \"conversation exists\""}},
      {"permission_denied",
       {"Identified caller lacks permission.", "",
        "Use `unauthenticated` when the caller has not been identified.",
        "fail permission_denied \"path is outside the project\""}},
      {"resource_exhausted",
       {"Quota or capacity is exhausted.", "",
        "Includes rate limits. A later retry may succeed when capacity "
        "returns.",
        "fail resource_exhausted \"provider rate limit\""}},
      {"failed_precondition",
       {"Required system state is absent.", "",
        "The request may be valid, but another state change must occur before "
        "it can run.",
        "fail failed_precondition \"gateway is not attached\""}},
      {"aborted",
       {"Operation aborted after a conflict.", "",
        "A retry of the whole operation may succeed after a concurrent "
        "conflict.",
        "fail aborted \"another writer owns the node\""}},
      {"out_of_range",
       {"Requested position exceeds a valid range.", "",
        "Use `not_found` when the resource itself is absent.",
        "fail out_of_range \"line is past the end\""}},
      {"unimplemented",
       {"Receiver does not implement the operation.", "",
        "A retry against the same receiver cannot supply a missing action.",
        "fail unimplemented \"audio capture is unavailable\""}},
      {"internal",
       {"Internal invariant failed.", "",
        "Indicates a fault in the receiving implementation.",
        "fail internal \"plan has no briefs\""}},
      {"unavailable",
       {"Receiver is temporarily unavailable.", "",
        "Covers transport failures and a receiver unable to accept work. A "
        "retry may succeed.",
        "fail unavailable \"gateway is not answering\""}},
      {"data_loss",
       {"Data was lost or corrupted.", "",
        "The operation cannot safely recover the missing data.",
        "fail data_loss \"recording is truncated\""}},
      {"unauthenticated",
       {"Caller identity is absent or invalid.", "",
        "Use `permission_denied` when the caller is identified but lacks "
        "permission.",
        "fail unauthenticated \"api key is missing\""}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& StatusFieldDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"ok",
       {"Report whether a subject succeeded.", "",
        "Boolean field. Reading any status field waits for subject completion.",
        "if not status fetch.ok { \"\" -> body }"}},
      {"code",
       {"Return the canonical status code name.", "",
        "Uses uppercase Abseil names such as `NOT_FOUND`. Branch on `code` "
        "or `ok`; message wording is not a stable contract.",
        "if status fetch.code == \"DEADLINE_EXCEEDED\" { .. }"}},
      {"number",
       {"Return the numeric canonical status code.", "",
        "Equivalent to `code`, represented as an integer.",
        "if status fetch.number == 5 { .. }"}},
      {"details",
       {"Structured status details.", "",
        "A list preserved through native status values and status chunks.",
        "(status work).details -> report"}},
      {"message",
       {"Return the human-readable status message.", "",
        "Message wording is not a stable contract. Use `ok` or `code` for "
        "branch conditions.",
        "std::strformat(\"fetch failed: %s\", status fetch.message) -> "
        "user_log"}},
  };
  return *table;
}

// Every duration unit shares the same documentation.
constexpr std::string_view kDurationDetail =
    "Durations support comparison and addition to another duration or an "
    "instant. `seconds()` converts one to a number. Units may be combined, "
    "as in `1m30s500ms`. A negative duration represents an infinite bound.";

const absl::flat_hash_map<std::string_view, WordDoc>& DurationUnitDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"ns", {"Nanoseconds.", "", kDurationDetail, "500ns"}},
      {"us", {"Microseconds.", "", kDurationDetail, "250us"}},
      {"ms", {"Milliseconds.", "", kDurationDetail, "250ms"}},
      {"s", {"Seconds.", "", kDurationDetail, "30s"}},
      {"m", {"Minutes.", "", kDurationDetail, "2m"}},
      {"h", {"Hours.", "", kDurationDetail, "1h"}},
  };
  return *table;
}

// Punctuation, in the order a listing reads it: what moves a stream, what binds
// a name, what compares, what arithmetic there is, then the marks that group.
constexpr std::array kSymbolOrder = {
    std::string_view("|"),  std::string_view("->"), std::string_view("<-"),
    std::string_view("="),  std::string_view("=="), std::string_view("!="),
    std::string_view("<"),  std::string_view("<="), std::string_view(">"),
    std::string_view(">="), std::string_view("+"),  std::string_view("-"),
    std::string_view("."),  std::string_view(".."), std::string_view("..."),
    std::string_view(":"),  std::string_view(","),  std::string_view("{"),
    std::string_view("}"),  std::string_view("("),  std::string_view(")"),
    std::string_view("["),  std::string_view("]"),
};

// The `takes` field says what stands on either side rather than what follows,
// since that is the question a reader has about a mark.
const absl::flat_hash_map<std::string_view, WordDoc>& SymbolDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"|",
       {"Pass a stream through a stage.",
        "a stream on the left, a stage on the right",
        "Stages compose left to right. `then` and `where` also accept an "
        "infix form without `|`.",
        "page.text | truncate 2000 -> brief.pages"}},
      {"->",
       {"Write a pipeline to one or more destinations.",
        "a pipeline on the left, ports or nodes on the right",
        "Returns a continuation sink. Arrows associate right: `a -> b -> "
        "out` forwards `b`, then `a`. Independent attachments interleave "
        "after their shared prefix; completion includes all attached writers. "
        "`then` orders separate streams.",
        "brief.summary -> answer"}},
      {"<-",
       {"Set the state for the next `repeat` pass.",
        "a `repeat`'s name on the left, a stream on the right",
        "Valid only inside a `repeat` body. The next pass receives the "
        "assigned value.",
        "asked <- llm.text_output"}},
      {"=",
       {"Binds a name to a step, a node, or a value.",
        "a name and a step, node, or value",
        "`x.port` accesses a step output; `status x` reads its outcome. A "
        "bound transfer names its completion, including attached writers. "
        "Use `after`, `wait`, or `status` to observe that completion. `==` "
        "compares values.",
        "search = run web-search(query: question)"}},
      {"==",
       {"Compare two values for equality.", "two values",
        "Comparison uses decoded values, independent of their source "
        "encoding. `=` binds a name.",
        "std::lower(header.value) == \"application/json\""}},
      {"!=",
       {"Compare two values for inequality.", "two values",
        "Logical negation of `==` over the same decoded values.",
        "if part.mime != \"text/html\" { .. }"}},
      {"<",
       {"Compare whether the left value is smaller.", "two values",
        "Supports numbers, durations and instants. Strings compare by bytes.",
        "if std::len(report) < 12 { fail invalid_argument \"too short\" }"}},
      {"<=",
       {"Compare whether the left value is at most the right.", "two values",
        "Uses the same ordering as `<`.", "if elapsed <= budget { .. }"}},
      {">",
       {"Compare whether the left value is larger.", "two values",
        "Supports numbers, durations and instants. Strings compare by bytes.",
        "if number(header.retries) > 3 { cancel fetch }"}},
      {">=",
       {"Compare whether the left value is at least the right.", "two values",
        "Uses the same ordering as `>`.",
        "if code >= 200 and code < 300 { .. }"}},
      {"+",
       {"Adds two values.", "two numbers, or an instant and a duration",
        "Supports numbers, durations, and an instant plus a duration. A "
        "number combined with a duration is interpreted as seconds.",
        "std::time deadline = std::now() + 30s"}},
      {"-",
       {"Subtracts one value from another.",
        "two numbers, two instants, or an instant and a duration",
        "Subtracting two instants produces a duration. Spaces distinguish "
        "subtraction (`a - b`) from a hyphenated name (`a-b`).",
        "std::seconds(std::now() - started) -> elapsed"}},
      {".",
       {"Access a member of a value, step, or node.",
        "a value, a step or a node on the left, a name on the right",
        "Accesses an output port, record field, status field, or node `id`. "
        "The token after `.` is resolved as a member name.",
        "page.text | truncate 2000 -> brief.pages"}},
      {"..",
       {"Constrain a struct field to a range.",
        "an optional lower bound and optional upper bound",
        "Bounds numbers, durations and instants directly; bounds the length "
        "of strings, bytes and lists. Either bound may be omitted.",
        "limit: int 1..100"}},
      {"...",
       {"Expand list elements or record fields in place.",
        "a list inside a list, or a record inside a record",
        "Supports `[...xs, y]` and `{...record, key: value}`. Later record "
        "entries overwrite earlier keys.",
        R"({...it, "role": "user"} as a11::sdk::Interaction)"}},
      {":",
       {"Separate a name from its type or value.",
        "a name on the left, a type or a value on the right",
        "Used in port and field declarations, action arguments, record "
        "entries, and `with` headers. Type names may include qualified "
        "registry tags and container parameters.",
        "in frames: std::list[a11::NodeFragment] stream"}},
      {",",
       {"Separate adjacent elements in a sequence.", "",
        "Used between arguments, destinations, list items, record entries, "
        "destructured names, and `after` subjects.",
        "search = run web-search(query: question, limit: 3)"}},
      {"{",
       {"Opens a block, a record, or a value of a named type.", "",
        "After `flow`, `if`, `for`, or `nodes`, opens a statement block. In "
        "an expression, opens a record. `Type{field: value}` constructs a "
        "typed value.",
        "a11::sdk::Interaction{role: \"user\", content: [..]}"}},
      {"}",
       {"Closes a block, a record, or a value of a named type.", "",
        "Matches the preceding `{`.", "if code >= 200 { page.text -> body }"}},
      {"(",
       {"Open call arguments or group an expression.", "",
        "Parentheses also allow a pipeline where an expression value is "
        "required, as in `(hits | count) > 0`.",
        "if (hits | count) > 0 { .. }"}},
      {")",
       {"Close call arguments or a grouped expression.", "",
        "Call modifiers such as `via`, `timeout`, and `after` follow the "
        "closing parenthesis.",
        "page = run web-fetch(url: hit.url) timeout 20s"}},
      {"[",
       {"Open a list, index expression, or type parameter list.", "",
        "Negative value indices count from the end.",
        "for x, y in std::zip(a, b) { std::text(it[0]) -> shown }"}},
      {"]",
       {"Close a list, index expression, or type parameter list.", "",
        "`T[]` is equivalent to `list[T]`.",
        "in frames: std::list[a11::NodeFragment] stream"}},
  };
  return *table;
}

// Quietest first, which is the order a reader thinks of them in and the order a
// completion list is most useful in.
constexpr std::array kLogLevels = {
    std::string_view("debug"),    std::string_view("info"),
    std::string_view("warning"),  std::string_view("error"),
    std::string_view("critical"),
};

constexpr std::array kStatusCodes = {
    std::string_view("ok"),
    std::string_view("cancelled"),
    std::string_view("unknown"),
    std::string_view("invalid_argument"),
    std::string_view("deadline_exceeded"),
    std::string_view("not_found"),
    std::string_view("already_exists"),
    std::string_view("permission_denied"),
    std::string_view("resource_exhausted"),
    std::string_view("failed_precondition"),
    std::string_view("aborted"),
    std::string_view("out_of_range"),
    std::string_view("unimplemented"),
    std::string_view("internal"),
    std::string_view("unavailable"),
    std::string_view("data_loss"),
    std::string_view("unauthenticated"),
};

// Shortest unit first, which is also the order a duration is formatted in.
constexpr std::array kDurationUnits = {
    std::string_view("ns"), std::string_view("us"), std::string_view("ms"),
    std::string_view("s"),  std::string_view("m"),  std::string_view("h"),
};

const absl::flat_hash_map<std::string_view, double>& DurationTable() {
  static const auto* table = new absl::flat_hash_map<std::string_view, double>{
      {"ns", 1e-9}, {"us", 1e-6}, {"ms", 0.001},
      {"s", 1.0},   {"m", 60.0},  {"h", 3600.0},
  };
  return *table;
}

const absl::flat_hash_set<std::string_view>* MakeSet(
    std::initializer_list<std::string_view> words) {
  return new absl::flat_hash_set<std::string_view>(words);
}

// A set of the words in an ordered table, so a list that has an order and a set
// that answers "is this one of them" cannot disagree about what the words are.
// An entry of two words -- `forward headers` -- contributes both.
const absl::flat_hash_set<std::string_view>* SetOf(
    absl::Span<const std::string_view> words) {
  auto* set = new absl::flat_hash_set<std::string_view>();
  for (const std::string_view word : words) {
    size_t start = 0;
    while (start <= word.size()) {
      const size_t space = word.find(' ', start);
      const size_t end = space == std::string_view::npos ? word.size() : space;
      if (end > start) {
        set->insert(word.substr(start, end - start));
      }
      if (space == std::string_view::npos) {
        break;
      }
      start = space + 1;
    }
  }
  return set;
}

// Types in the order a listing reads: the scalars, then the two the language
// has always had values of but no name for, then the containers, then the
// escape hatches.
constexpr std::array kTypeOrder = {
    std::string_view("string"),  std::string_view("text"),
    std::string_view("number"),  std::string_view("integer"),
    std::string_view("int"),     std::string_view("bool"),
    std::string_view("boolean"), std::string_view("duration"),
    std::string_view("time"),    std::string_view("object"),
    std::string_view("json"),    std::string_view("list"),
    std::string_view("array"),   std::string_view("bytes"),
    std::string_view("any"),     std::string_view("status"),
    std::string_view("null"),    std::string_view("option"),
    std::string_view("stream"),
};

// The functions, grouped the way the reference lists them: text, structure,
// then time.
constexpr std::array kBuiltinOrder = {
    std::string_view("len"),
    std::string_view("lower"),
    std::string_view("upper"),
    std::string_view("trim"),
    std::string_view("text"),
    std::string_view("number"),
    std::string_view("bool"),
    std::string_view("keys"),
    std::string_view("values"),
    std::string_view("get"),
    std::string_view("join"),
    std::string_view("split"),
    std::string_view("merge"),
    std::string_view("contains"),
    std::string_view("startswith"),
    std::string_view("endswith"),
    std::string_view("replace"),
    std::string_view("match"),
    std::string_view("slice"),
    std::string_view("default"),
    std::string_view("to_chunk"),
    std::string_view("from_chunk"),
    std::string_view("strformat"),
    std::string_view("b64encode"),
    std::string_view("b64decode"),
    std::string_view("b64urlencode"),
    std::string_view("b64urldecode"),
    std::string_view("utf8encode"),
    std::string_view("utf8decode"),
    std::string_view("now"),
    std::string_view("duration"),
    std::string_view("time"),
    std::string_view("seconds"),
    std::string_view("parse_time"),
    std::string_view("parse_duration"),
    std::string_view("integer"),
    std::string_view("int"),
    std::string_view("string"),
    std::string_view("bytes"),
    std::string_view("object"),
    std::string_view("list"),
    std::string_view("boolean"),
    std::string_view("array"),
    std::string_view("any"),
    std::string_view("json"),
    std::string_view("option"),
    std::string_view("status"),
    std::string_view("null"),
};

// The verbs first, then the barriers, then the blocks: the order somebody
// scanning a list of statements finds what they meant.
constexpr std::array kStatementOrder = {
    std::string_view("run"),   std::string_view("call"),
    std::string_view("try"),   std::string_view("skip"),
    std::string_view("wait"),  std::string_view("close"),
    std::string_view("abort"), std::string_view("cancel"),
    std::string_view("fail"),  std::string_view("log"),
    std::string_view("logf"),  std::string_view("if"),
    std::string_view("for"),   std::string_view("repeat"),
    std::string_view("after"), std::string_view("nodes"),
};

// `else` continues an `if`; `parallel` and `max` say how wide a loop or a stage
// runs and `unordered` gives up the order a parallel stage otherwise keeps;
// `of` joins the subjects of `wait first of` and `wait all of`.
constexpr std::array kClauseOrder = {
    std::string_view("else"),      std::string_view("parallel"),
    std::string_view("unordered"), std::string_view("max"),
    std::string_view("of"),        std::string_view("by"),
    std::string_view("before"),    std::string_view("desc"),
    std::string_view("into"),      std::string_view("until"),
    std::string_view("while"),     std::string_view("numbered"),
    std::string_view("base"),
};

// As a flow is written, top to bottom.
constexpr std::array kDeclarationOrder = {
    std::string_view("declare"),  std::string_view("exported"),
    std::string_view("mod"),      std::string_view("export"),
    std::string_view("import"),   std::string_view("from"),
    std::string_view("flow"),     std::string_view("struct"),
    std::string_view("describe"), std::string_view("in"),
    std::string_view("out"),      std::string_view("header"),
    std::string_view("as"),       std::string_view("default"),
    std::string_view("stream"),   std::string_view("required"),
    std::string_view("node"),     std::string_view("nodes"),
};

// As a field writes them: what it is, then what it may hold, then what it is
// when nothing was sent. `one of` is the one that is two words, as
// `forward headers` is among the call modifiers.
constexpr std::array kFieldModifierOrder = {
    std::string_view("required"), std::string_view("unique"),
    std::string_view("matching"), std::string_view("one of"),
    std::string_view("default"),
};

// As they read after a call: what it does with its nodes, then how long, then
// what it waits for, then what it is told.
constexpr std::array kModifierOrder = {
    std::string_view("tee"),     std::string_view("via"),
    std::string_view("timeout"), std::string_view("deadline"),
    std::string_view("after"),   std::string_view("with"),
    std::string_view("id"),      std::string_view("forward headers"),
};

constexpr std::array kPortModifierOrder = {
    std::string_view("stream"),
    std::string_view("required"),
};

constexpr std::array kStatusFieldOrder = {
    std::string_view("ok"),      std::string_view("code"),
    std::string_view("number"),  std::string_view("message"),
    std::string_view("details"),
};

}  // namespace

std::string Canonical(std::string_view word) {
  bool has_upper = false;
  bool has_lower = false;
  for (const char letter : word) {
    if (letter >= 'A' && letter <= 'Z') {
      has_upper = true;
    }
    if (letter >= 'a' && letter <= 'z') {
      has_lower = true;
    }
  }
  if (!has_upper || has_lower) {
    return std::string(word);
  }
  std::string lowered(word);
  for (char& letter : lowered) {
    if (letter >= 'A' && letter <= 'Z') {
      letter = static_cast<char>(letter + 32);
    }
  }
  return lowered;
}

bool IsShouted(std::string_view word) {
  bool has_upper = false;
  for (const char letter : word) {
    if (letter >= 'a' && letter <= 'z') {
      return false;
    }
    if (letter >= 'A' && letter <= 'Z') {
      has_upper = true;
    }
  }
  return has_upper;
}

absl::Span<const EntryPort> EntryPorts() {
  // `argv` holds every argument including the program's own name at index 0,
  // the way a C program's does -- so `argv[0]` is the same thing a reader
  // already expects it to be, and `argc` agrees with it.
  static constexpr EntryPort kPorts[] = {
      {"argc", "integer", true,
       "How many arguments there are, counting the program's own name."},
      {"argv", "string", false,
       "The arguments in order, the program's own name first."},
  };
  return absl::MakeConstSpan(kPorts);
}

absl::Span<const std::string_view> Stages() {
  return absl::MakeConstSpan(kStageOrder.data(), kStageOrder.size());
}

std::optional<StageArgument> StageTakes(std::string_view canonical_name) {
  const auto found = StageTable().find(canonical_name);
  if (found == StageTable().end()) {
    return std::nullopt;
  }
  return found->second;
}

std::string_view StageArgumentName(StageArgument argument) {
  switch (argument) {
    case StageArgument::kNone:
      return "none";
    case StageArgument::kNumber:
      return "number";
    case StageArgument::kExpression:
      return "expr";
    case StageArgument::kString:
      return "string";
    case StageArgument::kOptionalString:
      return "string?";
    case StageArgument::kOptionalExpression:
      return "expr?";
    case StageArgument::kSortKey:
      return "sort-key";
    case StageArgument::kFold:
      return "fold";
    case StageArgument::kDuration:
      return "duration";
    case StageArgument::kStream:
      return "stream";
    case StageArgument::kLog:
      return "log";
    case StageArgument::kLogFormat:
      return "logf";
  }
  return "none";
}

bool IsPrimitiveType(std::string_view name) {
  return name == "int" || name == "integer" || name == "number" ||
         name == "bool" || name == "string" || name == "bytes" ||
         name == "object" || name == "null" || name == "list";
}

const WordDoc* absl_nullable StageDocumentation(
    std::string_view canonical_name) {
  const auto found = StageDocs().find(canonical_name);
  return found == StageDocs().end() ? nullptr : &found->second;
}

const WordDoc* absl_nullable BuiltinDocumentation(
    std::string_view canonical_name) {
  const auto found = BuiltinDocs().find(canonical_name);
  return found == BuiltinDocs().end() ? nullptr : &found->second;
}

const WordDoc* absl_nullable Documentation(WordRole role,
                                           std::string_view canonical_name) {
  const absl::flat_hash_map<std::string_view, WordDoc>* table = nullptr;
  switch (role) {
    case WordRole::kStage:
      table = &StageDocs();
      break;
    case WordRole::kBuiltin:
      table = &BuiltinDocs();
      break;
    case WordRole::kStatement:
      table = canonical_name == "after" ? &ModifierDocs() : &StatementDocs();
      break;
    case WordRole::kDeclaration:
      table = &DeclarationDocs();
      break;
    case WordRole::kClause:
      table = &ClauseDocs();
      break;
    case WordRole::kModifier:
      table = &ModifierDocs();
      break;
    case WordRole::kSource:
      table = &SourceDocs();
      break;
    case WordRole::kPortModifier:
      table = &PortModifierDocs();
      break;
    case WordRole::kFieldModifier:
      table = &FieldModifierDocs();
      break;
    case WordRole::kType:
      table = &TypeDocs();
      break;
    case WordRole::kConstant:
      table = &ConstantDocs();
      break;
    case WordRole::kOperatorWord:
      table = &OperatorWordDocs();
      break;
    case WordRole::kStatusCode:
      table = &StatusCodeDocs();
      break;
    case WordRole::kLogLevel:
      table = &LogLevelDocs();
      break;
    case WordRole::kStatusField:
      table = &StatusFieldDocs();
      break;
    case WordRole::kDurationUnit:
      table = &DurationUnitDocs();
      break;
    case WordRole::kSymbol:
      table = &SymbolDocs();
      break;
  }
  if (table == nullptr) {
    return nullptr;
  }
  const auto found = table->find(canonical_name);
  return found == table->end() ? nullptr : &found->second;
}

absl::Span<const std::string_view> OrderedSymbols() {
  return absl::MakeConstSpan(kSymbolOrder.data(), kSymbolOrder.size());
}

const WordDoc* absl_nullable AnyDocumentation(std::string_view canonical_name) {
  for (const WordRole role : WordRoles()) {
    if (const WordDoc* doc = Documentation(role, canonical_name);
        doc != nullptr) {
      return doc;
    }
  }
  return nullptr;
}

absl::Span<const WordRole> WordRoles() {
  static constexpr std::array kAll = {
      WordRole::kStage,         WordRole::kBuiltin,
      WordRole::kStatement,     WordRole::kDeclaration,
      WordRole::kClause,        WordRole::kModifier,
      WordRole::kSource,        WordRole::kPortModifier,
      WordRole::kFieldModifier, WordRole::kType,
      WordRole::kConstant,      WordRole::kOperatorWord,
      WordRole::kStatusCode,    WordRole::kLogLevel,
      WordRole::kStatusField,   WordRole::kDurationUnit,
      WordRole::kSymbol,
  };
  return absl::MakeConstSpan(kAll.data(), kAll.size());
}

std::string_view WordRoleName(WordRole role) {
  switch (role) {
    case WordRole::kStage:
      return "stage";
    case WordRole::kBuiltin:
      return "builtin";
    case WordRole::kStatement:
      return "statement";
    case WordRole::kDeclaration:
      return "declaration";
    case WordRole::kClause:
      return "clause_word";
    case WordRole::kModifier:
      return "modifier";
    case WordRole::kSource:
      return "source";
    case WordRole::kPortModifier:
      return "port_modifier";
    case WordRole::kFieldModifier:
      return "field_modifier";
    case WordRole::kType:
      return "type";
    case WordRole::kConstant:
      return "constant";
    case WordRole::kOperatorWord:
      return "operator";
    case WordRole::kStatusCode:
      return "status_code";
    case WordRole::kLogLevel:
      return "log_level";
    case WordRole::kStatusField:
      return "status_field";
    case WordRole::kDurationUnit:
      return "duration_unit";
    case WordRole::kSymbol:
      return "symbol";
  }
  return "symbol";
}

std::vector<std::string_view> WordsOf(WordRole role) {
  const auto listed = [](absl::Span<const std::string_view> words) {
    return std::vector<std::string_view>(words.begin(), words.end());
  };
  // Generated files require stable order, so sets are sorted before export.
  const auto ordered_set = [](const absl::flat_hash_set<std::string_view>& in) {
    std::vector<std::string_view> out(in.begin(), in.end());
    std::sort(out.begin(), out.end());
    return out;
  };
  switch (role) {
    case WordRole::kStage:
      return listed(Stages());
    case WordRole::kBuiltin:
      return listed(OrderedBuiltins());
    case WordRole::kStatement:
      return listed(OrderedStatements());
    case WordRole::kDeclaration:
      return listed(OrderedDeclarations());
    case WordRole::kClause:
      return listed(OrderedClauseWords());
    case WordRole::kModifier:
      // The unsplit list, so `forward headers` is one word here as it is in the
      // ordered table. Each half is in the doc table as well, since a hover
      // lands on one word rather than on the pair.
      return listed(OrderedModifiers());
    case WordRole::kSource:
      return ordered_set(SourceWords());
    case WordRole::kPortModifier:
      return listed(OrderedPortModifiers());
    case WordRole::kFieldModifier:
      return listed(OrderedFieldModifiers());
    case WordRole::kType:
      return listed(OrderedTypeNames());
    case WordRole::kConstant:
      return ordered_set(ConstantWords());
    case WordRole::kOperatorWord:
      return ordered_set(OperatorWords());
    case WordRole::kStatusCode:
      return listed(StatusCodes());
    case WordRole::kLogLevel:
      return listed(LogLevels());
    case WordRole::kStatusField:
      return listed(OrderedStatusFields());
    case WordRole::kDurationUnit:
      return listed(DurationUnits());
    case WordRole::kSymbol:
      return listed(OrderedSymbols());
  }
  return {};
}

const absl::flat_hash_set<std::string_view>& BareStages() {
  static const auto* words = MakeSet({"then", "where"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& ReducingStages() {
  static const auto* words = MakeSet({"collect", "consume", "count", "join",
                                      "sum", "min", "max", "avg", "fold"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& PositionalStages() {
  static const auto* words =
      MakeSet({"first", "last", "drop", "batch", "window", "group", "distinct",
               "count", "sort", "flatten"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& ParallelStages() {
  // Every stage that reshapes or judges one value on its own, and nothing that
  // gathers, orders or counts: `| collect parallel 8` has one value to make and
  // `| sort parallel 8` has an order to keep, so both would be asking.
  static const auto* words =
      MakeSet({"map", "where", "at", "truncate", "match", "mime", "flatten",
               "text", "json", "packb", "strformat", "chunk"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& Builtins() {
  static const auto* words = SetOf(OrderedBuiltins());
  return *words;
}

absl::Span<const std::string_view> OrderedBuiltins() {
  return absl::MakeConstSpan(kBuiltinOrder.data(), kBuiltinOrder.size());
}

const absl::flat_hash_set<std::string_view>& TypeNames() {
  static const auto* words = SetOf(OrderedTypeNames());
  return *words;
}

absl::Span<const std::string_view> OrderedTypeNames() {
  return absl::MakeConstSpan(kTypeOrder.data(), kTypeOrder.size());
}

absl::Span<const int> TypeParameters(std::string_view canonical_name) {
  // A list says what it holds; an object says what it maps. Everything else
  // takes none.
  static constexpr std::array<int, 2> kZeroOrOne = {0, 1};
  static constexpr std::array<int, 1> kNone = {0};
  if (canonical_name == "list" || canonical_name == "array" ||
      canonical_name == "object" || canonical_name == "option" ||
      canonical_name == "stream") {
    return absl::MakeConstSpan(kZeroOrOne.data(), kZeroOrOne.size());
  }
  return absl::MakeConstSpan(kNone.data(), kNone.size());
}

const absl::flat_hash_set<std::string_view>& StatementWords() {
  static const auto* words = SetOf(OrderedStatements());
  return *words;
}

absl::Span<const std::string_view> OrderedStatements() {
  return absl::MakeConstSpan(kStatementOrder.data(), kStatementOrder.size());
}

const absl::flat_hash_set<std::string_view>& ClauseWords() {
  static const auto* words = SetOf(OrderedClauseWords());
  return *words;
}

absl::Span<const std::string_view> OrderedClauseWords() {
  return absl::MakeConstSpan(kClauseOrder.data(), kClauseOrder.size());
}

const absl::flat_hash_set<std::string_view>& DeclarationWords() {
  static const auto* words = SetOf(OrderedDeclarations());
  return *words;
}

absl::Span<const std::string_view> OrderedDeclarations() {
  return absl::MakeConstSpan(kDeclarationOrder.data(),
                             kDeclarationOrder.size());
}

const absl::flat_hash_set<std::string_view>& ModifierWords() {
  static const auto* words = SetOf(OrderedModifiers());
  return *words;
}

absl::Span<const std::string_view> OrderedModifiers() {
  return absl::MakeConstSpan(kModifierOrder.data(), kModifierOrder.size());
}

const absl::flat_hash_set<std::string_view>& SourceWords() {
  // `status x` reads an outcome, `zip(a, b)` reads several streams in step, and
  // `interleave(a, b)` reads several as one, in the order values arrive.
  static const auto* words = MakeSet({"take", "status", "zip", "interleave"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& PortModifierWords() {
  static const auto* words = SetOf(OrderedPortModifiers());
  return *words;
}

absl::Span<const std::string_view> OrderedPortModifiers() {
  return absl::MakeConstSpan(kPortModifierOrder.data(),
                             kPortModifierOrder.size());
}

const absl::flat_hash_set<std::string_view>& FieldModifierWords() {
  static const auto* words = SetOf(OrderedFieldModifiers());
  return *words;
}

absl::Span<const std::string_view> OrderedFieldModifiers() {
  return absl::MakeConstSpan(kFieldModifierOrder.data(),
                             kFieldModifierOrder.size());
}

absl::Span<const std::string_view> LogLevels() {
  return absl::MakeConstSpan(kLogLevels.data(), kLogLevels.size());
}

bool IsLogLevel(std::string_view word) {
  const std::string canonical = Canonical(word);
  for (const std::string_view level : kLogLevels) {
    if (level == canonical) {
      return true;
    }
  }
  return false;
}

absl::Span<const std::string_view> StatusCodes() {
  return absl::MakeConstSpan(kStatusCodes.data(), kStatusCodes.size());
}

bool IsStatusCode(std::string_view word) {
  const std::string canonical = Canonical(word);
  // A code may be written with dashes where the canonical name has underscores,
  // which is what `plan.status_code` accepts.
  std::string normalised = canonical;
  for (char& letter : normalised) {
    if (letter == '-') {
      letter = '_';
    }
  }
  for (const std::string_view code : kStatusCodes) {
    if (code == normalised) {
      return true;
    }
  }
  return false;
}

const absl::flat_hash_set<std::string_view>& StatusFields() {
  static const auto* words = SetOf(OrderedStatusFields());
  return *words;
}

absl::Span<const std::string_view> OrderedStatusFields() {
  return absl::MakeConstSpan(kStatusFieldOrder.data(),
                             kStatusFieldOrder.size());
}

const absl::flat_hash_set<std::string_view>& ConstantWords() {
  static const auto* words = MakeSet({"true", "false", "null", "it", "_"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& OperatorWords() {
  static const auto* words = MakeSet({"and", "or", "not"});
  return *words;
}

absl::Span<const std::string_view> DurationUnits() {
  return absl::MakeConstSpan(kDurationUnits.data(), kDurationUnits.size());
}

std::optional<double> DurationUnitSeconds(std::string_view unit) {
  const auto found = DurationTable().find(unit);
  if (found == DurationTable().end()) {
    return std::nullopt;
  }
  return found->second;
}

}  // namespace a11::flow::vocabulary
