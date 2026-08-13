package dev.curiositystack.a11.clion.flow

import com.intellij.psi.tree.IElementType
import com.intellij.psi.tree.TokenSet

/** One kind of token in a flow. */
class FlowTokenType(debugName: String) : IElementType(debugName, FlowLanguage) {
    override fun toString(): String = "A11Flow:" + super.toString()
}

/**
 * The tokens the highlighter colours.
 *
 * They are finer-grained than the compiler's: a word is a keyword to the
 * compiler wherever it is significant, but an editor wants to tell a port type
 * from a pipeline stage from a status code, because that is the distinction a
 * reader is making.
 */
object FlowTokens {
    val COMMENT = FlowTokenType("COMMENT")
    val STRING = FlowTokenType("STRING")
    val NUMBER = FlowTokenType("NUMBER")
    val DURATION = FlowTokenType("DURATION")

    /** `flow`, `in`, `out`, `header`, `node`, `nodes`, and their friends. */
    val DECLARATION_KEYWORD = FlowTokenType("DECLARATION_KEYWORD")

    /** `call`, `wait`, `for`, `if`, `fail`, and the rest of the statements. */
    val STATEMENT_KEYWORD = FlowTokenType("STATEMENT_KEYWORD")

    /** `local`, `via`, `timeout`, `with`, and the other call modifiers. */
    val MODIFIER_KEYWORD = FlowTokenType("MODIFIER_KEYWORD")

    /** A stage, directly after a `|`. */
    val STAGE = FlowTokenType("STAGE")

    /** One of the fixed functions, where it is being called. */
    val BUILTIN = FlowTokenType("BUILTIN")

    /** A port type: `string`, `object`, `bytes`. */
    val TYPE = FlowTokenType("TYPE")

    /** A canonical status code: `not_found`, `NOT_FOUND`. */
    val STATUS_CODE = FlowTokenType("STATUS_CODE")

    /** `true`, `false`, `null`, `it`. */
    val CONSTANT = FlowTokenType("CONSTANT")

    /** `and`, `or`, `not`. */
    val WORD_OPERATOR = FlowTokenType("WORD_OPERATOR")

    /** The name a `flow` declaration is giving. */
    val FLOW_NAME = FlowTokenType("FLOW_NAME")

    /** The action a `call` names. */
    val ACTION_NAME = FlowTokenType("ACTION_NAME")

    /** The name a `nodes` declaration, or a `via`, gives a node map. */
    val NODE_MAP_NAME = FlowTokenType("NODE_MAP_NAME")

    /** What follows a `.`: a port, a field, a node's id. */
    val MEMBER = FlowTokenType("MEMBER")

    /**
     * A port of the flow: its declaration, and every mention of it.
     *
     * Its own type because a port is the one name that crosses the flow's
     * boundary -- everything else a flow binds is local plumbing -- and seeing
     * which is which is what a reader following the data wants. The language
     * decides it (it needs name resolution); this only carries the answer.
     */
    val PORT_NAME = FlowTokenType("PORT_NAME")

    val IDENTIFIER = FlowTokenType("IDENTIFIER")

    val ARROW = FlowTokenType("ARROW")
    val CARRY = FlowTokenType("CARRY")
    val PIPE = FlowTokenType("PIPE")
    val ASSIGN = FlowTokenType("ASSIGN")
    val COMPARISON = FlowTokenType("COMPARISON")

    /**
     * `+` and `-` between two values.
     *
     * The only arithmetic the language has, and it is there for durations -- see
     * `Parser.parse_additive`. A `-` reaches here only when no digit follows it,
     * because `-3` is one number.
     */
    val ARITHMETIC = FlowTokenType("ARITHMETIC")
    val DOT = FlowTokenType("DOT")
    val COLON = FlowTokenType("COLON")
    val COMMA = FlowTokenType("COMMA")

    val LEFT_BRACE = FlowTokenType("LEFT_BRACE")
    val RIGHT_BRACE = FlowTokenType("RIGHT_BRACE")
    val LEFT_PAREN = FlowTokenType("LEFT_PAREN")
    val RIGHT_PAREN = FlowTokenType("RIGHT_PAREN")
    val LEFT_BRACKET = FlowTokenType("LEFT_BRACKET")
    val RIGHT_BRACKET = FlowTokenType("RIGHT_BRACKET")

    val COMMENTS = TokenSet.create(COMMENT)
    val STRINGS = TokenSet.create(STRING)

    val KEYWORDS = TokenSet.create(
        DECLARATION_KEYWORD,
        STATEMENT_KEYWORD,
        MODIFIER_KEYWORD,
        WORD_OPERATOR,
    )

    val BRACES = TokenSet.create(
        LEFT_BRACE,
        RIGHT_BRACE,
        LEFT_PAREN,
        RIGHT_PAREN,
        LEFT_BRACKET,
        RIGHT_BRACKET,
    )
}
