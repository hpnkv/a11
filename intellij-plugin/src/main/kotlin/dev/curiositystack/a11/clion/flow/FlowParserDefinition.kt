package dev.curiositystack.a11.clion.flow

import com.intellij.extapi.psi.ASTWrapperPsiElement
import com.intellij.extapi.psi.PsiFileBase
import com.intellij.lang.ASTNode
import com.intellij.lang.ParserDefinition
import com.intellij.lang.PsiParser
import com.intellij.lexer.Lexer
import com.intellij.openapi.project.Project
import com.intellij.psi.FileViewProvider
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IFileElementType
import com.intellij.psi.tree.TokenSet

/**
 * Enough of a language for the platform to hold a flow.
 *
 * The tree is deliberately flat: every token hangs off the file. Colour comes
 * from the lexer, and nothing here needs structure -- but a language cannot be
 * *injected* into a string literal without a parser definition, and a flat one
 * has the useful property that a fragment can never be a syntax error. A partial
 * flow pasted into a docstring highlights; it does not turn red.
 */
class FlowParserDefinition : ParserDefinition {

    override fun createLexer(project: Project?): Lexer = FlowLexer()

    override fun createParser(project: Project?): PsiParser = FlowParser()

    override fun getFileNodeType(): IFileElementType = FILE

    override fun getCommentTokens(): TokenSet = FlowTokens.COMMENTS

    override fun getStringLiteralElements(): TokenSet = FlowTokens.STRINGS

    override fun createElement(node: ASTNode): PsiElement =
        ASTWrapperPsiElement(node)

    override fun createFile(viewProvider: FileViewProvider): PsiFile =
        FlowPsiFile(viewProvider)

    companion object {
        val FILE = IFileElementType(FlowLanguage)
    }
}

/** A file of flows, or a flow injected into somebody else's string. */
class FlowPsiFile(viewProvider: FileViewProvider) :
    PsiFileBase(viewProvider, FlowLanguage) {

    override fun getFileType() = FlowFileType

    override fun toString(): String = "A11 Flow file"
}

private class FlowParser : PsiParser {
    override fun parse(root: com.intellij.psi.tree.IElementType, builder: com.intellij.lang.PsiBuilder): ASTNode {
        val file = builder.mark()
        while (!builder.eof()) {
            builder.advanceLexer()
        }
        file.done(root)
        return builder.treeBuilt
    }
}
