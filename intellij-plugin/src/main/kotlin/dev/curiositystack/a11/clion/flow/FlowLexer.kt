package dev.curiositystack.a11.clion.flow

import com.intellij.lexer.LexerBase
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

/**
 * The A11 Flow lexer, written by hand against `a11/flow/lexer.py`.
 *
 * The rules are the compiler's: `#` to the end of the line is a comment, a
 * string cannot span a line, a number may carry a duration unit, and an
 * identifier may contain a dash between word characters -- which is why `-3` is
 * a number and `for-each` is one name.
 *
 * What a word *means* is not something the compiler decides from the word alone,
 * and neither does this. A word is only a stage directly after a `|`, only a
 * function where it is being called, and what follows a `.` is a member however
 * it is spelled. Those few contexts are the lexer's [state], which is small
 * enough to be restartable: it depends on nothing but the token before.
 */
class FlowLexer : LexerBase() {

    private var buffer: CharSequence = ""
    private var bufferEnd = 0
    private var position = 0
    private var tokenStart = 0
    private var tokenType: IElementType? = null

    /** The state this token started in, which is what a restart resumes from. */
    private var state = DEFAULT

    /** The state the token after this one starts in. */
    private var nextState = DEFAULT

    override fun start(
        buffer: CharSequence,
        startOffset: Int,
        endOffset: Int,
        initialState: Int,
    ) {
        this.buffer = buffer
        this.bufferEnd = endOffset
        this.position = startOffset
        this.tokenStart = startOffset
        this.nextState = initialState
        advance()
    }

    override fun getState(): Int = state

    override fun getTokenType(): IElementType? = tokenType

    override fun getTokenStart(): Int = tokenStart

    override fun getTokenEnd(): Int = position

    override fun getBufferSequence(): CharSequence = buffer

    override fun getBufferEnd(): Int = bufferEnd

    override fun advance() {
        state = nextState
        tokenStart = position
        if (position >= bufferEnd) {
            tokenType = null
            return
        }
        val char = buffer[position]
        when {
            char.isWhitespace() -> {
                var crossedLine = false
                while (position < bufferEnd && buffer[position].isWhitespace()) {
                    if (buffer[position] == '\n') crossedLine = true
                    position++
                }
                // `nextState` is deliberately left alone: whitespace and
                // comments say nothing, so the context carries across them and
                // `call\n  some-action` still names an action. A port
                // declaration is the exception -- it is one line, and what
                // follows the next one is not a type.
                if (crossedLine && (state == IN_PORT_NAME || state == IN_PORT_TYPE)) {
                    nextState = DEFAULT
                }
                tokenType = TokenType.WHITE_SPACE
            }

            char == '#' -> {
                while (position < bufferEnd && buffer[position] != '\n') {
                    position++
                }
                tokenType = FlowTokens.COMMENT
            }

            char == '"' -> readString()

            char.isDigit() || (char == '-' && peekIsDigit(1)) -> readNumber()

            isNameStart(char) -> readWord()

            else -> readPunctuation()
        }
    }

    // --- pieces ---------------------------------------------------------------

    private fun readString() {
        position++
        while (position < bufferEnd) {
            when (buffer[position]) {
                '\n' -> break
                '\\' -> position = minOf(position + 2, bufferEnd)
                '"' -> {
                    position++
                    break
                }

                else -> position++
            }
        }
        tokenType = FlowTokens.STRING
        nextState = DEFAULT
    }

    private fun readNumber() {
        if (buffer[position] == '-') position++
        while (
            position < bufferEnd &&
            (buffer[position].isDigit() || buffer[position] == '.')
        ) {
            position++
        }
        val unitStart = position
        while (position < bufferEnd && buffer[position].isLetter()) {
            position++
        }
        tokenType = when {
            unitStart == position -> FlowTokens.NUMBER
            text(unitStart, position) in FlowVocabulary.DURATION_UNITS ->
                FlowTokens.DURATION
            // The compiler rejects any other suffix outright, so say so here
            // rather than colour it as if it were a number and a name.
            else -> TokenType.BAD_CHARACTER
        }
        nextState = DEFAULT
    }

