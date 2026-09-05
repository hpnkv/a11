/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
 * The tree is flat: every token is a child of the file. Colour comes from the
 * lexer, while semantic structure remains in the native service. A flat parser
 * definition also allows partial Flow fragments in string literals to
 * highlight without parser errors.
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
