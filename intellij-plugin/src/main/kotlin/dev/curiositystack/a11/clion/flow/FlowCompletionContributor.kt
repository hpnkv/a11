package dev.curiositystack.a11.clion.flow

import com.intellij.codeInsight.completion.CompletionContributor
import com.intellij.codeInsight.completion.CompletionParameters
import com.intellij.codeInsight.completion.CompletionResultSet
import com.intellij.codeInsight.completion.InsertHandler
import com.intellij.codeInsight.completion.InsertionContext
import com.intellij.codeInsight.completion.PrioritizedLookupElement
import com.intellij.codeInsight.lookup.LookupElement
import com.intellij.codeInsight.lookup.LookupElementBuilder
import com.intellij.icons.AllIcons
import javax.swing.Icon

/** One thing the language says may be written at the caret. */
data class FlowProposal(
    val name: String,
    val kind: String,
    val insert: String,
    val caret: Int,
    val tail: String,
    val type: String,
    /**
     * Everything known about it, as Markdown, for the popup beside the list.
     *
     * The same text hovering the finished word gives. Empty where the name says
     * all there is to say -- a stage, a status code -- so the popup stays shut
     * rather than repeating the one line already in the list.
     */
    val documentation: String,
)

/**
 * What may be written at the caret, offered.
 *
 * Every interesting decision is the language's: after a `|` only a stage, past a
 * port's `:` only a type, after `x.` only what `x` has, and the order they come in
 * is the order they should be shown in. What is left here is turning that list into
 * lookup elements -- an icon, a priority, and the text an accepted proposal writes.
 *
 * The original file is read rather than the platform's copy: the copy has a dummy
 * identifier spliced in at the caret to give a parser something to hold, and a
 * language whose meaning depends on the token before would be reading a word that
 * is not there.
 *
 * The prefix is taken from the text as well -- because a flow's names contain
 * dashes, which the platform's default prefix would cut in half: `web-` has to
 * still be looking for `web-search`.
 */
class FlowCompletionContributor : CompletionContributor() {

    override fun fillCompletionVariants(
        parameters: CompletionParameters,
        result: CompletionResultSet,
    ) {
        val file = parameters.originalFile
        if (file !is FlowPsiFile) return
        val text = file.text
        val offset = parameters.offset.coerceIn(0, text.length)

        val payload = FlowEngine.instance().complete(text, offset) ?: return
        val proposals = proposalsOf(payload)
        if (proposals.isEmpty()) return

        val prefixed = result.withPrefixMatcher(
            payload["prefix"] as? String ?: prefixBefore(text, offset),
        )
        // The language's order is the useful one, and the platform sorts by
        // priority: highest first, so the list is walked backwards into it.
        for ((index, proposal) in proposals.withIndex()) {
            prefixed.addElement(element(proposal, proposals.size - index))
        }
    }

    private fun proposalsOf(payload: Map<String, Any?>): List<FlowProposal> {
        val listed = payload["proposals"] as? List<*> ?: return emptyList()
        return listed.mapNotNull { entry ->
            val proposal = entry as? Map<*, *> ?: return@mapNotNull null
            val name = proposal["name"] as? String ?: return@mapNotNull null
            val insert = proposal["insert"] as? String ?: name
            FlowProposal(
                name = name,
                kind = proposal["kind"] as? String ?: "",
                insert = insert,
                caret = (proposal["caret"] as? Number)?.toInt() ?: insert.length,
                tail = proposal["tail"] as? String ?: "",
                type = proposal["type"] as? String ?: "",
                documentation = proposal["documentation"] as? String ?: "",
            )
        }
    }

    private fun prefixBefore(text: CharSequence, offset: Int): String {
        var start = offset
        while (start > 0 && isPrefixChar(text[start - 1])) start--
        return text.subSequence(start, offset).toString()
    }

    private fun isPrefixChar(char: Char): Boolean =
        char.isLetterOrDigit() || char == '_' || char == '$' || char == '-'

    private fun element(proposal: FlowProposal, rank: Int): LookupElement {
        var element = LookupElementBuilder
            .create(proposal, proposal.name)
            .withPresentableText(proposal.name)
            .withTailText(proposal.tail, true)
            .withIcon(iconFor(proposal.kind))
        if (proposal.type.isNotEmpty()) {
            element = element.withTypeText(proposal.type)
        }
        if (proposal.insert != proposal.name || proposal.caret != proposal.insert.length) {
            element = element.withInsertHandler(Insert(proposal))
        }
        return PrioritizedLookupElement.withPriority(element, rank.toDouble())
    }

    /**
     * The icon for one of the language's proposal kinds.
     *
     * The names are the `flow.completions/v1` contract. A kind this has not been
     * taught gets no icon, which is how a language that gains one degrades.
     */
    private fun iconFor(kind: String): Icon? = when (kind) {
        "stage", "function", "call" -> AllIcons.Nodes.Method
        "type", "flow" -> AllIcons.Nodes.Class
        "status-code", "constant" -> AllIcons.Nodes.Constant
        "port" -> AllIcons.Nodes.Parameter
        "field", "header" -> AllIcons.Nodes.Field
        "node", "barrier", "variable" -> AllIcons.Nodes.Variable
        "node-map" -> AllIcons.Nodes.Package
        else -> null
    }

    /** Writes what the proposal actually inserts, and puts the caret in it. */
    private class Insert(private val proposal: FlowProposal) :
        InsertHandler<LookupElement> {

        override fun handleInsert(context: InsertionContext, item: LookupElement) {
            context.document.replaceString(
                context.startOffset,
                context.tailOffset,
                proposal.insert,
            )
            context.commitDocument()
            context.editor.caretModel.moveToOffset(
                context.startOffset + proposal.caret.coerceIn(0, proposal.insert.length),
            )
        }
    }
}
