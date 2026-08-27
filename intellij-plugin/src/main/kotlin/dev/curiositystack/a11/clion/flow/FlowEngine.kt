package dev.curiositystack.a11.clion.flow

import a11.A11Json
import a11.valueOrThrow
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.PathManager
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Client for the native Flow language service.
 *
 * `a11-flow serve --protocol json` accepts one JSON request per line and emits
 * one response per line. The native service owns lexical and semantic language
 * judgments; this class exposes them to the plugin.
 *
 * When no binary is bundled or available on the path, methods return `null`.
 * [FlowLexer] can still highlight comments, strings, numbers, and punctuation.
 * One notification reports the unavailable service per project.
 */
@Service(Service.Level.APP)
class FlowEngine : Disposable {

    private val lock = ReentrantLock()
    private var process: Process? = null
    private var writer: BufferedWriter? = null
    private var reader: BufferedReader? = null
    private var complained = false

    /** The last answer for each method, keyed by the text it was about. */
    private val cache = HashMap<String, Pair<String, Map<String, Any?>>>()

    /** What the world contains, when something has said: see [setContext]. */
    private var context: Map<String, Any?>? = null

    /**
     * What each open project's own source declares: see [setProjectCatalogue].
     */
    private val projects = HashMap<String, Map<String, Any?>>()

    /** Whether a tool was found at all, which is what "degraded" means here. */
    val available: Boolean get() = executable() != null

    /**
     * Everything wrong with this text: `flow.diagnostics/v1`, or `null`.
     *
     * Both passes of the compiler, every problem rather than the first, each
     * with the fixes the language itself worked out -- which is why the quick
     * fixes here are edits to apply rather than repairs to invent.
     */
    fun check(text: String): Map<String, Any?>? = ask("check", text)

    /** Every token and what it *means*: `flow.tokens/v1`, or `null`. */
    fun tokens(text: String): Map<String, Any?>? = ask("tokens", text)

    /** The text formatted: `flow.format/v1`, or `null`. */
    fun format(text: String): Map<String, Any?>? = ask("format", text)

    /**
     * What may be written at `offset`: `flow.completions/v1`, or `null`.
     *
     * Not cached: the answer depends on the offset as well as the text, and a
     * caret moves more often than a document changes.
     */
    fun complete(text: String, offset: Int): Map<String, Any?>? =
        request(
            mapOf("method" to "complete", "source" to text, "offset" to offset) +
                contextArgument(),
        )

    /**
     * What is at `offset`: `flow.hover/v1`, or `null`.
     *
     * The language service resolves whether a word is a port or stage and
     * supplies action ports and shape fields.
     */
    fun describe(text: String, offset: Int): Map<String, Any?>? =
        request(
            mapOf("method" to "describe", "source" to text, "offset" to offset) +
                contextArgument(),
        )

    /** What the document declares, nested: `flow.symbols/v1`, or `null`. */
    fun symbols(text: String): Map<String, Any?>? = ask("symbols", text)

    /**
     * Where the name at `offset` was bound: `flow.definition/v1`, or `null`.
     */
    fun definition(text: String, offset: Int): Map<String, Any?>? =
        request(
            mapOf("method" to "definition", "source" to text, "offset" to offset) +
                contextArgument(),
        )

    /**
     * Set the actions and types available outside the current documents.
     *
     * The service uses its SDK snapshot by default. Supply a live registry for
     * inline flows; `null` restores the snapshot.
     */
    fun setContext(catalogue: Map<String, Any?>?) {
        lock.withLock {
            context = catalogue
            // Cached answers refer to the previous catalogue.
            cache.clear()
        }
    }

    /**
     * What one project's own source declares, under a key naming that project.
     *
     * One `a11-flow` process serves all open projects. Catalogues remain keyed
     * by project and are merged for requests so opening another project does
     * not replace existing action declarations.
     */
    fun setProjectCatalogue(key: String, catalogue: Map<String, Any?>?) {
        lock.withLock {
            if (catalogue == null) projects.remove(key) else projects[key] = catalogue
            cache.clear()
        }
    }

    /**
     * The actions `paths` declares in their own source: `flow.catalogue/v1`, or
     * `null`.
     *
     * Reads Python, C++ and TypeScript for `ActionSchema` declarations, each
     * with the file and line it was written at. That origin is what turns an
     * action somebody wrote this afternoon into a hover with a description and
     * a "go to declaration" that lands on it.
     */
    fun scan(paths: List<String>): Map<String, Any?>? =
        request(mapOf("method" to "scan", "paths" to paths))

