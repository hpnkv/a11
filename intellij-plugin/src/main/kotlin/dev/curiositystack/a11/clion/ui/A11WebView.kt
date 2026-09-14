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

package dev.curiositystack.a11.clion.ui

import a11.A11Json
import a11.valueOrThrow
import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationInfo
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ApplicationNamesInfo
import com.intellij.ide.ui.LafManagerListener
import com.intellij.openapi.diagnostic.thisLogger
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Disposer
import com.intellij.ui.jcef.JBCefBrowser
import com.intellij.ui.jcef.JBCefJSQuery
import com.intellij.ui.jcef.JBCefOSRHandlerFactory
import com.intellij.util.ui.UIUtil
import com.intellij.util.concurrency.AppExecutorUtil
import dev.curiositystack.a11.clion.highlights.HighlightNote
import dev.curiositystack.a11.clion.highlights.HighlightSuggestions
import dev.curiositystack.a11.clion.flow.FlowEngine
import dev.curiositystack.a11.clion.settings.A11Settings
import dev.curiositystack.a11.clion.session.GatewayConnection
import dev.curiositystack.a11.clion.tools.IdeTools
import java.awt.Color
import java.awt.GraphicsDevice
import java.awt.GraphicsEnvironment
import java.awt.Rectangle
import javax.swing.JComponent
import java.nio.ByteBuffer
import java.util.concurrent.CompletableFuture
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.ceil
import org.cef.browser.CefBrowser
import org.cef.browser.CefFrame
import org.cef.handler.CefLoadHandlerAdapter
import org.cef.handler.CefAcceleratedPaintInfo
import org.cef.handler.CefRenderHandler

/**
 * The JCEF-hosted A11 chat and action-explorer views, selected by [view].
 *
 * The page runs the TypeScript A11 library, which owns the WebSocket to the A11
 * gateway directly. Operations that require the live IDE (running a tool,
 * fetching connection config, reading a flow the plugin ships, leaving a
 * comment on a highlight) is reached through [JBCefJSQuery] bridges that call
 * back into Kotlin; [IdeTools] remains the single source of truth for the IDE
 * tools. The Kotlin A11 runtime remains available through
 * [dev.curiositystack.a11.clion.session].
 */
class A11WebView(private val project: Project, private val view: String, parent: Disposable) {

    private val log = thisLogger()
    private val paintCadence = PaintCadence()
    private val browser = newBrowser(paintCadence)
    private val ideTools = IdeTools(project)

    private val listActionsQuery = JBCefJSQuery.create(browser)
    private val helloQuery = JBCefJSQuery.create(browser)
    private val runActionQuery = JBCefJSQuery.create(browser)
    private val getConfigQuery = JBCefJSQuery.create(browser)
    private val readFlowQuery = JBCefJSQuery.create(browser)
    private val highlightFlowQuery = JBCefJSQuery.create(browser)
    private val requestFlowLanguageQuery = JBCefJSQuery.create(browser)
    private val suggestOnHighlightQuery = JBCefJSQuery.create(browser)
    private val clearSuggestionsQuery = JBCefJSQuery.create(browser)
    private val frameRateQuery = JBCefJSQuery.create(browser)

    /**
     * The frame rate last pushed to the browser, or 0 for "none yet".
     *
     * The builder's rate may be discarded for out-of-process JCEF. Starting at
     * zero ensures [followDisplayFrameRate] updates the live browser.
     */
    private var pushedFrameRate = 0
    private var desiredFrameRate = 0
    private var frameRateAdjustments = 0

    /** The AWT component the tool window embeds. */
    val component: JComponent get() = browser.component

