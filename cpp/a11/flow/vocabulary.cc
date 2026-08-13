// Copyright 2026 The A11 Authors.

#include "a11/flow/vocabulary.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>

namespace a11::flow::vocabulary {
namespace {

// Stages in the order a listing reads best: what shortens a stream, what reshapes
// each value, what reduces it to one, what encodes it.
constexpr std::array kStageOrder = {
    std::string_view("first"),  std::string_view("last"),
    std::string_view("drop"),   std::string_view("truncate"),
    std::string_view("batch"),  std::string_view("group"),
    std::string_view("where"),  std::string_view("map"),
    std::string_view("match"),  std::string_view("distinct"),
    std::string_view("then"),
    std::string_view("mime"),   std::string_view("strformat"),
    std::string_view("chunk"),  std::string_view("collect"),
    std::string_view("count"),  std::string_view("join"),
    std::string_view("text"),   std::string_view("json"),
    std::string_view("packb"),
};

const absl::flat_hash_map<std::string_view, StageArgument>& StageTable() {
  static const auto* table =
      new absl::flat_hash_map<std::string_view, StageArgument>{
          {"first", StageArgument::kNumber},
          {"last", StageArgument::kNumber},
          {"drop", StageArgument::kNumber},
          {"truncate", StageArgument::kNumber},
          {"batch", StageArgument::kNumber},
          {"chunk", StageArgument::kNumber},
          {"where", StageArgument::kExpression},
          {"map", StageArgument::kExpression},
          {"group", StageArgument::kExpression},
          {"join", StageArgument::kOptionalString},
          {"strformat", StageArgument::kString},
          {"match", StageArgument::kString},
          {"mime", StageArgument::kString},
          {"then", StageArgument::kStream},
          {"collect", StageArgument::kNone},
          {"count", StageArgument::kNone},
          {"distinct", StageArgument::kNone},
          {"text", StageArgument::kNone},
          {"json", StageArgument::kNone},
          {"packb", StageArgument::kNone},
      };
  return *table;
}

// What each stage does, as reference. Accuracy is from `Scope::ProduceStage` in
// `runtime.cc`, which is the one implementation of every one of these; where the
// two disagree the runtime is right and this is a bug.
//
// House style, since a table of 19 of these only reads well if they agree:
// present tense, "the stream" for the whole and "each value" for one of them, no
// second person, and the caveat last rather than buried. `--` is never written in
// text a reader sees: a colon or an em dash instead.
const absl::flat_hash_map<std::string_view, WordDoc>& StageDocs() {
  static const auto* table = new absl::flat_hash_map<std::string_view, WordDoc>{
      {"first",
       {"The first `n` values of the stream, and then nothing.", "a count",
        "Stops reading upstream as soon as it has them, so a producer still "
        "working is asked to stop rather than run to the end for values nobody "
        "will see. `| first 1` is how a stream becomes one value.",
        "hits | first 3 -> shown"}},
      {"last",
       {"The last `n` values of the stream.", "a count",
        "Reads the whole stream to find out which those are, holding `n` values "
        "while it does, so nothing comes out until the stream ends.",
        "lines | last 20 -> tail"}},
      {"drop",
       {"Everything except the first `n` values.", "a count",
        "Only this pipeline sees fewer values. `skip n port` is the other half "
        "of the pair: it takes them off the node itself, for every reader of it, "
        "which is how a header row stops being everybody's problem.",
        "rows | drop 1 -> body"}},
      {"truncate",
       {"Shortens each value. The stream keeps its length.", "a count",
        "`n` characters of a string, `n` bytes of a byte string, `n` items of a "
        "list, `n` keys of a record. Anything else goes through unchanged. "
        "Cutting a page down before it reaches a model is the difference "
        "between a cheap call and an expensive one.",
        "page.text | truncate 4000 -> brief.pages"}},
      {"batch",
       {"Gathers values into lists of `n`.", "a count",
        "Each list goes on as one value, so what follows reads a stream of "
        "lists. Whatever is left when the stream ends goes on too, short.",
        "samples | batch 100 -> frames"}},
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
        "`map Shape{…}` and `map it as Shape` also tell the language what the "
        "stream now carries, so the fields are completed and checked "
        "downstream. Anything else makes something the language cannot name, "
        "and it says nothing about it rather than guessing.",
        "hits | map Source{url: it.url} -> sources"}},
      {"match",
       {"Pulls named fields out of each value, and drops the ones that do not "
        "fit.",
        "a pattern",
        "Literal text matches itself, a run of spaces or tabs matches any run, "
        "and `{name}` captures up to whatever follows it: "
        "`match(\"name={name} age={age:int}\")` turns `name=Alice   age=27` "
        "into a record with `name` and `age`. A hole may say what it is: `int`, "
        "`number`, `bool`, `word`, `line`, `rest`, `duration`, `time`, `json`. "
        "The pattern searches, so it matches anywhere in the value and needs no "
        "leading or trailing wildcards. A value the pattern does not fit is "
        "dropped, which is what makes this a `where` and a `map` at once; the "
        "function of the same name answers null for one value instead. Where the "
        "pattern is written out, the fields are known, so `it.name` is completed "
        "and a typo in it is reported.",
        "lines | match \"{level:word}: {message:line}\" | map it.level -> levels"}},
      {"distinct",
       {"Drops a value equal to one already seen.", "",
        "Equal as text, which is what makes it work on records as well as on "
        "strings. Every distinct value seen so far is remembered for as long as "
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
        "Exactly `| map strformat(\"…\", it)`, which is the shape almost every "
        "use of it has. printf's conversions: `%s` for anything, `%d` and "
        "`%.2f` for numbers, `%%` for a literal per cent. The function of the "
        "same name is there for when more than one value goes in.",
        "elapsed | strformat \"took %s\" -> log"}},
      {"chunk",
       {"Cuts each value into pieces of at most `n` bytes.", "a size in bytes",
        "The other direction from `let`: one value becomes a stream of pieces, "
        "which is what an upload wanting 64 KiB frames and a model wanting a "
        "paragraph are both asking for. Text is cut on a character boundary, so "
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
        "Reads to the end and yields exactly one integer. Nothing is decoded to "
        "count it, so counting a stream of pages costs nothing per page.",
        "pages | count -> how_many"}},
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
        "A value carrying bytes anywhere inside it cannot be written as JSON at "
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

// The fixed function set, as reference. Same house style as [StageDocs], and the
// same rule about accuracy: `Call` in `values.cc` is the implementation.
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
        "Text that reads as a whole number gives an integer and text that reads "
        "as a decimal gives a double; a bool gives 0 or 1; a duration gives its "
        "seconds and an instant its seconds since the epoch. Text that is not a "
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
        "get(page.meta, \"title\", \"untitled\") -> title"}},
      {"join",
       {"Every item of a list as text, concatenated into one string.",
        "the list, and a separator",
        "The stage `| join` does this to a whole stream instead of to a list. A "
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
        "starts-with(part.mime, [\"text/\", \"application/json\"])"}},
      {"ends-with",
       {"Whether a value, as text, ends with another.",
        "the text, and one suffix or a list of them",
        "A list of candidates is one question rather than three: a piece that "
        "ends a sentence ends with any of `[\".\", \"?\", \"!\"]`.",
        "ends-with(piece, [\".\", \"?\", \"!\"])"}},
      {"match",
       {"The fields a pattern pulls out of a value, or null where it does not "
        "fit.",
        "a pattern, and the text to read",
        "The same pattern language the stage reads: literal text matches itself, "
        "`{name}` captures, and `{name:int}` says what to read it as. "
        "`match(\"name={name} age={age:int}\", line)` gives a record, so "
        "`obj.name` and `obj.age` are there; a line the pattern does not fit "
        "gives null, which `if not obj` asks about. Where the pattern is written "
        "out rather than computed, the fields are known and completed.",
        "let who = match(\"name={name}\", line)"}},
      {"replace",
       {"A value as text, with every occurrence of one string replaced by "
        "another.",
        "the text, what to find, and what to put there",
        "Plain text and not a pattern: nothing in the language interprets `.` "
        "or `*`. An empty string to find changes nothing.",
        "replace(path, \"\\\\\", \"/\") -> posix"}},
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
        "to_chunk({\"type\": \"text\", \"text\": said}, \"application/json\")"}},
      {"from_chunk",
       {"The value inside a chunk.", "one chunk",
        "Anything already decoded comes back as it is, so this is safe to write "
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
        "`text(b64decode(x))` where they were a string. Padding is not required "
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
        "Padding is not required, since the web-safe alphabet is routinely sent "
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
      if (end > start) set->insert(word.substr(start, end - start));
      if (space == std::string_view::npos) break;
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
    std::string_view("len"),        std::string_view("lower"),
    std::string_view("upper"),      std::string_view("trim"),
    std::string_view("text"),       std::string_view("number"),
    std::string_view("bool"),       std::string_view("keys"),
    std::string_view("values"),     std::string_view("get"),
    std::string_view("join"),       std::string_view("split"),
    std::string_view("merge"),      std::string_view("contains"),
    std::string_view("starts-with"), std::string_view("ends-with"),
    std::string_view("replace"),    std::string_view("match"),
    std::string_view("slice"),
    std::string_view("default"),    std::string_view("to_chunk"),
    std::string_view("from_chunk"), std::string_view("strformat"),
    std::string_view("b64encode"),  std::string_view("b64decode"),
    std::string_view("b64urlencode"), std::string_view("b64urldecode"),
    std::string_view("now"),        std::string_view("duration"),
    std::string_view("time"),       std::string_view("seconds"),
};

// The verbs first, then the barriers, then the blocks: the order somebody
// scanning a list of statements finds what they meant.
constexpr std::array kStatementOrder = {
    std::string_view("run"),    std::string_view("call"),
    std::string_view("try"),    std::string_view("let"),
    std::string_view("advance"), std::string_view("skip"),
    std::string_view("wait"),   std::string_view("drain"),
    std::string_view("cancel"), std::string_view("fail"),
    std::string_view("if"),     std::string_view("for"),
    std::string_view("repeat"), std::string_view("until"),
    std::string_view("while"),  std::string_view("nodes"),
};

// `else` continues an `if`; `parallel` and `max` say how wide a loop runs.
constexpr std::array kClauseOrder = {
    std::string_view("else"),
    std::string_view("parallel"),
    std::string_view("max"),
};

// As a flow is written, top to bottom. `struct` sits beside `flow` because it
// is the other thing a file declares; what a *field* says about itself is
// [OrderedFieldModifiers] rather than a declaration word, the same way a port's
// modifiers are their own table.
constexpr std::array kDeclarationOrder = {
    std::string_view("flow"),     std::string_view("struct"),
    std::string_view("describe"),
    std::string_view("in"),       std::string_view("out"),
    std::string_view("header"),   std::string_view("as"),
    std::string_view("default"),  std::string_view("stream"),
    std::string_view("required"), std::string_view("node"),
    std::string_view("nodes"),
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
    std::string_view("tee"),  std::string_view("via"),
    std::string_view("timeout"), std::string_view("after"),
    std::string_view("with"), std::string_view("id"),
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
    if (letter >= 'A' && letter <= 'Z') has_upper = true;
    if (letter >= 'a' && letter <= 'z') has_lower = true;
  }
  if (!has_upper || has_lower) return std::string(word);
  std::string lowered(word);
  for (char& letter : lowered) {
    if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter + 32);
  }
  return lowered;
}

