// Copyright 2026 The A11 Authors.

#include "a11/flow/discover.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/types/span.h>

#include "a11/flow/diagnostic.h"

namespace a11::flow::discover {
namespace {

using catalogue::ActionInfo;
using catalogue::PortInfo;

constexpr size_t kNowhere = static_cast<size_t>(-1);

/// The words that open a declaration this looks for.
constexpr std::string_view kSchemaWord = "ActionSchema";
constexpr std::string_view kPortWord = "ActionPortSchema";

bool IsWordChar(char c) {
  return absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsSpace(char c) {
  return absl::ascii_isspace(static_cast<unsigned char>(c));
}

// --- masking -----------------------------------------------------------------

/// A source file with its comments and its string *contents* blanked out, and
/// the strings kept to one side.
///
/// **Why a mask rather than a token stream.** Everything structural this has to
/// do -- find a word, match a bracket, split a list at its top-level commas --
/// is easy on text where no `)` hides inside a string and no `#` inside a URL
/// starts a comment, and hard on text where they do. Blanking those *in place*
/// keeps every offset equal to the offset in the real file, so an origin needs
/// no translation and is exact. The quotes themselves are left standing, which
/// is how a value is recognised as a string at all, and the decoded contents are
/// found by the offset of the opening quote.
///
/// Newlines survive blanking, so a line number is still a line number.
class Masked {
 public:
  Masked(std::string_view source, Language language)
      : source_(source), text_(source) {
    switch (language) {
      case Language::kPython:
        MaskPython();
        break;
      case Language::kCpp:
        MaskCpp();
        break;
      case Language::kTypeScript:
        MaskTypeScript();
        break;
    }
  }

  std::string_view text() const { return text_; }
  size_t size() const { return text_.size(); }
  char at(size_t index) const { return index < text_.size() ? text_[index] : '\0'; }

  /// The string that starts at `offset`, decoded, or `nullopt` if none does.
  const std::string* absl_nullable StringAt(size_t offset) const {
    const auto found = strings_.find(offset);
    return found == strings_.end() ? nullptr : &found->second.value;
  }

  /// One past the closing quote of the string starting at `offset`.
  size_t StringEnd(size_t offset) const {
    const auto found = strings_.find(offset);
    return found == strings_.end() ? offset : found->second.end;
  }

 private:
  struct Held {
    std::string value;
    size_t end = 0;
  };

  /// Blanks `[from, to)` but leaves the line breaks, so lines still count.
  void Blank(size_t from, size_t to) {
    for (size_t at = from; at < to && at < text_.size(); ++at) {
      if (text_[at] != '\n' && text_[at] != '\r') text_[at] = ' ';
    }
  }

  /// Records a string running `[open, end)` whose contents decode to `value`,
  /// and blanks everything between its quotes.
  void Keep(size_t open, size_t end, std::string value, size_t quote_length) {
    Blank(open + quote_length, end - quote_length);
    strings_.emplace(open, Held{std::move(value), end});
  }

  /// Reads a quoted run starting at `index`, whose delimiter is `quote` repeated
  /// `quote_length` times, honouring `\` escapes unless `raw`.
  ///
  /// Returns one past the closing delimiter, or the end of the file for a string
  /// nobody closed -- which is what a file somebody is in the middle of typing
  /// looks like, and is not a reason to stop reading it.
  size_t ReadQuoted(size_t index, char quote, size_t quote_length, bool raw,
                    std::string& value) {
    size_t at = index + quote_length;
    while (at < source_.size()) {
      const char c = source_[at];
      if (c == '\\' && !raw && at + 1 < source_.size()) {
        value.push_back(Unescape(source_[at + 1]));
        at += 2;
        continue;
      }
      if (c == '\\' && raw && at + 1 < source_.size()) {
        // A raw string keeps the backslash, but a quote after one still does not
        // close it, which is the one thing `r"..\"` needs.
        value.push_back(c);
        value.push_back(source_[at + 1]);
        at += 2;
        continue;
      }
      if (c == quote && Repeats(at, quote, quote_length)) {
        return at + quote_length;
      }
      value.push_back(c);
      ++at;
    }
    return source_.size();
  }

  bool Repeats(size_t at, char c, size_t times) const {
    for (size_t n = 0; n < times; ++n) {
      if (at + n >= source_.size() || source_[at + n] != c) return false;
    }
    return true;
  }

  static char Unescape(char c) {
    switch (c) {
      case 'n':
        return '\n';
      case 't':
        return '\t';
      case 'r':
        return '\r';
      case '0':
        return '\0';
      default:
        // `\"`, `\'`, `\\` and anything else stand for the character itself,
        // which is all this needs: a description is prose, not a byte string.
        return c;
    }
  }

  /// How many of `prefix` letters before `index` are a string prefix (`r`, `f`,
  /// `b`, `rb`, `u`), so `f"..."` is one string and `name"..."` is not.
  size_t PythonPrefix(size_t index) const {
    size_t letters = 0;
    while (letters < 2 && index > letters) {
      const char c = absl::ascii_tolower(
          static_cast<unsigned char>(source_[index - letters - 1]));
      if (c != 'r' && c != 'f' && c != 'b' && c != 'u') break;
      ++letters;
    }
    // Only a prefix if what is before it is not part of a longer word.
    if (letters > 0 && index > letters && IsWordChar(source_[index - letters - 1])) {
      return 0;
    }
    return letters;
  }

  void MaskPython() {
    for (size_t at = 0; at < source_.size();) {
      const char c = source_[at];
      if (c == '#') {
        const size_t line_end = source_.find('\n', at);
        const size_t to = line_end == std::string_view::npos ? source_.size()
                                                            : line_end;
        Blank(at, to);
        at = to;
        continue;
      }
      if (c == '"' || c == '\'') {
        const size_t prefix = PythonPrefix(at);
        bool raw = false;
        for (size_t n = 0; n < prefix; ++n) {
          const char letter = absl::ascii_tolower(
              static_cast<unsigned char>(source_[at - n - 1]));
          if (letter == 'r') raw = true;
        }
        const size_t quote_length = Repeats(at, c, 3) ? 3 : 1;
        std::string value;
        const size_t end = ReadQuoted(at, c, quote_length, raw, value);
        // A `"""..."""` description gives back the indentation the source put in
        // front of it, exactly as the Flow parser does for its own: the text is
        // what a reader is shown, and eight spaces of Python indentation are not
        // part of it.
        if (quote_length == 3) value = Dedent(value);
        Keep(at, end, std::move(value), quote_length);
        at = end;
        continue;
      }
      ++at;
    }
  }

  void MaskCpp() {
    for (size_t at = 0; at < source_.size();) {
      if (source_.compare(at, 2, "//") == 0) {
        const size_t line_end = source_.find('\n', at);
        const size_t to =
            line_end == std::string_view::npos ? source_.size() : line_end;
        Blank(at, to);
        at = to;
        continue;
      }
      if (source_.compare(at, 2, "/*") == 0) {
        const size_t close = source_.find("*/", at + 2);
        const size_t to =
            close == std::string_view::npos ? source_.size() : close + 2;
        Blank(at, to);
        at = to;
        continue;
      }
      // `R"delim(...)delim"`, which is how a long literal is written without
      // escaping every quote in it.
      if ((source_[at] == 'R' || source_[at] == 'u' || source_[at] == 'L') &&
          at + 1 < source_.size() && source_[at + 1] == '"' &&
          source_[at] == 'R') {
        const size_t open_paren = source_.find('(', at + 2);
        if (open_paren != std::string_view::npos) {
          const std::string_view delimiter =
              source_.substr(at + 2, open_paren - at - 2);
          const std::string closing = absl::StrCat(")", delimiter, "\"");
          const size_t close = source_.find(closing, open_paren + 1);
          const size_t end = close == std::string_view::npos
                                 ? source_.size()
                                 : close + closing.size();
          std::string value(source_.substr(
              open_paren + 1,
              (close == std::string_view::npos ? source_.size() : close) -
                  open_paren - 1));
          Blank(at + 1, end);
          strings_.emplace(at + 1, Held{std::move(value), end});
          at = end;
          continue;
        }
      }
      if (source_[at] == '"') {
        std::string value;
        const size_t end = ReadQuoted(at, '"', 1, /*raw=*/false, value);
        Keep(at, end, std::move(value), 1);
        at = end;
        continue;
      }
      if (source_[at] == '\'') {
        // A character literal, which is never a value this reads. Blanked so an
        // apostrophe in `'\''` cannot look like the start of something.
        std::string ignored;
        const size_t end = ReadQuoted(at, '\'', 1, /*raw=*/false, ignored);
        Blank(at, end);
        at = end;
        continue;
      }
      ++at;
    }
  }

  void MaskTypeScript() {
    for (size_t at = 0; at < source_.size();) {
      if (source_.compare(at, 2, "//") == 0) {
        const size_t line_end = source_.find('\n', at);
        const size_t to =
            line_end == std::string_view::npos ? source_.size() : line_end;
        Blank(at, to);
        at = to;
        continue;
      }
      if (source_.compare(at, 2, "/*") == 0) {
        const size_t close = source_.find("*/", at + 2);
        const size_t to =
            close == std::string_view::npos ? source_.size() : close + 2;
        Blank(at, to);
        at = to;
        continue;
      }
      const char c = source_[at];
      if (c == '"' || c == '\'' || c == '`') {
        std::string value;
        const size_t end = ReadQuoted(at, c, 1, /*raw=*/false, value);
        // A template literal holding `${..}` is not a literal value, and half of
        // one would be worse than none.
        if (c == '`' && value.find("${") != std::string::npos) {
          Blank(at, end);
        } else {
          if (c == '`') value = Dedent(value);
          Keep(at, end, std::move(value), 1);
        }
        at = end;
        continue;
      }
      ++at;
    }
  }

  /// A multi-line literal without the indentation its source put in front of it.
  ///
  /// The same rule the Flow parser applies to a `"""..."""` description: the
  /// smallest indentation of any non-blank line after the first is what the
  /// source added, so it comes off every line.
  static std::string Dedent(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
      const size_t br = text.find('\n', start);
      lines.push_back(text.substr(
          start, (br == std::string_view::npos ? text.size() : br) - start));
      if (br == std::string_view::npos) break;
      start = br + 1;
    }
    if (lines.size() < 2) return std::string(text);
    size_t common = std::string::npos;
    for (size_t n = 1; n < lines.size(); ++n) {
      const size_t indent = lines[n].find_first_not_of(" \t");
      if (indent == std::string_view::npos) continue;  // A blank line says nothing.
      common = std::min(common, indent);
    }
    if (common == std::string::npos || common == 0) return std::string(text);
    std::string out;
    for (size_t n = 0; n < lines.size(); ++n) {
      if (n > 0) out.push_back('\n');
      out.append(n == 0 || lines[n].size() < common ? lines[n]
                                                   : lines[n].substr(common));
    }
    // A literal opened on its own line begins with a break the reader never
    // meant to be part of the text.
    if (!out.empty() && out.front() == '\n') out.erase(0, 1);
    while (!out.empty() && IsSpace(out.back())) out.pop_back();
    return out;
  }

  std::string_view source_;
  std::string text_;
  absl::flat_hash_map<size_t, Held> strings_;
};

// --- structure ---------------------------------------------------------------

/// The next offset at or after `from` that is not whitespace.
size_t SkipSpace(const Masked& masked, size_t from) {
  size_t at = from;
  while (at < masked.size() && IsSpace(masked.at(at))) ++at;
  return at;
}

char Mate(char open) {
  switch (open) {
    case '(':
      return ')';
    case '[':
      return ']';
    case '{':
      return '}';
    default:
      return '\0';
  }
}

/// The offset of the bracket closing the one at `open`, or [kNowhere].
///
/// Safe on the mask because a bracket inside a string was blanked away.
size_t MatchBracket(const Masked& masked, size_t open) {
  const char opener = masked.at(open);
  const char closer = Mate(opener);
  if (closer == '\0') return kNowhere;
  int depth = 0;
  for (size_t at = open; at < masked.size(); ++at) {
    const char c = masked.at(at);
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      --depth;
      if (depth == 0) return c == closer ? at : kNowhere;
      if (depth < 0) return kNowhere;
    }
  }
  return kNowhere;
}

/// The half-open ranges `[from, to)` splits into at its top-level `separator`.
std::vector<std::pair<size_t, size_t>> SplitTopLevel(const Masked& masked,
                                                     size_t from, size_t to,
                                                     char separator) {
  std::vector<std::pair<size_t, size_t>> parts;
  int depth = 0;
  size_t start = from;
  for (size_t at = from; at < to; ++at) {
    const char c = masked.at(at);
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      --depth;
    } else if (c == separator && depth == 0) {
      parts.emplace_back(start, at);
      start = at + 1;
    }
  }
  if (start < to) parts.emplace_back(start, to);
  // A trailing separator leaves an empty last part, which is not an entry.
  while (!parts.empty() &&
         SkipSpace(masked, parts.back().first) >= parts.back().second) {
    parts.pop_back();
  }
  return parts;
}

