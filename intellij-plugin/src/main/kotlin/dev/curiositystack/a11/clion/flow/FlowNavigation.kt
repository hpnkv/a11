package dev.curiositystack.a11.clion.flow

import com.intellij.lang.Language
import com.intellij.lang.documentation.AbstractDocumentationProvider
import com.intellij.psi.PsiManager
import com.intellij.psi.impl.FakePsiElement
import com.intellij.navigation.ItemPresentation
import com.intellij.openapi.editor.Editor
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.ide.structureView.StructureViewBuilder
import com.intellij.ide.structureView.StructureViewModel
import com.intellij.ide.structureView.StructureViewModelBase
import com.intellij.ide.structureView.StructureViewTreeElement
import com.intellij.ide.structureView.TreeBasedStructureViewBuilder
import com.intellij.ide.util.treeView.smartTree.SortableTreeElement
import com.intellij.lang.PsiStructureViewFactory
import javax.swing.Icon

/**
 * Hover, go-to-declaration and the structure view, all answered by the language.
 *
 * A flow's PSI is a flat token stream -- deliberately, since the parse tree is
 * the compiler's and there is no second parser in Kotlin. So none of these can
 * be answered by walking a tree here; each is a question about *meaning*, and
 * each is put to [FlowEngine] at an offset. That is the same rule the annotator
 * and the completion contributor follow, and it is why hovering an action's name
 * can show that action's ports: the language knows what the world contains, and
 * the plugin does not have to.
 */

/** Markdown as the small subset of HTML the documentation popup renders. */
internal fun markdownToHtml(markdown: String): String {
    val out = StringBuilder("<html><body>")
    var inList = false
    for (raw in markdown.lines()) {
        val line = raw.trim()
        if (line.startsWith("- ")) {
            if (!inList) {
                out.append("<ul>")
                inList = true
            }
            out.append("<li>").append(inline(line.removePrefix("- "))).append("</li>")
            continue
        }
        if (inList) {
            out.append("</ul>")
            inList = false
        }
        if (line.isEmpty()) {
            out.append("<p>")
            continue
        }
        out.append(inline(line)).append("<br/>")
    }
    if (inList) out.append("</ul>")
    return out.append("</body></html>").toString()
}

/** `` `code` `` and `**bold**`, which is all the language's own prose uses. */
private fun inline(text: String): String {
    val escaped = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    return CODE.replace(BOLD.replace(escaped) { "<b>${it.groupValues[1]}</b>" }) {
        "<code>${it.groupValues[1]}</code>"
    }
}

private val BOLD = Regex("""\*\*(.+?)\*\*""")
private val CODE = Regex("""`([^`]+)`""")

/**
 * What hovering something in a flow shows.
 *
 * Over an action's name that is its description and a list of every port it has,
 * which is the thing a flow author most often has to leave the file to find out.
 * Over a shape it is the shape's fields; over a port, a node or a loop variable
 * it is what the flow said about it.
 */
class FlowDocumentationProvider : AbstractDocumentationProvider() {

    override fun getCustomDocumentationElement(
        editor: Editor,
        file: PsiFile,
        contextElement: PsiElement?,
        targetOffset: Int,
    ): PsiElement? {
        // The PSI is a token stream, so the element the platform found *is* the
        // target; taking it here is what stops the platform looking for a
        // declaration that a flat tree cannot have.
        if (file !is FlowPsiFile) return null
        return contextElement
    }

    override fun generateDoc(element: PsiElement, original: PsiElement?): String? {
        if (element is FlowLookupDocumentation) return markdownToHtml(element.markdown)
        val about = describe(element, original) ?: return null
        val markdown = about["markdown"] as? String ?: return null
        return markdownToHtml(markdown)
    }

    override fun getQuickNavigateInfo(element: PsiElement, original: PsiElement?): String? {
        val about = describe(element, original) ?: return null
        return (about["summary"] as? String)?.let { markdownToHtml(it) }
    }

    /**
     * The documentation popup beside an item in the completion list.
     *
     * A lookup item is a [FlowProposal] and not a PSI element -- there is no
     * declaration in the document to point at, since the whole point is that the
     * word has not been written yet -- so the language's answer is carried on a
     * throwaway element the platform can ask about. Without this the popup has
     * nothing to show, which is what left `interact_with_llm` in the list with
     * one line of grey text and no way to see its ports.
     */
    override fun getDocumentationElementForLookupItem(
        psiManager: PsiManager,
        obj: Any?,
        element: PsiElement?,
    ): PsiElement? {
        val proposal = obj as? FlowProposal ?: return null
        if (proposal.documentation.isEmpty()) return null
        val context = element ?: return null
        return FlowLookupDocumentation(context, proposal.documentation)
    }

