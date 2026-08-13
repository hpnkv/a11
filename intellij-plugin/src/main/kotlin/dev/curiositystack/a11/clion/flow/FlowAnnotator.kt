package dev.curiositystack.a11.clion.flow

import com.intellij.codeInsight.intention.IntentionAction
import com.intellij.lang.annotation.AnnotationHolder
import com.intellij.lang.annotation.ExternalAnnotator
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.colors.CodeInsightColors
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.TextRange
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiFile
import com.intellij.util.IncorrectOperationException

/** One edit the language says would fix something. */
data class FlowEdit(val start: Int, val end: Int, val text: String)

/** One fix: what to call it, and the edits that are it. */
data class FlowFix(val label: String, val edits: List<FlowEdit>)

/** One problem the language found, ready to be drawn. */
data class FlowProblem(
    val code: String,
    val severity: String,
    val family: String,
    val message: String,
    val start: Int,
    val end: Int,
    val fixes: List<FlowFix>,
)

/**
 * Everything wrong with a flow, from the language rather than from here.
 *
 * This replaced five `LocalInspectionTool`s over a Kotlin resolver. What they
 * found -- a `try` whose failure nothing looks at, a `| drop 3` after a
 * `| collect`, an `out` port nothing writes -- is now found by `a11-flow check`,
 * which is the same code `a11 flow check` and CI run: one set of messages, one set
 * of severities, one place a new check is added.
 *
 * An `ExternalAnnotator` because that is the platform's hook for "ask something
 * outside the IDE": [doAnnotate] runs off the UI thread, so a process round trip
 * costs nothing anybody waits for.
 *
 * A finding's range is an offset into the file being annotated, which for a flow
 * injected into somebody else's string is an offset into the fragment; the
 * platform maps it back to the host document itself.
 */
class FlowAnnotator : ExternalAnnotator<String, List<FlowProblem>>() {

    override fun collectInformation(file: PsiFile): String? =
        if (file is FlowPsiFile) file.text else null

    @Suppress("UNCHECKED_CAST")
    override fun doAnnotate(collectedInfo: String?): List<FlowProblem> {
        val text = collectedInfo ?: return emptyList()
        val payload = FlowEngine.instance().check(text) ?: return emptyList()
        val listed = payload["diagnostics"] as? List<*> ?: return emptyList()
        return listed.mapNotNull { entry -> problemOf(entry as? Map<*, *> ?: return@mapNotNull null) }
    }

    override fun apply(
        file: PsiFile,
        annotationResult: List<FlowProblem>?,
        holder: AnnotationHolder,
    ) {
        val problems = annotationResult ?: return
        val length = file.textLength
        for (problem in problems) {
            if (problem.start < 0 || problem.start > length) continue
            val range = TextRange(problem.start, problem.end.coerceIn(problem.start, length))
            var annotation = holder
                .newAnnotation(severityOf(problem.severity), problem.message)
                .range(range)
            // Greyed out, the way an unused variable is in every other language:
            // the flow works, and this part of it is doing nothing.
            if (problem.family == "unused" && problem.severity != "error") {
                annotation = annotation.textAttributes(
                    CodeInsightColors.NOT_USED_ELEMENT_ATTRIBUTES,
                )
            }
            for (fix in problem.fixes) {
                annotation = annotation.withFix(FlowApplyFix(fix))
            }
            // The code travels in the tooltip, because that is what somebody
            // switching an inspection off or searching for an explanation needs.
            annotation.tooltip("${problem.message} <code>[${problem.code}]</code>")
                .create()
        }
    }

    private fun severityOf(severity: String): HighlightSeverity = when (severity) {
        "error" -> HighlightSeverity.ERROR
        "warning" -> HighlightSeverity.WARNING
        "weak-warning" -> HighlightSeverity.WEAK_WARNING
        else -> HighlightSeverity.INFORMATION
    }

    private fun problemOf(entry: Map<*, *>): FlowProblem? {
        val range = entry["range"] as? Map<*, *> ?: return null
        val start = offsetOf(range["start"]) ?: return null
        val end = offsetOf(range["end"]) ?: return null
        return FlowProblem(
            code = entry["code"] as? String ?: "",
            severity = entry["severity"] as? String ?: "error",
            family = entry["family"] as? String ?: "",
            message = entry["message"] as? String ?: return null,
            start = start,
            end = end,
            fixes = fixesOf(entry["fixes"]),
        )
    }

    private fun offsetOf(position: Any?): Int? =
        ((position as? Map<*, *>)?.get("offset") as? Number)?.toInt()

    private fun fixesOf(value: Any?): List<FlowFix> {
        val listed = value as? List<*> ?: return emptyList()
        return listed.mapNotNull { entry ->
            val fix = entry as? Map<*, *> ?: return@mapNotNull null
            val edits = (fix["edits"] as? List<*>)?.mapNotNull { one ->
                val edit = one as? Map<*, *> ?: return@mapNotNull null
                val start = (edit["start"] as? Number)?.toInt() ?: return@mapNotNull null
                val end = (edit["end"] as? Number)?.toInt() ?: return@mapNotNull null
                FlowEdit(start, end, edit["text"] as? String ?: "")
            } ?: emptyList()
            if (edits.isEmpty()) null
            else FlowFix(fix["label"] as? String ?: "Fix", edits)
        }
    }
}

/**
 * Alt+Enter: apply the edits the diagnostic came with.
 *
 * Nothing here works out *what* the fix is. The language found the problem and
 * wrote down the edits that repair it, so this applies them blind -- which is the
 * only way a fix can be trusted: one that re-derived the repair from the message
 * would be a second implementation of the check, and would corrupt a file the day
 * the two disagreed.
 */
private class FlowApplyFix(private val fix: FlowFix) : IntentionAction {

    override fun getText(): String = fix.label

    override fun getFamilyName(): String = "A11 Flow"

    override fun startInWriteAction(): Boolean = true

    override fun isAvailable(project: Project, editor: Editor?, file: PsiFile?): Boolean =
        file is FlowPsiFile

    @Throws(IncorrectOperationException::class)
    override fun invoke(project: Project, editor: Editor?, file: PsiFile?) {
        val target = file ?: return
        val documents = PsiDocumentManager.getInstance(project)
        val document = documents.getDocument(target) ?: return
        // Back to front, so an earlier edit does not move a later one.
        for (edit in fix.edits.sortedByDescending { it.start }) {
            if (edit.end > document.textLength || edit.start > edit.end) continue
            document.replaceString(edit.start, edit.end, edit.text)
        }
        documents.commitDocument(document)
    }
}