    init {
        Disposer.register(parent, browser)
        Disposer.register(parent, listActionsQuery)
        Disposer.register(parent, helloQuery)
        Disposer.register(parent, runActionQuery)
        Disposer.register(parent, getConfigQuery)
        Disposer.register(parent, readFlowQuery)
        Disposer.register(parent, highlightFlowQuery)
        Disposer.register(parent, requestFlowLanguageQuery)
        Disposer.register(parent, suggestOnHighlightQuery)
        Disposer.register(parent, clearSuggestionsQuery)
        Disposer.register(parent, frameRateQuery)
        ApplicationManager.getApplication().messageBus.connect(parent).subscribe(
            LafManagerListener.TOPIC,
            LafManagerListener { applyTheme() },
        )
        if (view == "runner") {
            Disposer.register(parent) {
                FlowRunnerService.getInstance(project).detach(this)
            }
        }
        browser.jbCefClient.addLoadHandler(
            object : CefLoadHandlerAdapter() {
                override fun onLoadEnd(browser: CefBrowser?, frame: CefFrame?, httpStatusCode: Int) {
                    if (frame?.isMain != true) return
                    // Remote JCEF creates the browser at its own default rate.
                    // Reassert after creation even when the desired rate has
                    // not changed since the component was constructed.
                    followDisplayFrameRate(force = true)
                    measureFrameRate()
                    if (view == "runner") FlowRunnerService.getInstance(project).attach(this@A11WebView)
                }
            },
            browser.cefBrowser,
        )

        listActionsQuery.addHandler { respond { A11Json.encodeToString(ideTools.listDescriptors()).valueOrThrow() } }
        helloQuery.addHandler { respond { hello() } }
        runActionQuery.addHandler { request -> respond { runAction(request) } }
        getConfigQuery.addHandler { respond { config() } }
        readFlowQuery.addHandler { name -> respond { readFlow(name) } }
        highlightFlowQuery.addHandler { source -> respond {
            A11Json.encodeToString(FlowEngine.instance().tokens(source) ?: mapOf("tokens" to emptyList<Any>()))
                .valueOrThrow()
        } }
        requestFlowLanguageQuery.addHandler { request -> respond { requestFlowLanguage(request) } }
        suggestOnHighlightQuery.addHandler { note -> respond { suggestOnHighlight(note) } }
        clearSuggestionsQuery.addHandler { path -> respond { clearSuggestions(path) } }
        frameRateQuery.addHandler { measured -> respond {
            val javascript = measured.toDoubleOrNull() ?: 0.0
            adaptFrameRate(paintCadence.finish() ?: javascript, javascript)
            "{}"
        } }

        // Push the rate initially and whenever AWT reports a display change.
        followDisplayFrameRate()
        browser.component.addPropertyChangeListener("graphicsConfiguration") { followDisplayFrameRate() }

        browser.loadHTML(page(view))
        if (view == "runner") {
            FlowRunnerService.getInstance(project).attach(this)
        }
    }

    /** Push the declaration currently selected in an editor into the runner. */
    fun openFlow(flow: Map<String, Any?>) {
        val encoded = A11Json.encodeToString(flow).valueOrThrow()
            .replace("</script", "<\\/script")
        browser.cefBrowser.executeJavaScript(
            "window.__A11_OPEN_FLOW && window.__A11_OPEN_FLOW($encoded);",
            browser.cefBrowser.url,
            0,
        )
    }

    /**
     * Tell the browser to paint at the refresh rate of the display it is on.
     *
     * An off-screen browser uses its configured frame rate, and CEF defaults to
     * 30 fps. CEF's clamp only sets a minimum and Chromium's capture limit is
     * 1000 fps, so the display rate can be used directly.
     *
     * The rate must be pushed to the live browser. The IDE runs JCEF out
     * of process by default (`ide.browser.jcef.out-of-process.enabled`), and
     * its `RemoteBrowser` does not send `windowless_frame_rate` in
     * `Browser_Create`; the process starts at 30 fps. The
     * `ide.browser.jcef.osr.framerate` registry setting therefore has no effect
     * in this mode. `setWindowlessFrameRate` sends `Browser_SetFrameRate` and
     * defers the update until the browser exists.
     *
     * Do not verify through `getWindowlessFrameRate`: it returns a cached
     * value, not the browser's effective rate. The IDE's `JBCefOsrMeasureFps`
     * action measures the rendered frame rate.
     */
    private fun followDisplayFrameRate(force: Boolean = false) {
        val device = browser.component.graphicsConfiguration?.device
        val rate = frameRateOf(device)
        if (isMac()) {
            macRefreshRates.thenAccept { detected ->
                val current = browser.component.graphicsConfiguration?.device
                val refined = detected[current?.getIDstring()] ?: detected[MAIN_DISPLAY]
                if (refined == null) return@thenAccept
                ApplicationManager.getApplication().invokeLater {
                    val liveDevice = browser.component.graphicsConfiguration?.device
                    val liveRate = detected[liveDevice?.getIDstring()] ?: detected[MAIN_DISPLAY]
                    if (liveRate != null) applyDesiredFrameRate(liveRate, force)
                }
            }
        }
        applyDesiredFrameRate(rate, force)
    }

