package dev.curiositystack.a11.clion.flow

import com.intellij.formatting.service.AsyncDocumentFormattingService
import com.intellij.formatting.service.AsyncFormattingRequest
import com.intellij.formatting.service.FormattingService
import com.intellij.psi.PsiFile

/**
 * Ctrl+Alt+L on a flow: the language's own formatter.
 *
 * The native formatter owns indentation, token spacing, blank lines, and port
 * declaration alignment. It preserves author-selected line breaks.
 *
 * A file with an error comes back unchanged, with the reason.
 */
class FlowFormattingService : AsyncDocumentFormattingService() {

    override fun getFeatures(): Set<FormattingService.Feature> = emptySet()

    override fun canFormat(file: PsiFile): Boolean = file is FlowPsiFile

    override fun getName(): String = "a11-flow"

    override fun getNotificationGroupId(): String = "A11"

    override fun createFormattingTask(request: AsyncFormattingRequest): FormattingTask =
        object : FormattingTask {
            override fun run() {
                val payload = FlowEngine.instance().format(request.documentText)
                if (payload == null) {
                    request.onError(
                        "A11 Flow",
                        "No `a11-flow` for this platform, so a flow cannot be" +
                            " formatted here. Build it with `cmake --build ." +
                            " --target a11_flow_tool`, or put it on the path.",
                    )
                    return
                }
                val problem = firstError(payload)
                if (problem != null) {
                    request.onError("A11 Flow", problem)
                    return
                }
                request.onTextReady(payload["formatted"] as? String ?: request.documentText)
            }

            override fun cancel(): Boolean = true
        }

    /** The reason the formatter refused, if it did. */
    private fun firstError(payload: Map<String, Any?>): String? {
        val listed = payload["diagnostics"] as? List<*> ?: return null
        for (entry in listed) {
            val diagnostic = entry as? Map<*, *> ?: continue
            if (diagnostic["severity"] != "error") continue
            val range = diagnostic["range"] as? Map<*, *>
            val line = ((range?.get("start") as? Map<*, *>)?.get("line") as? Number)?.toInt()
            val message = diagnostic["message"] as? String ?: continue
            return if (line == null) message else "Line $line: $message"
        }
        return null
    }
}