    private fun readWord() {
        position++
        while (position < bufferEnd) {
            if (isNamePart(buffer[position])) {
                position++
                continue
            }
            // A dash continues a name only when a word follows it, which is what
            // keeps `a -> b` a pipe and `starts-with` a name.
            if (
                buffer[position] == '-' &&
                position + 1 < bufferEnd &&
                isNamePart(buffer[position + 1])
            ) {
                position += 2
                continue
            }
            break
        }
        val word = FlowVocabulary.canonical(text(tokenStart, position))
        val produced = classify(word)
        tokenType = produced
        nextState = stateAfter(word, produced)
    }

    private fun classify(word: String): IElementType = when {
        state == AFTER_DOT -> FlowTokens.MEMBER
        state == AFTER_FLOW -> FlowTokens.FLOW_NAME
        state == AFTER_CALL -> FlowTokens.ACTION_NAME
        state == AFTER_NODE_MAP -> FlowTokens.NODE_MAP_NAME
        state == AFTER_PIPE && word in FlowVocabulary.STAGE_WORDS ->
            FlowTokens.STAGE
        // The word between `in`/`out` and the `:` is the port's own name,
        // whatever else it might have meant somewhere else.
        state == IN_PORT_NAME -> FlowTokens.IDENTIFIER
        // `expr as TYPE`, and `a11.sdk.Interaction{...}`: both name a type
        // where no list of types could know it, so position decides.
        state == AFTER_AS || typeLiteralFollows() -> FlowTokens.TYPE
        // Past a port's `:` a word is its type, whether or not it is one of the
        // built-in names: a type may be named by the tag a serialisation
        // registry knows it by. `stream` and `required` follow the type and
        // say what the port is like instead.
        state == IN_PORT_TYPE ->
            if (word in FlowVocabulary.PORT_MODIFIER_WORDS) {
                FlowTokens.DECLARATION_KEYWORD
            } else {
                FlowTokens.TYPE
            }
        // `then` and `where` may be written without their `|`, and are stages
        // there too -- but only with an operand after them, since a bare one is
        // a port that happens to be spelled like a stage.
        word in FlowVocabulary.BARE_STAGE_WORDS && operandFollows() ->
            FlowTokens.STAGE
        // Making a node takes parentheses, so `node` is the keyword only where
        // one opens: a port called `node` is a name like any other.
        word == "node" && !callFollows() -> FlowTokens.IDENTIFIER
        // A function is only a function where it is called, which is what tells
        // `text(it)` from the port type and the stage of the same name.
        word in FlowVocabulary.BUILTIN_WORDS && callFollows() ->
            FlowTokens.BUILTIN
        word in FlowVocabulary.CONSTANT_WORDS -> FlowTokens.CONSTANT
        word in FlowVocabulary.OPERATOR_WORDS -> FlowTokens.WORD_OPERATOR
        word in FlowVocabulary.DECLARATION_WORDS ->
            FlowTokens.DECLARATION_KEYWORD
        word in FlowVocabulary.STATEMENT_WORDS ->
            if (assignmentFollows()) {
                FlowTokens.IDENTIFIER
            } else {
                FlowTokens.STATEMENT_KEYWORD
            }
        word in FlowVocabulary.MODIFIER_WORDS -> FlowTokens.MODIFIER_KEYWORD
        word in FlowVocabulary.TYPE_WORDS -> FlowTokens.TYPE
        word in FlowVocabulary.STATUS_CODES -> FlowTokens.STATUS_CODE
        else -> FlowTokens.IDENTIFIER
    }

    private fun stateAfter(word: String, produced: IElementType): Int = when {
        // A port declaration runs from `in`/`out` to the end of its line, and
        // the words in it mean what that position makes them mean.
        state == IN_PORT_NAME || state == IN_PORT_TYPE -> state
        // A cast's type is one dotted name; the dots keep the state.
        state == AFTER_AS -> AFTER_AS
        word == "as" && produced === FlowTokens.DECLARATION_KEYWORD -> AFTER_AS
        produced === FlowTokens.MEMBER -> DEFAULT
        produced === FlowTokens.STAGE -> DEFAULT
        (word == "in" || word == "out") &&
            produced === FlowTokens.DECLARATION_KEYWORD &&
            portDeclarationFollows() -> IN_PORT_NAME
        word == "flow" && produced === FlowTokens.DECLARATION_KEYWORD ->
            AFTER_FLOW
        // Both dispatch verbs are followed by the action's name.
        (word == "run" || word == "call") &&
            produced === FlowTokens.STATEMENT_KEYWORD -> AFTER_CALL
        word == "nodes" && produced === FlowTokens.DECLARATION_KEYWORD ->
            AFTER_NODE_MAP
        word == "via" && produced === FlowTokens.MODIFIER_KEYWORD ->
            AFTER_NODE_MAP
        else -> DEFAULT
    }

