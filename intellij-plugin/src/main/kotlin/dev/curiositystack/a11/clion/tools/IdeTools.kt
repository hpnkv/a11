package dev.curiositystack.a11.clion.tools

import a11.Action
import a11.ActionPortSchema
import a11.ActionRegistry
import a11.ActionSchema
import a11.Status
import a11.orElse
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ReadAction
import com.intellij.openapi.editor.Document
import com.intellij.openapi.fileEditor.FileEditorManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.TextRange
import com.intellij.psi.PsiDocumentManager
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiNameIdentifierOwner
import com.intellij.psi.PsiNamedElement
import com.intellij.psi.search.FilenameIndex
import com.intellij.psi.search.GlobalSearchScope
import com.intellij.psi.util.PsiTreeUtil
import com.intellij.refactoring.RefactoringFactory
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope

/** Default cap on the number of paths a search tool returns. */
private const val DEFAULT_MAX_RESULTS = 20

/** How long a handler waits for an input value the caller already sent. */
private const val READ_TIMEOUT_MS = 5_000L

/**
 * Assemble the JSON Schema of one object — a request DTO or a tool result.
 *
 * Kept to the keywords every provider's tool-schema dialect accepts (Gemini's
 * function declarations, for one, reject `additionalProperties` and `default`),
 * so the same schema can be surfaced to any model the backend talks to.
 */
private fun objectSchema(
    description: String,
    properties: LinkedHashMap<String, Any?>,
    required: List<String>,
): Map<String, Any?> = linkedMapOf(
    "type" to "object",
    "description" to description,
    "properties" to properties,
    "required" to required,
)

/** JSON Schema for one scalar field, with an optional lower bound. */
private fun property(type: String, description: String, minimum: Int? = null): Map<String, Any?> {
    val schema = linkedMapOf<String, Any?>("type" to type, "description" to description)
    if (minimum != null) schema["minimum"] = minimum
    return schema
}

/** JSON Schema for a field holding a list of strings. */
private fun stringListProperty(description: String): Map<String, Any?> = linkedMapOf(
    "type" to "array",
    "description" to description,
    "items" to linkedMapOf("type" to "string"),
)

/** JSON Schema for `max_results`, shared by every search request DTO. */
private val MAX_RESULTS_PROPERTY = property(
    "integer",
    "Maximum number of paths to return; omit for $DEFAULT_MAX_RESULTS.",
    minimum = 1,
)

/** JSON Schema for the file path a result points at. */
private val PATH_PROPERTY = property("string", "Absolute path of the file, absent when no file is open.")

/** The file name a path ends with, for a summary line that fits on one line. */
private fun fileName(path: String?): String = path?.substringAfterLast('/') ?: "no file"

/**
 * A run log: one summary line, then markdown detail.
 *
 * The first line is what the UI shows folded, so it has to stand alone; the rest
 * is only read when someone opens the box.
 */
private fun runLog(summary: String, vararg details: String): String {
    val body = details.filter { it.isNotBlank() }
    return if (body.isEmpty()) summary else "$summary\n\n${body.joinToString("\n\n")}"
}

/** A markdown bullet list of [items], truncated with a count of what it hides. */
private fun bullets(items: List<String>, limit: Int = 10): String {
    if (items.isEmpty()) return ""
    val shown = items.take(limit).joinToString("\n") { "- `$it`" }
    val hidden = items.size - limit
    return if (hidden > 0) "$shown\n- …and $hidden more" else shown
}

/** Pull the JSON object a tool's [port] carries out of its inputs. */
private fun objectOn(inputs: Map<String, Any?>, port: ActionPortSchema): Map<String, Any?> {
    val value = inputs[port.name] ?: return emptyMap()
    require(value is Map<*, *>) { "'${port.name}' must be a JSON object." }
    @Suppress("UNCHECKED_CAST")
    return value as Map<String, Any?>
}

/** Read a required non-blank string field, or fail with a caller-facing message. */
private fun requiredString(json: Map<String, Any?>, field: String): String {
    val value = json[field]
    require(value is String && value.isNotBlank()) {
        "'$field' is required and must be a non-empty string."
    }
    return value
}

/** Read an optional integer field bounded below by [minimum]; null when absent. */
private fun optionalInt(json: Map<String, Any?>, field: String, minimum: Int): Int? {
    val value = json[field] ?: return null
    require(value is Number) { "'$field' must be a number." }
    val count = value.toInt()
    require(count >= minimum) { "'$field' must be at least $minimum." }
    return count
}

/** Read an optional non-blank string field; null when absent or blank. */
private fun optionalString(json: Map<String, Any?>, field: String): String? {
    val value = json[field] ?: return null
    require(value is String) { "'$field' must be a string." }
    return value.ifBlank { null }
}

/** Read an optional list-of-strings field; empty when absent. */
private fun optionalStrings(json: Map<String, Any?>, field: String): List<String> {
    val value = json[field] ?: return emptyList()
    require(value is Iterable<*>) { "'$field' must be a list of strings." }
    return value.map { item ->
        require(item is String && item.isNotBlank()) { "'$field' must hold non-empty strings." }
        item
    }
}

/**
 * Request DTO for `get_active_file`: which slice of the file's text to return.
 *
 * [lineLimit] is how a caller keeps a large file from flooding a model's context;
 * paging through the file means repeating the call with a bumped [lineOffset].
 */