    /**
     * The `context` argument: whatever was set, with every project's own
     * actions.
     *
     * The language merges catalogue lists and uses the later description when a
     * name appears twice. Apply that rule once at this boundary.
     */
    private fun contextArgument(): Map<String, Any?> {
        val (given, scanned) = lock.withLock { context to projects.values.toList() }
        if (given == null && scanned.isEmpty()) return emptyMap()
        val actions = mutableListOf<Any?>()
        val types = mutableListOf<Any?>()
        for (catalogue in scanned + listOfNotNull(given)) {
            (catalogue["actions"] as? List<*>)?.let { actions.addAll(it) }
            (catalogue["types"] as? List<*>)?.let { types.addAll(it) }
        }
        val merged = mutableMapOf<String, Any?>("actions" to actions, "types" to types)
        // `replace` is the caller's word
        // about the *whole* world, so it survives
        // the merge: an IDE that knows exactly which registry an inline flow is
        // attached to means it.
        (given?.get("replace") as? Boolean)?.let { merged["replace"] = it }
        return mapOf("context" to merged)
    }

    /**
     * One request, with the last answer reused when the text has not changed.
     */
    private fun ask(method: String, text: String): Map<String, Any?>? {
        lock.withLock {
            val held = cache[method]
            if (held != null && held.first == text) return held.second
        }
        val answer = request(mapOf("method" to method, "source" to text))
        if (answer != null) {
            lock.withLock { cache[method] = text to answer }
        }
        return answer
    }

    @Suppress("UNCHECKED_CAST")
    private fun request(payload: Map<String, Any?>): Map<String, Any?>? {
        val answer = try {
            // IntelliJ offsets use UTF-16 code units. The service converts
            // its native byte offsets at this shared request boundary.
            val line = A11Json.encodeToString(payload + ("offsets" to "utf16"))
                .valueOrThrow()
            val response = exchange(line) ?: return null
            A11Json.parse(response).valueOrThrow() as? Map<String, Any?>
        } catch (error: Throwable) {
            LOG.warn("a11-flow answered something unreadable", error)
            null
        } ?: return null
        if (answer["ok"] != true) {
            // A request the tool does not understand is this plugin's bug, not
            // the author's: it goes to the log, and the editor shows nothing.
            LOG.warn("a11-flow refused ${payload["method"]}: ${answer["error"]}")
            return null
        }
        return answer["result"] as? Map<String, Any?>
    }

    /**
     * Write one line, read one line, restarting the process if it has gone.
     *
     * Serialised: the protocol is one answer per request in order, and a
     * document being retyped produces bursts of them from several threads.
     */
    private fun exchange(line: String): String? = lock.withLock {
        for (attempt in 0..1) {
            val pipes = pipes() ?: return null
            try {
                pipes.first.write(line)
                pipes.first.write("\n")
                pipes.first.flush()
                val answer = pipes.second.readLine()
                if (answer != null) return answer
            } catch (error: Throwable) {
                LOG.info("a11-flow stopped answering; restarting it", error)
            }
            // The tool died, or the pipe broke. One restart, then give up for
            // this request: an editor that retried for ever would hang.
            stop()
        }
        return null
    }

    private fun pipes(): Pair<BufferedWriter, BufferedReader>? {
        val running = process
        if (running != null && running.isAlive) {
            val out = writer
            val input = reader
            if (out != null && input != null) return out to input
        }
        stop()
        val tool = executable() ?: return null
        return try {
            val started = ProcessBuilder(tool.absolutePath, "serve", "--protocol", "json")
                .redirectErrorStream(false)
                .start()
            process = started
            writer = started.outputWriter()
            reader = started.inputReader()
            writer!! to reader!!
        } catch (error: Throwable) {
            LOG.warn("Could not start ${tool.absolutePath}", error)
            complain("A11 Flow: could not start ${tool.name} (${error.message}).")
            null
        }
    }

    private fun stop() {
        try {
            writer?.close()
        } catch (_: Throwable) {
        }
        try {
            reader?.close()
        } catch (_: Throwable) {
        }
        process?.let { running ->
            running.destroy()
            running.waitFor(200, TimeUnit.MILLISECONDS)
        }
        process = null
        writer = null
        reader = null
    }

