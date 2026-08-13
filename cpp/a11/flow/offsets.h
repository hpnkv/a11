// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_OFFSETS_H_
#define A11_FLOW_OFFSETS_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace a11::flow {

/// A document, and the offset arithmetic every editor protocol needs over it.
///
/// The language counts **bytes** from the start of the file, because that is what
/// a lexer reads and what an edit has to be applied in. Editors do not: LSP counts
/// **UTF-16 code units** from the start of a line, and the JVM -- so IntelliJ, so
/// every `CharSequence` and `TextRange` in a plugin -- counts UTF-16 code units
/// from the start of the document. For ASCII the three agree, which is exactly why
/// this is easy to get wrong: it works on every test file until somebody writes a
/// `§` in a description, and then everything after it is coloured one column to
/// the left.
///
/// So the conversion lives here, once, and both protocol adapters use it. It is a
/// real conversion and not a cast.
class TextIndex {
 public:
  TextIndex() = default;
  explicit TextIndex(std::string text);

  const std::string& Text() const { return text_; }
  size_t LineCount() const { return line_starts_.size(); }

  /// How many UTF-16 units of the document precede this byte offset.
  size_t Utf16Of(size_t byte_offset) const;

  /// The byte offset that many UTF-16 units into the document.
  size_t ByteOf(size_t utf16_offset) const;

  /// The LSP position -- zero-based line, UTF-16 units into it -- of a byte
  /// offset.
  std::pair<int, int> PositionOf(size_t byte_offset) const;

  /// The byte offset of an LSP position, clamped into the document.
  size_t ByteOfPosition(int line, int character) const;

 private:
  /// How many bytes the character at `offset` takes, and how many UTF-16 units
  /// it is: two only outside the basic plane, which is the one case a count of
  /// code points and a count of UTF-16 units disagree about.
  std::pair<size_t, size_t> Step(size_t offset) const;

  size_t LineEnd(size_t line) const;

  std::string text_;
  /// The byte offset each line starts at.
  std::vector<size_t> line_starts_;
  /// How many UTF-16 units precede each line, so a position inside one is a walk
  /// along that line rather than along the file.
  std::vector<size_t> line_utf16_starts_;
};

/// Which basis the offsets in a request and its answer are counted in.
enum class OffsetBasis {
  /// Bytes from the start of the file: the language's own, and the default.
  kBytes,
  /// UTF-16 code units from the start of the file: what a JVM or JavaScript
  /// editor host indexes its document buffer with.
  kUtf16,
};

/// The basis a request named, or `kBytes` when it named none.
///
/// `false` when the name is not one of the two, so a client with a typo is told
/// rather than quietly served the wrong arithmetic.
bool OffsetBasisFromName(std::string_view name, OffsetBasis& basis);

/// The name of a basis, for a message and for the JSON.
std::string_view OffsetBasisName(OffsetBasis basis);

/// Rewrite every document offset in an answer from bytes into UTF-16 units.
///
/// The rule, and it is the whole rule: a **numeric** field named `start`, `end`,
/// `offset` or `prefix_start` is a byte offset into the document, at any depth.
/// That is true of every `flow.*` envelope by construction -- those four names are
/// not used for anything else in any of them -- so this converts exactly the right
/// set and needs no table of shapes to fall out of step with the emitters.
///
/// Two near misses are deliberately left alone. `range.start` and `range.end` are
/// *objects*, and the offset inside each is the `offset` field this does convert.
/// A proposal's `caret` counts into the text that proposal inserts, not into the
/// document, so a document-basis conversion of it would be wrong.
void RebaseToUtf16(nlohmann::json& answer, const TextIndex& index);

}  // namespace a11::flow

#endif  // A11_FLOW_OFFSETS_H_