    /**
     * What the language says is at the caret.
     *
     * `original` is the token the reader is actually on; `element` is whatever
     * the platform resolved it to, which for a name this file declares is the
     * *declaration* -- so asking about `element` answered a hover over a call to
     * `research` with whatever is at `flow research`. The word under the caret is
     * the question, so it is the offset that goes out.
     */
    private fun describe(element: PsiElement, original: PsiElement?): Map<String, Any?>? {
        val at = original ?: element
        val file = at.containingFile as? FlowPsiFile ?: return null
        val about = FlowEngine.instance()
            .describe(file.text, at.textRange.startOffset) ?: return null
        return about.takeIf { it["found"] == true }
    }
}

/**
 * A completion proposal's documentation, as something the platform can ask about.
 *
 * Not in the file and never in the tree: it exists for the length of one popup,
 * which is why it is a fake element rather than anything the PSI has to know.
 */
private class FlowLookupDocumentation(
    private val context: PsiElement,
    val markdown: String,
) : FakePsiElement() {

    override fun getParent(): PsiElement = context

    override fun getLanguage(): Language = FlowLanguage

    override fun getContainingFile(): PsiFile? = context.containingFile
}

/** One entry of a `flow.symbols/v1` answer, as the structure view shows it. */
private class FlowSymbol(
    private val file: FlowPsiFile,
    private val described: Map<String, Any?>,
) : StructureViewTreeElement, SortableTreeElement, ItemPresentation {

    override fun getValue(): Any = described

    override fun getPresentation(): ItemPresentation = this

    override fun getPresentableText(): String = described["name"] as? String ?: "?"

    override fun getLocationString(): String? = described["detail"] as? String

    override fun getIcon(open: Boolean): Icon? = null

    override fun getAlphaSortKey(): String = presentableText

    @Suppress("UNCHECKED_CAST")
    override fun getChildren(): Array<StructureViewTreeElement> {
        val children = described["children"] as? List<Map<String, Any?>> ?: return emptyArray()
        return children.map { FlowSymbol(file, it) }.toTypedArray()
    }

    override fun navigate(requestFocus: Boolean) {
        val offset = offsetOf("selection") ?: return
        PsiTreeUtil.findElementOfClassAtOffset(file, offset, PsiElement::class.java, false)
        com.intellij.openapi.fileEditor.OpenFileDescriptor(
            file.project,
            file.virtualFile ?: return,
            offset,
        ).navigate(requestFocus)
    }

    override fun canNavigate(): Boolean = file.virtualFile != null

    override fun canNavigateToSource(): Boolean = canNavigate()

    /** The start offset of one of the answer's ranges, in the units the IDE uses. */
    @Suppress("UNCHECKED_CAST")
    private fun offsetOf(key: String): Int? {
        val range = described[key] as? Map<String, Any?> ?: return null
        val start = range["start"] as? Map<String, Any?> ?: return null
        return (start["offset"] as? Number)?.toInt()
    }
}

/** The root of the structure view: the file, and what it declares. */
private class FlowStructureModel(private val file: FlowPsiFile) :
    StructureViewModelBase(file, FlowPsiFileRoot(file)),
    StructureViewModel.ElementInfoProvider {

    override fun isAlwaysShowsPlus(element: StructureViewTreeElement): Boolean = false

    override fun isAlwaysLeaf(element: StructureViewTreeElement): Boolean =
        element.children.isEmpty()
}

private class FlowPsiFileRoot(private val file: FlowPsiFile) :
    StructureViewTreeElement, ItemPresentation {

    override fun getValue(): Any = file

    override fun getPresentation(): ItemPresentation = this

    override fun getPresentableText(): String = file.name

    override fun getLocationString(): String? = null

    override fun getIcon(open: Boolean): Icon? = null

    override fun navigate(requestFocus: Boolean) = Unit

    override fun canNavigate(): Boolean = false

    override fun canNavigateToSource(): Boolean = false

    @Suppress("UNCHECKED_CAST")
    override fun getChildren(): Array<StructureViewTreeElement> {
        val answer = FlowEngine.instance().symbols(file.text) ?: return emptyArray()
        val symbols = answer["symbols"] as? List<Map<String, Any?>> ?: return emptyArray()
        return symbols.map { FlowSymbol(file, it) }.toTypedArray()
    }
}

