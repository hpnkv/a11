package dev.curiositystack.a11.clion.flow

import com.intellij.application.options.CodeStyle
import com.intellij.codeInsight.editorActions.enter.EnterHandlerDelegate
import com.intellij.openapi.actionSystem.DataContext
import com.intellij.openapi.editor.Editor
import com.intellij.psi.PsiFile
import com.intellij.psi.TokenType

/**
 * Where the next line lands after Enter, inside a flow.
 *
 * A flow's blocks are `{ }`, and the platform's own default -- copy the
 * previous line's indentation -- already gets those right, because the line
 * that opens one is exactly one level shallower than what follows it. What it
 * cannot know is a *continuation*: a `skip` list still running past a `,`, a
 * pipeline left open after `|` or `->`, an `(o1, o2 of act)` group whose `)`
 * has not been typed yet. Each of those wants one more level than the
 * statement they are part of, and the line after the continuation ends comes
 * back to the block's own indent -- which is what "provably ends" means here:
 * no trailing `,`, `|`, `->` or `<-`, and every `(`/`[` opened since the
 * block's own line is closed again. Re-derived fresh on every Enter from the
 * whole document, so there is no state to fall out of step with what is
 * actually written.
 *
 * Built on [FlowShape] rather than [FlowEngine] on purpose: this runs
 * synchronously while Enter is being processed, on the write thread, and a
 * subprocess round trip has no place there. The punctuation is enough to
 * answer "is this a continuation" -- nothing here needs to know what a word
 * means.
 */
class FlowEnterHandler : EnterHandlerDelegate {

    override fun postProcessEnter(
        file: PsiFile,
        editor: Editor,
        dataContext: DataContext,
    ): EnterHandlerDelegate.Result {
        if (file !is FlowPsiFile) return EnterHandlerDelegate.Result.Continue
        val document = editor.document
        val lineNumber = document.getLineNumber(editor.caretModel.offset)
        if (lineNumber == 0) return EnterHandlerDelegate.Result.Continue

        val text = document.charsSequence
        val before = text.subSequence(0, document.getLineEndOffset(lineNumber - 1)).toString()
        val unit = " ".repeat(CodeStyle.getIndentOptions(file).INDENT_SIZE.coerceAtLeast(1))
        val desired = indentAfter(before, unit) ?: return EnterHandlerDelegate.Result.Continue

        val lineStart = document.getLineStartOffset(lineNumber)
        var whitespaceEnd = lineStart
        while (whitespaceEnd < document.textLength &&
            (text[whitespaceEnd] == ' ' || text[whitespaceEnd] == '\t')
        ) {
            whitespaceEnd++
        }
        if (text.subSequence(lineStart, whitespaceEnd).toString() != desired) {
            document.replaceString(lineStart, whitespaceEnd, desired)
        }
        editor.caretModel.moveToOffset(lineStart + desired.length)
        return EnterHandlerDelegate.Result.Continue
    }
}

/**
 * The indentation for the line after `before`, or `null` to leave whatever the
 * platform's default already put there.
 *
 * `null` inside an unterminated multi-line string: the shape of a flow has no
 * opinion about a description still running, and copying the previous line --
 * what the default already does -- is what keeps it aligned.
 */
internal fun indentAfter(before: String, unit: String): String? {
    val tokens = FlowShape.tokenize(before)
    if (tokens.isEmpty()) return null

    val last = tokens.last()
    if (last.type == FlowTokens.STRING && !isClosedString(before, last)) return null

    var braceDepth = 0
    var groupDepth = 0
    var continues = false
    for (token in tokens) {
        when (token.type) {
            FlowTokens.LEFT_BRACE -> braceDepth++
            FlowTokens.RIGHT_BRACE -> braceDepth = (braceDepth - 1).coerceAtLeast(0)
            FlowTokens.LEFT_PAREN, FlowTokens.LEFT_BRACKET -> groupDepth++
            FlowTokens.RIGHT_PAREN, FlowTokens.RIGHT_BRACKET -> groupDepth = (groupDepth - 1).coerceAtLeast(0)
            else -> {}
        }
        if (token.type != TokenType.WHITE_SPACE && token.type != FlowTokens.COMMENT) {
            continues = token.type == FlowTokens.COMMA || token.type == FlowTokens.PIPE ||
                token.type == FlowTokens.ARROW || token.type == FlowTokens.CARRY
        }
    }

    val blockWidth = braceDepth * unit.length
    if (groupDepth <= 0 && !continues) return " ".repeat(blockWidth)

    // A continuation: one level deeper than the block, unless the line just
    // finished was itself already a continuation -- in which case that line's
    // own width is the one running, and this one matches it rather than
    // adding another level on top.
    val previousLineStart = before.lastIndexOf('\n') + 1
    var indentEnd = previousLineStart
    while (indentEnd < before.length && (before[indentEnd] == ' ' || before[indentEnd] == '\t')) {
        indentEnd++
    }
    val previousIndent = before.substring(previousLineStart, indentEnd)
    return if (previousIndent.length > blockWidth) previousIndent
    else " ".repeat(blockWidth + unit.length)
}

/** Whether a string token's own text ends with the quote it opened with. */
private fun isClosedString(source: String, token: FlowShapeToken): Boolean {
    val raw = source.substring(token.start, token.end)
    val quote = if (raw.startsWith("\"\"\"")) "\"\"\"" else "\""
    return raw.length >= quote.length * 2 && raw.endsWith(quote)
}
