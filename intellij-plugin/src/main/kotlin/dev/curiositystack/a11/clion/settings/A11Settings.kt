package dev.curiositystack.a11.clion.settings

import com.intellij.credentialStore.CredentialAttributes
import com.intellij.credentialStore.generateServiceName
import com.intellij.ide.passwordSafe.PasswordSafe
import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.components.service

/**
 * Persisted A11 chat configuration. The API key is kept in the IDE
 * [PasswordSafe], never in this serialized state.
 *
 * The chat runs against the **A11 gateway** (`a11 gateway`), a service the user
 * runs; the plugin does not start or own one. So the only connection setting is
 * where to find it, defaulting to the gateway's own default address.
 */
@State(name = "A11ChatSettings", storages = [Storage("a11-chat.xml")])
class A11Settings : PersistentStateComponent<A11Settings.State> {

    data class State(
        var gatewayUrl: String = DEFAULT_GATEWAY_URL,
        /**
         * Extra allowed-tool patterns, comma-separated, sent with every turn on
         * top of the IDE's own tools.
         *
         * These are what let the model reach tools that live in the *gateway*
         * rather than the IDE — `shell_.*` for its shell tools, by default. The
         * gateway offers a tool only if a pattern here matches its name, so this
         * is also how they are turned off: empty the field.
         */
        var allowedToolPatterns: String = DEFAULT_ALLOWED_TOOL_PATTERNS,
        var provider: String = "claude",
        var model: String = "claude-sonnet-4-6",
        var baseUrl: String = "",
    )

    private var state = State()

    override fun getState(): State = state
    override fun loadState(state: State) { this.state = state }

    var apiKey: String
        get() = PasswordSafe.instance.getPassword(keyAttributes()) ?: ""
        set(value) = PasswordSafe.instance.setPassword(keyAttributes(), value.ifBlank { null })

    /**
     * The gateway WebSocket URL to dial.
     *
     * Typing `127.0.0.1:8011` is the common case and means the same thing as the
     * full URL, so a missing scheme and a missing path are both filled in rather
     * than rejected — a connection error two steps later is a poor way to learn
     * that a prefix was left off.
     */
    fun gatewayUrl(): String {
        val configured = state.gatewayUrl.trim().ifBlank { DEFAULT_GATEWAY_URL }
        val withScheme =
            if (configured.startsWith("ws://") || configured.startsWith("wss://")) configured
            else "ws://$configured"
        val afterScheme = withScheme.substringAfter("://")
        return if (afterScheme.contains('/')) withScheme else "$withScheme$DEFAULT_GATEWAY_PATH"
    }

    /** The extra allowed-tool patterns, split and cleaned. */
    fun allowedToolPatterns(): List<String> =
        state.allowedToolPatterns.split(',').map { it.trim() }.filter { it.isNotEmpty() }

    private fun keyAttributes(): CredentialAttributes =
        CredentialAttributes(generateServiceName("A11 Chat", "llm-api-key-${state.provider}"))

    companion object {
        /** Where `a11 gateway` listens unless told otherwise. */
        const val DEFAULT_GATEWAY_URL = "ws://127.0.0.1:8011/a11"
        const val DEFAULT_GATEWAY_PATH = "/a11"
        /** The gateway's shell tools, on by default. */
        const val DEFAULT_ALLOWED_TOOL_PATTERNS = "shell_.*"

        fun getInstance(): A11Settings = service()
    }
}
