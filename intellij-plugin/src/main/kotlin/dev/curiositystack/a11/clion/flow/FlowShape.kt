package dev.curiositystack.a11.clion.flow

import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

/** One token, found without asking the language what it means. */
data class FlowShapeToken(val type: IElementType, val start: Int, val end: Int)

/**
 * The shape of a flow, without the language: comments, strings, numbers, words
 * and punctuation, and nothing about which words are keywords.
 *
 * [FlowLexer] falls back to this when there is no `a11-flow` for this platform.
 * [FlowEnterHandler] uses it outright, on purpose, because it runs on the write
 * thread while Enter is being processed, and a subprocess round trip has no
 * place there -- the punctuation is enough to answer "is this line a
 * continuation", and nothing here needs to know what a word means to say that.
 */
object FlowShape {

    /**
     * `text`, as a run of tokens tiling it exactly: every character in one of
     * them, none skipped, none overlapping.
     */
    fun tokenize(text: String): List<FlowShapeToken> {
        val slices = ArrayList<FlowShapeToken>()
        var at = 0
        while (at < text.length) {
            val letter = text[at]
            when {
                letter == '#' -> {
                    val stop = text.indexOf('\n', at).let { if (it < 0) text.length else it }
                    slices.add(FlowShapeToken(FlowTokens.COMMENT, at, stop))
                    at = stop
                }

                letter == '"' -> {
                    val triple = text.startsWith("\"\"\"", at)
                    val quote = if (triple) "\"\"\"" else "\""
                    var stop = at + quote.length
                    while (stop < text.length) {
                        if (text[stop] == '\\') {
                            stop += 2
                            continue
                        }
                        if (text.startsWith(quote, stop)) {
                            stop += quote.length
                            break
                        }
                        // A single-quoted string cannot span a line.
                        if (!triple && text[stop] == '\n') break
                        stop++
                    }
                    slices.add(FlowShapeToken(FlowTokens.STRING, at, stop.coerceAtMost(text.length)))
                    at = stop.coerceAtMost(text.length)
                }

                letter.isDigit() || (letter == '-' && at + 1 < text.length && text[at + 1].isDigit()) -> {
                    var stop = at + 1
                    while (stop < text.length && (text[stop].isDigit() || text[stop] == '.')) stop++
                    val digits = stop
                    while (stop < text.length && text[stop].isLetter()) stop++
                    slices.add(
                        FlowShapeToken(
                            if (stop > digits) FlowTokens.DURATION else FlowTokens.NUMBER,
                            at,
                            stop,
                        ),
                    )
                    at = stop
                }

                letter.isLetter() || letter == '_' || letter == '$' -> {
                    var stop = at
                    while (stop < text.length &&
                        (text[stop].isLetterOrDigit() || text[stop] == '_' || text[stop] == '$' ||
                            (text[stop] == '-' && stop + 1 < text.length && text[stop + 1].isLetterOrDigit()))
                    ) {
                        stop++
                    }
                    slices.add(FlowShapeToken(FlowTokens.IDENTIFIER, at, stop))
                    at = stop
                }

                letter.isWhitespace() -> at++

                else -> {
                    val two = if (at + 1 < text.length) text.substring(at, at + 2) else ""
                    val type = punctuation(two)
                    if (type != TokenType.WHITE_SPACE) {
                        slices.add(FlowShapeToken(type, at, at + 2))
                        at += 2
                    } else {
                        val one = punctuation(letter.toString())
                        slices.add(
                            FlowShapeToken(
                                if (one == TokenType.WHITE_SPACE) TokenType.BAD_CHARACTER else one,
                                at,
                                at + 1,
                            ),
                        )
                        at++
                    }
                }
            }
        }
        return slices
    }

    /** The token type one or two characters of punctuation spell, if any. */
    fun punctuation(lexical: String?): IElementType = when (lexical) {
        "{" -> FlowTokens.LEFT_BRACE
        "}" -> FlowTokens.RIGHT_BRACE
        "(" -> FlowTokens.LEFT_PAREN
        ")" -> FlowTokens.RIGHT_PAREN
        "[" -> FlowTokens.LEFT_BRACKET
        "]" -> FlowTokens.RIGHT_BRACKET
        "." -> FlowTokens.DOT
        ":" -> FlowTokens.COLON
        "," -> FlowTokens.COMMA
        "->" -> FlowTokens.ARROW
        "<-" -> FlowTokens.CARRY
        "|" -> FlowTokens.PIPE
        "=" -> FlowTokens.ASSIGN
        "==", "!=", "<", "<=", ">", ">=" -> FlowTokens.COMPARISON
        "+", "-" -> FlowTokens.ARITHMETIC
        // A line break is not a token the platform wants: it is whitespace, and
        // the caller fills it in.
        else -> TokenType.WHITE_SPACE
    }
}
