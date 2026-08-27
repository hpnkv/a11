package dev.curiositystack.a11.clion.ui

import a11.A11Json
import a11.valueOrThrow
import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationInfo
import com.intellij.openapi.application.ApplicationNamesInfo
import com.intellij.openapi.diagnostic.thisLogger
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.colors.EditorColorsManager
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Disposer
import com.intellij.ui.JBColor
import com.intellij.ui.jcef.JBCefBrowser
import com.intellij.ui.jcef.JBCefJSQuery
import com.intellij.util.ui.UIUtil
import dev.curiositystack.a11.clion.highlights.HighlightNote
import dev.curiositystack.a11.clion.highlights.HighlightSuggestions
import dev.curiositystack.a11.clion.settings.A11Settings
import dev.curiositystack.a11.clion.tools.IdeTools
import java.awt.Color
import java.awt.GraphicsDevice
import java.awt.GraphicsEnvironment
import javax.swing.JComponent
import javax.swing.UIManager

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
class A11WebView(private val project: Project, view: String, parent: Disposable) {

    private val log = thisLogger()
    private val browser = newBrowser()
    private val ideTools = IdeTools(project)

    private val listActionsQuery = JBCefJSQuery.create(browser)
    private val runActionQuery = JBCefJSQuery.create(browser)
    private val getConfigQuery = JBCefJSQuery.create(browser)
    private val readFlowQuery = JBCefJSQuery.create(browser)
    private val suggestOnHighlightQuery = JBCefJSQuery.create(browser)
    private val clearSuggestionsQuery = JBCefJSQuery.create(browser)

    /**
     * The frame rate last pushed to the browser, or 0 for "none yet".
     *
     * The builder's rate may be discarded for out-of-process JCEF. Starting at
     * zero ensures [followDisplayFrameRate] updates the live browser.
     */
    private var pushedFrameRate = 0

    /** The AWT component the tool window embeds. */
    val component: JComponent get() = browser.component