bool IsShouted(std::string_view word) {
  bool has_upper = false;
  for (const char letter : word) {
    if (letter >= 'a' && letter <= 'z') return false;
    if (letter >= 'A' && letter <= 'Z') has_upper = true;
  }
  return has_upper;
}

absl::Span<const std::string_view> Stages() {
  return absl::MakeConstSpan(kStageOrder.data(), kStageOrder.size());
}

std::optional<StageArgument> StageTakes(std::string_view canonical_name) {
  const auto found = StageTable().find(canonical_name);
  if (found == StageTable().end()) return std::nullopt;
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
    case StageArgument::kStream:
      return "stream";
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

const absl::flat_hash_set<std::string_view>& BareStages() {
  static const auto* words = MakeSet({"then", "where"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& ReducingStages() {
  static const auto* words = MakeSet({"collect", "count", "join"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& PositionalStages() {
  static const auto* words =
      MakeSet({"first", "last", "drop", "batch", "group", "distinct", "count"});
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
  // `status x` reads an outcome and `zip(a, b)` reads several streams in step.
  // Both stand where a pipeline's source does and nowhere else, which is what
  // makes them source words rather than statements or functions.
  static const auto* words = MakeSet({"status", "zip"});
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

absl::Span<const std::string_view> StatusCodes() {
  return absl::MakeConstSpan(kStatusCodes.data(), kStatusCodes.size());
}

bool IsStatusCode(std::string_view word) {
  const std::string canonical = Canonical(word);
  // A code may be written with dashes where the canonical name has underscores,
  // which is what `plan.status_code` accepts.
  std::string normalised = canonical;
  for (char& letter : normalised) {
    if (letter == '-') letter = '_';
  }
  for (const std::string_view code : kStatusCodes) {
    if (code == normalised) return true;
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
  if (found == DurationTable().end()) return std::nullopt;
  return found->second;
}

}  // namespace a11::flow::vocabulary