    private fun applyDesiredFrameRate(rate: Int, force: Boolean) {
        if (rate != desiredFrameRate) {
            desiredFrameRate = rate
            frameRateAdjustments = 0
            pushedFrameRate = 0
        }
        pushFrameRate(if (frameRateAdjustments == 0) rate else pushedFrameRate, force)
    }

    private fun pushFrameRate(rate: Int, force: Boolean = false) {
        if (!force && rate == pushedFrameRate) return
        try {
            browser.cefBrowser.setWindowlessFrameRate(rate)
            pushedFrameRate = rate
            log.debug("A11 chat browser set to ${rate}fps")
        } catch (error: LinkageError) {
            log.debug("JCEF cannot be re-rated at runtime here", error)
        } catch (error: Exception) {
            // Left for the next display change to retry; the browser keeps
            // whatever rate it already had.
            log.debug("could not set the JCEF frame rate", error)
        }
    }

    /** Compensate when remote JCEF delivers fewer painted frames than asked. */
    private fun adaptFrameRate(measured: Double, javascript: Double) {
        if (measured <= 0.0 || desiredFrameRate <= 0) return
        log.info(
            "A11 JCEF cadence: %.1ffps painted, %.1ffps JavaScript, %dfps display, %dfps requested"
                .format(measured, javascript, desiredFrameRate, pushedFrameRate),
        )
        if (measured >= desiredFrameRate * 0.85 || frameRateAdjustments >= 2) return
        val corrected = ceil(pushedFrameRate * desiredFrameRate / measured)
            .toInt().coerceIn(desiredFrameRate + 1, MAX_REQUESTED_FRAMERATE)
        if (corrected <= pushedFrameRate) return
        frameRateAdjustments += 1
        pushFrameRate(corrected, force = true)
        AppExecutorUtil.getAppScheduledExecutorService().schedule(
            { ApplicationManager.getApplication().invokeLater { measureFrameRate() } },
            500,
            java.util.concurrent.TimeUnit.MILLISECONDS,
        )
    }

    private fun measureFrameRate() {
        paintCadence.begin()
        browser.cefBrowser.executeJavaScript(
            "window.__A11_MEASURE_FPS && window.__A11_MEASURE_FPS();",
            browser.cefBrowser.url,
            0,
        )
    }

    /**
     * Run a bridge handler, mapping the result/exception onto the JS promise.
     */
    private fun respond(block: () -> String): JBCefJSQuery.Response =
        try {
            JBCefJSQuery.Response(block())
        } catch (error: Throwable) {
            log.warn("A11 bridge call failed", error)
            JBCefJSQuery.Response(null, ERROR_CODE, error.message ?: error.toString())
        }

    /**
     * Handle `runAction`: `{name, inputs}` in, the tool's JSON result out,
     * where
     * `inputs` holds one entry per input port (a list for a streaming port).
     */
    private fun runAction(request: String): String {
        GatewayConnection.getInstance(project).ensureConnected()
        @Suppress("UNCHECKED_CAST")
        val parsed = A11Json.parse(request).valueOrThrow() as? Map<String, Any?>
            ?: error("Malformed runAction request.")
        val name = parsed["name"] as? String ?: error("runAction requires a 'name'.")
        @Suppress("UNCHECKED_CAST")
        val inputs = (parsed["inputs"] as? Map<String, Any?>) ?: emptyMap()
        val result = ideTools.runByName(name, inputs)
        return A11Json.encodeToString(result).valueOrThrow()
    }