    init {
        Disposer.register(parent, browser)
        Disposer.register(parent, listActionsQuery)
        Disposer.register(parent, runActionQuery)
        Disposer.register(parent, getConfigQuery)
        Disposer.register(parent, readFlowQuery)
        Disposer.register(parent, suggestOnHighlightQuery)
        Disposer.register(parent, clearSuggestionsQuery)

        listActionsQuery.addHandler { respond { A11Json.encodeToString(ideTools.listDescriptors()).valueOrThrow() } }
        runActionQuery.addHandler { request -> respond { runAction(request) } }
        getConfigQuery.addHandler { respond { config() } }
        readFlowQuery.addHandler { name -> respond { readFlow(name) } }
        suggestOnHighlightQuery.addHandler { note -> respond { suggestOnHighlight(note) } }
        clearSuggestionsQuery.addHandler { path -> respond { clearSuggestions(path) } }

        // Push the rate initially and whenever AWT reports a display change.
        followDisplayFrameRate()
        browser.component.addPropertyChangeListener("graphicsConfiguration") { followDisplayFrameRate() }

        browser.loadHTML(page(view))
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
    private fun followDisplayFrameRate() {
        val rate = frameRateOf(browser.component.graphicsConfiguration?.device)
        if (rate == pushedFrameRate) return
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
        @Suppress("UNCHECKED_CAST")
        val parsed = A11Json.parse(request).valueOrThrow() as? Map<String, Any?>
            ?: error("Malformed runAction request.")
        val name = parsed["name"] as? String ?: error("runAction requires a 'name'.")
        @Suppress("UNCHECKED_CAST")
        val inputs = (parsed["inputs"] as? Map<String, Any?>) ?: emptyMap()
        val result = ideTools.runByName(name, inputs)
        return A11Json.encodeToString(result).valueOrThrow()
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
            .replace("%%VIEW%%", if (view == "actions") "actions" else "chat")
            .replace("%%BRIDGE_JS%%", bridgeJs())
            .replace("%%APP_JS%%", appJs)
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
        return """
            window.__a11Bridge = {
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
              suggestOnHighlight: function(note) {
                return new Promise(function(resolve, reject) { var arg = JSON.stringify(note); ${wrap(suggestOnHighlightQuery)} });
              },
              clearSuggestions: function(path) {
                return new Promise(function(resolve, reject) { var arg = String(path || ""); ${wrap(clearSuggestionsQuery)} });
              }
            };
        """.trimIndent()
    }

    // --- theming -------------------------------------------------------------

    /** Map the current IDE look-and-feel onto the page's CSS variables. */
    private fun themeVars(): String {
        val bg = UIUtil.getPanelBackground()
        val fg = UIUtil.getLabelForeground()
        val bgAlt = UIUtil.getTextFieldBackground()
        // JSON highlighting in the action explorer follows the editor's scheme,
        // so a hand-typed value is colored like the same JSON would be in an
        // editor.
        val scheme = EditorColorsManager.getInstance().globalScheme
        fun syntax(key: TextAttributesKey, fallback: Color): Color =
            scheme.getAttributes(key)?.foregroundColor ?: fallback
        val border = UIManager.getColor("Component.borderColor") ?: JBColor.border()
        val accent = UIManager.getColor("Component.focusColor")
            ?: UIManager.getColor("ProgressBar.progressColor")
            ?: JBColor(Color(0x3574F0), Color(0x3574F0))
        val vars = linkedMapOf(
            "--a11-bg" to bg,
            "--a11-bg-alt" to bgAlt,
            "--a11-fg" to fg,
            "--a11-muted" to JBColor.GRAY,
            "--a11-border" to border,
            "--a11-accent" to accent,
            "--a11-accent-fg" to contrastingFg(accent),
            "--a11-assistant-bg" to bgAlt,
            "--a11-user-bg" to blend(accent, bg, 0.72),
            "--a11-json-key" to syntax(DefaultLanguageHighlighterColors.INSTANCE_FIELD, fg),
            "--a11-json-string" to syntax(DefaultLanguageHighlighterColors.STRING, fg),
            "--a11-json-number" to syntax(DefaultLanguageHighlighterColors.NUMBER, fg),
            "--a11-json-keyword" to syntax(DefaultLanguageHighlighterColors.KEYWORD, fg),
        )
        return vars.entries.joinToString("\n      ") { (name, color) -> "$name: ${hex(color)};" }
    }

    private fun hex(color: Color): String = "#%02x%02x%02x".format(color.red, color.green, color.blue)

    /**
     * Text colour with sufficient contrast against [background].
     * [JBColor.WHITE] is a theme pair and resolves to near-black in dark mode,
     * while the accent background remains blue in both themes.
     */
    private fun contrastingFg(background: Color): Color {
        val luma = (0.299 * background.red + 0.587 * background.green + 0.114 * background.blue) / 255.0
        return if (luma > 0.6) Color(0x1E, 0x1F, 0x22) else Color.WHITE
    }

    /** Mix [a] into [b] by [towardB] in [0,1] (0 = all a, 1 = all b). */
    private fun blend(a: Color, b: Color, towardB: Double): Color {
        val t = towardB.coerceIn(0.0, 1.0)
        fun mix(x: Int, y: Int) = (x * (1 - t) + y * t).toInt().coerceIn(0, 255)
        return Color(mix(a.red, b.red), mix(a.green, b.green), mix(a.blue, b.blue))
    }

    private fun readResource(path: String): String =
        javaClass.getResourceAsStream(path)?.use { it.readBytes().toString(Charsets.UTF_8) }
            ?: error("Missing plugin resource $path; run the webview build (see README).")

    private companion object {
        const val ERROR_CODE = 1

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
        val PLAUSIBLE_FRAMERATES = 24..240

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
            val hz = try {
                screen.displayMode?.refreshRate ?: 0
            } catch (error: Throwable) {
                0
            }
            return if (hz in PLAUSIBLE_FRAMERATES) hz else FALLBACK_FRAMERATE
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
        private fun newBrowser(): JBCefBrowser {
            val builder = JBCefBrowser.createBuilder()
            try {
                builder.setWindowlessFramerate(frameRateOf(null))
            } catch (error: LinkageError) {
                thisLogger().debug("JCEF windowless frame rate is not settable here", error)
            }
            return builder.build()
        }
    }
}
