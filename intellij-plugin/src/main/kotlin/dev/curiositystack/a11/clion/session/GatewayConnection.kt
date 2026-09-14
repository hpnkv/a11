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

package dev.curiositystack.a11.clion.session

import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.Disposable
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.startup.ProjectActivity
import com.intellij.util.concurrency.AppExecutorUtil
import dev.curiositystack.a11.clion.settings.A11Settings
import java.net.URI
import java.net.http.HttpClient
import java.net.http.WebSocket
import java.nio.ByteBuffer
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/** Maintains a Gateway connection for the project, independently of tool windows. */
@Service(Service.Level.PROJECT)
class GatewayConnection(private val project: Project) : Disposable, WebSocket.Listener {
    private var socket: WebSocket? = null
    private var retry: ScheduledFuture<*>? = null
    private var stopped = false
    private var notified = false
    private var connecting = false

    fun start() = ensureConnected()

    /** Start a dial immediately when no live or pending socket exists. */
    @Synchronized
    fun ensureConnected() {
        if (stopped || socket != null || connecting) return
        retry?.cancel(false)
        retry = null
        connecting = true
        connect()
    }

    private fun connect() {
        if (stopped) {
            connecting = false
            return
        }
        try {
            HttpClient.newHttpClient().newWebSocketBuilder()
                .buildAsync(URI(A11Settings.getInstance().gatewayUrl()), this)
                .whenComplete { connected, error ->
                    connecting = false
                    if (error != null) failed(error.message ?: error.javaClass.simpleName)
                    else socket = connected
                }
        } catch (error: Exception) {
            connecting = false
            failed(error.message ?: error.javaClass.simpleName)
        }
    }

    override fun onOpen(webSocket: WebSocket) {
        socket = webSocket
        notified = false
        webSocket.request(1)
    }

    override fun onBinary(webSocket: WebSocket, data: ByteBuffer, last: Boolean): CompletionStage<*> {
        webSocket.request(1)
        return CompletableFuture.completedFuture(null)
    }

    override fun onText(webSocket: WebSocket, data: CharSequence, last: Boolean): CompletionStage<*> {
        webSocket.request(1)
        return CompletableFuture.completedFuture(null)
    }

    override fun onClose(webSocket: WebSocket, statusCode: Int, reason: String): CompletionStage<*> {
        socket = null
        scheduleRetry()
        return CompletableFuture.completedFuture(null)
    }

    override fun onError(webSocket: WebSocket, error: Throwable) {
        socket = null
        failed(error.message ?: error.javaClass.simpleName)
    }

    private fun failed(reason: String) {
        if (!notified && !stopped) {
            notified = true
            NotificationGroupManager.getInstance().getNotificationGroup("A11")
                .createNotification(
                    "A11 Gateway is not available yet",
                    "$reason. The plugin will keep trying in the background.",
                    NotificationType.INFORMATION,
                ).notify(project)
        }
        scheduleRetry()
    }

    private fun scheduleRetry() {
        if (stopped || retry?.isDone == false) return
        retry = AppExecutorUtil.getAppScheduledExecutorService()
            .schedule({ ensureConnected() }, 5, TimeUnit.SECONDS)
    }

    override fun dispose() {
        stopped = true
        retry?.cancel(false)
        socket?.sendClose(WebSocket.NORMAL_CLOSURE, "IDE project closed")
        socket = null
    }

    companion object {
        fun getInstance(project: Project): GatewayConnection = project.service()
    }
}

/** Establishes the project Gateway connection during IDE startup. */
class GatewayStartup : ProjectActivity, DumbAware {
    override suspend fun execute(project: Project) {
        GatewayConnection.getInstance(project).start()
    }
}
