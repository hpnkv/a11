package dev.curiositystack.a11.clion.flow

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.editor.colors.TextAttributesKey.createTextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.openapi.fileTypes.SyntaxHighlighterFactory
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

/**
 * What each kind of token looks like.
 *
 * Every key falls back to one of the platform's own, so a flow inherits whatever
 * colour scheme the IDE is wearing and a user can still override any of it under
 * Settings | Editor | Color Scheme | A11 Flow.
 */
object FlowColors {
    val COMMENT = key("A11FLOW_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT)
    val STRING = key("A11FLOW_STRING", DefaultLanguageHighlighterColors.STRING)
    val NUMBER = key("A11FLOW_NUMBER", DefaultLanguageHighlighterColors.NUMBER)
    val DURATION = key("A11FLOW_DURATION", DefaultLanguageHighlighterColors.NUMBER)
    val KEYWORD = key("A11FLOW_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD)
    val DECLARATION =
        key("A11FLOW_DECLARATION", DefaultLanguageHighlighterColors.KEYWORD)
    val MODIFIER =
        key("A11FLOW_MODIFIER", DefaultLanguageHighlighterColors.KEYWORD)
    val STAGE =
        key("A11FLOW_STAGE", DefaultLanguageHighlighterColors.INSTANCE_METHOD)
    val BUILTIN =
        key("A11FLOW_BUILTIN", DefaultLanguageHighlighterColors.STATIC_METHOD)
    val TYPE = key("A11FLOW_TYPE", DefaultLanguageHighlighterColors.CLASS_NAME)
    val STATUS_CODE =
        key("A11FLOW_STATUS_CODE", DefaultLanguageHighlighterColors.CONSTANT)
    val CONSTANT =
        key("A11FLOW_CONSTANT", DefaultLanguageHighlighterColors.KEYWORD)
    val FLOW_NAME =
        key("A11FLOW_FLOW_NAME", DefaultLanguageHighlighterColors.CLASS_NAME)
    val ACTION_NAME =
        key("A11FLOW_ACTION_NAME", DefaultLanguageHighlighterColors.FUNCTION_CALL)
    val NODE_MAP_NAME =
        key("A11FLOW_NODE_MAP", DefaultLanguageHighlighterColors.CLASS_REFERENCE)
    val MEMBER =
        key("A11FLOW_MEMBER", DefaultLanguageHighlighterColors.INSTANCE_FIELD)
    val IDENTIFIER =
        key("A11FLOW_IDENTIFIER", DefaultLanguageHighlighterColors.IDENTIFIER)
    val ARROW =
        key("A11FLOW_ARROW", DefaultLanguageHighlighterColors.OPERATION_SIGN)
    val OPERATOR =
        key("A11FLOW_OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN)
    val BRACES = key("A11FLOW_BRACES", DefaultLanguageHighlighterColors.BRACES)
    val PARENTHESES =
        key("A11FLOW_PARENTHESES", DefaultLanguageHighlighterColors.PARENTHESES)
    val BRACKETS =
        key("A11FLOW_BRACKETS", DefaultLanguageHighlighterColors.BRACKETS)
    val PUNCTUATION =
        key("A11FLOW_PUNCTUATION", DefaultLanguageHighlighterColors.SEMICOLON)
    val BAD_CHARACTER =
        key("A11FLOW_BAD_CHARACTER", com.intellij.openapi.editor.HighlighterColors.BAD_CHARACTER)

    private fun key(name: String, fallback: TextAttributesKey) =
        createTextAttributesKey(name, fallback)
}

class FlowSyntaxHighlighter : SyntaxHighlighterBase() {

    override fun getHighlightingLexer(): Lexer = FlowLexer()

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> =
        ATTRIBUTES[tokenType]?.let { arrayOf(it) } ?: EMPTY

    private companion object {
        val EMPTY = emptyArray<TextAttributesKey>()

        val ATTRIBUTES: Map<IElementType, TextAttributesKey> = mapOf(
            FlowTokens.COMMENT to FlowColors.COMMENT,
            FlowTokens.STRING to FlowColors.STRING,
            FlowTokens.NUMBER to FlowColors.NUMBER,
            FlowTokens.DURATION to FlowColors.DURATION,
            FlowTokens.DECLARATION_KEYWORD to FlowColors.DECLARATION,
            FlowTokens.STATEMENT_KEYWORD to FlowColors.KEYWORD,
            FlowTokens.MODIFIER_KEYWORD to FlowColors.MODIFIER,
            FlowTokens.WORD_OPERATOR to FlowColors.KEYWORD,
            FlowTokens.STAGE to FlowColors.STAGE,
            FlowTokens.BUILTIN to FlowColors.BUILTIN,
            FlowTokens.TYPE to FlowColors.TYPE,
            FlowTokens.STATUS_CODE to FlowColors.STATUS_CODE,
            FlowTokens.CONSTANT to FlowColors.CONSTANT,
            FlowTokens.FLOW_NAME to FlowColors.FLOW_NAME,
            FlowTokens.ACTION_NAME to FlowColors.ACTION_NAME,
            FlowTokens.NODE_MAP_NAME to FlowColors.NODE_MAP_NAME,
            FlowTokens.MEMBER to FlowColors.MEMBER,
            FlowTokens.IDENTIFIER to FlowColors.IDENTIFIER,
            FlowTokens.ARROW to FlowColors.ARROW,
            FlowTokens.CARRY to FlowColors.ARROW,
            FlowTokens.PIPE to FlowColors.ARROW,
            FlowTokens.ASSIGN to FlowColors.OPERATOR,
            FlowTokens.COMPARISON to FlowColors.OPERATOR,
            FlowTokens.DOT to FlowColors.PUNCTUATION,
            FlowTokens.COLON to FlowColors.PUNCTUATION,
            FlowTokens.COMMA to FlowColors.PUNCTUATION,
            FlowTokens.LEFT_BRACE to FlowColors.BRACES,
            FlowTokens.RIGHT_BRACE to FlowColors.BRACES,
            FlowTokens.LEFT_PAREN to FlowColors.PARENTHESES,
            FlowTokens.RIGHT_PAREN to FlowColors.PARENTHESES,
            FlowTokens.LEFT_BRACKET to FlowColors.BRACKETS,
            FlowTokens.RIGHT_BRACKET to FlowColors.BRACKETS,
            TokenType.BAD_CHARACTER to FlowColors.BAD_CHARACTER,
        )
    }
}

class FlowSyntaxHighlighterFactory : SyntaxHighlighterFactory() {
    override fun getSyntaxHighlighter(
        project: Project?,
        virtualFile: VirtualFile?,
    ): SyntaxHighlighter = FlowSyntaxHighlighter()
}