data class ActiveFileRequest(val lineOffset: Int, val lineLimit: Int?) {
    companion object {
        val JSON_SCHEMA: Map<String, Any?> = objectSchema(
            "Which slice of the active file's text to return; omit it for the whole file.",
            linkedMapOf(
                "line_offset" to property(
                    "integer",
                    "0-based line to start from; omit for the top of the file.",
                    minimum = 0,
                ),
                "line_limit" to property(
                    "integer",
                    "Maximum number of lines to return; omit for the rest of the file.",
                    minimum = 1,
                ),
            ),
            emptyList(),
        )

        fun fromJson(json: Map<String, Any?>) = ActiveFileRequest(
            lineOffset = optionalInt(json, "line_offset", minimum = 0) ?: 0,
            lineLimit = optionalInt(json, "line_limit", minimum = 1),
        )
    }
}

/** Request DTO for `find_file`. */
data class FindFileRequest(val name: String, val maxResults: Int = DEFAULT_MAX_RESULTS) {
    companion object {
        val JSON_SCHEMA: Map<String, Any?> = objectSchema(
            "The file name to look for, and how many matches to return.",
            linkedMapOf(
                "name" to property(
                    "string",
                    "Exact file name, with extension and without any directories (e.g. \"main.cpp\").",
                ),
                "max_results" to MAX_RESULTS_PROPERTY,
            ),
            listOf("name"),
        )

        fun fromJson(json: Map<String, Any?>) = FindFileRequest(
            name = requiredString(json, "name"),
            maxResults = optionalInt(json, "max_results", minimum = 1) ?: DEFAULT_MAX_RESULTS,
        )
    }
}

/** Request DTO for `search_project`. */
data class SearchProjectRequest(val query: String, val maxResults: Int = DEFAULT_MAX_RESULTS) {
    companion object {
        val JSON_SCHEMA: Map<String, Any?> = objectSchema(
            "The substring to match file names against, and how many matches to return.",
            linkedMapOf(
                "query" to property(
                    "string",
                    "Case-insensitive substring matched against project file names (e.g. \"widget\").",
                ),
                "max_results" to MAX_RESULTS_PROPERTY,
            ),
            listOf("query"),
        )

        fun fromJson(json: Map<String, Any?>) = SearchProjectRequest(
            query = requiredString(json, "query"),
            maxResults = optionalInt(json, "max_results", minimum = 1) ?: DEFAULT_MAX_RESULTS,
        )
    }
}

/**
 * Request DTO for `rename_symbol`.
 *
 * [name] alone is usually enough; [line] and [column] are there for the case where
 * one file declares several symbols with the same name, which the tool reports
 * rather than guessing at.
 */
data class RenameSymbolRequest(
    val name: String,
    val newName: String,
    val line: Int?,
    val column: Int?,
) {
    companion object {
        val JSON_SCHEMA: Map<String, Any?> = objectSchema(
            "Which symbol in the active file to rename, and what to call it.",
            linkedMapOf(
                "name" to property("string", "Current name of the symbol to rename."),
                "new_name" to property("string", "Name to give it."),
                "line" to property(
                    "integer",
                    "1-based line the symbol is declared on; pass it only to disambiguate a repeated name.",
                    minimum = 1,
                ),
                "column" to property(
                    "integer",
                    "1-based caret column of the symbol's name; pass it with `line` to disambiguate further.",
                    minimum = 1,
                ),
            ),
            listOf("name", "new_name"),
        )

        fun fromJson(json: Map<String, Any?>) = RenameSymbolRequest(
            name = requiredString(json, "name"),
            newName = requiredString(json, "new_name"),
            line = optionalInt(json, "line", minimum = 1),
            column = optionalInt(json, "column", minimum = 1),
        )
    }
}

/**
 * Which symbols `get_file_symbols` should report; every field is optional and an
 * omitted one narrows nothing. Filtering happens in the IDE, so a caller asking
 * about one part of a large file pays for that part only.
 */
