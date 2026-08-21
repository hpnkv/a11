// Copyright 2026 The A11 Authors.

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
    std::string_view("first"),     std::string_view("last"),
    std::string_view("drop"),      std::string_view("truncate"),
    std::string_view("batch"),     std::string_view("window"),
    std::string_view("flatten"),   std::string_view("group"),
    std::string_view("sort"),      std::string_view("where"),
    std::string_view("map"),       std::string_view("scan"),
    std::string_view("match"),     std::string_view("distinct"),
    std::string_view("then"),      std::string_view("log"),
    std::string_view("logf"),      std::string_view("mime"),
    std::string_view("strformat"), std::string_view("chunk"),
    std::string_view("collect"),   std::string_view("count"),
    std::string_view("sum"),       std::string_view("min"),
    std::string_view("max"),       std::string_view("avg"),
    std::string_view("fold"),      std::string_view("join"),
    std::string_view("text"),      std::string_view("json"),
    std::string_view("packb"),     std::string_view("timeout"),
    std::string_view("pace"),
};

const absl::flat_hash_map<std::string_view, StageArgument>& StageTable() {
  static const auto* table =
      new absl::flat_hash_map<std::string_view, StageArgument>{
          {"first", StageArgument::kNumber},
          {"last", StageArgument::kNumber},
          {"drop", StageArgument::kNumber},
          {"truncate", StageArgument::kNumber},
          {"batch", StageArgument::kNumber},
          {"window", StageArgument::kNumber},
          {"chunk", StageArgument::kNumber},
          {"where", StageArgument::kExpression},
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

// What each stage does, as reference. Accuracy is from `Scope::ProduceStage` in
// `runtime.cc`, which is the one implementation of every one of these; where
// the two disagree the runtime is right and this is a bug.
//
// House style, since a table of 19 of these only reads well if they agree:
// present tense, "the stream" for the whole and "each value" for one of them,
// no second person, and the caveat last rather than buried. `--` is never
// written in text a reader sees: a colon or an em dash instead.
const absl::flat_hash_map<std::string_view, WordDoc>& StageDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"first",
       {"The first `n` values of the stream, and then nothing.", "a count",
        "Stops reading upstream as soon as it has them, so nothing downstream "
        "waits for values nobody will see. It does not *cancel* whoever is "
        "producing: a step feeding a node that nobody drains would stall, and "
        "a "
        "`first 3` must not be able to wedge what it reads from. An action "
        "that "
        "can be asked to finish takes that on a control port of its own. "
        "`| first 1` is how a stream becomes one value.",
        "hits | first 3 -> shown"}},
      {"last",
       {"The last `n` values of the stream.", "a count",
        "Reads the whole stream to find out which those are, holding `n` "
        "values "
        "while it does, so nothing comes out until the stream ends.",
        "lines | last 20 -> tail"}},
      {"drop",
       {"Everything except the first `n` values.", "a count",
        "Only this pipeline sees fewer values. `skip n port` is the other half "
        "of the pair: it takes them off the node itself, for every reader of "
        "it, "
        "which is how a header row stops being everybody's problem.",
        "rows | drop 1 -> body"}},
      {"truncate",
       {"Shortens each value. The stream keeps its length.", "a count",
        "`n` characters of a string, `n` bytes of a byte string, `n` items of "
        "a "
        "list, `n` keys of a record. Anything else goes through unchanged. "
        "Cutting a page down before it reaches a model is the difference "
        "between a cheap call and an expensive one.",
        "page.text | truncate 4000 -> brief.pages"}},
      {"batch",
       {"Gathers values into lists of `n`.", "a count",
        "Each list goes on as one value, so what follows reads a stream of "
        "lists. Whatever is left when the stream ends goes on too, short.",
        "samples | batch 100 -> frames"}},
      {"window",
       {"Gathers values into overlapping lists of the last `n`.", "a count",
        "One list per value once `n` have arrived, each sharing all but one "
        "value with the list before it. This is `batch` for a question about "
        "neighbours rather than about groups: a pattern spanning two lines is "
        "invisible to a `batch`, because a boundary falls somewhere and half "
        "the matches fall on it. Holds `n` values and no more, so a window "
        "over "
        "a stream that never ends costs nothing that grows.",
        R"(lines | window 3 | map join(it, "\n") -> paragraphs)"}},
      {"group",
       {"Gathers values into lists, closed by a question rather than a count.",
        "an expression, with `it` bound to the value in hand",
        "Values pile up until the expression is true of the one just added; "
        "that list goes on and gathering starts again. Whatever is left when "
        "the stream ends goes on too. This is `batch` for a stream whose own "
        "values say where the boundaries are.",
        "words | group it.final -> sentences"}},
      {"where",
       {"Keeps the values the expression is true of.",
        "an expression, with `it` bound to the value in hand",
        "A kept value goes on exactly as it arrived, bytes and mimetype and "
        "all, so filtering costs nothing beyond reading it. May be written "
        "without the leading `|`, which reads as a word joining two things: "
        "`hits where it.ok`.",
        "hits | where it.score > 0.5 -> kept"}},
      {"map",
       {"Replaces each value with what the expression makes of it.",
        "an expression, with `it` bound to the value in hand",
        "`map Shape{...}` and `map it as Shape` also tell the language what "
        "the "
        "stream now carries, so the fields are completed and checked "
        "downstream. Anything else makes something the language cannot name, "
        "and it says nothing about it rather than guessing.",
        "hits | map Source{url: it.url} -> sources"}},
      {"log",
       {"Logs each value and passes it on unchanged.",
        "optionally a level, and an expression with `it` bound to the value in "
        "hand",
        "Written without an expression it logs the value itself, so a stage "
        "may "
        "be dropped into a pipeline to see what is going through it without "
        "changing what comes out. The log goes to the flow's own log, which no "
        "port has to declare and nothing has to drain.",
        "hits | log warning it.error | collect -> problems"}},
      {"logf",
       {"Logs a formatted line per value and passes the value on unchanged.",
        "optionally a level, a format, and the values to fill it with",
        "The format is `strformat`'s, and `it` is the value in hand, so the "
        "usual shape is one format and one field of it. Like `log` it changes "
        "nothing about the stream.",
        "pages | logf \"fetched %s\" it.url -> fetched"}},
      {"match",
       {"Pulls named fields out of each value, and drops the ones that do not "
        "fit.",
        "a pattern",
        "Literal text matches itself, a run of spaces or tabs matches any run, "
        "and `{name}` captures up to whatever follows it: "
        "`match(\"name={name} age={age:int}\")` turns `name=Alice   age=27` "
        "into a record with `name` and `age`. A hole may say what it is: "
        "`int`, "
        "`number`, `bool`, `word`, `line`, `rest`, `duration`, `time`, `json`. "
        "The pattern searches, so it matches anywhere in the value and needs "
        "no "
        "leading or trailing wildcards. A value the pattern does not fit is "
        "dropped, which is what makes this a `where` and a `map` at once; the "
        "function of the same name answers null for one value instead. Where "
        "the "
        "pattern is written out, the fields are known, so `it.name` is "
        "completed "
        "and a typo in it is reported.",
        "lines | match \"{level:word}: {message:line}\" | map it.level -> "
        "levels"}},
      {"distinct",
       {"Drops a value equal to one already seen.", "",
        "Equal as text, which is what makes it work on records as well as on "
        "strings. Every distinct value seen so far is remembered for as long "
        "as "
        "the stream runs.",
        "urls | distinct -> once_each"}},
      {"then",
       {"All of this stream, and then all of another.", "another stream",
        "Two writers to one node interleave by arrival, which is right for "
        "pages and wrong for a conversation; this is how a flow says which "
        "comes first. May be written without the leading `|`.",
        "history then asked -> prompt"}},
      {"mime",
       {"Keeps the values whose mimetype matches.",
        "a mimetype, which may end in `*`",
        "Looks at how the value was written rather than at the value, so "
        "nothing is decoded to decide. On a mixed stream this is how a flow "
        "takes the part it understands.",
        "parts | mime \"text/*\" -> readable"}},
      {"strformat",
       {"Formats each value into a string.", "a format string",
        "Exactly `| map strformat(\"...\", it)`, which is the shape almost "
        "every "
        "use of it has. printf's conversions: `%s` for anything, `%d` and "
        "`%.2f` for numbers, `%%` for a literal per cent. The function of the "
        "same name is there for when more than one value goes in.",
        "elapsed | strformat \"took %s\" -> log"}},
      {"chunk",
       {"Cuts each value into pieces of at most `n` bytes.", "a size in bytes",
        "The other direction from `let`: one value becomes a stream of pieces, "
        "which is what an upload wanting 64 KiB frames and a model wanting a "
        "paragraph are both asking for. Text is cut on a character boundary, "
        "so "
        "every piece is something that can be read. A value that is neither "
        "text nor bytes is not cut and goes through whole.",
        "body | chunk 65536 -> upload.parts"}},
      {"collect",
       {"The whole stream, as one list.", "",
        "Reads to the end and yields exactly one value, which changes the "
        "arithmetic of everything after it: `| drop 2` yields nothing and "
        "`| count` is 1 however long the stream was.",
        "hits | collect -> all"}},
      {"count",
       {"How many values the stream had.", "",
        "Reads to the end and yields exactly one integer. Nothing is decoded "
        "to "
        "count it, so counting a stream of pages costs nothing per page.",
        "pages | count -> how_many"}},
      {"sum",
       {"The values added together, as one value.",
        "an expression over each value, or nothing for the values themselves",
        "Reads to the end and yields exactly one value. Numbers add as numbers "
        "and durations as durations; a stream with nothing in it sums to 0. "
        "`| sum it.price` is the shorthand for `| map it.price | sum`.",
        "orders | sum it.price -> revenue"}},
      {"min",
       {"The smallest value of the stream.",
        "an expression over each value, or nothing for the values themselves",
        "Reads to the end and yields exactly one value, compared the way `<` "
        "compares them. An empty stream yields nothing rather than a zero, "
        "because the smallest of no values is not a value.",
        "samples | min it.latency -> fastest"}},
      {"max",
       {"The largest value of the stream.",
        "an expression over each value, or nothing for the values themselves",
        "The counterpart of `min`, and the same about an empty stream.",
        "samples | max it.latency -> slowest"}},
      {"avg",
       {"The mean of the stream's values.",
        "an expression over each value, or nothing for the values themselves",
        "Reads to the end and yields exactly one number; an empty stream "
        "yields nothing. Durations average as durations, which is what makes "
        "`| avg it.elapsed` the useful form.",
        "runs | avg it.elapsed -> typical"}},
      {"fold",
       {"The stream folded into one value, carrying what has been seen so far.",
        "a literal to start from, a name for what it carries, an expression",
        "Written `fold 0 as total, total + it`: the name is bound to what the "
        "last pass produced and `it` to the value in hand. The general form of "
        "`sum`, `min` and `max`, for the shape none of them is.",
        "orders | fold 0 as total, total + it.price -> revenue"}},
      {"scan",
       {"Every value the fold passed through, rather than only the last.",
        "a literal to start from, a name for what it carries, an expression",
        "Written `scan 0 as n, n + 1`, exactly as `fold` is, and the "
        "difference "
        "is where the values go: `fold` yields one when the stream ends and "
        "this yields one per value as it arrives. That is what a state machine "
        "is, a state carried forward and read at every step, so a stream whose "
        "meaning depends on what came before it is expressible without holding "
        "the stream. The start may be a record, which is what a state of more "
        "than one part needs; `scan 0 as n, n + 1` numbers a stream, which is "
        "the smallest useful one.",
        "lines | scan 0 as n, n + 1 -> numbered"}},
      {"sort",
       {"The stream in order.", "optionally `by` an expression, and `desc`",
        "Reads the whole stream to find out what the order is, so nothing "
        "comes out until it ends. `by` names what to compare (`sort by "
        "it.score`) and `desc` reverses it; values compare as `<` does.",
        "hits | sort by it.score desc | first 10 -> best"}},
      {"flatten",
       {"Each list of the stream, as its own values.", "",
        "The inverse of `batch`: a stream of lists becomes a stream of what "
        "they held, in order. A value that is not a list goes through as "
        "itself, so a mixed stream is flattened rather than refused.",
        "pages | map it.lines | flatten -> lines"}},
      {"timeout",
       {"Fails the pipeline when the stream goes quiet for too long.",
        "how long a gap is too long",
        "The gap *between* values, not the total: a stream that keeps arriving "
        "runs for as long as it likes, and one that stops for longer than this "
        "ends the flow with `deadline_exceeded`. A whole-step budget is "
        "`wait ... timeout` instead.",
        "tokens | timeout 30s -> answer"}},
      {"pace",
       {"Slows the stream to at most one value per interval.",
        "the least time between two values",
        "Nothing is dropped: the values are spaced out and whoever produces "
        "them is held back behind the buffer, which is what makes this a rate "
        "limit rather than a sample. What it costs is latency, on purpose.",
        "requests | pace 100ms -> to_api"}},
      {"join",
       {"Every value of the stream as text, concatenated into one string.",
        "a separator string, or nothing for none",
        "Reads to the end and yields exactly one value. The function of the "
        "same name does this to a list that is already one value.",
        "words | join \", \" -> sentence"}},
      {"text",
       {"Each value of the stream as text.", "",
        "One value in, one value out: this re-writes each value and does not "
        "shorten the stream. The function `text(x)` does the same to a single "
        "value.",
        "numbers | text -> labels"}},
      {"json",
       {"Each value of the stream as JSON.", "",
        "A value carrying bytes anywhere inside it cannot be written as JSON "
        "at "
        "all, and the language says so before anything runs rather than at the "
        "first value: `| packb` is the encoding that carries bytes.",
        "records | json -> payload"}},
      {"packb",
       {"Says the values travel as MessagePack rather than as JSON.", "",
        "A value that already arrived packed is passed on untouched, tag and "
        "all, so this costs nothing when the producer wrote it that way. It is "
        "also the only encoding here that can carry bytes.",
        "clips | packb -> transcribe.audio"}},
  };
  return *table;
}