/**
 * "Go to symbol" and the structure view, from `flow.symbols/v1`.
 *
 * A flat PSI has no declarations for the platform to index, so the outline comes
 * from the language instead -- which is also how it knows that a `struct`'s fields
 * belong under it and a flow's ports belong under it.
 */
class FlowStructureViewFactory : PsiStructureViewFactory {
    override fun getStructureViewBuilder(file: PsiFile): StructureViewBuilder? {
        if (file !is FlowPsiFile) return null
        return object : TreeBasedStructureViewBuilder() {
            override fun createStructureViewModel(editor: Editor?): StructureViewModel =
                FlowStructureModel(file)
        }
    }
}

/** Go to declaration: the language says where the name under the caret was bound. */
class FlowDeclarationHandler :
    com.intellij.codeInsight.navigation.actions.GotoDeclarationHandler {

    override fun getGotoDeclarationTargets(
        source: PsiElement?,
        offset: Int,
        editor: Editor?,
    ): Array<PsiElement>? {
        val file = source?.containingFile as? FlowPsiFile ?: return null
        val answer = FlowEngine.instance().definition(file.text, offset) ?: return null
        if (answer["found"] != true) {
            // Not a name of this document, which used to be the end of it. It may
            // still be an action declared in one of the project's own files, and
            // the language now says where: that is the one target that leaves the
            // flow, and the reason an `ActionSchema` in a `.py` two directories
            // away is a jump rather than a search.
            return elsewhere(file, answer)
        }
        @Suppress("UNCHECKED_CAST")
        val range = answer["range"] as? Map<String, Any?> ?: return null
        @Suppress("UNCHECKED_CAST")
        val start = range["start"] as? Map<String, Any?> ?: return null
        val at = (start["offset"] as? Number)?.toInt() ?: return null
        if (at !in 0..file.textLength) return null
        // A flat PSI has nothing to point at but the token, which is exactly
        // where the declaration is -- so the element covering that offset is the
        // target.
        val target = file.findElementAt(at.coerceAtMost(file.textLength - 1)) ?: return null
        // Jumping to where you already are is not navigation; the platform shows
        // a "no declaration found" hint, which is the honest answer. The caret
        // being *inside* the declaration's own token is what that looks like --
        // comparing against an empty range at the offset never matched anything,
        // since a token's range is never empty.
        if (target.textRange.containsOffset(offset)) return null
        return arrayOf(target)
    }

    /**
     * The declaration an `origin` points at, in another file.
     *
     * The path is the one the scan was given, which is the project's own base
     * path, so a relative one is resolved against it. A line and a column rather
     * than an offset, because the answer travelled from a file this process never
     * opened; the document is here now, so the offset is worked out from them.
     */
    private fun elsewhere(
        file: FlowPsiFile,
        answer: Map<String, Any?>,
    ): Array<PsiElement>? {
        @Suppress("UNCHECKED_CAST")
        val origin = answer["origin"] as? Map<String, Any?> ?: return null
        val path = origin["file"] as? String ?: return null
        val project = file.project
        val found = com.intellij.openapi.vfs.LocalFileSystem.getInstance().let { fs ->
            fs.findFileByPath(path)
                ?: project.basePath?.let { fs.findFileByPath("$it/$path") }
        } ?: return null
        val declared = PsiManager.getInstance(project).findFile(found) ?: return null
        val line = (origin["line"] as? Number)?.toInt() ?: 1
        val column = (origin["column"] as? Number)?.toInt() ?: 1
        val document = com.intellij.psi.PsiDocumentManager.getInstance(project)
            .getDocument(declared) ?: return arrayOf(declared)
        val zeroLine = (line - 1).coerceIn(0, (document.lineCount - 1).coerceAtLeast(0))
        val lineStart = document.getLineStartOffset(zeroLine)
        val lineEnd = document.getLineEndOffset(zeroLine)
        val at = (lineStart + (column - 1).coerceAtLeast(0)).coerceIn(lineStart, lineEnd)
        // The element covering the declaration's own position, or the file itself
        // where the host language has no PSI there. Either way the caret lands on
        // the line, which is what somebody following the jump wanted.
        return arrayOf(declared.findElementAt(at) ?: declared)
    }
}