    private fun hello(): String = A11Json.encodeToString(
        linkedMapOf<String, Any?>(
            "protocol" to "a11.ide-webview/v1",
            "host" to "intellij",
            "capabilities" to linkedMapOf(
                "flowLanguage" to FlowEngine.instance().available,
                "incrementalActions" to true,
                "typedValues" to true,
            ),
        ),
    ).valueOrThrow()

    /** Forward the framework-neutral editor controller to the one native Flow service. */
    private fun requestFlowLanguage(encoded: String): String {
        @Suppress("UNCHECKED_CAST")
        val request = A11Json.parse(encoded).valueOrThrow() as? Map<String, Any?>
            ?: error("A Flow language request must be a JSON object.")
        val method = request["method"] as? String ?: error("A Flow language request needs a method.")
        val source = request["source"] as? String ?: ""
        val offset = (request["offset"] as? Number)?.toInt() ?: 0
        val engine = FlowEngine.instance()
        val result = when (method) {
            "check" -> engine.check(source)
            "complete" -> engine.complete(source, offset)
            "definition" -> engine.definition(source, offset)
            "describe" -> engine.describe(source, offset)
            "format" -> engine.format(source)
            "symbols" -> engine.symbols(source)
            "tokens" -> engine.tokens(source)
            else -> error("Unsupported Flow language method '$method'.")
        }
        val reply = if (result == null) {
            linkedMapOf<String, Any?>(
                "ok" to false,
                "error" to linkedMapOf("message" to "The native Flow language service is unavailable."),
            )
        } else {
            linkedMapOf<String, Any?>("ok" to true, "result" to result)
        }
        return A11Json.encodeToString(reply).valueOrThrow()
    }

    /**
     * Handle `suggestOnHighlight`: one record the review flow produced — a
     * comment or a patch — attached to the range of the file it is about.
     *
     * One suggestion normally arrives as two of these, off the flow's two
     * output ports, and the second is merged into the first by its `id` rather
     * than marking the range again; see `HighlightSuggestions.suggest`. So
     * `has_patch` in the reply is the state of the whole suggestion after this
     * record, not of the record.
     *
     * This UI sink is not registered as an [IdeTools] model tool. The review
     * flow writes suggestions to output ports, and the page forwards them to
     * the editor without exposing the sink to chat turns.
     */
    private fun suggestOnHighlight(note: String): String {
        @Suppress("UNCHECKED_CAST")
        val parsed = A11Json.parse(note).valueOrThrow() as? Map<String, Any?>
            ?: error("A highlight note must be a JSON object.")
        val suggestion = HighlightSuggestions.getInstance(project).suggest(HighlightNote.fromJson(parsed))
        return A11Json.encodeToString(
            linkedMapOf<String, Any?>("path" to suggestion.path, "has_patch" to suggestion.patch.isNotEmpty()),
        ).valueOrThrow()
    }

    /**
     * Handle `clearSuggestions`: drop what the last run left, for one file or
     * for all of them (an empty argument means all).
     *
     * A run of the flow replaces the previous run's suggestions rather than
     * adding to them: two models' opinions about the same range, one of them
     * about a version of the file that no longer exists, is not twice the help.
     */
    private fun clearSuggestions(path: String): String {
        val suggestions = HighlightSuggestions.getInstance(project)
        if (path.isBlank()) suggestions.clearAll() else suggestions.clear(path)
        return "{}"
    }

    /**
     * Handle `readFlow`: the text of one flow the plugin ships, by bare name.
     *
     * `processResources` packages flows from the repository's `scripts`
     * directory. Only a bare name is accepted; directories, `..`, and file
     * extensions are rejected to keep access within the packaged flow folder.
     */
    private fun readFlow(name: String): String {
        require(name.isNotEmpty() && name.all { it.isLetterOrDigit() || it == '-' || it == '_' }) {
            "A flow name is letters, digits, '-' and '_'; got '$name'."
        }
        val path = "/flows/$name.flow"
        return javaClass.getResourceAsStream(path)?.use { it.readBytes().toString(Charsets.UTF_8) }
            ?: error("No flow named '$name' ships with this plugin.")
    }