// The fixed function set, as reference. Same house style as [StageDocs], and
// the same rule about accuracy: `Call` in `values.cc` is the implementation.
const absl::flat_hash_map<std::string_view, WordDoc>& BuiltinDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"len",
       {"How long a value is.", "one value",
        "Characters of a string, bytes of a byte string, items of a list, keys "
        "of a record. Anything else is 0.",
        "if len(report) < 12 { fail invalid_argument \"too short\" }"}},
      {"lower",
       {"A value as text, in lower case.", "one value",
        "ASCII only: a letter outside ASCII is left as it is.",
        "lower(header.value) == \"application/json\""}},
      {"upper",
       {"A value as text, in upper case.", "one value",
        "ASCII only: a letter outside ASCII is left as it is.",
        "upper(code) -> shouted"}},
      {"trim",
       {"A value as text, without the whitespace at either end.", "one value",
        "Spaces, tabs and line breaks. The inside of the text is untouched.",
        "trim(page.title) -> title"}},
      {"text",
       {"A value as text.", "one value",
        "A string is itself; a number, a bool, a duration or an instant is "
        "written the way this language writes one; a record or a list is its "
        "JSON. The stage `| text` does the same to every value of a stream.",
        "text(count) -> label"}},
      {"number",
       {"A value as a number.", "one value",
        "Text that reads as a whole number gives an integer and text that "
        "reads "
        "as a decimal gives a double; a bool gives 0 or 1; a duration gives "
        "its "
        "seconds and an instant its seconds since the epoch. Text that is not "
        "a "
        "number at all gives 0 rather than failing.",
        "number(header.retries) > 3"}},
      {"bool",
       {"Whether a value counts as true.", "one value",
        "Null, `false`, 0, an empty string, an empty list and an empty record "
        "are false; everything else is true. This is the same question an `if` "
        "asks of its condition.",
        "bool(page.body) -> anything_there"}},
      {"keys",
       {"The keys of a record, sorted, as a list of strings.", "one record",
        "Sorted so that reading them twice reads them the same way. A value "
        "that is not a record has no keys, and the list is empty.",
        "keys(headers) | join \", \" -> named"}},
      {"values",
       {"The values of a record, in the order their keys sort.", "one value",
        "A list is already its own values and comes back unchanged, which is "
        "what makes this safe to write over either.",
        "values(scores) | collect -> all"}},
      {"get",
       {"One field or item of a value, with a fallback.",
        "the value, the key, and what to use instead",
        "The key is a string for a record and a whole number for a list, where "
        "a negative index counts from the end. The fallback is used when the "
        "field is missing or null, which is what tells this from `x.name`.",
        R"(get(page.meta, "title", "untitled") -> title)"}},
      {"join",
       {"Every item of a list as text, concatenated into one string.",
        "the list, and a separator",
        "The stage `| join` does this to a whole stream instead of to a list. "
        "A "
        "value that is not a list is simply its own text.",
        "join(keys(headers), \", \") -> named"}},
      {"split",
       {"A value as text, cut into a list.", "the text, and a separator",
        "With no separator it cuts on runs of whitespace and keeps nothing "
        "empty, which is what most callers of it want.",
        "split(line, \",\") -> fields"}},
      {"merge",
       {"Several records as one.", "two or more records",
        "A later argument wins where two name the same key. Anything that is "
        "not a record is ignored rather than refused, so `merge(defaults, "
        "given)` is safe when `given` turned out to be null.",
        "merge(defaults, page.meta) -> settings"}},
      {"contains",
       {"Whether a value holds another.", "the container, and the member",
        "A substring of text, an equal item of a list, or a key of a record. "
        "Anything else is false.",
        "contains(page.text, \"error\")"}},
      {"starts-with",
       {"Whether a value, as text, begins with another.",
        "the text, and one prefix or a list of them",
        "A list of candidates is one question rather than three: a mimetype "
        "starts with any of `[\"text/\", \"application/json\"]`.",
        R"(starts-with(part.mime, ["text/", "application/json"]))"}},
      {"ends-with",
       {"Whether a value, as text, ends with another.",
        "the text, and one suffix or a list of them",
        "A list of candidates is one question rather than three: a piece that "
        "ends a sentence ends with any of `[\".\", \"?\", \"!\"]`.",
        R"(ends-with(piece, [".", "?", "!"]))"}},
      {"match",
       {"The fields a pattern pulls out of a value, or null where it does not "
        "fit.",
        "a pattern, and the text to read",
        "The same pattern language the stage reads: literal text matches "
        "itself, "
        "`{name}` captures, and `{name:int}` says what to read it as. "
        "`match(\"name={name} age={age:int}\", line)` gives a record, so "
        "`obj.name` and `obj.age` are there; a line the pattern does not fit "
        "gives null, which `if not obj` asks about. Where the pattern is "
        "written "
        "out rather than computed, the fields are known and completed.",
        "let who = match(\"name={name}\", line)"}},
      {"replace",
       {"A value as text, with every occurrence of one string replaced by "
        "another.",
        "the text, what to find, and what to put there",
        "Plain text and not a pattern: nothing in the language interprets `.` "
        "or `*`. An empty string to find changes nothing.",
        R"(replace(path, "\\", "/") -> posix)"}},
      {"slice",
       {"The part of a value between two positions.",
        "the value, where to start, and where to stop",
        "Characters of a string, bytes of a byte string, items of a list. "
        "Either position may be negative, counting from the end, and leaving "
        "the second one off runs to the end.",
        "slice(page.text, 0, 200) -> preview"}},
      {"default",
       {"A value, or something else where it is empty.",
        "the value, and what to use instead",
        "Empty means null, an empty string, an empty list or an empty record. "
        "Not 0 and not `false`: those are values somebody meant.",
        "default(header.locale, \"en\") -> locale"}},
      {"to_chunk",
       {"A value written as a chunk in one mimetype.",
        "the value, and the mimetype to write it as",
        "What an action expecting a fragment of a particular type is handed. "
        "The mimetype may name a registered type, as in "
        "`\"application/json;type=a11.sdk.Interaction\"`.",
        R"(to_chunk({"type": "text", "text": said}, "application/json"))"}},
      {"from_chunk",
       {"The value inside a chunk.", "one chunk",
        "Anything already decoded comes back as it is, so this is safe to "
        "write "
        "over a value that may or may not have arrived as a chunk.",
        "from_chunk(it) -> decoded"}},
      {"strformat",
       {"A string with values written into it.",
        "a format string, and the values it names",
        "printf's conversions: `%s` for anything, `%d` and `%.2f` for numbers, "
        "`%x` for hexadecimal, `%%` for a literal per cent. `%2$s` picks an "
        "argument by position, and a duration or an instant may carry its own "
        "format in parentheses, as in `%(%H:%M)s`. A conversion with no "
        "argument for it is left in the output as written, because a visible "
        "`%3$s` is easier to diagnose than a flow that died formatting a log "
        "line.",
        "strformat(\"%s took %s\", name, elapsed) -> line"}},
      {"b64encode",
       {"A value as base64 text.", "one value",
        "Encoding gives text, which is the whole point of it: text is what a "
        "JSON field or a header can carry. Padding is written.",
        "b64encode(page.bytes) -> encoded"}},
      {"b64decode",
       {"The bytes that base64 text encodes.", "one value",
        "Gives bytes, because bytes are what was encoded; write "
        "`text(b64decode(x))` where they were a string. Padding is not "
        "required "
        "on the way back. Text that is not base64 ends the flow with "
        "`invalid_argument`.",
        "b64decode(header.token) -> raw"}},
      {"b64urlencode",
       {"A value as base64 text, in the web-safe alphabet.", "one value",
        "`-` and `_` where `b64encode` writes `+` and `/`, which is what a URL "
        "or a JWT carries.",
        "b64urlencode(claims) -> segment"}},
      {"b64urldecode",
       {"The bytes that web-safe base64 text encodes.", "one value",
        "Padding is not required, since the web-safe alphabet is routinely "
        "sent "
        "without it.",
        "b64urldecode(segment) -> claims"}},
      {"now",
       {"The instant this is evaluated.", "",
        "Read once, where it is written. An expression naming it twice may see "
        "two different instants, so a flow that wants one instant names it "
        "once with a `let`.",
        "let started = now()"}},
      {"duration",
       {"A value as a duration.", "one value",
        "Text is parsed the way the language writes one: `500ns`, `250ms`, "
        "`30s`, `5m`, `1h`, and compounds like `1m30s`. A bare number is a "
        "count of seconds.",
        "duration(header.budget) -> budget"}},
      {"time",
       {"A value as an instant.", "one value",
        "RFC 3339, with or without a zone, and a bare date. Text with no zone "
        "is read as UTC, which is the zone every instant this language writes "
        "is in.",
        "time(page.published) -> published"}},
      {"seconds",
       {"A duration as a number of seconds.", "one duration",
        "A double, so a fraction of a second survives. This is how a duration "
        "reaches an action whose port wants a number.",
        "seconds(now() - started) -> elapsed"}},
  };
  return *table;
}