    private fun readPunctuation() {
        val char = buffer[position]
        val second = if (position + 1 < bufferEnd) buffer[position + 1] else ' '
        nextState = DEFAULT
        when {
            char == '-' && second == '>' -> {
                position += 2
                tokenType = FlowTokens.ARROW
            }

            char == '<' && second == '-' -> {
                position += 2
                tokenType = FlowTokens.CARRY
            }

            (char == '=' || char == '!' || char == '<' || char == '>') &&
                second == '=' -> {
                position += 2
                tokenType = FlowTokens.COMPARISON
            }

            else -> {
                position++
                // A type is written with dots and brackets -- `list[a11.Frag]`
                // -- and none of them ends the type, so they carry the state
                // rather than clearing it.
                val inType = state == IN_PORT_TYPE
                tokenType = when (char) {
                    '{' -> FlowTokens.LEFT_BRACE
                    '}' -> FlowTokens.RIGHT_BRACE
                    '(' -> FlowTokens.LEFT_PAREN
                    ')' -> FlowTokens.RIGHT_PAREN
                    '[' -> {
                        if (inType) nextState = IN_PORT_TYPE
                        FlowTokens.LEFT_BRACKET
                    }

                    ']' -> {
                        if (inType) nextState = IN_PORT_TYPE
                        FlowTokens.RIGHT_BRACKET
                    }

                    ':' -> {
                        // The colon of a port declaration opens its type.
                        if (state == IN_PORT_NAME) nextState = IN_PORT_TYPE
                        FlowTokens.COLON
                    }

                    ',' -> {
                        if (inType) nextState = IN_PORT_TYPE
                        FlowTokens.COMMA
                    }

                    '=' -> FlowTokens.ASSIGN
                    '<', '>' -> FlowTokens.COMPARISON
                    '|' -> {
                        nextState = AFTER_PIPE
                        FlowTokens.PIPE
                    }

                    '.' -> {
                        // Inside a type the dots are part of a tag, not the
                        // accessor that reads a member off a value.
                        nextState = when {
                            inType -> IN_PORT_TYPE
                            state == AFTER_AS -> AFTER_AS
                            typeLiteralFollows() -> DEFAULT
                            else -> AFTER_DOT
                        }
                        FlowTokens.DOT
                    }

                    else -> TokenType.BAD_CHARACTER
                }
            }
        }
    }

    // --- helpers ---------------------------------------------------------------

    private fun text(from: Int, until: Int): String =
        buffer.subSequence(from, until).toString()

    private fun peekIsDigit(ahead: Int): Boolean =
        position + ahead < bufferEnd && buffer[position + ahead].isDigit()

    /**
     * Whether `in`/`out` just read opens a port declaration.
     *
     * `in` is four other things as well -- a `for`'s, a node's, and the
     * comparison -- so the word alone will not do. A port is the one shape
     * where a name and a `:` follow, which is the same lookahead the Sublime
     * definition makes and the same one the parser makes.
     */
    private fun portDeclarationFollows(): Boolean {
        var index = position
        while (index < bufferEnd && (buffer[index] == ' ' || buffer[index] == '\t')) {
            index++
        }
        if (index >= bufferEnd || !isNameStart(buffer[index])) return false
        while (index < bufferEnd) {
            if (isNamePart(buffer[index])) {
                index++
                continue
            }
            if (
                buffer[index] == '-' &&
                index + 1 < bufferEnd &&
                isNamePart(buffer[index + 1])
            ) {
                index += 2
                continue
            }
            break
        }
        while (index < bufferEnd && (buffer[index] == ' ' || buffer[index] == '\t')) {
            index++
        }
        return index < bufferEnd && buffer[index] == ':'
    }

