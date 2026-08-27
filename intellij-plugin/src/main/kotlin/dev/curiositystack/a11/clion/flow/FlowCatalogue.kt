package dev.curiositystack.a11.clion.flow

import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.openapi.vfs.newvfs.BulkFileListener
import com.intellij.openapi.vfs.newvfs.events.VFileContentChangeEvent
import com.intellij.openapi.vfs.newvfs.events.VFileCreateEvent
import com.intellij.openapi.vfs.newvfs.events.VFileDeleteEvent
import com.intellij.openapi.vfs.newvfs.events.VFileEvent
import com.intellij.util.messages.MessageBusConnection
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * What *this project* declares that a flow may call.
 *
 * **The gap this closes.** The language ships a snapshot of what the SDK
 * registers, so hovering `interact_with_llm` has always said something useful.
 * An action somebody wrote this afternoon in a file two directories away was in
 * no snapshot: hovering its name said "action name", completing its ports
 * offered nothing, and there was nowhere for Ctrl+B to go. That is the common
 * case for anybody composing their own actions, and it was the case the editor
 * knew least about.
 *
 * **How.** `a11-flow` reads the project's own `.py`, `.cc` and `.ts` for
 * `ActionSchema` declarations and answers a catalogue in which every entry
 * carries the file and line it was written at. This hands that to [FlowEngine],
 * which sends it with every hover, completion and go-to-declaration afterwards.
 * No language knowledge here and no second scanner: the reading is the same
 * code
 * `a11 flow scan` and CI run, and this is the wiring.
 *
 * **When.** Once when the project opens, and again a moment after a source file
 * is saved. Debounced, because a save-all over twenty files is one interesting
 * event, and off the UI thread, because walking a tree is not something to do
 * on it.
 */
@Service(Service.Level.PROJECT)
class FlowCatalogueService(private val project: Project) : Disposable {

    private val scheduled = AtomicBoolean(false)
    private val generation = AtomicLong(0)
    private var connection: MessageBusConnection? = null

    /**
     * Start watching, and read the project once.
     *
     * Nothing happens at all without a tool for this platform: the editor is
     * already saying so once through [FlowEngine], and a second complaint about
     * the same missing binary is worse than one.
     */
    fun start() {
        if (!FlowEngine.instance().available) return
        connection = project.messageBus.connect(this).also { bus ->
            bus.subscribe(
                com.intellij.openapi.vfs.VirtualFileManager.VFS_CHANGES,
                object : BulkFileListener {
                    override fun after(events: MutableList<out VFileEvent>) {
                        if (events.any { interesting(it) }) rescan()
                    }
                },
            )
        }
        rescan()
    }

    /**
     * Whether one filesystem event could have changed what this project
     * declares.
     *
     * Only the three languages a scan reads, and only the events that change
     * what is in a file. Everything else -- a `.flow` saved, a build directory
     * rewritten, a file opened -- is not a reason to walk the tree again.
     */
    private fun interesting(event: VFileEvent): Boolean {
        val path = event.path
        val readable = SCANNED_EXTENSIONS.any { path.endsWith(it) }
        if (!readable) return false
        if (SKIPPED.any { path.contains(it) }) return false
        return event is VFileContentChangeEvent ||
            event is VFileCreateEvent ||
            event is VFileDeleteEvent
    }

    /**
     * Read the project again, shortly.
     *
     * The delay is what makes a save-all one scan rather than twenty, and the
     * generation counter is what makes a scan that started before the newest
     * request throw its answer away rather than overwrite a fresher one.
     */
    fun rescan() {
        val mine = generation.incrementAndGet()
        if (!scheduled.compareAndSet(false, true)) return
        ApplicationManager.getApplication().executeOnPooledThread {
            try {
                Thread.sleep(SETTLE_MILLIS)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                return@executeOnPooledThread
            }
            scheduled.set(false)
            if (generation.get() != mine) {
                // Something asked again while this was waiting, and that
                // request scheduled its own run.
                return@executeOnPooledThread
            }
            read()
        }
    }

    private fun read() {
        val roots = project.basePath?.let { listOf(it) } ?: return
        val answer = FlowEngine.instance().scan(roots)
        if (answer == null) {
            LOG.debug("no scan: a11-flow answered nothing for $roots")
            return
        }
        val actions = (answer["actions"] as? List<*>)?.size ?: 0
        @Suppress("UNCHECKED_CAST")
        val scanned = answer["scanned"] as? Map<String, Any?>
        if (scanned?.get("reached_file_limit") == true) {
            // Said rather than silently applied: a half-read project otherwise
            // looks exactly like a project with two actions in it.
            LOG.warn(
                "a11-flow stopped after ${scanned["files_read"]} files;" +
                    " some of $roots was not read"
            )
        }
        LOG.debug("$actions action(s) declared in ${project.name}")
        FlowEngine.instance().setProjectCatalogue(key(), answer)
    }

    /**
     * This project's key in the engine,
     * which is one process for the whole IDE.
     */
    private fun key(): String = project.basePath ?: project.name

    override fun dispose() {
        connection?.disconnect()
        connection = null
        FlowEngine.instance().setProjectCatalogue(key(), null)
    }

    companion object {
        private val LOG = Logger.getInstance(FlowCatalogueService::class.java)

        /** How long to let a burst of saves settle before reading the tree. */
        private const val SETTLE_MILLIS = 750L

        /** The extensions `a11-flow scan` reads. */
        private val SCANNED_EXTENSIONS =
            listOf(".py", ".pyi", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".ts", ".tsx", ".mts", ".js", ".mjs", ".jsx")

        /**
         * Directories a scan does not descend
         * into, so an event in one is not news.
         */
        private val SKIPPED =
            listOf("/node_modules/", "/.venv/", "/build/", "/dist/", "/__pycache__/", "/.git/")

        fun of(project: Project): FlowCatalogueService = project.service()
    }
}

/**
 * Read the project's actions when it opens.
 *
 * A startup activity defers scanning until the IDE is idle and avoids work for
 * projects that never open Flow files.
 */
class FlowCatalogueStartup : ProjectActivity {
    override suspend fun execute(project: Project) {
        FlowCatalogueService.of(project).start()
    }
}

/**
 * Whether `file` is one a scan would read, for a caller with a [VirtualFile].
 */
internal fun scannable(file: VirtualFile): Boolean =
    file.extension?.lowercase() in
        setOf("py", "pyi", "cc", "cpp", "cxx", "h", "hpp", "ts", "tsx", "mts", "js", "mjs", "jsx")