// --- the rest of the language -----------------------------------------------
//
// Everything below documents the forms an editor used to have nothing to say
// about: a hover on `|` answered "flow operator", which is the token's kind
// rather than an answer, and a hover on `in` answered "declaration keyword".
//
// Same house style as the two tables above, and the same sources of truth: what
// a form *admits* is `parser.cc`, what it *does* is `runtime.cc`, and the prose
// agrees with `a11/sdk/flow_tools/SKILL.md`, which is the reference a model is
// given. Where any of those disagrees with a line here, this is the bug.
//
// One table per role rather than one keyed by the word, because a word means
// different things in different positions. A word genuinely in two sets has one
// [WordDoc] named here and referenced from each table that lists it, so the two
// cannot drift apart.

constexpr WordDoc kStreamDoc = {
    "Says the port carries many values rather than one.", "",
    "Everything moving through a flow is a stream, and this is how a port says "
    "its own side of the boundary is too. Without it the port is one value: a "
    "caller sends a single value rather than a list, and a second value "
    "arriving ends the flow with `invalid_argument` rather than passing "
    "unnoticed. That promise is also what lets the language *share* such a "
    "stream between readers instead of making them take turns on it.",
    "out briefs: string stream \"One instruction per investigation.\""};

constexpr WordDoc kRequiredDoc = {
    "Says the port or field has to be there.", "",
    "On a port, a caller that sends nothing is refused before the flow runs. "
    "On "
    "a `struct` field, a record that leaves it out is not a value of that "
    "shape. A port that is not required and receives nothing is simply an "
    "empty "
    "stream, which a `let` of it and `if not` is how to ask about.",
    "in topic: string required \"What to research.\""};

// The two forms written as two words. Each is documented under its full
// spelling, which is how the ordered lists carry it, and under each word on its
// own, which is what a hover on one of them asks for.
constexpr WordDoc kOneOfDoc = {
    "The values the field is allowed to hold.", "a list of literals",
    "Written `one of [..]`, which is what another language calls an "
    "enumeration. A value outside the list is not a value of the shape, and "
    "the "
    "list travels with the field, so something completing it knows what to "
    "offer.",
    R"(role: string one of ["user", "assistant"])"};

constexpr WordDoc kForwardHeadersDoc = {
    "Passes on headers this flow was called with, as they arrived.",
    "one or more names, `*` matching a family",
    "Every `x-a11-` header already reaches a step, so this is for the others: "
    "what a gateway or a caller set that the flow itself has no opinion about. "
    "An explicit `with` of the same name wins.",
    R"(llm = run ask_model(..) forward headers "x-tenant", "x-trace-*")"};

constexpr WordDoc kNodesDoc = {
    "Declares a node map, which keeps a step's traffic local.",
    "a name, and optionally a block",
    "Nodes made inside the block, and steps named `via` it, keep their streams "
    "on this side instead of replicating them to whoever dispatched the "
    "composition. That is the difference between fetching three pages here and "
    "sending three pages across the wire. A map nothing is placed in, by a "
    "block or by a `via`, is reported.",
    "nodes fetched { for hit in search.hits { .. } }"};