/// The offset of the first top-level `mark` in `[from, to)`, or [kNowhere].
///
/// `skip_double` is for `=`, which must not match the `==` of a comparison.
size_t FindTopLevel(const Masked& masked, size_t from, size_t to, char mark,
                    bool skip_double) {
  int depth = 0;
  for (size_t at = from; at < to; ++at) {
    const char c = masked.at(at);
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      --depth;
    } else if (c == mark && depth == 0) {
      if (skip_double &&
          (masked.at(at + 1) == '=' || masked.at(at + 1) == mark ||
           masked.at(at ? at - 1 : 0) == '=' || masked.at(at ? at - 1 : 0) == '!' ||
           masked.at(at ? at - 1 : 0) == '<' || masked.at(at ? at - 1 : 0) == '>')) {
        continue;
      }
      return at;
    }
  }
  return kNowhere;
}

/// The word ending at `to`, walking back over word characters.
std::string_view WordBefore(const Masked& masked, size_t to) {
  size_t end = to;
  while (end > 0 && IsSpace(masked.at(end - 1))) --end;
  size_t start = end;
  while (start > 0 && IsWordChar(masked.at(start - 1))) --start;
  return masked.text().substr(start, end - start);
}

/// The word starting at `from`.
std::string_view WordAt(const Masked& masked, size_t from) {
  size_t end = from;
  while (end < masked.size() && IsWordChar(masked.at(end))) ++end;
  return masked.text().substr(from, end - from);
}