data class FileSymbolFilters(
    val namePattern: Regex?,
    val lineOffset: Int?,
    val lineLimit: Int?,
    val kinds: List<String>,
) {
    /** Whether [symbol] — one entry of the `symbols` output — survives the filters. */
    fun keeps(symbol: Map<String, Any?>): Boolean {
        val name = symbol["name"] as? String ?: ""
        if (namePattern != null && !namePattern.containsMatchIn(name)) return false

        val line = symbol["line"] as? Int ?: return false
        val from = (lineOffset ?: 0) + 1
        if (line < from) return false
        if (lineLimit != null && line > from + lineLimit - 1) return false

        if (kinds.isEmpty()) return true
        val kind = (symbol["kind"] as? String).orEmpty()
        return kinds.any { kind.contains(it, ignoreCase = true) }
    }

    /** How the filters read in a run log; empty when nothing was narrowed. */
    fun describe(): String = listOfNotNull(
        namePattern?.let { "name matching `${it.pattern}`" },
        lineOffset?.let { "from line ${it + 1}" },
        lineLimit?.let { "at most $it lines" },
        kinds.takeIf { it.isNotEmpty() }?.let { "kinds ${it.joinToString(", ")}" },
    ).joinToString("; ")

    companion object {
        val JSON_SCHEMA: Map<String, Any?> = objectSchema(
            "Which symbols to report; omit a field to leave that dimension unfiltered.",
            linkedMapOf(
                "name_pattern" to property(
                    "string",
                    "Case-insensitive regular expression; a symbol is reported when its name matches" +
                        " anywhere (e.g. \"^get\" for getters, \"Fragment\" for anything containing it).",
                ),
                "line_offset" to property(
                    "integer",
                    "0-based line to start at; symbols declared above it are skipped.",
                    minimum = 0,
                ),
                "line_limit" to property(
                    "integer",
                    "How many lines from `line_offset` to cover; omit for the rest of the file.",
                    minimum = 1,
                ),
                "kinds" to stringListProperty(
                    "Keep only symbols whose `kind` contains one of these, case-insensitively" +
                        " (e.g. \"class\", \"function\"). Kinds are the language parser's own names," +
                        " so call once unfiltered to see which ones this file uses.",
                ),
            ),
            emptyList(),
        )

        /** The filters a request asked for; all-empty JSON means "no filtering". */
        fun fromJson(json: Map<String, Any?>) = FileSymbolFilters(
            namePattern = optionalString(json, "name_pattern")?.let { pattern ->
                try {
                    Regex(pattern, RegexOption.IGNORE_CASE)
                } catch (invalid: IllegalArgumentException) {
                    throw IllegalArgumentException(
                        "'name_pattern' is not a valid regular expression: ${invalid.message}",
                    )
                }
            },
            lineOffset = optionalInt(json, "line_offset", minimum = 0),
            lineLimit = optionalInt(json, "line_limit", minimum = 1),
            kinds = optionalStrings(json, "kinds"),
        )
    }
}

/** JSON Schema of one entry on the `get_file_symbols` `symbols` output. */
private val SYMBOL_SCHEMA: Map<String, Any?> = objectSchema(
    "One named symbol declared in the file, after any filters.",
    linkedMapOf(
        "name" to property("string", "The symbol's name."),
        "kind" to property(
            "string",
            "How the language's own parser classifies the symbol (e.g. \"FUNCTION_DEFINITION\").",
        ),
        "line" to property("integer", "1-based line the symbol's name starts on.", minimum = 1),
        "column" to property("integer", "1-based column the symbol's name starts at.", minimum = 1),
    ),
    listOf("name", "kind", "line", "column"),
)

/** JSON Schema of the `rename_symbol` metadata object. */
private val RENAME_METADATA_SCHEMA: Map<String, Any?> = objectSchema(
    "What was renamed, and where.",
    linkedMapOf(
        "path" to PATH_PROPERTY,
        "previous_name" to property("string", "The name the symbol had."),
        "new_name" to property("string", "The name it has now."),
        "line" to property("integer", "1-based line the renamed symbol is declared on.", minimum = 1),
        "column" to property("integer", "1-based column the renamed symbol's name starts at.", minimum = 1),
        "usages" to property("integer", "How many references were updated alongside the declaration.", minimum = 0),
    ),
    listOf("previous_name", "new_name", "line", "column", "usages"),
)

/** The active file's path, and the symbols its PSI tree declares. */
data class FileSymbols(val path: String?, val symbols: List<Map<String, Any?>>)

/** The active editor's path, and the slice of its lines a caller asked for. */
data class ActiveFileSlice(val path: String?, val lines: List<String>)

/** Where the selection sits, and the lines it covers; both empty without one. */
data class SelectionSlice(val metadata: Map<String, Any?>?, val lines: List<String>)

/** JSON Schema of the `get_selection` metadata object. */
private val SELECTION_METADATA_SCHEMA: Map<String, Any?> = objectSchema(
    "Where the selection sits; absent when nothing is selected.",
    linkedMapOf(
        "path" to PATH_PROPERTY,
        "start_line" to property("integer", "1-based line the selection starts on.", minimum = 1),
        "end_line" to property("integer", "1-based line the selection ends on.", minimum = 1),
    ),
    listOf("start_line", "end_line"),
)

// --- port constructors -----------------------------------------------------

/**
 * A unary JSON input port typed by the JSON Schema of the object it carries.
 *
 * Required by default: a tool that cannot run without its arguments should say
 * so. An input the tool has a sensible answer for when it is omitted — a filter,
 * say — passes `required = false`.
 */
private fun jsonInput(
    name: String,
    jsonSchema: Map<String, Any?>,
    required: Boolean = true,
) = ActionPortSchema(
    name,
    "application/json",
    description = jsonSchema["description"] as? String ?: "",
    unary = true,
    required = required,
    jsonSchema = jsonSchema,
)

/**
 * A unary JSON output port typed by the JSON Schema of the object it carries.
 *
 * Outputs default to optional: a port only counts as [required] when the tool
 * always has something to put there. Where nothing — no selection, no open file,
 * no match — is a perfectly good answer, saying "required" would promise a value
 * the tool cannot always produce.
 */
private fun jsonOutput(
    name: String,
    jsonSchema: Map<String, Any?>,
    unary: Boolean = true,
    required: Boolean = false,
) = ActionPortSchema(
    name,
    "application/json",
    description = jsonSchema["description"] as? String ?: "",
    unary = unary,
    required = required,
    jsonSchema = jsonSchema,
)

