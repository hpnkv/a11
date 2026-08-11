package dev.curiositystack.a11.clion.flow

import com.intellij.lang.BracePair
import com.intellij.lang.Commenter
import com.intellij.lang.PairedBraceMatcher
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.options.colors.AttributesDescriptor
import com.intellij.openapi.options.colors.ColorDescriptor
import com.intellij.openapi.options.colors.ColorSettingsPage
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IElementType
import javax.swing.Icon

/** `#` to the end of the line, which is the only comment a flow has. */
class FlowCommenter : Commenter {
    override fun getLineCommentPrefix(): String = "# "

    override fun getBlockCommentPrefix(): String? = null

    override fun getBlockCommentSuffix(): String? = null

    override fun getCommentedBlockCommentPrefix(): String? = null

    override fun getCommentedBlockCommentSuffix(): String? = null
}

/** Brace, parenthesis and bracket matching, and the structural highlight. */
class FlowBraceMatcher : PairedBraceMatcher {

    override fun getPairs(): Array<BracePair> = PAIRS

    override fun isPairedBracesAllowedBeforeType(
        type: IElementType,
        contextType: IElementType?,
    ): Boolean = true

    override fun getCodeConstructStart(file: PsiFile?, openingBraceOffset: Int): Int =
        openingBraceOffset

    private companion object {
        val PAIRS = arrayOf(
            // A `{` opens a structural block -- a flow, a loop, a branch -- so
            // the platform may fold on it and show the matching line.
            BracePair(FlowTokens.LEFT_BRACE, FlowTokens.RIGHT_BRACE, true),
            BracePair(FlowTokens.LEFT_PAREN, FlowTokens.RIGHT_PAREN, false),
            BracePair(FlowTokens.LEFT_BRACKET, FlowTokens.RIGHT_BRACKET, false),
        )
    }
}

/**
 * Settings | Editor | Color Scheme | A11 Flow.
 *
 * The sample is a real flow rather than a list of tokens: it is the page's job
 * to show what reading one looks like.
 */
class FlowColorSettingsPage : ColorSettingsPage {

    override fun getDisplayName(): String = "A11 Flow"

    override fun getIcon(): Icon? = FlowIcons.FILE

    override fun getHighlighter(): SyntaxHighlighter = FlowSyntaxHighlighter()

    override fun getAttributeDescriptors(): Array<AttributesDescriptor> =
        DESCRIPTORS

    override fun getColorDescriptors(): Array<ColorDescriptor> =
        ColorDescriptor.EMPTY_ARRAY

    override fun getAdditionalHighlightingTagToDescriptorMap():
        MutableMap<String, TextAttributesKey>? = null

    override fun getDemoText(): String = DEMO

    private companion object {
        val DESCRIPTORS = arrayOf(
            AttributesDescriptor("Comment", FlowColors.COMMENT),
            AttributesDescriptor("String", FlowColors.STRING),
            AttributesDescriptor("Number", FlowColors.NUMBER),
            AttributesDescriptor("Duration", FlowColors.DURATION),
            AttributesDescriptor("Keyword//Statement", FlowColors.KEYWORD),
            AttributesDescriptor("Keyword//Declaration", FlowColors.DECLARATION),
            AttributesDescriptor("Keyword//Call modifier", FlowColors.MODIFIER),
            AttributesDescriptor("Keyword//Literal", FlowColors.CONSTANT),
            AttributesDescriptor("Pipeline stage", FlowColors.STAGE),
            AttributesDescriptor("Built-in function", FlowColors.BUILTIN),
            AttributesDescriptor("Port type", FlowColors.TYPE),
            AttributesDescriptor("Status code", FlowColors.STATUS_CODE),
            AttributesDescriptor("Name//Flow", FlowColors.FLOW_NAME),
            AttributesDescriptor("Name//Action called", FlowColors.ACTION_NAME),
            AttributesDescriptor("Name//Node map", FlowColors.NODE_MAP_NAME),
            AttributesDescriptor("Name//Port or field", FlowColors.MEMBER),
            AttributesDescriptor("Name//Identifier", FlowColors.IDENTIFIER),
            AttributesDescriptor("Operator//Into and pipe", FlowColors.ARROW),
            AttributesDescriptor("Operator//Other", FlowColors.OPERATOR),
            AttributesDescriptor("Braces", FlowColors.BRACES),
            AttributesDescriptor("Parentheses", FlowColors.PARENTHESES),
            AttributesDescriptor("Brackets", FlowColors.BRACKETS),
            AttributesDescriptor("Punctuation", FlowColors.PUNCTUATION),
            AttributesDescriptor("Bad character", FlowColors.BAD_CHARACTER),
        )

        val DEMO = """
            # A composition of existing actions, which is itself an action.
            flow research {
              describe "Search, read the best hits, and answer from them."

              in  question: string required "What to find out."
              in  clips:    a11.sdk.AudioBuffer stream
              out answer:   string
              out sources:  string stream
              out heard:    list[string]

              header "x-a11-deadline" as deadline

              search = call web-search(query: question, limit: 3)
              brief  = call llm-summarize(question: question)
                  with "x-a11-deadline": deadline

              spoken = call transcribe(audio: clips | packb)
              spoken.words | collect -> heard

              nodes fetched {
                for hit in search.hits parallel 2 {
                  page = try call web-fetch(url: hit.url) timeout 20s
                  outcome = wait page

                  hit.url -> sources
                  page.text | truncate 200 -> brief.pages
                  skip page.bytes

                  if not outcome.ok {
                    fail unavailable outcome.message
                  }
                }
              }

              brief.summary -> answer
              skip search.debug
            }
        """.trimIndent()
    }
}