/// Every offset where `word` stands as a whole word.
std::vector<size_t> WholeWords(const Masked& masked, std::string_view word) {
  std::vector<size_t> found;
  const std::string_view text = masked.text();
  size_t at = text.find(word);
  while (at != std::string_view::npos) {
    const bool before = at > 0 && IsWordChar(text[at - 1]);
    const size_t after_at = at + word.size();
    const bool after = after_at < text.size() && IsWordChar(text[after_at]);
    if (!before && !after) found.push_back(at);
    at = text.find(word, at + 1);
  }
  return found;
}

// --- values ------------------------------------------------------------------

/// The names a file binds to a string literal, so a schema naming one resolves.
///
/// `NAME = "..."` in Python, `const NAME = '...'` in TypeScript, and
/// `constexpr std::string_view kName = "...";` in C++ -- which is how nearly
/// every C++ action names itself, so without this the C++ side would find almost
/// nothing.
using Constants = absl::flat_hash_map<std::string, std::string>;

void CollectConstants(const Masked& masked, Constants& into) {
  // Parentheses and brackets only. A binding that is really a binding stands at
  // the top level of a module, a namespace, a class body or a function body --
  // all of which are braces, or nothing at all in Python -- whereas `name="x"`
  // inside a call is a keyword *argument*. Counting braces too would drop every
  // C++ constant for being inside its namespace; not counting parens at all made
  // `ActionSchema(name="bench-echo")` bind the word `name`, which is how this
  // came to be written.
  int depth = 0;
  for (size_t at = 0; at < masked.size(); ++at) {
    const char c = masked.at(at);
    if (c == '(' || c == '[') {
      ++depth;
      continue;
    }
    if (c == ')' || c == ']') {
      if (depth > 0) --depth;
      continue;
    }
    if (c != '=' || depth != 0) continue;
    if (masked.at(at + 1) == '=') continue;
    const char previous = at > 0 ? masked.at(at - 1) : '\0';
    if (previous == '=' || previous == '!' || previous == '<' ||
        previous == '>' || previous == '+' || previous == '-') {
      continue;
    }
    const std::string_view name = WordBefore(masked, at);
    if (name.empty()) continue;
    const size_t value_at = SkipSpace(masked, at + 1);
    const std::string* text = masked.StringAt(value_at);
    if (text == nullptr) continue;
    // The first binding wins: a name rebound later in the file is beyond what a
    // textual read can follow, and the first is the one a reader sees.
    into.try_emplace(std::string(name), *text);
  }
}