/**
 * A text output port. Its MIME type already says the values are text, so it needs
 * no JSON Schema: [a11.sdk.ToolAdapter] derives a string (or an array of strings,
 * when the port streams) from `text/plain` alone. Optional by default, for the
 * reason [jsonOutput] gives.
 */
private fun textOutput(
    name: String,
    description: String,
    unary: Boolean,
    required: Boolean = false,
) = ActionPortSchema(
    name,
    "text/plain",
    description = description,
    unary = unary,
    required = required,
)

/**
 * A tool's run log for the person watching: one short summary line, optionally
 * followed by markdown detail. It is *not* part of the model's contract — the
 * mirrors strip it before the tool is announced, so it never enters the
 * conversation — it exists so the UI can show what a tool call actually did.
 */
private fun userLogOutput(name: String) = textOutput(
    name,
    "Human-readable log of this run: first line a one-sentence summary, the rest markdown detail.",
    unary = true,
)

/**
 * One IDE tool: its port contract, and the body that runs it.
 *
 * [run] takes the inputs keyed by port name and returns the outputs keyed by port
 * name — a unary port's single value, or the list of values a streaming port
 * carries. Both the A11 handler and the direct bridge call go through it, so the
 * two paths cannot drift.
 */
private class Tool(
    val schema: ActionSchema,
    /** Output ports meant for the user's eyes, never for the model's context. */
    val userFacingOutputs: Set<String>,
    val run: (Map<String, Any?>) -> Map<String, Any?>,
)

/**
 * Assemble a tool from its ports and body.
 *
 * [userLog] is appended to the outputs and flagged as user-facing, so every tool
 * reports what it did without that report reaching the model.
 */
private fun tool(
    name: String,
    description: String,
    inputs: List<ActionPortSchema>,
    outputs: List<ActionPortSchema>,
    userLog: ActionPortSchema,
    run: (Map<String, Any?>) -> Map<String, Any?>,
): Tool = Tool(
    ActionSchema(
        name = name,
        description = description,
        inputs = LinkedHashMap(inputs.associateBy { it.name }),
        outputs = LinkedHashMap((outputs + userLog).associateBy { it.name }),
    ),
    setOf(userLog.name),
    run,
)

/**
 * IDE-backed tools the model can call. Each is an A11 action registered on the
 * client [ActionRegistry]; when the backend's `interact_with_llm` reverse-
 * dispatches a tool call, the plugin's [a11.Session] runs the handler here with
 * live IDE access and tees the JSON result back.
 *
 * The same tools are also reachable directly — without any A11 session — via
 * [listDescriptors] and [runByName], which the JCEF webview drives through the
 * JS↔Kotlin bridge (for both the reverse-dispatched chat tools and the manual
 * action explorer). Both paths run the same [Tool.run] bodies, so the schemas and
 * behavior stay identical.
 *
 * Each tool names its own ports and types them: an argument-taking tool takes a
 * JSON object typed by its request DTO's schema (e.g. [FindFileRequest]), and
 * results come back on ports typed either by a MIME type (`text/plain`, streaming
 * one value per line or path) or by a JSON Schema. An output is marked required
 * only when the tool always has something to put on it; no selection, no open
 * file, and no match are all valid answers here, so most outputs are optional and
 * simply carry nothing. `request` is a conventional name these tools happen to
 * use, nothing more — the plumbing below reads and writes whatever ports a schema
 * declares.
 *
 * All IDE reads happen under a read action (and the EDT where the editor model
 * requires it), so handlers are safe to run off the platform's UI thread.
 */
class IdeTools(private val project: Project) {

    /** Every tool, in a stable order, keyed by name. */
    private val tools: Map<String, Tool> = listOf(
        activeFileTool(),
        openEditorsTool(),
        selectionTool(),
        fileSymbolsTool(),
        renameSymbolTool(),
        findFileTool(),
        searchProjectTool(),
    ).associateBy { it.schema.name }

    /** Build the client registry of IDE tools and their reverse-dispatch descriptors. */
    fun buildRegistry(): Pair<ActionRegistry, List<Map<String, Any?>>> {
        val registry = ActionRegistry()
        for (tool in tools.values) register(registry, tool)
        return registry to listDescriptors()
    }

    /** The reverse-dispatch/tool descriptors for every IDE tool, in stable order. */
    fun listDescriptors(): List<Map<String, Any?>> = tools.values.map { describe(it) }

    /**
     * Run one IDE tool by name with its inputs keyed by port name — a unary port
     * holds a single value, a streaming port a list of them — and return its
     * outputs the same way. Used by the JCEF bridge (both the chat tool round-trip
     * and the action explorer); no A11 session is involved.
     */
    fun runByName(name: String, inputs: Map<String, Any?>): Map<String, Any?> {
        val tool = tools[name] ?: error("Unknown IDE tool '$name'.")
        return tool.run(inputs)
    }

    // --- registration helpers ------------------------------------------------

    private fun register(registry: ActionRegistry, tool: Tool) {
        val schema = tool.schema
        registry.register(schema.name, schema) handler@{ action ->
            val inputs = readInputs(action, schema)
            val outputs = try {
                tool.run(inputs)
            } catch (error: Throwable) {
                return@handler Status.fromException(error, "IDE tool '${schema.name}' failed.")
            }
            writeOutputs(action, schema, outputs).let { if (!it.isOk) return@handler it }
            Status.ok()
        }
    }