const absl::flat_hash_map<std::string_view, WordDoc>& StatementDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"run",
       {"Dispatches an action where this flow is running.",
        "an action, its arguments in parentheses, and any modifiers",
        "Needs a handler registered in the runtime the flow runs in, and keeps "
        "the step's nodes off the wire. A `run` of something with no handler "
        "is "
        "refused rather than quietly sent to the peer, which is what `call` is "
        "for. Either verb may name another flow of the same file, in any order "
        "and with nothing registered for it, which is how one composition is "
        "factored into several. Naming the step binds it: `x.port` reads its "
        "outputs and `status x` its outcome.",
        "search = run web-search(query: question, limit: 3)"}},
      {"call",
       {"Dispatches an action on the stream this flow is attached to, for the "
        "peer to run.",
        "an action, its arguments in parentheses, and any modifiers",
        "Needs nothing registered on this side, and the step's nodes cross the "
        "wire, which is what a `nodes` block or a `via` is for. `run` is the "
        "other verb, for an action this side has a handler for.",
        "said = call text-upper(text: words)"}},
      {"try",
       {"Lets a step or a block fail without ending the flow.",
        "a `run`, a `call`, or a block",
        "Without it a failing step ends the whole flow. With it the failure "
        "becomes a value: `wait x` or `status x` says how it went, and the "
        "ports the step feeds are left empty. A `try` whose status nothing "
        "reads is reported, because a failure then leaves those ports silently "
        "empty and nothing says why.",
        "page = try run web-fetch(url: hit.url)"}},
      {"let",
       {"Names one value of a stream, where everything else here is a stream.",
        "one or more names, and the stream to read",
        "The name then stands where an expression does: `if code >= 200 and "
        "code < 300`, `strformat(\"%d\", code)`, `code == other`. It is also a "
        "stream of one wherever a source goes, which is the other direction. "
        "Lazy, so nothing is read until the name is, and it may be written "
        "where it reads best rather than where the value is first needed; one "
        "nothing reads is reported. An empty stream binds nothing, which `if "
        "not code` asks about. Several names take one value apart, by field or "
        "by position. `advance` moves the name on to the next value.",
        "let code = http.status_code"}},
      {"advance",
       {"Rebinds a `let` value to the next value of the same stream.",
        "a name a `let` bound",
        "How a flow reads several values of one stream one at a time and knows "
        "which is which. The guarantee is positional rather than an ordering, "
        "so the *k*th binding of a name is the *k*th value of its stream "
        "however the flow is scheduled, and it holds without a barrier. "
        "Statements written above the `advance` keep the value they were "
        "resolved against, which is what makes the name read top to bottom. "
        "Advancing past the end binds nothing, exactly as a `let` on an empty "
        "stream does.",
        "advance word"}},
      {"skip",
       {"Reads a stream to its end and keeps nothing.",
        "a stream, a count and a port, or several of either, separated by ','",
        "Every output of every step is read whether the flow names it or not, "
        "so this changes nothing the runtime would not have done: it says "
        "plainly that an output is not wanted, and is worth writing where the "
        "output is large. `skip n port` is the other form and a different "
        "thing, dropping the port's first `n` values for *every* reader of it, "
        "which is how a header row stops being everybody's problem; several of "
        "them naming one port add up. A bare call skips every one of its "
        "outputs, and `skip o1, o2 of act` (or `skip (o1, o2) of act`) skips "
        "just those; several subjects may share one `skip`, across lines.",
        "skip search.debug"}},
      {"wait",
       {"Holds until a step, node, port or barrier has finished.",
        "a subject, and optionally `timeout`",
        "Naming it binds how it went, as a status record, which is how a "
        "`try`'s failure is read. This is also the way to say that a statement "
        "is the last word about a step rather than a line racing it, though "
        "`after` says the same thing without a name. Waiting twice in one body "
        "is over as soon as it starts the second time, and is reported.",
        "done = wait llm timeout 30s"}},
      {"drain",
       {"Ends a node and says how it ended.", "a node",
        "Writes both of the two facts that end a stream: the node is marked "
        "*final*, so an ordered reader stops, and the writer is *closed*, so "
        "the store admits nothing more. Then it reads what is left, so every "
        "reader sees the end rather than waiting on a stream nobody will write "
        "to again. The name binds the outcome, as `wait` does. `abort` is the "
        "other ending, for a stream that failed rather than finished.",
        "ended = drain findings"}},
      {"abort",
       {"Ends a node with a failure rather than with an end.",
        "a node, and optionally a code and a message",
        "The other ending a stream can have. `drain` marks a node final and "
        "closes it, which says the stream is over; this aborts it, which says "
        "it went wrong. A reader cannot otherwise tell the two apart, and a "
        "stream cut short by something the flow noticed looks exactly like one "
        "that finished. Takes the same code and message a `fail` does, and "
        "waits for nothing for the same reason, so it belongs in an `if` or a "
        "loop body or carries an `after`.",
        "if not status page.ok { abort findings unavailable \"gone\" }"}},
      {"cancel",
       {"Asks a step to stop.", "a step",
        "Waits for nothing, so at the top of a body it runs at once and races "
        "every other statement; it belongs in an `if` or a loop body, or "
        "carries an `after` saying what it waits for. Cancelling something "
        "this "
        "body already waited for leaves nothing to stop, and is reported.",
        "if code == 429 { cancel fetch }"}},
      {"fail",
       {"Ends the flow with a status.", "a code or a number, and a message",
        "`fail invalid_argument \"..\"`, `fail 5 \"..\"`, or `fail STATUS` to "
        "pass one on. Waits for nothing, so like `cancel` it belongs in an "
        "`if` or a loop body, or carries an `after`. One alone at the end of a "
        "body reads like a last resort and is refused, because it is in fact "
        "the first thing that would happen.",
        "fail invalid_argument \"the topic is empty\""}},
      {"log",
       {"Writes a line to the flow's log.",
        "optionally a level, and what to log",
        "The log is the flow's own: no port declares it, nothing has to drain "
        "it, and a flow that never logs pays nothing for it. Waits for "
        "nothing, "
        "so like `fail` it belongs in an `if` or a loop body, or carries an "
        "`after` saying what it waits for; one at the top of a body would run "
        "before the thing it is describing.",
        "log \"searching\" after plan"}},
      {"logf",
       {"Writes a formatted line to the flow's log.",
        "optionally a level, a format, and the values to fill it with",
        "The format is `strformat`'s. Waits for nothing, so like `log` it "
        "needs "
        "an `if`, a loop body, or an `after`.",
        "logf \"found %s results\" count after search"}},
      {"if",
       {"Runs a block when a condition holds, and another when it does not.",
        "a condition, a block, and optionally `else`",
        "The condition is asked the question `bool()` asks: null, `false`, 0, "
        "an empty string, an empty list and an empty record are false, and "
        "everything else is true. A `let` that bound nothing is false too, "
        "which is how `if not code` asks whether a stream was empty. A "
        "condition here blocks only what is in the braces.",
        "if code >= 200 and code < 300 { page.text -> body }"}},
      {"for",
       {"Runs a block once per value of a stream.",
        "one or more names, a stream, and optionally `parallel`",
        "Several names take a tuple apart, which is what `for x, y in zip(a, "
        "b)` reads. A pass that reads none of its variables is reported. This "
        "is one pass over one stream: a stream of lists is still one value per "
        "pass, and flattening one is an action's job. `until`/`while` in the "
        "body ends it early, as it ends a `repeat`; a `for` carries nothing "
        "between passes, so `<-` is a `repeat`'s and not this one's. Bound to "
        "a "
        "name it reads as its own outcome, which is how a flow says what "
        "happens once the loop is over: `drain taken after done`.",
        "for hit in search.hits parallel 2 { .. }"}},
      {"repeat",
       {"Runs a block again and again, carrying a value from each pass to the "
        "next.",
        "a name and what it starts as, and optionally `max`",
        "Bound to a name it reads as its own outcome, as a `for` does. "
        "`name <- source` inside the body is what the next pass starts from. A "
        "`repeat` needs an `until`/`while`, or a `max n`, or both: there is no "
        "default bound, and a loop with nothing ending it is refused rather "
        "than quietly stopping after some number of passes and calling that "
        "success.",
        "repeat asked = question max 4 { .. }"}},
      {"until",
       {"Ends a `repeat` or a `for` when a condition holds.", "a condition",
        "Asked at the tail of a pass, so the body always runs at least once "
        "however the condition starts out. `while` is the same statement with "
        "the question the other way round. In a `for` it is how a loop over a "
        "stream stops before the stream does; it stops reading, which is what "
        "`| first n` does, and does not cancel whatever was producing.",
        "until len(answer) > 0"}},
      {"while",
       {"Keeps a `repeat` or a `for` going while a condition holds.",
        "a condition",
        "Asked at the tail of a pass, like `until`, so the body always runs at "
        "least once however the condition starts out.",
        "while not found"}},
      {"nodes", kNodesDoc},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& DeclarationDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"flow",
       {"Declares a composition, which is itself an action.",
        "a name, and a block",
        "The name is what a caller dispatches, and what another flow of the "
        "same file may `run` or `call`. Its `in` and `out` ports are its whole "
        "interface: a caller sees those and nothing else of what is inside, "
        "which is what makes the intermediate values of a composition free.",
        "flow answer-from-the-web { .. }"}},
      {"struct",
       {"Declares a shape: a record with named, typed, constrained fields.",
        "a name, and a block of fields",
        "A port may be typed with it and a value may be made into one, "
        "defaults and all. A shape this file declares outranks a serialisation "
        "tag of the same name, because what the file says about a name is what "
        "the file means by it, and a shape may hold and be held by another. "
        "One "
        "nothing names is reported. A file of nothing but shapes is a file of "
        "types, which is a reasonable thing to write.",
        "struct Finding { url: string required }"}},
      {"describe",
       {"What a flow or a shape is for.", "a string",
        "Read by everything that lists what may be composed, so this is what a "
        "caller, a tool listing or a model is told the thing does. A "
        "`\"\"\"..\"\"\"` string holds line breaks and gives back the "
        "indentation the source put in front of it, so a paragraph needs no "
        "escaping.",
        "describe \"Search, read the best hits, and answer from them.\""}},
      {"in",
       {"Declares an input port: what a caller sends.",
        "a name, a type, any of `stream`/`required`, and a description",
        "Without `stream` the port carries one value. The description travels "
        "with the port, so it is what a caller reading the interface sees, and "
        "what a model choosing whether to call this reads.",
        "in question: string required \"What to find out.\""}},
      {"out",
       {"Declares an output port: what a caller reads back.",
        "a name, a type, any of `stream`/`required`, and a description",
        "These are what cross back, so they are the part worth keeping small: "
        "an output carrying every fetched page is the pages paid for after "
        "all. "
        "Nothing in the flow writing to a declared output is reported, because "
        "a caller reading it would get nothing.",
        "out answer: string \"The answer, as it is written.\""}},
      {"header",
       {"Reads one of the headers this flow was called with.",
        "a header name, `as` and an alias, and optionally `default`",
        "Every `x-a11-` header reaches a step anyway, so a declaration here is "
        "for *reading* the value inside the flow: the alias then stands where "
        "an expression does. An alias nothing uses is reported, since the "
        "header would have travelled either way.",
        R"(header "x-a11-locale" as locale default "en")"}},
      {"as",
       {"Names the thing just declared, or the type a value is made into.",
        "an alias, or a type",
        "In a `header` it gives the alias the flow reads the value by. In an "
        "expression `value as Type` makes the value that type, partial in and "
        "valid out. That is the other way of writing `Type{..}`, and the way "
        "that works where a `{` would open a block.",
        R"({...it, "role": "user"} as a11.sdk.Interaction)"}},
      {"default",
       {"What a header falls back to when the caller sent none.", "a literal",
        "Written out rather than computed: this is read before anything in the "
        "body runs, so there is nothing yet for an expression here to read. A "
        "`struct` field's `default` is the same word doing the same job one "
        "level down.",
        R"(header "x-a11-locale" as locale default "en")"}},
      // `stream` and `required` are in [OrderedDeclarations] because a port
      // declaration is where they are offered, but they are documented in
      // [PortModifierDocs] rather than here: what they modify is the port, and
      // "a declaration" is the wrong word above the summary. The lookup chain
      // for a declaration keyword reaches the modifier tables after this one.
      {"node",
       {"Makes a stream of the flow's own.",
        "optionally an id, and optionally `in` a map",
        "Somewhere to write values that no step's port is the right home for: "
        "several statements may write to one, and its readers see the values "
        "as "
        "they arrive. The keyword only where its parentheses open, so a port "
        "called `node` is a name like any other. `n.id` is its id, which is "
        "what a caller filling it from outside the flow needs.",
        "findings = node()"}},
      {"nodes", kNodesDoc},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& ClauseDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"else",
       {"The block an `if` runs when its condition does not hold.", "a block",
        "A condition blocks only what is in the braces, which is what a block "
        "is for: the statements outside it are not waiting on the question.",
        "if ok { page.text -> body } else { \"\" -> body }"}},
      {"parallel",
       {"How many passes of a `for`, or values of a stage, run at a time.",
        "a count",
        "Without it they run one after another. The number is a ceiling rather "
        "than a target, and it bounds the passes rather than the work inside "
        "one: a pass waiting on a step it dispatched is still one of the `n`. "
        "On a stage it says how many values may be in hand at once, and what "
        "follows still reads them in order.",
        "for hit in search.hits parallel 2 { .. }"}},
      {"unordered",
       {"Lets a parallel stage publish its values as they finish.", "",
        "A parallel stage otherwise puts the stream back in the order it read "
        "it, which is what makes `parallel` safe to add to a pipeline nobody "
        "else changed. This gives that up for whatever it saves, so it is "
        "worth writing only where the consumer does not care.",
        "urls | map fetch(it) parallel 8 unordered -> bodies"}},
      {"by",
       {"What to compare, where a stage orders values.",
        "an expression, with "
        "`it` bound to the value",
        "Only `sort` takes one. Without it the values compare as themselves, "
        "which is what a stream of numbers or of text wants.",
        "hits | sort by it.score desc -> ranked"}},
      {"desc",
       {"Reverses the order a `sort` puts values in.", "",
        "Largest first, by the same comparison `>` uses. Ascending is the "
        "default and has no word of its own.",
        "hits | sort by it.score desc -> ranked"}},
      {"into",
       {"Where a `try` stage sends a value it could not do.",
        "a node or an "
        "out-port",
        "The failure arrives as a status record, the same shape `status x` "
        "yields, so a stream of failures is an ordinary stream. Without it a "
        "tolerated failure is logged at warning and the value is dropped.",
        "docs | try map parse(it) into bad -> good"}},
      {"max",
       {"The most passes a `repeat` may make.", "a count",
        "One of the two things that may end a `repeat`, and either is enough: "
        "with no `until`/`while` and no `max` a loop is refused. With both, "
        "whichever comes first ends it.",
        "repeat asked = question max 4 { .. }"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& ModifierDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"tee",
       {"Gives every reader of the step's outputs its own copy.", "",
        "Without it the readers of one output take turns on one view of it, so "
        "two of them see two different values rather than the same value "
        "twice. With it each reader gets the whole stream, at the cost of "
        "holding what the slowest has not read yet.",
        "page = run web-fetch(url: url) tee"}},
      {"via",
       {"Puts the step's nodes in a node map.", "a map name",
        "The other half of `nodes`: a step named `via` a map keeps its streams "
        "on this side instead of replicating them to whoever dispatched the "
        "composition. A map nothing is placed in is reported.",
        "page = call web-fetch(url: url) via fetched"}},
      {"timeout",
       {"How long the step may take before it is given up on.", "a duration",
        "Reaching it fails the step with `deadline_exceeded`, which ends the "
        "flow unless the step was a `try`. A negative duration is infinite, "
        "which is this language's way of writing no bound rather than a bound "
        "in the past.",
        "llm = run ask_model(interactions: asked, config: {}) timeout 90s"}},
      {"after",
       {"What has to have finished before the step starts.",
        "one or more steps, ports or nodes",
        "Order in a flow comes from the data, so this is for the order the "
        "data does not imply: a log line that should read as the last word "
        "about a step rather than a line racing it. It is also what lets a "
        "`fail` or a `cancel` stand at the top of a body, by saying what it "
        "waits for. A named loop is a step like any other, so `after done` is "
        "how the rest of a flow waits for one.",
        "strformat(\"[done] %s\", brief) -> user_log after done"}},
      {"with",
       {"Headers to send on this step.", "one or more names and their values",
        "The value is an expression rather than a literal, so a header may "
        "carry something the flow worked out. Every `x-a11-` header this flow "
        "was called with already reaches a step; `forward headers` is for "
        "passing on the others, and an explicit `with` of the same name wins "
        "over it.",
        "llm = run ask_model(..) with \"x-a11-provider\": provider"}},
      {"id",
       {"The id the step's node is made with.", "an expression",
        "What a caller outside the flow needs in order to write to or read "
        "from "
        "that node, which is how a client with a session of its own fills a "
        "port while the flow runs rather than before it starts.",
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
       {"The outcome of a call, a node or a barrier, as a record.", "a subject",
        "`{\"ok\": .., \"code\": .., \"number\": .., \"message\": ..}`. "
        "Reading one waits for the subject to finish, which makes it a barrier "
        "as well as a value. `wait x` is the other way to the same record, and "
        "gives it a name.",
        "if not status fetch.ok { \"\" -> body }"}},
      {"zip",
       {"Reads several streams in step, as one stream of tuples.",
        "two or more streams",
        "Read as `it[0]` and `it[1]`, or taken apart by `for x, y in zip(a, "
        "b)`. A source that ends *well* contributes a null to every tuple "
        "after "
        "it, so the longer stream is still read to its end; one that ends with "
        "an error ends the iteration with that status. It stops when every "
        "source has, and it is a stream like any other, so `wait`, `drain`, "
        "`| first n` and `| count` all work on one.",
        "for url, body in zip(urls, bodies) { .. }"}},
      {"interleave",
       {"Reads several streams at once, as one stream of their values.",
        "two or more streams",
        "Each value goes on as it arrives, so a fast stream is not held behind "
        "a slow one and the order between the sources is whatever the values "
        "did. `zip` is the other shape: one tuple per round, in step. A source "
        "that ends well is simply done; one that ends with an error ends the "
        "stream with that status.",
        "interleave(tokens, progress) -> events"}},
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
       {"Says a list field holds no value twice.", "",
        "Checked when a value is made into the shape, so a record with a "
        "repeat "
        "in that field is not a value of it. Says nothing about the order of "
        "the list.",
        "tags: list[string] unique"}},
      {"matching",
       {"A regular expression the field's text has to fit.",
        "a pattern, as one string literal",
        "The argument is a literal rather than an expression, since a name "
        "there could not be told from the description that may follow it. A "
        "value that does not fit is not a value of the shape.",
        "url: string matching \"^https?://\""}},
      {"one of", kOneOfDoc},
      {"one", kOneOfDoc},
      {"of", kOneOfDoc},
      {"default",
       {"What the field is when a record does not name it.", "a literal",
        "Filled in when a value is made into the shape, so a reader of the "
        "shape never sees the field missing. A field with a default is not "
        "`required`: the two say opposite things about a record that leaves it "
        "out.",
        "limit: integer default 10"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& TypeDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"string",
       {"Text.", "",
        "The type most ports have. It crosses as `text/plain`, which is what a "
        "caller sending one sends. `text` is the same type under the other "
        "name.",
        "in question: string required"}},
      {"text",
       {"Text. The other spelling of `string`.", "",
        "The same type: both are here because both read naturally, and a file "
        "may use whichever fits the sentence it is in.",
        "out answer: text"}},
      {"number",
       {"A number, whole or not.", "",
        "A whole number stays whole and a decimal stays decimal. `integer` is "
        "the one that admits only whole numbers.",
        "in temperature: number"}},
      {"integer",
       {"A whole number.", "",
        "`int` is the same type. A value with a fraction is not one.",
        "in limit: integer"}},
      {"int",
       {"A whole number. The other spelling of `integer`.", "",
        "The same type. A value with a fraction is not one.", "in limit: int"}},
      {"bool",
       {"True or false.", "",
        "`boolean` is the same type. What counts as true where a *condition* "
        "asks is a wider question, and `bool()` is what answers it.",
        "in verbose: bool"}},
      {"boolean",
       {"True or false. The other spelling of `bool`.", "", "The same type.",
        "in verbose: boolean"}},
      {"duration",
       {"A length of time.", "",
        "Written `500ns`, `250ms`, `30s`, `2m`, `1h`, or compound as `1m30s`. "
        "JSON has no word for one, so it crosses as a string carrying the "
        "format that says how to read it, which is what makes the round trip "
        "lossless.",
        "in budget: duration"}},
      {"time",
       {"An instant.", "",
        "RFC 3339, and every instant this language writes is in UTC. JSON has "
        "no word for one either, so it crosses as a string with the format "
        "beside it.",
        "out published: time"}},
      {"object",
       {"A record: keys with values.", "",
        "May say what it maps, as `object[string]`. `json` is the same type "
        "under the name a caller thinks in. A `struct` is the version with "
        "named, typed, constrained fields, and says far more to everything "
        "reading the interface.",
        "out settings: object"}},
      {"json",
       {"A record or a list: whatever JSON can carry.", "",
        "`object` is the same type. A value holding `bytes` anywhere in it "
        "cannot go through `| json`, which has nothing to carry them in; "
        "`| packb` can.",
        "out result: json"}},
      {"list",
       {"Several values in order, as one value.", "",
        "May say what it holds, as `list[string]` or `list[a11.NodeFragment]`, "
        "and `T[]` is the same thing. A list is *one* value: a port of "
        "`list[string]` carries one list and a port of `string stream` carries "
        "many strings, which are different things.",
        "in frames: list[a11.NodeFragment] stream"}},
      {"array",
       {"Several values in order. The other spelling of `list`.", "",
        "The same type, and it may say what it holds in the same way.",
        "in tags: array[string]"}},
      {"bytes",
       {"Raw bytes.", "",
        "Crosses as `application/octet-stream`. A shape holding bytes anywhere "
        "in it cannot go through `| json`; `| packb` can.",
        "out audio: bytes stream"}},
      {"any",
       {"Whatever arrives.", "",
        "The escape hatch, for a port that really does carry anything: nothing "
        "is checked and nothing is converted. A named type, a shape or a "
        "quoted "
        "mimetype says more, and everything reading the interface benefits "
        "from "
        "it, so this is worth a second thought.",
        "in payload: any"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& ConstantDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"true",
       {"The true value.", "",
        "One of the two `bool` literals. Every condition in the language asks "
        "one question of its value, and this is one of the two that answer it "
        "directly.",
        "accept_pushes: bool default true"}},
      {"false",
       {"The false value.", "",
        "One of the two `bool` literals. Not the same as empty: 0, an empty "
        "string and an empty list also count as false where a condition asks, "
        "but `default()` leaves them alone because they are values somebody "
        "meant.",
        "reuse_connection: bool default false"}},
      {"null",
       {"Nothing.", "",
        "What a field that is not there reads as, what `match` gives for a "
        "line "
        "its pattern does not fit, and what a `zip` source that has ended "
        "contributes to every tuple after it. Counts as false where a "
        "condition "
        "asks.",
        "if part.title == null { \"untitled\" -> title }"}},
      {"it",
       {"The value in hand, inside a stage that takes an expression.", "",
        "Bound by `where`, `map` and `group`, and by nothing else: outside one "
        "of those there is no value in hand for it to mean. It is a whole "
        "value, so `it.url` and `it[0]` read into it.",
        "hits | where it.ok | map it.url"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& OperatorWordDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"and",
       {"True when both sides are.", "two conditions",
        "Each side is asked the question a condition is asked, so a plain "
        "value "
        "stands here as well as a comparison: `if code and body` is about "
        "whether each of them is there at all.",
        "if code >= 200 and code < 300 { .. }"}},
      {"or",
       {"True when either side is.", "two conditions",
        "Each side is asked the question a condition is asked, as with `and`.",
        "if not code or code >= 500 { cancel fetch }"}},
      {"not",
       {"True when what follows is not.", "a condition",
        "The one that reads a `let` which bound nothing: `if not code` asks "
        "whether that stream was empty, since a name that bound nothing counts "
        "as false.",
        "lines | where not starts-with(it, \"FINALLY:\")"}},
  };
  return *table;
}

// The codes are Abseil's, and so are these meanings. The value of documenting
// them here is the pairs a reader confuses: `permission_denied` against
// `unauthenticated`, `failed_precondition` against `invalid_argument`, and
// which of them a *flow* itself can raise.
const absl::flat_hash_map<std::string_view, WordDoc>& LogLevelDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"debug",
       {"Detail for whoever is working on the flow.", "",
        "The level to reach for when the line is only interesting while "
        "something is wrong. A consumer showing a user what a flow is doing "
        "normally leaves these out.",
        "log debug it after step"}},
      {"info",
       {"What the flow is doing, as a person would describe it.", "",
        "The level a `log` with no level written takes, and the one a "
        "narration "
        "meant for a reader belongs at.",
        "log info \"searching\" after plan"}},
      {"warning",
       {"Something is off, and the flow is carrying on.", "",
        "A retry, a partial result, an input that had to be corrected: the "
        "flow "
        "still produces what it promised.",
        "log warning \"retrying\" after fetch"}},
      {"error",
       {"Something failed, whether or not the flow did.", "",
        "A step that failed inside a `try`, or a value that had to be dropped. "
        "Logging this does not end the flow: `fail` is what does that.",
        "log error status fetch.message after fetch"}},
      {"critical",
       {"The worst the log has to say.", "",
        "Kept apart from `error` for consumers that separate the two; A11 "
        "reports both at its own error severity, so nothing is aborted by "
        "naming it.",
        "log critical \"the index is gone\" after check"}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& StatusCodeDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"ok",
       {"Nothing went wrong.", "",
        "The code a status carries when it succeeded. `status x.ok` is the "
        "shorter way to ask, and the usual one.",
        "if status fetch.code == \"OK\" { .. }"}},
      {"cancelled",
       {"The work was asked to stop.", "",
        "What `cancel` leaves behind, and what a caller that dropped the call "
        "produces. Not a failure of the work so much as a decision about it.",
        "fail cancelled \"the caller went away\""}},
      {"unknown",
       {"Something went wrong that does not fit another code.", "",
        "The one to reach for last: a code that says nothing is a code nothing "
        "can act on.",
        "fail unknown \"the provider said no more than that\""}},
      {"invalid_argument",
       {"What was sent is not something this can work with.", "",
        "The language's own, as well as an action's: a port that did not say "
        "`stream` receiving a second value fails this way, as does a value "
        "that "
        "is not of the shape its port names. `failed_precondition` is the "
        "neighbour to tell it from, where what was sent is fine and the world "
        "is not ready for it.",
        "fail invalid_argument \"the topic is empty\""}},
      {"deadline_exceeded",
       {"The time allowed ran out.", "",
        "What a `timeout` modifier leaves behind, and what the "
        "`x-a11-deadline` "
        "header produces when it passes. Says nothing about whether the work "
        "would have succeeded given longer.",
        "fail deadline_exceeded \"the model did not answer in time\""}},
      {"not_found",
       {"What was asked for is not there.", "",
        "About one named thing. `out_of_range` is the one for a position past "
        "the end of something that is there.",
        "fail not_found \"no page at that url\""}},
      {"already_exists",
       {"What was to be created is there already.", "",
        "The mirror of `not_found`, for the direction that creates.",
        "fail already_exists \"that conversation has been recorded\""}},
      {"permission_denied",
       {"The caller is known, and is not allowed this.", "",
        "`unauthenticated` is the pair to tell it from: not allowed, against "
        "not identified. Retrying changes nothing without a change of "
        "authority.",
        "fail permission_denied \"that path is outside the project\""}},
      {"resource_exhausted",
       {"Something ran out: a quota, a rate limit, room to work in.", "",
        "Often worth retrying later, which is what tells it from "
        "`failed_precondition`.",
        "fail resource_exhausted \"the provider is rate limiting\""}},
      {"failed_precondition",
       {"The system is not in a state where this can be done.", "",
        "Distinct from `invalid_argument`: what was sent is fine, and the "
        "world "
        "is not ready for it. Retrying helps only once something else has "
        "changed.",
        "fail failed_precondition \"no gateway is attached\""}},
      {"aborted",
       {"The work was given up on part-way.", "",
        "Usually a conflict rather than a fault, so retrying the whole thing "
        "may well work.",
        "fail aborted \"another writer took the node\""}},
      {"out_of_range",
       {"What was asked for is past the end.", "",
        "About a position in something that exists. `not_found` is the one for "
        "a thing that does not.",
        "fail out_of_range \"that line is past the end of the file\""}},
      {"unimplemented",
       {"This is not something the receiver does at all.", "",
        "Not a failure of the request but of the pairing: a `call` of an "
        "action "
        "the peer does not have. No amount of retrying changes it.",
        "fail unimplemented \"this peer has no audio capture\""}},
      {"internal",
       {"Something broke that should not have.", "",
        "An invariant, rather than anything about the request. A flow raising "
        "this is saying the fault is on this side.",
        "fail internal \"the plan came back with no briefs\""}},
      {"unavailable",
       {"The receiver cannot be reached, or cannot take work right now.", "",
        "The retryable one: the request was fine and the moment was not.",
        "fail unavailable \"the gateway is not answering\""}},
      {"data_loss",
       {"Something was lost or corrupted beyond recovery.", "",
        "The gravest of them, and the one that says the work should not simply "
        "be retried as though nothing happened.",
        "fail data_loss \"the recording is truncated\""}},
      {"unauthenticated",
       {"The caller is not identified.", "",
        "`permission_denied` is the pair to tell it from: not identified, "
        "against identified and not allowed.",
        "fail unauthenticated \"no api key was sent\""}},
  };
  return *table;
}