/// One value, as far as this needs to read one.
struct Value {
  /// The text, where the value is a string literal or a name bound to one.
  std::optional<std::string> text;
  /// The flag, where the value is `true` or `false`.
  std::optional<bool> flag;
  /// The call the value is, where it is one: `ActionPortSchema(..)`.
  std::string call;
  /// Where that call's arguments run, `[from, to)`.
  size_t args_from = 0;
  size_t args_to = 0;
  /// Where a `{..}` object runs, for a value that is one.
  size_t object_from = 0;
  size_t object_to = 0;
  bool is_object = false;
};

Value ReadValue(const Masked& masked, size_t from, size_t to,
                const Constants& constants);

/// A value written as a call: `Port("a", "b")`, `new ActionPortSchema({..})`,
/// `std::string(kName)`.
///
/// A one-argument call is looked *through* rather than at, which is what makes
/// `std::string(kMakeHttpRequestAction)` the name it wraps. A call of more than
/// one argument is kept as a call, since its arguments are the ports' details.
bool ReadCall(const Masked& masked, size_t from, size_t to,
              const Constants& constants, Value& value) {
  size_t at = SkipSpace(masked, from);
  // `new` before a constructor is TypeScript's, and says nothing here.
  if (WordAt(masked, at) == "new") at = SkipSpace(masked, at + 3);
  size_t name_end = at;
  // A qualified name: `a11.ActionSchema`, `std::string`.
  while (name_end < to) {
    if (IsWordChar(masked.at(name_end))) {
      ++name_end;
    } else if (masked.at(name_end) == '.' ||
               (masked.at(name_end) == ':' && masked.at(name_end + 1) == ':')) {
      name_end += masked.at(name_end) == '.' ? 1 : 2;
    } else {
      break;
    }
  }
  const size_t paren = SkipSpace(masked, name_end);
  if (paren >= to || masked.at(paren) != '(') return false;
  const size_t close = MatchBracket(masked, paren);
  if (close == kNowhere || close > to) return false;

  std::string_view qualified = masked.text().substr(at, name_end - at);
  // The last segment is the name that matters: `a11.ActionPortSchema` is an
  // `ActionPortSchema`.
  const size_t dot = qualified.find_last_of('.');
  const size_t colon = qualified.rfind("::");
  size_t bare = 0;
  if (dot != std::string_view::npos) bare = dot + 1;
  if (colon != std::string_view::npos) bare = std::max(bare, colon + 2);
  value.call = std::string(qualified.substr(bare));
  value.args_from = paren + 1;
  value.args_to = close;

  const std::vector<std::pair<size_t, size_t>> args =
      SplitTopLevel(masked, paren + 1, close, ',');
  if (args.size() == 1 && value.call != std::string(kPortWord)) {
    // A wrapper: read what it wraps, and keep the call beside it so a caller
    // that wanted the call still has it.
    const Value inner =
        ReadValue(masked, args[0].first, args[0].second, constants);
    if (inner.text.has_value()) value.text = inner.text;
    if (inner.flag.has_value()) value.flag = inner.flag;
    if (inner.is_object) {
      value.is_object = true;
      value.object_from = inner.object_from;
      value.object_to = inner.object_to;
    }
  }
  if (args.size() == 1 && value.call == std::string(kPortWord)) {
    // `new ActionPortSchema({name: .., type: ..})`: the object is the arguments.
    const Value inner =
        ReadValue(masked, args[0].first, args[0].second, constants);
    if (inner.is_object) {
      value.args_from = inner.object_from;
      value.args_to = inner.object_to;
    }
  }
  return true;
}

Value ReadValue(const Masked& masked, size_t from, size_t to,
                const Constants& constants) {
  Value value;
  const size_t at = SkipSpace(masked, from);
  if (at >= to) return value;

  // A string, and every literal written next to it: prose that outgrew its line
  // is written as adjacent literals in Python and C++ and joined with `+` in
  // TypeScript, and all three mean one string.
  if (masked.StringAt(at) != nullptr) {
    std::string joined;
    size_t cursor = at;
    while (cursor < to) {
      const std::string* piece = masked.StringAt(cursor);
      if (piece == nullptr) break;
      joined.append(*piece);
      cursor = SkipSpace(masked, masked.StringEnd(cursor));
      if (cursor < to && masked.at(cursor) == '+') {
        cursor = SkipSpace(masked, cursor + 1);
      }
    }
    value.text = std::move(joined);
    return value;
  }

  if (masked.at(at) == '{') {
    const size_t close = MatchBracket(masked, at);
    if (close != kNowhere && close <= to) {
      value.is_object = true;
      value.object_from = at + 1;
      value.object_to = close;
    }
    return value;
  }

  if (ReadCall(masked, at, to, constants, value)) return value;

  const std::string_view word = WordAt(masked, at);
  if (word == "true" || word == "True") {
    value.flag = true;
    return value;
  }
  if (word == "false" || word == "False") {
    value.flag = false;
    return value;
  }
  if (!word.empty()) {
    if (const auto found = constants.find(std::string(word));
        found != constants.end()) {
      value.text = found->second;
    }
  }
  return value;
}