    /**
     * Whether what is being read is the tag of a `Tag{...}` value.
     *
     * The compiler decides this with the token *after* the whole dotted name,
     * which a lexer does not have; so it looks ahead over the rest of the name
     * for the `{` that makes it a type. A `.` is required somewhere in it,
     * because a bare `name {` is a name and a block -- which is what keeps
     * `if outcome {` reading the way it always has.
     */
    private fun typeLiteralFollows(): Boolean {
        var index = position
        var dotted = false
        while (index < bufferEnd) {
            val char = buffer[index]
            if (isNamePart(char)) {
                index++
                continue
            }
            if (char == '-' && index + 1 < bufferEnd && isNamePart(buffer[index + 1])) {
                index += 2
                continue
            }
            if (char == '.' && index + 1 < bufferEnd && isNameStart(buffer[index + 1])) {
                dotted = true
                index++
                continue
            }
            break
        }
        while (index < bufferEnd && (buffer[index] == ' ' || buffer[index] == '\t')) {
            index++
        }
        return dotted && index < bufferEnd && buffer[index] == '{'
    }

    /**
     * Whether something a stage could be applied to follows the word just read.
     *
     * The compiler's rule for a bare `then`/`where`, from
     * `Parser._at_bare_stage`: the next token is not the end of the statement
     * and not one of the punctuation marks that would end the pipeline. Said
     * the other way round, an operand starts with a name, a literal, or an
     * opening bracket.
     */
    private fun operandFollows(): Boolean {
        var index = position
        while (index < bufferEnd && (buffer[index] == ' ' || buffer[index] == '\t')) {
            index++
        }
        if (index == position) return false
        if (index >= bufferEnd) return false
        val char = buffer[index]
        // A `-` is only an operand when a number follows it: `-> port` is where
        // the statement is going, not something to read.
        if (char == '-') {
            return index + 1 < bufferEnd && buffer[index + 1].isDigit()
        }
        return isNameStart(char) || char.isDigit() || char == '"' ||
            char == '(' || char == '[' || char == '{'
    }

    /** Whether an argument list opens right after the word just read. */
    private fun callFollows(): Boolean {
        var index = position
        while (index < bufferEnd && (buffer[index] == ' ' || buffer[index] == '\t')) {
            index++
        }
        return index < bufferEnd && buffer[index] == '('
    }

    /**
     * Whether a single `=` follows, making the word before it a binding name.
     *
     * The compiler's rule, from `Parser._opens_statement`: a statement word is
     * only a keyword where a statement can start, and `run = run x()` binds a
     * step called `run`. `==` is a comparison, so it does not count.
     */
    private fun assignmentFollows(): Boolean {
        var index = position
        while (index < bufferEnd && (buffer[index] == ' ' || buffer[index] == '\t')) {
            index++
        }
        if (index >= bufferEnd || buffer[index] != '=') return false
        return index + 1 >= bufferEnd || buffer[index + 1] != '='
    }

    private fun isNameStart(char: Char): Boolean =
        char.isLetter() || char == '_' || char == '$'

    private fun isNamePart(char: Char): Boolean =
        char.isLetterOrDigit() || char == '_' || char == '$'

    companion object {
        /** Anywhere the word before says nothing about what comes next. */
        const val DEFAULT = 0

        /** Straight after a `.`: whatever follows is a member. */
        const val AFTER_DOT = 1

        /** Straight after `flow`: the flow's own name. */
        const val AFTER_FLOW = 2

        /** Straight after `call`: the action being called. */
        const val AFTER_CALL = 3

        /** Straight after `nodes` or `via`: a node map. */
        const val AFTER_NODE_MAP = 4

        /** Straight after `|`: a stage, and only here. */
        const val AFTER_PIPE = 5

        /** Between a port's `in`/`out` and its `:`. */
        const val IN_PORT_NAME = 6

        /** Past a port's `:`: its type, and what the port is like. */
        const val IN_PORT_TYPE = 7

        /** Straight after `as`: the type a value is being made into. */
        const val AFTER_AS = 8
    }
}