    /**
     * Where the tool is.
     *
     * In order: what a developer pointed at, what the plugin bundles for this
     * platform, and what is on the path. The bundled copy is extracted once
     * into the IDE's own system directory, because a file inside a jar is not
     * executable.
     */
    private fun executable(): File? {
        held?.let { return it.takeIf { file -> file.canExecute() } }
        val found = fromProperty() ?: bundled() ?: onPath()
        if (found == null && !complained) {
            complain(
                "A11 Flow: no `a11-flow` for this platform, so flows are coloured" +
                    " but not checked. Build it with `cmake --build . --target" +
                    " a11_flow_tool`, or put it on the path.",
            )
        }
        held = found
        return found
    }

    private var held: File? = null

    private fun fromProperty(): File? {
        val named = System.getProperty(TOOL_PROPERTY) ?: System.getenv(TOOL_ENVIRONMENT)
        val file = named?.let(::File) ?: return null
        return file.takeIf { it.canExecute() }
    }

    /**
     * The copy in the plugin's own resources, extracted so it can be run.
     *
     * `processResources` places supplied binaries at
     * `bin/<os>-<arch>/a11-flow`. Unsupported platforms have no entry.
     */
    private fun bundled(): File? {
        val name = "bin/$PLATFORM/$TOOL_NAME"
        val stream = javaClass.classLoader.getResourceAsStream(name) ?: return null
        return try {
            val directory: Path = systemDirectory().resolve("a11-flow")
            Files.createDirectories(directory)
            val target = directory.resolve("$PLATFORM-$TOOL_NAME")
            stream.use { source ->
                Files.copy(source, target, StandardCopyOption.REPLACE_EXISTING)
            }
            val file = target.toFile()
            file.setExecutable(true)
            file
        } catch (error: Throwable) {
            LOG.warn("Could not unpack the bundled $name", error)
            null
        }
    }

    /// Where an extracted binary may live: the IDE's own system directory, or a
    /// temporary one when there is no IDE.
    private fun systemDirectory(): Path = try {
        PathManager.getSystemDir()
    } catch (error: Throwable) {
        Path.of(System.getProperty("java.io.tmpdir"))
    }

    private fun onPath(): File? {
        val path = System.getenv("PATH") ?: return null
        for (piece in path.split(File.pathSeparator)) {
            if (piece.isEmpty()) continue
            val candidate = File(piece, TOOL_NAME)
            if (candidate.canExecute()) return candidate
        }
        return null
    }

    private fun complain(message: String) {
        if (complained) return
        complained = true
        try {
            NotificationGroupManager.getInstance()
                .getNotificationGroup("A11")
                .createNotification(message, NotificationType.WARNING)
                .notify(null)
        } catch (error: Throwable) {
            // No notification group in a test fixture. The log is enough there.
            LOG.info(message)
        }
    }

    override fun dispose() {
        lock.withLock { stop() }
    }

    companion object {
        private val LOG = Logger.getInstance(FlowEngine::class.java)

        /** What to run, pointed at by a developer working on the language. */
        const val TOOL_PROPERTY = "a11.flow.tool"
        const val TOOL_ENVIRONMENT = "A11_FLOW_TOOL"

        private val TOOL_NAME =
            if (System.getProperty("os.name").startsWith("Windows")) "a11-flow.exe"
            else "a11-flow"

        /** `macos-aarch64`, `linux-x86_64`, `windows-x86_64`. */
        private val PLATFORM: String by lazy {
            val name = System.getProperty("os.name").lowercase()
            val os = when {
                name.startsWith("mac") -> "macos"
                name.startsWith("windows") -> "windows"
                else -> "linux"
            }
            val architecture = when (val arch = System.getProperty("os.arch").lowercase()) {
                "aarch64", "arm64" -> "aarch64"
                "x86_64", "amd64" -> "x86_64"
                else -> arch
            }
            "$os-$architecture"
        }

        /** Held for a caller with no application: a plain unit test. */
        private var standalone: FlowEngine? = null

        /**
         * The one tool process this IDE talks to.
         *
         * Outside an IDE -- a unit test of the lexer, which is an ordinary
         * JUnit test because lexing is not something that needs a platform --
         * there is no service registry, so one engine is held here instead.
         * Same process, same protocol, no fixture.
         */
        @Synchronized
        fun instance(): FlowEngine {
            val application = ApplicationManager.getApplication()
            if (application != null) return application.getService(FlowEngine::class.java)
            return standalone ?: FlowEngine().also { standalone = it }
        }
    }
}