/// The key and the value of one entry of an argument list or a map literal.
///
/// Four spellings, one shape. `name=value` is Python's keyword argument,
/// `name: value` is TypeScript's property and C++'s map pair written as
/// `{"name", value}`, and `.name = value` is C++'s designated initialiser. A
/// key that is a string literal (`"actions": ..`) is that string; one that is a
/// bare word is the word.
struct Entry {
  std::string key;
  size_t value_from = 0;
  size_t value_to = 0;
  bool keyed = false;
  /// Whether the key was written as a string literal (`"actions": ..`) rather
  /// than as a bare word (`name=..`, `text: ..`).
  ///
  /// The difference matters for a *map* key and only there: `{PORT: ..}` in
  /// Python is a dict whose key is a variable and has to be resolved, while
  /// `{text: ..}` in TypeScript is a property literally called `text`. A
  /// keyword argument's key is never a variable in either language, which is
  /// why resolving happens in [ReadPorts] and not here.
  bool key_was_literal = false;
  /// Whether the key was written as `[NAME]`: a key that is deliberately a
  /// variable, which is TypeScript's way of saying so.
  bool computed_key = false;
};

Entry ReadEntry(const Masked& masked, size_t from, size_t to,
                const Constants& constants) {
  Entry entry;
  const size_t start = SkipSpace(masked, from);

  // `{"name", value}` -- a C++ map pair, whose key is its first element.
  if (masked.at(start) == '{') {
    const size_t close = MatchBracket(masked, start);
    if (close != kNowhere && close <= to) {
      const std::vector<std::pair<size_t, size_t>> parts =
          SplitTopLevel(masked, start + 1, close, ',');
      if (parts.size() == 2) {
        const Value key =
            ReadValue(masked, parts[0].first, parts[0].second, constants);
        if (key.text.has_value()) {
          entry.key = *key.text;
          entry.keyed = true;
          entry.key_was_literal = true;
          entry.value_from = parts[1].first;
          entry.value_to = parts[1].second;
          return entry;
        }
      }
    }
  }

  const size_t colon = FindTopLevel(masked, start, to, ':', /*skip_double=*/true);
  const size_t equals = FindTopLevel(masked, start, to, '=', /*skip_double=*/true);
  size_t split = kNowhere;
  if (colon != kNowhere && (equals == kNowhere || colon < equals)) {
    split = colon;
  } else if (equals != kNowhere) {
    split = equals;
  }
  if (split == kNowhere) {
    entry.value_from = start;
    entry.value_to = to;
    return entry;
  }

  size_t key_from = start;
  if (masked.at(key_from) == '.') ++key_from;  // A designated initialiser.
  key_from = SkipSpace(masked, key_from);
  // A key is a string literal (`"actions": ..`) or a bare word (`name=..`), and
  // never a name resolved through the constants: a keyword argument called
  // `name` is the word `name`, whatever else in the file happens to bind it.
  if (const std::string* literal = masked.StringAt(key_from);
      literal != nullptr) {
    entry.key = *literal;
    entry.key_was_literal = true;
  } else {
    std::string_view word = masked.text().substr(key_from, split - key_from);
    while (!word.empty() && IsSpace(word.back())) word.remove_suffix(1);
    // A computed key: `[PORT]: ..`, which is how TypeScript writes an object
    // whose key is a variable. The brackets come off and the name inside is
    // read as a name.
    if (word.size() > 2 && word.front() == '[' && word.back() == ']') {
      word = word.substr(1, word.size() - 2);
      while (!word.empty() && IsSpace(word.back())) word.remove_suffix(1);
      while (!word.empty() && IsSpace(word.front())) word.remove_prefix(1);
      entry.computed_key = true;
    }
    // Only a plain word is a key. Anything else -- an index, a call -- is a
    // key this cannot read, and a wrong one would be a port that does not exist.
    if (std::all_of(word.begin(), word.end(), IsWordChar)) {
      entry.key = std::string(word);
    }
  }
  entry.keyed = !entry.key.empty();
  entry.value_from = split + 1;
  entry.value_to = to;
  return entry;
}

// --- ports -------------------------------------------------------------------

/// Whether a string reads as prose rather than as a type name.
///
/// A description is a sentence and a type is a word or a mimetype, which is the
/// distinction that lets a positional read of an unknown helper's arguments
/// leave a field empty rather than fill it with the wrong thing.
bool ReadsAsProse(std::string_view text) {
  return text.find(' ') != std::string_view::npos;
}

