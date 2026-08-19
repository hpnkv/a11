package dev.curiositystack.a11.clion.flow

import com.intellij.lexer.LexerBase
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

/**
 * The A11 Flow lexer: a replay of the token stream the language itself produced.
 *
 * There used to be a hand-written lexer here, 500 lines of it, and it had to be
 * taught every word the compiler learned -- with a test whose only job was to
 * catch it falling behind. What a word *means* is a judgement the language makes
 * (a word is a stage only after a `|`, a type only past a port's `:`, a member
 * whatever follows a `.`), and it now makes it in one place, in C++. This asks
 * [FlowEngine] for the answer and turns it into the platform's token types.
 *
 * The whole document is classified at once and the answer cached against its text,
 * so an incremental relex of one line costs a map lookup rather than a process
 * round trip.
 *
 * **Without the tool** -- no binary for this platform -- it falls back to the
 * shape of a flow: comments, strings, numbers, words and punctuation, with no
 * opinion about which words are keywords. That is a plainer editor, not a broken
 * one, and it is the only lexing rule left in Kotlin.
 */
class FlowLexer : LexerBase() {

    private var buffer: CharSequence = ""
    private var start = 0
    private var end = 0

    /** The tokens covering `[start, end)`, whitespace filled in, in order. */
    private var tokens: List<Slice> = emptyList()
    private var at = 0

    private data class Slice(val type: IElementType, val start: Int, val end: Int)

    override fun start(
        buffer: CharSequence,
        startOffset: Int,
        endOffset: Int,
        initialState: Int,
    ) {
        this.buffer = buffer
        this.start = startOffset
        this.end = endOffset
        // The classifier is given the whole document, because the meaning of a
        // word depends on what came before it -- which is exactly what a relex
        // from the middle of a file does not have.
        val text = buffer.toString()
        val classified = classify(text) ?: shapeOf(text)
        tokens = fill(classified, startOffset, endOffset)
        at = 0
    }

    override fun getState(): Int = 0

    override fun getTokenType(): IElementType? = tokens.getOrNull(at)?.type

    override fun getTokenStart(): Int = tokens.getOrNull(at)?.start ?: end

    override fun getTokenEnd(): Int = tokens.getOrNull(at)?.end ?: end

    override fun advance() {
        if (at < tokens.size) at++
    }

    override fun getBufferSequence(): CharSequence = buffer

    override fun getBufferEnd(): Int = end

    /** The language's own answer, or `null` when the tool is not there. */
    @Suppress("UNCHECKED_CAST")
    private fun classify(text: String): List<Slice>? {
        if (text.isEmpty()) return emptyList()
        val payload = FlowEngine.instance().tokens(text) ?: return null
        val listed = payload["tokens"] as? List<*> ?: return null
        val slices = ArrayList<Slice>(listed.size)
        for (entry in listed) {
            val token = entry as? Map<*, *> ?: continue
            val from = (token["start"] as? Number)?.toInt() ?: continue
            val to = (token["end"] as? Number)?.toInt() ?: continue
            // Offsets arrive in the platform's own units (see FlowEngine), so a
            // token past the end of the document is not a unit mismatch to be
            // absorbed -- it is a disagreement about the text, and the document
            // this was asked about has already been retyped. Bailing out is right:
            // the answer is about a document that no longer exists, and the next
            // relex will ask again.
            if (to > text.length) return null
            if (to <= from) continue
            val kind = token["kind"] as? String ?: continue
            slices.add(Slice(typeOf(kind, token["lexical"] as? String), from, to))
        }
        return slices
    }

    /**
     * The token type for one of the language's semantic kinds.
     *
     * The names are the `flow.tokens/v1` contract, so this is a translation and
     * not a decision. A kind this plugin has not been taught is an identifier,
     * which is what a word looks like when nobody has an opinion about it -- so a
     * language that gains a kind degrades to plain rather than to nothing.
     */
    private fun typeOf(kind: String, lexical: String?): IElementType = when (kind) {
        "comment" -> FlowTokens.COMMENT
        "string" -> FlowTokens.STRING
        "number" -> FlowTokens.NUMBER
        "duration" -> FlowTokens.DURATION
        "declaration-keyword" -> FlowTokens.DECLARATION_KEYWORD
        "statement-keyword" -> FlowTokens.STATEMENT_KEYWORD
        "modifier-keyword" -> FlowTokens.MODIFIER_KEYWORD
        "stage" -> FlowTokens.STAGE
        "builtin" -> FlowTokens.BUILTIN
        "type" -> FlowTokens.TYPE
        "status-code" -> FlowTokens.STATUS_CODE
        "constant" -> FlowTokens.CONSTANT
        "word-operator" -> FlowTokens.WORD_OPERATOR
        "flow-name" -> FlowTokens.FLOW_NAME
        "action-name" -> FlowTokens.ACTION_NAME
        "node-map-name" -> FlowTokens.NODE_MAP_NAME
        "member" -> FlowTokens.MEMBER
        "port-name" -> FlowTokens.PORT_NAME
        "identifier" -> FlowTokens.IDENTIFIER
        // Punctuation is told apart by what it is rather than what it means,
        // because the platform matches braces and folds on the token type.
        "brace", "parenthesis", "bracket", "punctuation", "operator",
        "flow-operator",
        -> punctuation(lexical)

        "bad" -> TokenType.BAD_CHARACTER
        else -> FlowTokens.IDENTIFIER
    }

    private fun punctuation(lexical: String?): IElementType = FlowShape.punctuation(lexical)

    /**
     * The gaps between tokens, as whitespace, clipped to the requested range.
     *
     * The platform's contract is that a lexer tiles its whole range: every
     * character belongs to exactly one token. The language reports the tokens and
     * says nothing about the space between them, so the space is filled in here.
     */
    private fun fill(slices: List<Slice>, from: Int, to: Int): List<Slice> {
        val filled = ArrayList<Slice>(slices.size * 2 + 1)
        var position = from
        for (slice in slices) {
            if (slice.end <= from || slice.start >= to) continue
            val begin = slice.start.coerceAtLeast(from)
            val finish = slice.end.coerceAtMost(to)
            if (begin > position) {
                filled.add(Slice(TokenType.WHITE_SPACE, position, begin))
            }
            // A newline reported as a token is whitespace, and two runs of it in a
            // row would be two tokens where the platform expects one.
            if (slice.type == TokenType.WHITE_SPACE &&
                filled.isNotEmpty() &&
                filled.last().type == TokenType.WHITE_SPACE &&
                filled.last().end == begin
            ) {
                val last = filled.removeAt(filled.size - 1)
                filled.add(Slice(TokenType.WHITE_SPACE, last.start, finish))
            } else if (finish > begin) {
                filled.add(Slice(slice.type, begin, finish))
            }
            position = finish
        }
        if (position < to) filled.add(Slice(TokenType.WHITE_SPACE, position, to))
        return filled
    }

    /**
     * The shape of a flow, for when the language cannot be asked.
     *
     * Comments, strings, numbers, words and punctuation -- and deliberately no
     * word lists, because a copy of those is the thing this rework exists to
     * delete. A word is an identifier here however significant it is.
     */
    private fun shapeOf(text: String): List<Slice> =
        FlowShape.tokenize(text).map { Slice(it.type, it.start, it.end) }
}