    /**
     * Write the tool's outputs onto the ports its schema declares: a unary port
     * takes its single value, a streaming port one value per item with the last
     * marked final, and a port the tool had nothing for is simply closed.
     *
     * Nothing is ever written as an explicit null. A null chunk is a *value* on
     * the wire, and a consumer that decodes an action's outputs — the LLM tool
     * runner, for one — has no type to decode it as; closing an empty port says
     * "nothing here" without putting an undecodable value in front of anyone.
     */
    private suspend fun writeOutputs(
        action: Action,
        schema: ActionSchema,
        outputs: Map<String, Any?>,
    ): Status = coroutineScope {
        // One writer per port, all at once: the reader on the other side drains
        // the ports in an order this side cannot know, and the transport pushes
        // back when a port fills up. Filling one port to completion before
        // starting the next would wedge both peers as soon as a result is large
        // enough to hit that backpressure.
        val written = schema.outputs.values.map { port ->
            async {
                val node = action.getOutput(port.name).orElse { return@async it }
                val value = outputs[port.name]
                val values = when {
                    value == null -> emptyList()
                    port.unary -> listOf(value)
                    else -> (value as? Iterable<*>)?.toList() ?: listOf(value)
                }
                for ((index, item) in values.withIndex()) {
                    node.put(item, final = index == values.lastIndex).orElse { return@async it }
                }
                node.drainAndClose()
            }
        }.awaitAll()
        written.firstOrNull { !it.isOk } ?: Status.ok()
    }

    /**
     * Read the tool's declared inputs, keyed by port name: a unary port yields its
     * single value, a streaming port the list of values it carried. A port that is
     * absent or empty is left out, so a handler sees exactly what the caller sent.
     */
    private suspend fun readInputs(action: Action, schema: ActionSchema): Map<String, Any?> = coroutineScope {
        // Read every port concurrently, for the same reason the outputs are
        // written concurrently: the caller fills them in its own order, not ours.
        val read = schema.inputs.values.map { port ->
            async {
                if (!action.containsPort(port.name)) return@async null
                val node = action.getInput(port.name).orElse { return@async null }
                val value: Any? = if (port.unary) {
                    node.consume(timeoutMs = READ_TIMEOUT_MS, allowNone = true).orElse { return@async null }
                } else {
                    val values = ArrayList<Any?>()
                    while (true) {
                        val next = node.next(timeoutMs = READ_TIMEOUT_MS).orElse { break } ?: break
                        values.add(next)
                    }
                    values.ifEmpty { null }
                }
                value?.let { port.name to it }
            }
        }.awaitAll()
        read.filterNotNull().toMap(LinkedHashMap())
    }

    // --- tool implementations ------------------------------------------------

    /**
     * The active file's path and the requested slice of its lines.
     *
     * An offset past the end of the file is not an error, it just yields no lines;
     * a caller pages through a large file by bumping the offset until that happens.
     */
    private fun getActiveFile(request: ActiveFileRequest): ActiveFileSlice = onEdtRead {
        val manager = FileEditorManager.getInstance(project)
        val path = manager.selectedFiles.firstOrNull()?.path
        val document = manager.selectedTextEditor?.document
            ?: return@onEdtRead ActiveFileSlice(path, emptyList())
        val start = request.lineOffset.coerceAtMost(document.lineCount)
        val end = request.lineLimit?.let { (start + it).coerceAtMost(document.lineCount) } ?: document.lineCount
        val lines = (start until end).map { line ->
            document.getText(TextRange(document.getLineStartOffset(line), document.getLineEndOffset(line)))
        }
        ActiveFileSlice(path, lines)
    }

    private fun getOpenEditors(): List<String> = onEdtRead {
        FileEditorManager.getInstance(project).openFiles.map { it.path }
    }

    /**
     * Where the current selection sits, and the lines it covers.
     *
     * `lines` holds the selected text itself, so a selection that starts or ends
     * mid-line yields that partial line — what the user highlighted, not the whole
     * lines around it. With no selection there is nothing to describe: no metadata
     * and no lines.
     */
    private fun getSelection(): SelectionSlice = onEdtRead {
        val manager = FileEditorManager.getInstance(project)
        val editor = manager.selectedTextEditor ?: return@onEdtRead SelectionSlice(null, emptyList())
        val selection = editor.selectionModel
        val text = selection.selectedText
        if (text.isNullOrEmpty()) return@onEdtRead SelectionSlice(null, emptyList())
        val document = editor.document
        val metadata = linkedMapOf<String, Any?>()
        manager.selectedFiles.firstOrNull()?.path?.let { metadata["path"] = it }
        metadata["start_line"] = document.getLineNumber(selection.selectionStart) + 1
        metadata["end_line"] = document.getLineNumber(selection.selectionEnd) + 1
        SelectionSlice(metadata, text.split("\n"))
    }