/// One port, out of whatever built it.
///
/// Two shapes. `ActionPortSchema` is read by its own argument names, positional
/// or keyword, because that is a contract: the first two positional arguments
/// are the name and the type. Any *other* call is a helper this cannot know, so
/// its string arguments are read for what they look like -- the sentence is the
/// description, a word that is not the port's name is the type -- and a helper
/// whose arguments run in an unexpected order gives a port with a name and
/// nothing else rather than a port with a description in its type.
PortInfo ReadPort(const Masked& masked, std::string_view port_name,
                  size_t from, size_t to, const Constants& constants) {
  PortInfo port;
  port.name = std::string(port_name);

  const Value value = ReadValue(masked, from, to, constants);
  if (value.call.empty()) {
    // Not a call at all: `"text": "text/plain"` names a type and nothing else.
    if (value.text.has_value() && !ReadsAsProse(*value.text)) {
      port.type = *value.text;
    }
    return port;
  }

  const std::vector<std::pair<size_t, size_t>> args =
      SplitTopLevel(masked, value.args_from, value.args_to, ',');
  const bool known = value.call == std::string(kPortWord);

  std::vector<std::string> positional;
  std::vector<bool> flags;
  for (const auto& [start, end] : args) {
    const Entry entry = ReadEntry(masked, start, end, constants);
    const Value held =
        ReadValue(masked, entry.value_from, entry.value_to, constants);
    if (entry.keyed) {
      if (entry.key == "name" && held.text.has_value()) {
        // The map key already named the port; a `name=` that disagrees is the
        // schema's business and not this port's.
        continue;
      }
      if (entry.key == "type" && held.text.has_value()) port.type = *held.text;
      if (entry.key == "description" && held.text.has_value()) {
        port.description = *held.text;
      }
      if (entry.key == "required" && held.flag.has_value()) {
        port.required = *held.flag;
      }
      if (entry.key == "unary" && held.flag.has_value()) port.unary = *held.flag;
      continue;
    }
    if (held.text.has_value()) positional.push_back(*held.text);
    if (held.flag.has_value()) flags.push_back(*held.flag);
  }

  if (known) {
    // `ActionPortSchema(name, type, ...)`.
    if (positional.size() >= 2 && port.type.empty()) port.type = positional[1];
    if (positional.size() >= 3 && port.description.empty()) {
      port.description = positional[2];
    }
    return port;
  }

  // An unknown helper: read the arguments for what they look like.
  for (const std::string& text : positional) {
    if (text == port.name) continue;
    if (ReadsAsProse(text)) {
      if (port.description.empty()) port.description = text;
    } else if (port.type.empty()) {
      port.type = text;
    }
  }
  // Two flags, in the order every such helper in this repository takes them.
  // One flag says nothing about which it is, so nothing is read from it.
  if (flags.size() == 2) {
    port.required = flags[0];
    port.unary = flags[1];
  }
  return port;
}

/// The name a map key stands for, or empty where it names nothing this can read.
///
/// A map key is the one place a bare word may be a *variable*, and the languages
/// disagree about when: `{PORT: ..}` in Python is a dict whose key is whatever
/// `PORT` holds, while `{text: ..}` in TypeScript is a property literally called
/// `text` and `{[PORT]: ..}` is the variable form. Getting this wrong is not
/// harmless -- it offers a port called `NARRATION_PORT`, which is a port
/// that does not exist -- so a name that cannot be resolved gives nothing and the
/// port is dropped.
std::string KeyName(const Entry& entry, Language language,
                    const Constants& constants) {
  if (entry.key_was_literal) return entry.key;
  const bool is_variable =
      entry.computed_key || language == Language::kPython;
  if (!is_variable) return entry.key;
  const auto found = constants.find(entry.key);
  return found == constants.end() ? std::string() : found->second;
}

/// Every port of one `inputs`/`outputs`/`headers` map.
std::vector<PortInfo> ReadPorts(const Masked& masked, size_t from, size_t to,
                                const Constants& constants,
                                Language language) {
  std::vector<PortInfo> ports;
  const Value map = ReadValue(masked, from, to, constants);
  size_t body_from = from;
  size_t body_to = to;
  if (map.is_object) {
    body_from = map.object_from;
    body_to = map.object_to;
  } else {
    // A Python `dict(...)`, or a C++ map written as `{{"a", ..}, {"b", ..}}`
    // whose outer braces this already unwrapped -- otherwise there is nothing
    // here to read.
    const size_t open = SkipSpace(masked, from);
    if (masked.at(open) != '{' && masked.at(open) != '[') return ports;
  }
  for (const auto& [start, end] :
       SplitTopLevel(masked, body_from, body_to, ',')) {
    const Entry entry = ReadEntry(masked, start, end, constants);
    // A port whose key could not be read is dropped rather than invented: a
    // port named after an unresolved constant would be a port that does not
    // exist, offered in completion.
    if (!entry.keyed) continue;
    const std::string name = KeyName(entry, language, constants);
    if (name.empty()) continue;
    ports.push_back(
        ReadPort(masked, name, entry.value_from, entry.value_to, constants));
  }
  return ports;
}

// --- schemas -----------------------------------------------------------------

/// The region of the block the statement at `at` is in, `[from, to)`.
///
/// For the C++ shape, where a schema is a local variable filled in statement by
/// statement: everything that assigns to it is inside the enclosing braces, and
/// the enclosing braces end where the depth from here first goes negative.
std::pair<size_t, size_t> EnclosingBlock(const Masked& masked, size_t at) {
  int depth = 0;
  size_t to = masked.size();
  for (size_t index = at; index < masked.size(); ++index) {
    const char c = masked.at(index);
    if (c == '{' || c == '(' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ')' || c == ']') {
      if (depth == 0) {
        to = index;
        break;
      }
      --depth;
    }
  }
  return {at, to};
}

