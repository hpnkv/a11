package dev.curiositystack.a11.clion.settings

import com.intellij.openapi.options.Configurable
import com.intellij.openapi.ui.DialogPanel
import com.intellij.ui.dsl.builder.bindText
import com.intellij.ui.dsl.builder.panel
import javax.swing.JComponent
import javax.swing.JPasswordField

/** Settings UI under Preferences → Tools → A11 Chat. */
class A11Configurable : Configurable {
    private val settings = A11Settings.getInstance()
    private val state = settings.state
    private val apiKeyField = JPasswordField(String(CharArray(0)), 30).apply { text = settings.apiKey }
    private var panel: DialogPanel? = null

    override fun getDisplayName(): String = "A11 Chat"

    override fun createComponent(): JComponent {
        val built = panel {
            group("Gateway") {
                row("Gateway URL:") { textField().bindText(state::gatewayUrl) }
                    .comment("Run it with <code>a11 gateway</code>; the plugin does not start one.")
                row("Extra allowed tools:") { textField().bindText(state::allowedToolPatterns) }
                    .comment("Comma-separated name patterns for the gateway's own tools, e.g. <code>shell_.*</code>.")
            }
            group("Model") {
                row("Provider:") { textField().bindText(state::provider) }
                row("Model:") { textField().bindText(state::model) }
                row("Base URL (optional):") { textField().bindText(state::baseUrl) }
                row("API key:") { cell(apiKeyField) }
            }
        }
        panel = built
        return built
    }

    override fun isModified(): Boolean = (panel?.isModified() ?: false) || String(apiKeyField.password) != settings.apiKey

    override fun apply() {
        panel?.apply()
        settings.apiKey = String(apiKeyField.password)
    }

    override fun reset() {
        panel?.reset()
        apiKeyField.text = settings.apiKey
    }
}