const absl::flat_hash_map<std::string_view, WordDoc>& StatusFieldDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"ok",
       {"Whether it succeeded.", "",
        "A bool, and the field most readers of a status want. Reading it waits "
        "for the subject to finish, as reading any part of a status does.",
        "if not status fetch.ok { \"\" -> body }"}},
      {"code",
       {"The canonical code, as a name.", "",
        "One of Abseil's, in upper case: `\"NOT_FOUND\"`. This is the part a "
        "gate should match on, because a code is stable and the wording of a "
        "message is not.",
        "if status fetch.code == \"DEADLINE_EXCEEDED\" { .. }"}},
      {"number",
       {"The canonical code, as its number.", "",
        "The same code as `code`, for a reader that would rather compare "
        "numbers than names.",
        "if status fetch.number == 5 { .. }"}},
      {"message",
       {"What went wrong, in words.", "",
        "Written for a person. A gate should read `ok` or `code` instead: a "
        "message's wording is not a contract and is not promised to stay as it "
        "is.",
        "strformat(\"fetch failed: %s\", status fetch.message) -> user_log"}},
  };
  return *table;
}

// Every unit shares one paragraph, because what is worth knowing is true of all
// six and nothing else about `ms` distinguishes it from `s`.
constexpr std::string_view kDurationDetail =
    "A duration is a value like any other: it may be compared, added to an "
    "instant or to another duration, and read as a number with `seconds()`. "
    "Units compound, as in `1m30s500ms`, and a negative duration means "
    "infinite rather than a length below zero.";

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
       {"Puts a stream through a stage.",
        "a stream on the left, a stage on the right",
        "The stages chain, and each one reads what the one before it produced: "
        "`hits | where it.ok | first 3` is three stages and one pass. A stage "
        "that shrinks a stream does so before the next step ever sees the "
        "values, which is what makes `| truncate` cheap. `then` and `where` "
        "may "
        "drop the `|` where they have an operand; every other stage keeps it.",
        "page.text | truncate 2000 -> brief.pages"}},
      {"->",
       {"Writes a stream into one or more destinations.",
        "a pipeline on the left, ports or nodes on the right",
        "Several destinations are written the same values. Two statements "
        "writing to one destination interleave by arrival, which is what "
        "`| then` is for when one lot has to come before another. The left "
        "side "
        "is a whole pipeline, so the stages happen before anything is written.",
        "brief.summary -> answer"}},
      {"<-",
       {"Carries a value into the next pass of a `repeat`.",
        "a `repeat`'s name on the left, a stream on the right",
        "What the loop starts the next pass from, which is how a `repeat` "
        "carries state without a node. Only inside a `repeat` body: nowhere "
        "else is there a next pass for it to mean.",
        "asked <- llm.text_output"}},
      {"=",
       {"Binds a name to a step, a node, or a value.",
        "a name on the left, what it is on the right",
        "The name is then what reads the thing: `x.port` for a step's outputs, "
        "`status x` for its outcome, the name itself for a node or a `let` "
        "value. One `=` is a binding; `==` is the comparison, and a statement "
        "word before a single `=` is read as a name rather than a keyword, so "
        "`run = run x()` means what it says.",
        "search = run web-search(query: question)"}},
      {"==",
       {"Whether two values are equal.", "two values",
        "Compares what the values are rather than how they were written, so a "
        "number is a number whichever way it arrived. `=` is the binding, "
        "which "
        "is the slip this is worth knowing about.",
        "lower(header.value) == \"application/json\""}},
      {"!=",
       {"Whether two values differ.", "two values",
        "The negation of `==`, over the same comparison.",
        "if part.mime != \"text/html\" { .. }"}},
      {"<",
       {"Whether the left value is less than the right.", "two values",
        "Numbers, durations and instants compare as you would expect; strings "
        "compare by their bytes.",
        "if len(report) < 12 { fail invalid_argument \"too short\" }"}},
      {"<=",
       {"Whether the left value is less than or equal to the right.",
        "two values", "As `<`, including the equal case.",
        "if elapsed <= budget { .. }"}},
      {">",
       {"Whether the left value is greater than the right.", "two values",
        "Numbers, durations and instants compare as you would expect; strings "
        "compare by their bytes.",
        "if number(header.retries) > 3 { cancel fetch }"}},
      {">=",
       {"Whether the left value is greater than or equal to the right.",
        "two values", "As `>`, including the equal case.",
        "if code >= 200 and code < 300 { .. }"}},
      {"+",
       {"Adds two values.", "two numbers, or an instant and a duration",
        "Numbers, durations, and an instant plus a duration, which gives an "
        "instant. A bare number on either side of a duration counts as "
        "seconds. There is no arithmetic beyond `+` and `-`: real computation "
        "is what an action is for.",
        "let deadline = now() + 30s"}},
      {"-",
       {"Subtracts one value from another.",
        "two numbers, two instants, or an instant and a duration",
        "An instant minus an instant is a duration, which is how a flow times "
        "itself. It needs its spaces: `text-upper` is one name, so `a - b` and "
        "`a-b` are different things.",
        "seconds(now() - started) -> elapsed"}},
      {".",
       {"Reads a part of something.",
        "a value, a step or a node on the left, a name on the right",
        "A step's output port, a record's field, a status's field, or a node's "
        "`id`. Whatever follows it is a member however it is spelled, so a "
        "field called `text` is that field rather than the stage of the same "
        "name.",
        "page.text | truncate 2000 -> brief.pages"}},
      {"..",
       {"The range between two bounds.",
        "a low bound, a high bound, or one of them",
        "What a `struct` field is bounded by: a number, a duration or an "
        "instant, and the *length* of a string, a byte string or a list. "
        "Either "
        "end may be left off, as `1..` or `..200`.",
        "limit: integer 1..100"}},
      {"...",
       {"Everything the value after it holds, spread in here.",
        "a list inside a list, or a record inside a record",
        "`[...xs, y]` and `{...it, \"tags\": [..]}`, where a later key wins. "
        "`...` is the same thing. This is how a record is extended without "
        "writing out the fields already in it.",
        R"({...it, "role": "user"} as a11.sdk.Interaction)"}},
      {":",
       {"Separates a name from what it is.",
        "a name on the left, a type or a value on the right",
        "A port's type, a field's type, an argument's value, a record key's "
        "value, a `with` header's value. Past a port's `:` the whole of the "
        "rest is the type, dots and brackets included, which is what lets a "
        "type be written as a registry tag.",
        "in frames: list[a11.NodeFragment] stream"}},
      {",",
       {"Separates one thing from the next.", "",
        "Arguments, destinations, list items, record pairs, the names a `let` "
        "or a `for` takes a value apart with, and the subjects of an `after`.",
        "search = run web-search(query: question, limit: 3)"}},
      {"{",
       {"Opens a block, a record, or a value of a named type.", "",
        "Which one it is is decided by where it stands: after a `flow`, an "
        "`if`, a `for` or a `nodes` it opens a block of statements, and in an "
        "expression it opens a record. `Type{field: ..}` makes a value of that "
        "type, and is not written where a `{` would open a block: in an `if` "
        "or "
        "`for` header it goes in brackets.",
        "a11.sdk.Interaction{role: \"user\", content: [..]}"}},
      {"}",
       {"Closes a block, a record, or a value of a named type.", "",
        "The mate of `{`.", "if code >= 200 { page.text -> body }"}},
      {"(",
       {"Opens a call's arguments, or groups an expression.", "",
        "An action's or a function's arguments, a `node()`, or a parenthesised "
        "expression. A whole pipeline may stand in parentheses where a value "
        "belongs, which is how `(hits | count) > 0` asks about a stream.",
        "if (hits | count) > 0 { .. }"}},
      {")",
       {"Closes a call's arguments, or a grouped expression.", "",
        "The mate of `(`. A call's modifiers follow it: `via`, `timeout`, "
        "`after` and the rest.",
        "page = run web-fetch(url: hit.url) timeout 20s"}},
      {"[",
       {"Opens a list, an index, or a type's parameters.", "",
        "A list literal, an index into a value, or what a container type "
        "holds. "
        "A negative index counts from the end.",
        "for x, y in zip(a, b) { text(it[0]) -> shown }"}},
      {"]",
       {"Closes a list, an index, or a type's parameters.", "",
        "The mate of `[`. `T[]` after a type name is the same thing as "
        "`list[T]`.",
        "in frames: list[a11.NodeFragment] stream"}},
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
    std::string_view("any"),
};