    /**
     * Handle `getConfig`: gateway URL + provider/model/apiKey from settings,
     * plus where the model actually is — which IDE, which project, which
     * directory.
     *
     * That last part is resolved here rather than guessed in the page: the IDE
     * name is whatever product this build is running in (the plugin is not
     * CLion-only), and the project is the one this tool window belongs to,
     * which the page has no other way to learn.
     */
    private fun config(): String {
        GatewayConnection.getInstance(project).ensureConnected()
        val settings = A11Settings.getInstance()
        val cfg = settings.state
        // The IDE's own tools, plus whatever patterns the user allowed the
        // gateway to add (its shell tools, by default). Both go into the
        // allowed-tools header, which is what decides whether the gateway
        // offers the model a tool of its own.
        val allowedTools = ideTools.listDescriptors().map { it["name"] } + settings.allowedToolPatterns()
        val appInfo = ApplicationInfo.getInstance()
        val config = linkedMapOf<String, Any?>(
            "url" to settings.gatewayUrl(),
            "provider" to cfg.provider,
            "model" to cfg.model,
            "apiKey" to settings.apiKey,
            "baseUrl" to cfg.baseUrl,
            "allowedTools" to allowedTools,
            "ide" to ApplicationNamesInfo.getInstance().fullProductName,
            "ideVersion" to appInfo.fullVersion,
            "projectName" to project.name,
            // Null for the default (project-less) frame, and for a project
            // opened as a set of unrelated roots; the page omits the line
            // rather than telling the model the project lives at "null".
            "projectPath" to project.basePath,
        )
        return A11Json.encodeToString(config).valueOrThrow()
    }

    /**
     * Build the page HTML: template + theme vars + bridge shims + app bundle.
     */
    private fun page(view: String): String {
        // Guard against the (currently absent) "</script>" inlining hazard so a
        // future bundle change cannot terminate the inline <script> early.
        val appJs = readResource("/webview/app.js").replace("</script", "<\\/script")
        return readResource("/webview/index.html")
            .replace("%%THEME_VARS%%", themeVars())
            .replace("%%VIEW%%", when (view) {
                "actions" -> "actions"
                "runner" -> "runner"
                else -> "chat"
            })
            .replace("%%BRIDGE_JS%%", bridgeJs())
            .replace("%%APP_JS%%", appJs)
    }

    /** Update an open page after an IDE light/dark theme change. */
    private fun applyTheme() {
        val declarations = A11Json.encodeToString(themeVars()).valueOrThrow()
        browser.cefBrowser.executeJavaScript(
            "document.documentElement.style.cssText = $declarations;",
            browser.cefBrowser.url,
            0,
        )
    }

    /**
     * Define `window.__a11Bridge` before the app bundle runs. Each method wraps
     * its [JBCefJSQuery] injection in a Promise for TypeScript callers.
     * `inject(request, onSuccess, onFailure)` splices
     * `onSuccess`/`onFailure` into the generated `window.<func>({request,
     * onSuccess, onFailure})` call as values. They must be complete
     * `function(...) {...}` expressions, not function bodies. `response` and
     * `error_message` are parameters supplied by the query router.
     */
    private fun bridgeJs(): String {
        fun wrap(query: JBCefJSQuery): String = query.inject(
            "arg",
            "function(response) { resolve(response); }",
            "function(error_code, error_message) { reject(new Error(error_message)); }",
        )
        val reportFrameRate = frameRateQuery.inject(
            "String(fps)",
            "function() {}",
            "function() {}",
        )
        return """
            window.__a11Bridge = {
              hello: function() {
                return new Promise(function(resolve, reject) { var arg = ""; ${wrap(helloQuery)} });
              },
              listActions: function() {
                return new Promise(function(resolve, reject) { var arg = ""; ${wrap(listActionsQuery)} });
              },
              runAction: function(name, inputs) {
                return new Promise(function(resolve, reject) { var arg = JSON.stringify({ name: name, inputs: inputs }); ${wrap(runActionQuery)} });
              },
              getConfig: function() {
                return new Promise(function(resolve, reject) { var arg = ""; ${wrap(getConfigQuery)} });
              },
              readFlow: function(name) {
                return new Promise(function(resolve, reject) { var arg = String(name); ${wrap(readFlowQuery)} });
              },
              highlightFlow: function(source) {
                return new Promise(function(resolve, reject) { var arg = String(source); ${wrap(highlightFlowQuery)} });
              },
              requestFlowLanguage: function(request) {
                return new Promise(function(resolve, reject) { var arg = JSON.stringify(request); ${wrap(requestFlowLanguageQuery)} });
              },
              suggestOnHighlight: function(note) {
                return new Promise(function(resolve, reject) { var arg = JSON.stringify(note); ${wrap(suggestOnHighlightQuery)} });
              },
              clearSuggestions: function(path) {
                return new Promise(function(resolve, reject) { var arg = String(path || ""); ${wrap(clearSuggestionsQuery)} });
              }
            };
            window.__A11_MEASURE_FPS = function() {
              var frames = 0;
              var started = 0;
              var probe = document.createElement("span");
              probe.setAttribute("aria-hidden", "true");
              probe.style.cssText = "position:fixed;right:0;bottom:0;width:1px;height:1px;background:currentColor;opacity:.01;pointer-events:none";
              document.body.appendChild(probe);
              function frame(now) {
                if (!started) started = now;
                frames += 1;
                probe.style.transform = "translateX(" + (frames % 2) + "px)";
                if (now - started < 1000) {
                  requestAnimationFrame(frame);
                  return;
                }
                probe.remove();
                var fps = frames * 1000 / (now - started);
                $reportFrameRate
              }
              requestAnimationFrame(frame);
            };
        """.trimIndent()
    }