    /**
     * Every named symbol the active file's PSI tree declares, in source order.
     *
     * `PsiNamedElement` is the language-agnostic notion of "something with a name",
     * so this works in any language the IDE can parse without knowing the dialect;
     * `kind` is the parser's own element type, which is as close to a portable
     * classification as the platform offers.
     */
    private fun getFileSymbols(): FileSymbols = onEdtRead {
        val manager = FileEditorManager.getInstance(project)
        val path = manager.selectedFiles.firstOrNull()?.path
        val document = manager.selectedTextEditor?.document ?: return@onEdtRead FileSymbols(path, emptyList())
        val psiFile = PsiDocumentManager.getInstance(project).getPsiFile(document)
            ?: return@onEdtRead FileSymbols(path, emptyList())
        FileSymbols(path, namedElements(psiFile).map { describeSymbol(it, document) })
    }

    /** The file's named elements, in source order; the file itself is not a symbol. */
    private fun namedElements(psiFile: PsiFile): List<PsiNamedElement> =
        PsiTreeUtil.findChildrenOfType(psiFile, PsiNamedElement::class.java)
            .filter { it !is PsiFile && !it.name.isNullOrBlank() }
            .sortedBy { nameOffset(it) }

    /**
     * Where the symbol's *name* starts — which is what a caret position means to a
     * reader, and not always where the element starts: an `XmlTag`'s own offset is
     * the `<`, and a function's is the start of its whole declaration. Languages
     * that model a name identifier answer authoritatively; for the rest, the name's
     * first occurrence inside the element is the best available answer.
     */
    private fun nameOffset(element: PsiNamedElement): Int {
        (element as? PsiNameIdentifierOwner)?.nameIdentifier?.let { return it.textRange.startOffset }
        val name = element.name ?: return element.textOffset
        val within = element.text.indexOf(name)
        return if (within < 0) element.textOffset else element.textRange.startOffset + within
    }

    private fun describeSymbol(element: PsiNamedElement, document: Document): Map<String, Any?> {
        val offset = nameOffset(element)
        val line = document.getLineNumber(offset)
        return linkedMapOf(
            "name" to element.name,
            "kind" to (element.node?.elementType?.toString() ?: element.javaClass.simpleName),
            "line" to line + 1,
            "column" to offset - document.getLineStartOffset(line) + 1,
        )
    }

    /**
     * Rename one symbol in the active file, updating its references with it.
     *
     * The refactoring runs on the EDT (it takes its own write action), so this one
     * hops to the EDT without wrapping the whole thing in a read action. A name
     * that matches nothing, or several things the request does not pin down, is an
     * error carrying the candidates' positions — never a guess at which was meant.
     */
    private fun renameSymbol(request: RenameSymbolRequest): Map<String, Any?> = onEdt {
        val manager = FileEditorManager.getInstance(project)
        val located = ReadAction.compute<Located, RuntimeException> { locate(manager, request) }
        val refactoring = RefactoringFactory.getInstance(project).createRename(located.element, request.newName)
        // Rename the symbol and the references to it, nothing else: a mention in a
        // comment or a string literal is not a reference, and rewriting one would
        // go beyond what the caller asked for.
        refactoring.setSearchInComments(false)
        refactoring.setSearchInNonJavaFiles(false)
        val found = refactoring.findUsages()
        refactoring.doRefactoring(found)
        val usages = found.size
        val metadata = linkedMapOf<String, Any?>()
        located.path?.let { metadata["path"] = it }
        metadata["previous_name"] = request.name
        metadata["new_name"] = request.newName
        metadata["line"] = located.line
        metadata["column"] = located.column
        metadata["usages"] = usages
        metadata
    }

    /** The symbol a rename request points at, with the position it was found at. */
    private class Located(
        val element: PsiNamedElement,
        val path: String?,
        val line: Int,
        val column: Int,
    )

    /** Resolve a rename request to exactly one symbol, or explain why it cannot. */
    private fun locate(manager: FileEditorManager, request: RenameSymbolRequest): Located {
        val path = manager.selectedFiles.firstOrNull()?.path
        val document = manager.selectedTextEditor?.document
            ?: throw IllegalStateException("No file is open in an editor.")
        val psiFile = PsiDocumentManager.getInstance(project).getPsiFile(document)
            ?: throw IllegalStateException("The active file has no PSI tree to rename in.")

        val named = namedElements(psiFile).filter { it.name == request.name }
        require(named.isNotEmpty()) { "No symbol named '${request.name}' in the active file." }
        val positioned = named.map { element ->
            val offset = nameOffset(element)
            val line = document.getLineNumber(offset)
            Located(element, path, line + 1, offset - document.getLineStartOffset(line) + 1)
        }

        var candidates = positioned
        request.line?.let { line -> candidates = candidates.filter { it.line == line } }
        request.column?.let { column ->
            // The caret may sit anywhere inside the name, not just on its first character.
            candidates = candidates.filter { column >= it.column && column <= it.column + request.name.length }
        }
        val at = if (request.line == null && request.column == null) "" else " at the position given"
        require(candidates.isNotEmpty()) { "No symbol named '${request.name}'$at in the active file." }
        if (candidates.size > 1) {
            val positions = candidates.joinToString(", ") { "${it.line}:${it.column}" }
            throw IllegalArgumentException(
                "'${request.name}' is declared ${candidates.size} times in the active file ($positions); " +
                    "pass line (and column) to say which one.",
            )
        }
        return candidates.single()
    }