// The functions, grouped the way the reference lists them: text, structure,
// then time.
constexpr std::array kBuiltinOrder = {
    std::string_view("len"),          std::string_view("lower"),
    std::string_view("upper"),        std::string_view("trim"),
    std::string_view("text"),         std::string_view("number"),
    std::string_view("bool"),         std::string_view("keys"),
    std::string_view("values"),       std::string_view("get"),
    std::string_view("join"),         std::string_view("split"),
    std::string_view("merge"),        std::string_view("contains"),
    std::string_view("starts-with"),  std::string_view("ends-with"),
    std::string_view("replace"),      std::string_view("match"),
    std::string_view("slice"),        std::string_view("default"),
    std::string_view("to_chunk"),     std::string_view("from_chunk"),
    std::string_view("strformat"),    std::string_view("b64encode"),
    std::string_view("b64decode"),    std::string_view("b64urlencode"),
    std::string_view("b64urldecode"), std::string_view("now"),
    std::string_view("duration"),     std::string_view("time"),
    std::string_view("seconds"),
};

// The verbs first, then the barriers, then the blocks: the order somebody
// scanning a list of statements finds what they meant.
constexpr std::array kStatementOrder = {
    std::string_view("run"),     std::string_view("call"),
    std::string_view("try"),     std::string_view("let"),
    std::string_view("advance"), std::string_view("skip"),
    std::string_view("wait"),    std::string_view("drain"),
    std::string_view("abort"),   std::string_view("cancel"),
    std::string_view("fail"),    std::string_view("log"),
    std::string_view("logf"),    std::string_view("if"),
    std::string_view("for"),     std::string_view("repeat"),
    std::string_view("until"),   std::string_view("while"),
    std::string_view("nodes"),
};

