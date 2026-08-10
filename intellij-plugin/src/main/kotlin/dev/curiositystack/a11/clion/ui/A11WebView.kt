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
import dev.curiositystack.a11.clion.settings.A11Settings
import dev.curiositystack.a11.clion.tools.IdeTools
import java.awt.Color
import java.awt.GraphicsDevice
import java.awt.GraphicsEnvironment
import javax.swing.JComponent
import javax.swing.UIManager

/**
 * The JCEF-hosted A11 surface. One page bundle drives two views — the chat
 * window and the action explorer — selected by [view] (`"chat"` / `"actions"`).
 *
 * The page runs the TypeScript A11 library, which owns the WebSocket to the A11
 * gateway directly. Everything that needs the live IDE (running a tool,
 * fetching connection config) is reached through three [JBCefJSQuery] bridges
 * that call back into Kotlin; [IdeTools] remains the single source of truth for
 * the IDE tools. The Kotlin A11 runtime ([dev.curiositystack.a11.clion.session])
 * is untouched and still available for richer, Kotlin-driven experiences.
 */
class A11WebView(private val project: Project, view: String, parent: Disposable) {

    private val log = thisLogger()
    private val browser = newBrowser()
    private val ideTools = IdeTools(project)

    private val listActionsQuery = JBCefJSQuery.create(browser)
    private val runActionQuery = JBCefJSQuery.create(browser)
    private val getConfigQuery = JBCefJSQuery.create(browser)

    /**
     * The frame rate last pushed to the browser, or 0 for "none yet".
     *
     * Not seeded with the rate the browser was *built* with, however plausible
     * that looks: out of process, that value is dropped (see
     * [followDisplayFrameRate]), so seeding it here is what made the one call
     * that works look redundant and skip.
     */
    private var pushedFrameRate = 0

    /** The AWT component the tool window embeds. */
    val component: JComponent get() = browser.component

    init {
        Disposer.register(parent, browser)
        Disposer.register(parent, listActionsQuery)
        Disposer.register(parent, runActionQuery)
        Disposer.register(parent, getConfigQuery)

        listActionsQuery.addHandler { respond { A11Json.encodeToString(ideTools.listDescriptors()).valueOrThrow() } }
        runActionQuery.addHandler { request -> respond { runAction(request) } }
        getConfigQuery.addHandler { respond { config() } }

        // Push the rate rather than trusting the one the browser was built with,
        // and push it again whenever the window changes display: AWT fires this
        // property then.
        followDisplayFrameRate()
        browser.component.addPropertyChangeListener("graphicsConfiguration") { followDisplayFrameRate() }

        browser.loadHTML(page(view))
    }

    /**
     * Tell the browser to paint at the refresh rate of the display it is on.
     *
     * An off-screen browser paints at a rate it is told, not at the display's, and
     * CEF's default is 30 — a third of the frames on a 120 Hz screen, which is
     * what "scrolling skips" looks like. Neither CEF nor Chromium caps the number
     * (CEF's `ClampFrameRate` only floors it; the capture pipeline's bound is
     * 1000fps), so the rate can simply follow the hardware.
     *
     * It has to be *pushed* to the live browser, though. The IDE runs JCEF out of
     * process by default (`ide.browser.jcef.out-of-process.enabled`), and its
     * `RemoteBrowser` keeps the `CefBrowserSettings` it was constructed with in a
     * field it never reads: `windowless_frame_rate` appears nowhere in that class,
     * `Browser_Create` sends only the request context, and the rate starts life as
     * a hardcoded 30. So the builder's value — and with it the registry key it
     * defaults from — is silently dropped, which is why setting
     * `ide.browser.jcef.osr.framerate` has no effect either. What does work is
     * `setWindowlessFrameRate`: it sends `Browser_SetFrameRate` over the wire, and
     * defers itself until the browser exists, so calling it this early is safe.
     *
     * There is deliberately no read-back to confirm it: `getWindowlessFrameRate`
     * in that same class returns its own cached field, with a TODO where the real
     * implementation should be, so it would only ever echo what we set. The IDE's
     * own OSR FPS meter (internal action `JBCefOsrMeasureFps`) is what actually
     * measures this.
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

    /** Run a bridge handler, mapping the result/exception onto the JS promise. */
    private fun respond(block: () -> String): JBCefJSQuery.Response =
        try {
            JBCefJSQuery.Response(block())
        } catch (error: Throwable) {
            log.warn("A11 bridge call failed", error)
            JBCefJSQuery.Response(null, ERROR_CODE, error.message ?: error.toString())
        }

    /**
     * Handle `runAction`: `{name, inputs}` in, the tool's JSON result out, where
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
     * Handle `getConfig`: gateway URL + provider/model/apiKey from settings, plus
     * where the model actually is — which IDE, which project, which directory.
     *
     * That last part is resolved here rather than guessed in the page: the IDE
     * name is whatever product this build is running in (the plugin is not
     * CLion-only), and the project is the one this tool window belongs to, which
     * the page has no other way to learn.
     */
    private fun config(): String {
        val settings = A11Settings.getInstance()
        val cfg = settings.state
        // The IDE's own tools, plus whatever patterns the user allowed the
        // gateway to add (its shell tools, by default). Both go into the
        // allowed-tools header, which is what decides whether the gateway offers
        // the model a tool of its own.
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
            // Null for the default (project-less) frame, and for a project opened
            // as a set of unrelated roots; the page omits the line rather than
            // telling the model the project lives at "null".
            "projectPath" to project.basePath,
        )
        return A11Json.encodeToString(config).valueOrThrow()
    }

    /** Build the page HTML: template + theme vars + bridge shims + app bundle. */
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
     * its [JBCefJSQuery] injection in a Promise so the TypeScript side can await
     * it. `inject(request, onSuccess, onFailure)` splices `onSuccess`/`onFailure`
     * into the generated `window.<func>({request, onSuccess, onFailure})` call as
     * *values*, so they must be full `function(...) {...}` expressions (not bare
     * bodies) — otherwise the emitted object literal is a syntax error and the
     * whole bridge fails to define. `response` / `error_message` are the params
     * the query router passes back.
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
        // JSON highlighting in the action explorer follows the editor's scheme, so
        // a hand-typed value is colored like the same JSON would be in an editor.
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
     * Text color to lay on top of [background]. Not [JBColor.WHITE]: the JBColor
     * constants are light/dark *pairs*, so under a dark theme it resolves to a
     * near-black — unreadable on the saturated accent blue, which stays blue in
     * both themes. Contrast follows the accent, not the theme.
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
         * The rate to fall back to when the display will not say what it runs at.
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
         * display's refresh rate. Pass null for the default screen — which is the
         * best available answer before the component has been added to a window.
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
         * A browser that paints at the default display's refresh rate, rather than
         * at CEF's 30fps default for a windowless browser.
         *
         * The rate is corrected to the display the tool window ends up on by
         * [followDisplayFrameRate]; this is the starting value, and the only one an
         * IDE too old for the runtime setter will ever have.
         *
         * `setWindowlessFramerate` is newer than this plugin's compatibility floor
         * (build 243), and the plugin is compiled against a much later platform
         * than the oldest it claims to run on — so on an older IDE the call is
         * simply not there. That is a missing frame rate, not a missing chat: the
         * browser is built either way.
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