    private fun findFile(request: FindFileRequest): List<String> =
        ReadAction.compute<List<String>, RuntimeException> {
            FilenameIndex.getVirtualFilesByName(request.name, GlobalSearchScope.projectScope(project))
                .map { it.path }
                .take(request.maxResults)
        }

    private fun searchProject(request: SearchProjectRequest): List<String> {
        val query = request.query.lowercase()
        val limit = request.maxResults
        return ReadAction.compute<List<String>, RuntimeException> {
            val names = mutableListOf<String>()
            FilenameIndex.processAllFileNames({ fileName ->
                if (fileName.lowercase().contains(query)) names.add(fileName)
                names.size < limit * 4
            }, GlobalSearchScope.projectScope(project), null)
            names.flatMap { n ->
                FilenameIndex.getVirtualFilesByName(n, GlobalSearchScope.projectScope(project)).map { it.path }
            }.distinct().take(limit)
        }
    }

    // --- threading -----------------------------------------------------------

    /** Run [block] on the EDT and wait; it takes whatever read/write action it needs. */
    private fun <T> onEdt(block: () -> T): T {
        var result: Result<T>? = null
        ApplicationManager.getApplication().invokeAndWait { result = runCatching(block) }
        return result!!.getOrThrow()
    }

    private fun <T> onEdtRead(block: () -> T): T {
        var result: T? = null
        ApplicationManager.getApplication().invokeAndWait {
            result = ReadAction.compute<T, RuntimeException> { block() }
        }
        @Suppress("UNCHECKED_CAST")
        return result as T
    }

    // --- tool declarations ---------------------------------------------------
    //
    // Each builder names its ports once and closes over them, so the body reads
    // and writes exactly the ports the schema declares.

    private fun activeFileTool(): Tool {
        val request = jsonInput("request", ActiveFileRequest.JSON_SCHEMA, required = false)
        val lines = textOutput("lines", "The requested lines of the file, one value per line.", unary = false)
        val path = textOutput(
            "path",
            "Absolute path of the file in the active editor; absent when no file is open.",
            unary = true,
        )
        val log = userLogOutput("user_log_for_run")
        return tool(
            "get_active_file",
            "Return the path of the file in the active editor and its text. Pass a request to" +
                " read only part of a large file; with none, the whole file is returned.",
            inputs = listOf(request),
            outputs = listOf(lines, path),
            userLog = log,
        ) { inputs ->
            val asked = ActiveFileRequest.fromJson(objectOn(inputs, request))
            val slice = getActiveFile(asked)
            val summary = when {
                slice.path == null -> "No file is open in an editor"
                slice.lines.isEmpty() -> "Read no lines of ${fileName(slice.path)}"
                else -> "Read ${slice.lines.size} lines of ${fileName(slice.path)}"
            }
            val range = if (slice.lines.isEmpty()) {
                ""
            } else {
                "Lines ${asked.lineOffset + 1}–${asked.lineOffset + slice.lines.size}."
            }
            mapOf(
                lines.name to slice.lines,
                path.name to slice.path,
                log.name to runLog(summary, slice.path?.let { "`$it`" } ?: "", range),
            )
        }
    }

    private fun openEditorsTool(): Tool {
        val files = textOutput("files", "Absolute path of each file open in an editor.", unary = false)
        val log = userLogOutput("user_log_for_run")
        return tool(
            "get_open_editors",
            "List the paths of all files open in editors.",
            inputs = emptyList(),
            outputs = listOf(files),
            userLog = log,
        ) {
            val open = getOpenEditors()
            val summary = if (open.isEmpty()) "No files are open in editors" else "Listed ${open.size} open editors"
            mapOf(files.name to open, log.name to runLog(summary, bullets(open)))
        }
    }

    private fun selectionTool(): Tool {
        val metadata = jsonOutput("metadata", SELECTION_METADATA_SCHEMA)
        val lines = textOutput("lines", "The selected lines, one value per line.", unary = false)
        val log = userLogOutput("user_log_for_run")
        return tool(
            "get_selection",
            "Return the current editor selection: where it sits, and the lines it covers.",
            inputs = emptyList(),
            outputs = listOf(metadata, lines),
            userLog = log,
        ) {
            val selection = getSelection()
            val where = selection.metadata
            val summary = if (where == null) {
                "Nothing is selected in the active editor"
            } else {
                "Read the selection in ${fileName(where["path"] as? String)} " +
                    "(lines ${where["start_line"]}–${where["end_line"]})"
            }
            val excerpt = if (selection.lines.isEmpty()) "" else "```\n${selection.lines.joinToString("\n")}\n```"
            mapOf(
                metadata.name to selection.metadata,
                lines.name to selection.lines,
                log.name to runLog(summary, excerpt),
            )
        }
    }