// `else` continues an `if`; `parallel` and `max` say how wide a loop or a stage
// runs and `unordered` gives up the order a parallel stage otherwise keeps;
// `of` ties a `skip`'s output names to the call they belong to, and a `wait
// first of` to its candidates; `by` and `desc` say how to `sort`; `into` says
// where a `try` stage sends what it could not do.
constexpr std::array kClauseOrder = {
    std::string_view("else"),      std::string_view("parallel"),
    std::string_view("unordered"), std::string_view("max"),
    std::string_view("of"),        std::string_view("by"),
    std::string_view("desc"),      std::string_view("into"),
};

// As a flow is written, top to bottom. `struct` sits beside `flow` because it
// is the other thing a file declares; what a *field* says about itself is
// [OrderedFieldModifiers] rather than a declaration word, the same way a port's
// modifiers are their own table.
constexpr std::array kDeclarationOrder = {
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
    std::string_view("tee"),
    std::string_view("via"),
    std::string_view("timeout"),
    std::string_view("after"),
    std::string_view("with"),
    std::string_view("id"),
    std::string_view("forward headers"),
};

constexpr std::array kPortModifierOrder = {
    std::string_view("stream"),
    std::string_view("required"),
};

constexpr std::array kStatusFieldOrder = {
    std::string_view("ok"),
    std::string_view("code"),
    std::string_view("number"),
    std::string_view("message"),
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
      table = &StatementDocs();
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
  // Sorted rather than in an order the language does not have, so that walking
  // a role twice walks it the same way: a payload that changed because a hash
  // table was rebuilt would break every generated file that reads one.
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
  static const auto* words =
      MakeSet({"collect", "count", "join", "sum", "min", "max", "avg", "fold"});
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
  // `| sort parallel 8` has an order to keep, so both would be asking for
  // workers with nothing to do. `flatten` is here because each list is cut on
  // its own; `pace` and `timeout` are not, because both are about *when*.
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
  static constexpr std::array<int, 3> kZeroOneOrTwo = {0, 1, 2};
  static constexpr std::array<int, 1> kNone = {0};
  if (canonical_name == "list" || canonical_name == "array") {
    return absl::MakeConstSpan(kZeroOrOne.data(), kZeroOrOne.size());
  }
  if (canonical_name == "object" || canonical_name == "json") {
    return absl::MakeConstSpan(kZeroOneOrTwo.data(), kZeroOneOrTwo.size());
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
  // Both stand where a pipeline's source does and nowhere else, which is what
  // makes them source words rather than statements or functions.
  static const auto* words = MakeSet({"status", "zip", "interleave"});
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
  static const auto* words = MakeSet({"true", "false", "null", "it"});
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
