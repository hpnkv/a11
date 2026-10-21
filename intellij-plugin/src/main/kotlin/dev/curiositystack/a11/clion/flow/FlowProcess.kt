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
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.progress.ProcessCanceledException
import com.intellij.openapi.progress.ProgressManager
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.File
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ExecutionException
import java.util.concurrent.Future
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/** One serialized JSONL connection, with cancellable waits and bounded I/O. */
internal class FlowProcess(
    private val executable: () -> File?,
    private val responseTimeoutMillis: Long = 5_000,
) : Disposable {
    private val lock = ReentrantLock()
    private val disposed = AtomicBoolean(false)
    private val io = ThreadPoolExecutor(
        1, 1, 0, TimeUnit.MILLISECONDS, ArrayBlockingQueue(1),
        { task -> Thread(task, "A11 Flow transport").apply { isDaemon = true } },
    )
    @Volatile private var process: Process? = null
    private var writer: BufferedWriter? = null
    private var reader: BufferedReader? = null

    fun exchange(line: String): String? {
        try {
            while (!lock.tryLock(25, TimeUnit.MILLISECONDS)) {
                ProgressManager.checkCanceled()
                if (disposed.get()) return null
            }
            try {
                for (attempt in 0..1) {
                    ProgressManager.checkCanceled()
                    if (disposed.get()) return null
                    val pipes = pipes() ?: return null
                    val pending = io.submit<String?> {
                        pipes.first.write(line)
                        pipes.first.write("\n")
                        pipes.first.flush()
                        pipes.second.readLine()
                    }
                    try {
                        val answer = await(pending)
                        if (answer != null) return answer
                    } catch (cancelled: ProcessCanceledException) {
                        stop()
                        pending.cancel(true)
                        throw cancelled
                    } catch (_: TimeoutException) {
                        stop()
                        pending.cancel(true)
                        LOG.warn("a11-flow exceeded its response deadline")
                        return null
                    } catch (error: ExecutionException) {
                        LOG.info("a11-flow stopped answering", error.cause)
                    } finally {
                        if (!pending.isDone) {
                            stop()
                            pending.cancel(true)
                        }
                        io.purge()
                    }
                    stop()
                }
                return null
            } finally {
                lock.unlock()
            }
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            throw ProcessCanceledException()
        }
    }

    private fun await(pending: Future<String?>): String? {
        val deadline = System.nanoTime() +
            TimeUnit.MILLISECONDS.toNanos(responseTimeoutMillis)
        while (true) {
            ProgressManager.checkCanceled()
            val remaining = deadline - System.nanoTime()
            if (remaining <= 0) throw TimeoutException()
            try {
                return pending.get(minOf(remaining, TimeUnit.MILLISECONDS.toNanos(25)), TimeUnit.NANOSECONDS)
            } catch (_: TimeoutException) {
                if (System.nanoTime() >= deadline) throw TimeoutException()
            }
        }
    }

    private fun pipes(): Pair<BufferedWriter, BufferedReader>? {
        if (process?.isAlive == true) {
            val out = writer
            val input = reader
            if (out != null && input != null) return out to input
        }
        stop()
        val tool = executable() ?: return null
        return try {
            val started = ProcessBuilder(tool.absolutePath, "serve", "--protocol", "json")
                .redirectError(ProcessBuilder.Redirect.DISCARD)
                .start()
            process = started
            if (disposed.get()) {
                stop()
                return null
            }
            writer = started.outputWriter(Charsets.UTF_8)
            reader = started.inputReader(Charsets.UTF_8)
            writer!! to reader!!
        } catch (error: Exception) {
            LOG.warn("Could not start ${tool.absolutePath}", error)
            null
        }
    }

    private fun stop() {
        // Terminate before closing streams: a pipe operation holds their locks.
        process?.destroyForcibly()
        runCatching { writer?.close() }
        runCatching { reader?.close() }
        process = null
        writer = null
        reader = null
    }

    override fun dispose() {
        disposed.set(true)
        process?.destroyForcibly()
        lock.withLock { stop() }
        io.shutdownNow()
    }

    companion object {
        private val LOG = Logger.getInstance(FlowProcess::class.java)
    }
}
