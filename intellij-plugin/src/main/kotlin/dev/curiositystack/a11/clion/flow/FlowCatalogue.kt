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

import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.DumbAware
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
 * Supplies project action declarations to Flow editor requests.
 *
 * `a11-flow` scans source for `ActionSchema` declarations and returns a
 * catalogue with file locations. The scan runs on a pooled thread when the
 * project opens and after relevant source changes, with a short debounce.
 */
@Service(Service.Level.PROJECT)
class FlowCatalogueService(private val project: Project) : Disposable {

    private val scheduled = AtomicBoolean(false)
    private val generation = AtomicLong(0)
    private var connection: MessageBusConnection? = null

    /**
     * Starts watching source changes and scans the project when a tool exists.
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
     * Whether an event changes a scannable source file.
     */
    private fun interesting(event: VFileEvent): Boolean {
        val path = event.path
        val readable = SCANNED_EXTENSIONS.any { path.endsWith(it) }
        if (!readable) return false
        if (path.split('/').any { name ->
                SKIPPED.any { pattern ->
                    if (pattern.endsWith('*')) name.startsWith(pattern.dropLast(1))
                    else name == pattern
                }
            }) return false
        return event is VFileContentChangeEvent ||
            event is VFileCreateEvent ||
            event is VFileDeleteEvent
    }

    /**
     * Schedules a debounced scan; newer requests supersede older results.
     */
    fun rescan() {
        generation.incrementAndGet()
        schedule()
    }

    private fun schedule() {
        if (project.isDisposed) return
        if (!scheduled.compareAndSet(false, true)) return
        ApplicationManager.getApplication().executeOnPooledThread {
            var mine = generation.get()
            try {
                Thread.sleep(SETTLE_MILLIS)
                mine = generation.get()
                if (!project.isDisposed) read(mine)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            } finally {
                scheduled.set(false)
                if (generation.get() != mine && !Thread.currentThread().isInterrupted) {
                    schedule()
                }
            }
        }
    }

    private fun read(mine: Long) {
        val roots = project.basePath?.let { listOf(it) } ?: return
        val answer = FlowEngine.instance().scan(roots)
        if (answer == null) {
            LOG.debug("no scan: a11-flow answered nothing for $roots")
            return
        }
        if (project.isDisposed || generation.get() != mine) return
        val actions = (answer["actions"] as? List<*>)?.size ?: 0
        @Suppress("UNCHECKED_CAST")
        val scanned = answer["scanned"] as? Map<String, Any?>
        if (scanned?.get("reached_file_limit") == true) {
            // Report incomplete scans with their file limit.
            LOG.warn(
                "a11-flow stopped after ${scanned["files_read"]} files;" +
                    " some of $roots was not read"
            )
        }
        if (scanned?.get("reached_entry_limit") == true) {
            LOG.warn(
                "a11-flow stopped after ${scanned["entries_visited"]} entries;" +
                    " some of $roots was not read"
            )
        }
        LOG.debug("$actions action(s) declared in ${project.name}")
        FlowEngine.instance().setProjectCatalogue(key(), answer)
    }

    /**
     * This project's key in the shared engine process.
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
         * Directory names excluded from scans and change events.
         */
        private val SKIPPED =
            listOf(
                "node_modules", ".venv*", "venv", "build*", "cmake-build*",
                "dist", "__pycache__", ".git", ".idea", ".gradle", "_deps",
                ".next", ".nuxt", ".svelte-kit", ".ruff_cache",
                ".vscode-test", ".mypy_cache", ".pytest_cache", ".tox",
                "site-packages", "target",
            )

        fun of(project: Project): FlowCatalogueService = project.service()
    }
}

/**
 * Read the project's actions when it opens.
 *
 * A startup activity defers scanning until the IDE is idle and avoids work for
 * projects that never open Flow files.
 */
class FlowCatalogueStartup : ProjectActivity, DumbAware {
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
