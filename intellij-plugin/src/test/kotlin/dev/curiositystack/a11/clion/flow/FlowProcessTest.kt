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

import com.intellij.openapi.progress.ProcessCanceledException
import com.intellij.openapi.progress.ProgressManager
import com.intellij.openapi.progress.util.ProgressIndicatorBase
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import com.intellij.codeInsight.CodeInsightSettings
import java.io.File
import java.nio.file.Files
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class FlowProcessTest : BasePlatformTestCase() {
    private val directory = Files.createTempDirectory("a11-flow-process-").toFile()

    private fun tool(body: String): File = File(directory, "tool.sh").apply {
        writeText("#!/bin/sh\n$body\n")
        setExecutable(true)
    }

    private fun awaitStarted(marker: File) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!marker.exists() && System.nanoTime() < deadline) Thread.sleep(10)
        assertTrue("the fake service did not receive its request", marker.exists())
    }

    fun testBlockedScanDoesNotBlockHighlightingOrCatalogueUpdates() {
        val marker = File(directory, "scan-started")
        val executable = tool("""
            while IFS= read -r line; do
              case "${'$'}line" in
                *'"scan"'*)
                  : > '${marker.absolutePath}'
                  IFS= read -r blocked
                  ;;
                *) printf '%s\n' '{"ok":true,"result":{"format":"flow.tokens/v1","tokens":[]}}' ;;
              esac
            done
        """.trimIndent())
        val property = FlowEngine.TOOL_PROPERTY
        val previous = System.getProperty(property)
        val engine = FlowEngine()
        try {
            System.setProperty(property, executable.absolutePath)
            assertTrue(engine.available)
        } finally {
            if (previous == null) System.clearProperty(property)
            else System.setProperty(property, previous)
        }
        val executor = Executors.newFixedThreadPool(2)
        val indicator = ProgressIndicatorBase()
        val scanning = executor.submit<Boolean> {
            try {
                ProgressManager.getInstance().runProcess(
                    { engine.scan(listOf(directory.absolutePath)) }, indicator,
                )
                false
            } catch (_: ProcessCanceledException) {
                true
            }
        }
        try {
            awaitStarted(marker)
            val editor = executor.submit<Map<String, Any?>?> {
                engine.setProjectCatalogue("project", emptyMap())
                engine.tokens("flow test {}")
            }
            assertEquals("flow.tokens/v1", editor.get(1, TimeUnit.SECONDS)?.get("format"))
            indicator.cancel()
            assertTrue(scanning.get(1, TimeUnit.SECONDS))
        } finally {
            indicator.cancel()
            engine.dispose()
            executor.shutdownNow()
        }
    }

    fun testCancelledPipeReadReleasesTheConnectionAndRestarts() {
        val marker = File(directory, "received")
        val executable = tool("""
            IFS= read -r line
            if [ ! -f '${marker.absolutePath}' ]; then
              : > '${marker.absolutePath}'
              printf '%s' '{"ok":'
              IFS= read -r blocked
            else
              printf '%s\n' '{"ok":true}'
            fi
        """.trimIndent())
        val transport = FlowProcess({ executable })
        val executor = Executors.newSingleThreadExecutor()
        val indicator = ProgressIndicatorBase()
        val reading = executor.submit<Boolean> {
            try {
                ProgressManager.getInstance().runProcess(
                    { transport.exchange("{}") }, indicator,
                )
                false
            } catch (_: ProcessCanceledException) {
                true
            }
        }
        try {
            awaitStarted(marker)
            indicator.cancel()
            assertTrue(reading.get(1, TimeUnit.SECONDS))
            assertEquals("{\"ok\":true}", transport.exchange("{}"))
        } finally {
            indicator.cancel()
            transport.dispose()
            executor.shutdownNow()
        }
    }

    fun testUnresponsiveServiceHasABoundedResponseDeadline() {
        val executable = tool("IFS= read -r first\nIFS= read -r blocked")
        val transport = FlowProcess({ executable }, responseTimeoutMillis = 100)
        val executor = Executors.newSingleThreadExecutor()
        try {
            assertNull(executor.submit<String?> { transport.exchange("{}") }.get(2, TimeUnit.SECONDS))
        } finally {
            transport.dispose()
            executor.shutdownNow()
        }
    }

    override fun tearDown() {
        try {
            directory.deleteRecursively()
            CodeInsightSettings.getInstance().AUTO_POPUP_JAVADOC_INFO =
                CodeInsightSettings().AUTO_POPUP_JAVADOC_INFO
        } finally {
            super.tearDown()
        }
    }
}