    // --- theming -------------------------------------------------------------

    /** Map the current IDE look-and-feel onto the page's CSS variables. */
    private fun themeVars(): String {
        val schemeName = if (UIUtil.isUnderDarcula()) "dark" else "light"
        return "color-scheme: $schemeName;\n      --a11-color-scheme: $schemeName;\n" +
            "      --a11-host-bg: ${hex(UIUtil.getPanelBackground())};"
    }

    private fun hex(color: Color): String = "#%02x%02x%02x".format(color.red, color.green, color.blue)

    private fun readResource(path: String): String =
        javaClass.getResourceAsStream(path)?.use { it.readBytes().toString(Charsets.UTF_8) }
            ?: error("Missing plugin resource $path; run the webview build (see README).")

    /** Counts frames that reached JCEF's off-screen paint handler, not merely rAF callbacks. */
    private class PaintCadence {
        private val frames = AtomicInteger()
        @Volatile private var startedAt = 0L
        @Volatile private var measuring = false

        fun begin() {
            frames.set(0)
            startedAt = System.nanoTime()
            measuring = true
        }

        fun painted() {
            if (measuring) frames.incrementAndGet()
        }

        fun finish(): Double? {
            if (!measuring) return null
            measuring = false
            val elapsed = System.nanoTime() - startedAt
            if (elapsed <= 0) return null
            return frames.get() * 1_000_000_000.0 / elapsed
        }
    }