/// A schema assembled statement by statement: the C++ shape.
///
/// `ActionSchema schema; schema.name = ..; schema.outputs.emplace("p", ..);`.
/// Read by looking for assignments to the variable the declaration named, within
/// the block it was declared in.
void ReadAssembled(const Masked& masked, size_t word_at,
                   const Constants& constants, Language language,
                   ActionInfo& action) {
  const size_t name_at = SkipSpace(masked, word_at + kSchemaWord.size());
  // `ActionSchema& schema` in a helper that fills one in, or `ActionSchema
  // schema`.
  size_t variable_at = name_at;
  while (variable_at < masked.size() &&
         (masked.at(variable_at) == '&' || masked.at(variable_at) == '*')) {
    variable_at = SkipSpace(masked, variable_at + 1);
  }
  const std::string_view variable = WordAt(masked, variable_at);
  if (variable.empty()) return;

  const auto [from, to] = EnclosingBlock(masked, variable_at);
  const std::string prefix = absl::StrCat(variable, ".");
  size_t at = from;
  while (at < to) {
    const size_t found = masked.text().find(prefix, at);
    if (found == std::string_view::npos || found >= to) break;
    at = found + prefix.size();
    if (found > 0 && IsWordChar(masked.at(found - 1))) continue;
    const std::string_view field = WordAt(masked, at);
    const size_t after = SkipSpace(masked, at + field.size());

    if (field == "name" || field == "description") {
      if (masked.at(after) != '=' || masked.at(after + 1) == '=') continue;
      const size_t statement_end =
          std::min(to, masked.text().find(';', after) == std::string_view::npos
                           ? to
                           : masked.text().find(';', after));
      const Value value = ReadValue(masked, after + 1, statement_end, constants);
      if (!value.text.has_value()) continue;
      if (field == "name" && action.name.empty()) action.name = *value.text;
      if (field == "description" && action.description.empty()) {
        action.description = *value.text;
      }
      continue;
    }

    const bool inputs = field == "inputs";
    const bool outputs = field == "outputs";
    const bool headers = field == "headers";
    if (!inputs && !outputs && !headers) continue;
    // `.emplace("port", Port(..))` or `.insert({"port", Port(..)})`.
    if (masked.at(after) != '.') continue;
    const std::string_view verb = WordAt(masked, after + 1);
    if (verb != "emplace" && verb != "insert" && verb != "try_emplace") continue;
    const size_t paren = SkipSpace(masked, after + 1 + verb.size());
    if (masked.at(paren) != '(') continue;
    const size_t close = MatchBracket(masked, paren);
    if (close == kNowhere) continue;
    const std::vector<std::pair<size_t, size_t>> args =
        SplitTopLevel(masked, paren + 1, close, ',');
    if (args.empty()) continue;
    std::vector<PortInfo>& side =
        inputs ? action.inputs : (outputs ? action.outputs : action.headers);
    if (args.size() >= 2) {
      const Value key =
          ReadValue(masked, args[0].first, args[0].second, constants);
      if (!key.text.has_value()) continue;
      side.push_back(ReadPort(masked, *key.text, args[1].first,
                              args.back().second, constants));
    } else {
      // `.insert({"port", ..})` -- one argument that is the pair.
      const Entry entry =
          ReadEntry(masked, args[0].first, args[0].second, constants);
      if (!entry.keyed) continue;
      const std::string name = KeyName(entry, language, constants);
      if (name.empty()) continue;
      side.push_back(
          ReadPort(masked, name, entry.value_from, entry.value_to, constants));
    }
  }
}

/// A schema written as a constructor call: the Python and TypeScript shape.
void ReadConstructed(const Masked& masked, size_t paren,
                     const Constants& constants, Language language,
                     ActionInfo& action) {
  const size_t close = MatchBracket(masked, paren);
  if (close == kNowhere) return;
  size_t from = paren + 1;
  size_t to = close;
  // `new ActionSchema({..})`: the object is the argument list.
  const size_t inner = SkipSpace(masked, from);
  if (masked.at(inner) == '{') {
    const size_t inner_close = MatchBracket(masked, inner);
    if (inner_close != kNowhere && inner_close <= close) {
      from = inner + 1;
      to = inner_close;
    }
  }
  for (const auto& [start, end] : SplitTopLevel(masked, from, to, ',')) {
    const Entry entry = ReadEntry(masked, start, end, constants);
    if (!entry.keyed) continue;
    if (entry.key == "name" || entry.key == "description") {
      const Value value =
          ReadValue(masked, entry.value_from, entry.value_to, constants);
      if (!value.text.has_value()) continue;
      if (entry.key == "name") action.name = *value.text;
      if (entry.key == "description") action.description = *value.text;
      continue;
    }
    if (entry.key == "inputs") {
      action.inputs = ReadPorts(masked, entry.value_from, entry.value_to,
                                constants, language);
    } else if (entry.key == "outputs") {
      action.outputs = ReadPorts(masked, entry.value_from, entry.value_to,
                                 constants, language);
    } else if (entry.key == "headers") {
      action.headers = ReadPorts(masked, entry.value_from, entry.value_to,
                                 constants, language);
    }
  }
}

std::vector<ActionInfo> ReadSchemas(std::string_view source,
                                    std::string_view path, Language language,
                                    const Constants& shared) {
  const Masked masked(source, language);
  Constants constants = shared;
  CollectConstants(masked, constants);
  const LineIndex lines(source);

  std::vector<ActionInfo> found;
  for (const size_t at : WholeWords(masked, kSchemaWord)) {
    ActionInfo action;
    const size_t after = SkipSpace(masked, at + kSchemaWord.size());
    if (masked.at(after) == '(') {
      ReadConstructed(masked, after, constants, language, action);
    } else {
      ReadAssembled(masked, at, constants, language, action);
    }
    // Nothing can look up an action with no name, so half an entry is dropped
    // rather than offered.
    if (action.name.empty()) continue;
    Origin origin;
    origin.file = std::string(path);
    const Position position = lines.At(at);
    origin.line = position.line;
    origin.column = position.column;
    action.origin = std::move(origin);
    found.push_back(std::move(action));
  }
  return found;
}