    private fun fileSymbolsTool(): Tool {
        val filters = jsonInput("filters", FileSymbolFilters.JSON_SCHEMA, required = false)
        val symbols = jsonOutput("symbols", SYMBOL_SCHEMA, unary = false)
        val path = textOutput(
            "path",
            "Absolute path of the file the symbols come from; absent when no file is open.",
            unary = true,
        )
        val log = userLogOutput("user_log_for_run")
        return tool(
            "get_file_symbols",
            "List the named symbols declared in the active file, with each one's kind and" +
                " position. Pass filters to narrow by name, by kind, or to a range of lines;" +
                " with no filters the whole file is reported. Use filters to avoid reading complete files.",
            inputs = listOf(filters),
            outputs = listOf(symbols, path),
            userLog = log,
        ) { inputs ->
            val asked = FileSymbolFilters.fromJson(objectOn(inputs, filters))
            val found = getFileSymbols()
            val kept = found.symbols.filter(asked::keeps)
            val narrowed = asked.describe()
            val summary = when {
                kept.isEmpty() && narrowed.isEmpty() -> "No symbols found in ${fileName(found.path)}"
                kept.isEmpty() -> "No symbols in ${fileName(found.path)} match the filters"
                else -> "Found ${kept.size} symbols in ${fileName(found.path)}"
            }
            val listed = kept.map { "${it["name"]} — ${it["kind"]} (line ${it["line"]})" }
            mapOf(
                symbols.name to kept,
                path.name to found.path,
                log.name to runLog(
                    summary,
                    if (narrowed.isEmpty()) "" else "Filtered to $narrowed (${found.symbols.size} in the file).",
                    bullets(listed),
                ),
            )
        }
    }

    private fun renameSymbolTool(): Tool {
        val request = jsonInput("request", RenameSymbolRequest.JSON_SCHEMA)
        val metadata = jsonOutput("metadata", RENAME_METADATA_SCHEMA)
        val log = userLogOutput("user_log_for_run")
        return tool(
            "rename_symbol",
            "Rename a symbol in the active file, updating the references to it.",
            inputs = listOf(request),
            outputs = listOf(metadata),
            userLog = log,
        ) { inputs ->
            val asked = RenameSymbolRequest.fromJson(objectOn(inputs, request))
            val done = renameSymbol(asked)
            val usages = done["usages"] as? Int ?: 0
            val references = if (usages == 1) "1 reference" else "$usages references"
            mapOf(
                metadata.name to done,
                log.name to runLog(
                    "Renamed ${asked.name} → ${asked.newName}",
                    (done["path"] as? String)?.let { "`$it`" } ?: "",
                    "Declared at line ${done["line"]}, column ${done["column"]}; $references updated.",
                ),
            )
        }
    }

    private fun findFileTool(): Tool {
        val request = jsonInput("request", FindFileRequest.JSON_SCHEMA)
        val matches = textOutput("matches", "Absolute path of each matching file.", unary = false)
        val log = userLogOutput("user_log_for_run")
        return tool(
            "find_file",
            "Find project files by exact file name.",
            inputs = listOf(request),
            outputs = listOf(matches),
            userLog = log,
        ) { inputs ->
            val asked = FindFileRequest.fromJson(objectOn(inputs, request))
            val found = findFile(asked)
            val summary = if (found.isEmpty()) {
                "No file named ${asked.name} in the project"
            } else {
                "Found ${found.size} file(s) named ${asked.name}"
            }
            mapOf(matches.name to found, log.name to runLog(summary, bullets(found)))
        }
    }

    private fun searchProjectTool(): Tool {
        val request = jsonInput("request", SearchProjectRequest.JSON_SCHEMA)
        val matches = textOutput("matches", "Absolute path of each matching file.", unary = false)
        val log = userLogOutput("user_log_for_run")
        return tool(
            "search_project",
            "Find project files whose name contains a query substring.",
            inputs = listOf(request),
            outputs = listOf(matches),
            userLog = log,
        ) { inputs ->
            val asked = SearchProjectRequest.fromJson(objectOn(inputs, request))
            val found = searchProject(asked)
            val summary = if (found.isEmpty()) {
                "No file name matches \"${asked.query}\""
            } else {
                "Found ${found.size} file(s) matching \"${asked.query}\""
            }
            mapOf(matches.name to found, log.name to runLog(summary, bullets(found)))
        }
    }

    // --- descriptors ---------------------------------------------------------

    /**
     * One tool descriptor. `output_to_json_field` carries the schema's own
     * output-to-JSON mapping, so a mirror can reproduce it without knowing which
     * port a given tool happens to map.
     */
    private fun describe(tool: Tool): Map<String, Any?> {
        val schema = tool.schema
        return linkedMapOf(
            "name" to schema.name,
            "description" to schema.description,
            "inputs" to schema.inputs.values.map { port(it, tool) },
            "outputs" to schema.outputs.values.map { port(it, tool) },
            "output_to_json_field" to LinkedHashMap(schema.outputToJsonField),
        )
    }

    /**
     * One port descriptor. `schema` carries the port's JSON Schema when it has
     * one, so the webview mirror can surface the request fields to the model
     * (its `ToolAdapter` would otherwise see only `application/json`), and
     * `user_facing` marks a port the model must never be shown.
     */
    private fun port(p: ActionPortSchema, tool: Tool): Map<String, Any?> {
        val descriptor = linkedMapOf<String, Any?>(
            "name" to p.name, "type" to p.type, "required" to p.required, "unary" to p.unary,
            "description" to p.description,
        )
        p.jsonSchema?.let { descriptor["schema"] = it }
        // Flagged, not named: a consumer keeps this port away from the model by
        // reading the flag, so the port could be called anything.
        if (p.name in tool.userFacingOutputs) descriptor["user_facing"] = true
        return descriptor
    }
}