    private companion object {
        const val ERROR_CODE = 1
        const val MAIN_DISPLAY = "__main__"
        const val MAX_REQUESTED_FRAMERATE = 1000

        /**
         * The rate to fall back to when the display will not say what it runs
         * at.
         *
         * `DisplayMode.getRefreshRate` returns `REFRESH_RATE_UNKNOWN` on some
         * setups, and this is a better guess than CEF's own default of 30 —
         * nothing shipping today refreshes that slowly.
         */
        const val FALLBACK_FRAMERATE = 60

        /** The range a reported refresh rate has to be in to be believed. */
        val PLAUSIBLE_FRAMERATES = 24..500

        /**
         * The frame rate to run an off-screen browser on [device] at: that
         * display's refresh rate. Pass null for the default screen — which is
         * the best available answer before the component has been added to a
         * window.
         */
        fun frameRateOf(device: GraphicsDevice?): Int {
            val screen = try {
                device ?: GraphicsEnvironment.getLocalGraphicsEnvironment().defaultScreenDevice
            } catch (error: Throwable) {
                // Headless, or between display configurations.
                return FALLBACK_FRAMERATE
            }
            return reportedFrameRate(screen) ?: FALLBACK_FRAMERATE
        }

        private fun reportedFrameRate(device: GraphicsDevice?): Int? {
            val hz = try {
                device?.displayMode?.refreshRate ?: 0
            } catch (error: Throwable) {
                0
            }
            return hz.takeIf { it in PLAUSIBLE_FRAMERATES }
        }

        private fun isMac(): Boolean =
            System.getProperty("os.name", "").startsWith("Mac", ignoreCase = true)

        /**
         * The main display rate macOS reports when AWT returns
         * `REFRESH_RATE_UNKNOWN`, including variable-refresh ProMotion panels.
         */
        private val macRefreshRates: CompletableFuture<Map<String, Int>> by lazy {
            CompletableFuture.supplyAsync({
                if (!isMac()) return@supplyAsync emptyMap()
                try {
                    val process = ProcessBuilder(
                        "/usr/sbin/system_profiler",
                        "SPDisplaysDataType",
                        "-json",
                        "-detailLevel",
                        "mini",
                    ).redirectErrorStream(true).start()
                    val output = process.inputStream.bufferedReader().use { it.readText() }
                    process.waitFor()
                    @Suppress("UNCHECKED_CAST")
                    val root = A11Json.parse(output).valueOrThrow() as? Map<String, Any?>
                        ?: return@supplyAsync emptyMap()
                    val adapters = root["SPDisplaysDataType"] as? List<Map<String, Any?>>
                        ?: return@supplyAsync emptyMap()
                    val displays = adapters.asSequence()
                        .flatMap { (it["spdisplays_ndrvs"] as? List<Map<String, Any?>>).orEmpty().asSequence() }
                    buildMap {
                        for (display in displays) {
                            val resolution = display["_spdisplays_resolution"] as? String ?: continue
                            val rate = Regex("@\\s*([0-9]+(?:\\.[0-9]+)?)Hz")
                                .find(resolution)?.groupValues?.get(1)?.toDoubleOrNull()?.toInt()
                                ?.takeIf { it in PLAUSIBLE_FRAMERATES } ?: continue
                            val id = display["_spdisplays_displayID"]?.toString()
                            if (!id.isNullOrBlank()) put("Display $id", rate)
                            if (display["spdisplays_main"] == "spdisplays_yes") put(MAIN_DISPLAY, rate)
                        }
                    }
                } catch (_: Exception) {
                    emptyMap()
                }
            }, AppExecutorUtil.getAppExecutorService())
        }

        /**
         * A browser configured for the default display's refresh rate.
         *
         * The rate is corrected to the display the tool window ends up on by
         * [followDisplayFrameRate]. This remains the only configured value on
         * IDE versions without the runtime setter.
         *
         * `setWindowlessFramerate` is newer than the compatibility floor (build
         * 243). Older IDEs may raise [LinkageError]; browser creation continues
         * with the default frame rate.
         */
        private fun newBrowser(cadence: PaintCadence): JBCefBrowser {
            val builder = JBCefBrowser.createBuilder()
            try {
                builder.setWindowlessFramerate(frameRateOf(null))
                builder.setOSRHandlerFactory(object : JBCefOSRHandlerFactory {
                    override fun createCefRenderHandler(component: JComponent): CefRenderHandler {
                        val delegate = JBCefOSRHandlerFactory.getInstance().createCefRenderHandler(component)
                        return object : CefRenderHandler by delegate {
                            override fun onPaint(
                                browser: CefBrowser,
                                popup: Boolean,
                                dirtyRects: Array<Rectangle>,
                                buffer: ByteBuffer,
                                width: Int,
                                height: Int,
                            ) {
                                cadence.painted()
                                delegate.onPaint(browser, popup, dirtyRects, buffer, width, height)
                            }

                            override fun onAcceleratedPaint(
                                browser: CefBrowser,
                                popup: Boolean,
                                dirtyRects: Array<Rectangle>,
                                info: CefAcceleratedPaintInfo,
                            ) {
                                cadence.painted()
                                delegate.onAcceleratedPaint(browser, popup, dirtyRects, info)
                            }
                        }
                    }
                })
            } catch (error: LinkageError) {
                thisLogger().debug("JCEF windowless frame rate is not settable here", error)
            }
            return builder.build()
        }
    }
}