// --- files -------------------------------------------------------------------

std::optional<std::string> ReadFile(const std::filesystem::path& path,
                                    size_t limit, bool& too_large) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return std::nullopt;
  if (size > limit) {
    too_large = true;
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream held;
  held << in.rdbuf();
  return held.str();
}

/// The constants of a `.cc` file's sibling header.
///
/// Nearly every C++ action names itself with a `constexpr std::string_view`
/// declared in the header beside the implementation, so a scan that read only
/// the `.cc` would drop every one of them for having no name. One sibling, not a
/// search: this is the idiom, not an include graph.
Constants SiblingConstants(const std::filesystem::path& path, size_t limit) {
  Constants constants;
  if (path.extension() != ".cc" && path.extension() != ".cpp") return constants;
  for (const char* extension : {".h", ".hpp"}) {
    std::filesystem::path header = path;
    header.replace_extension(extension);
    bool ignored = false;
    if (const std::optional<std::string> text =
            ReadFile(header, limit, ignored);
        text.has_value()) {
      CollectConstants(Masked(*text, Language::kCpp), constants);
    }
  }
  return constants;
}

bool Skipped(const Options& options, std::string_view name) {
  for (const std::string& pattern : options.skip_directories) {
    if (pattern == name) return true;
    // One trailing `*`, which is what `cmake-build-*` needs and the whole of
    // the matching anybody has asked for here.
    if (!pattern.empty() && pattern.back() == '*' &&
        absl::StartsWith(name, std::string_view(pattern).substr(
                                   0, pattern.size() - 1))) {
      return true;
    }
  }
  return false;
}

void ScanOneFile(const std::filesystem::path& path, std::string_view as_given,
                 const Options& options, Result& result,
                 std::vector<ActionInfo>& into) {
  const std::optional<Language> language = LanguageOf(path.string());
  if (!language.has_value()) return;
  bool too_large = false;
  const std::optional<std::string> text =
      ReadFile(path, options.max_file_bytes, too_large);
  if (too_large) result.too_large.push_back(std::string(as_given));
  if (!text.has_value()) return;
  ++result.files_read;
  // A file with none of the word in it is the common case, and reading it any
  // further would be masking a megabyte to find nothing.
  if (text->find(kSchemaWord) == std::string::npos) return;
  const Constants shared =
      *language == Language::kCpp
          ? SiblingConstants(path, options.max_file_bytes)
          : Constants();
  for (ActionInfo& action :
       ReadSchemas(*text, as_given, *language, shared)) {
    into.push_back(std::move(action));
  }
}

}  // namespace

std::optional<Language> LanguageOf(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) return std::nullopt;
  const std::string_view extension = path.substr(dot);
  if (extension == ".py" || extension == ".pyi") return Language::kPython;
  if (extension == ".cc" || extension == ".cpp" || extension == ".cxx" ||
      extension == ".h" || extension == ".hpp") {
    return Language::kCpp;
  }
  if (extension == ".ts" || extension == ".tsx" || extension == ".mts" ||
      extension == ".js" || extension == ".mjs" || extension == ".jsx") {
    return Language::kTypeScript;
  }
  return std::nullopt;
}

Options Options::Default() {
  Options options;
  options.skip_directories = {
      "node_modules", ".venv",         "venv",   "build*",
      "cmake-build*", "dist",          ".git",   "__pycache__",
      ".mypy_cache",  ".pytest_cache", ".tox",   "site-packages",
      ".idea",        ".gradle",       "target", "_deps",
  };
  return options;
}

catalogue::Catalogue DiscoverInSource(std::string_view source,
                                     std::string_view path, Language language) {
  return catalogue::Catalogue::Of(ReadSchemas(source, path, language, {}));
}

Result Discover(absl::Span<const std::string> roots, const Options& options) {
  Result result;
  std::vector<ActionInfo> found;
  for (const std::string& root : roots) {
    std::error_code error;
    const std::filesystem::path path(root);
    if (std::filesystem::is_regular_file(path, error)) {
      ScanOneFile(path, root, options, result, found);
      continue;
    }
    if (!std::filesystem::is_directory(path, error)) continue;
    std::filesystem::recursive_directory_iterator walk(
        path, std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error) continue;
    for (auto entry = begin(walk); entry != end(walk); entry.increment(error)) {
      if (error) break;
      if (result.files_read >= options.max_files) {
        result.reached_file_limit = true;
        break;
      }
      if (entry->is_directory(error)) {
        if (Skipped(options, entry->path().filename().string())) {
          entry.disable_recursion_pending();
        }
        continue;
      }
      if (!entry->is_regular_file(error)) continue;
      ScanOneFile(entry->path(), entry->path().string(), options, result, found);
    }
  }
  // A name declared twice keeps the first, which is the one a reader scanning
  // the tree top to bottom would find. Two files really declaring one action is
  // a problem in the project rather than in the scan, and answering with both
  // would make a hover flicker between them.
  std::vector<ActionInfo> unique;
  for (ActionInfo& action : found) {
    const bool seen = std::any_of(
        unique.begin(), unique.end(),
        [&](const ActionInfo& kept) { return kept.name == action.name; });
    if (!seen) unique.push_back(std::move(action));
  }
  result.found = catalogue::Catalogue::Of(std::move(unique));
  return result;
}

}  // namespace a11::flow::discover
