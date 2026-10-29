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

package a11.internal

import a11.A11Json
import a11.A11Runtime
import a11.NativeObject
import a11.Ok
import a11.Status
import a11.StatusCode
import a11.StatusOr
import a11.cancelled
import a11.failedPrecondition
import a11.fatal
import a11.orElse
import a11.value
import java.lang.ref.Cleaner
import java.lang.ref.Reference
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import kotlin.coroutines.resume
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine

/** JNI transport only; all A11 judgments run in C++. */
internal object NativeBridge {
    private val callbacks = ConcurrentHashMap<Long, (NativeEvent.Completion) -> Unit>()
    private val nextToken = AtomicLong(1)
    private val handlers = ConcurrentHashMap<Long, suspend (Any?) -> Status>()
    private val jobs = ConcurrentHashMap<Long, Job>()
    private val stopping = AtomicBoolean(false)
    private val loaded: Unit =
        loadLibrary().also {
            Runtime.getRuntime()
                .addShutdownHook(
                    Thread(
                        {
                            stopping.set(true)
                            shutdown()
                        },
                        "a11-jvm-shutdown",
                    )
                )
        }
    private val pump: Thread by lazy {
        Thread(
                {
                    while (!stopping.get()) {
                        for (event in poll()) {
                            when (event) {
                                is NativeEvent.Release -> handlers.remove(event.token)
                                is NativeEvent.Cancellation -> jobs.remove(event.promise)?.cancel()
                                is NativeEvent.Invocation -> dispatchCallback(event)
                                is NativeEvent.Completion -> {
                                    val callback = callbacks.remove(event.token)
                                    if (callback == null) releaseOwned(event.owned)
                                    else callback(event)
                                }
                            }
                        }
                    }
                },
                "a11-jvm-completions",
            )
            .apply {
                isDaemon = true
                start()
            }
    }

    fun ensureLoaded() {
        loaded
    }

    private external fun request(encoded: ByteArray): ByteArray

    private external fun poll(): Array<NativeEvent>

    private external fun shutdown()

    external fun currentFiberId(): Long

    private fun loadLibrary() {
        val explicit = System.getProperty("a11.native.library")
        if (explicit != null) {
            System.load(Path.of(explicit).toAbsolutePath().toString())
            return
        }
        val os =
            when {
                System.getProperty("os.name").startsWith("Mac") -> "macos"
                System.getProperty("os.name").startsWith("Linux") -> "linux"
                else -> "windows"
            }
        val arch =
            when (System.getProperty("os.arch")) {
                "aarch64",
                "arm64" -> "aarch64"
                "amd64",
                "x86_64" -> "x86_64"
                else -> System.getProperty("os.arch")
            }
        val filename = System.mapLibraryName("a11_jvm")
        val resource = "/a11/native/$os-$arch/$filename"
        val stream =
            NativeBridge::class.java.getResourceAsStream(resource)
                ?: throw UnsatisfiedLinkError(
                    "A11 native library missing: $resource; run the native build or set a11.native.library"
                )
        val directory = Files.createTempDirectory("a11-jvm-").toFile().apply { deleteOnExit() }
        val library = directory.toPath().resolve(filename).toFile().apply { deleteOnExit() }
        stream.use { input -> library.outputStream().use { output -> input.copyTo(output) } }
        System.load(library.absolutePath)
    }

    fun call(
        operation: String,
        self: Long = 0,
        args: Map<String, Any?> = emptyMap(),
    ): StatusOr<Any?> {
        try {
            val encoded =
                JvmCodec.encodeOne(mapOf("op" to operation, "self" to self, "args" to args))
                    .orElse { status ->
                        return status
                    }
            val response =
                JvmCodec.decodeOne(request(encoded)).orElse { status ->
                    return status
                }
            return decode(fields(response))
        } finally {
            Reference.reachabilityFence(args)
        }
    }

    fun call(
        operation: String,
        self: NativeObject,
        args: Map<String, Any?> = emptyMap(),
    ): StatusOr<Any?> {
        try {
            return call(operation, self.nativeId, args)
        } finally {
            Reference.reachabilityFence(self)
        }
    }

    fun status(
        operation: String,
        self: NativeObject,
        args: Map<String, Any?> = emptyMap(),
    ): Status = call(operation, self, args).status

    suspend fun async(
        operation: String,
        self: NativeObject,
        args: Map<String, Any?> = emptyMap(),
    ): StatusOr<Any?> {
        try {
            return async(operation, self.nativeId, args)
        } finally {
            Reference.reachabilityFence(self)
        }
    }

    suspend fun await(self: NativeObject): StatusOr<Any?> {
        try {
            return await(self.nativeId)
        } finally {
            Reference.reachabilityFence(self)
        }
    }

    fun status(operation: String, self: Long = 0, args: Map<String, Any?> = emptyMap()): Status =
        call(operation, self, args).status

    private fun decode(envelope: Map<String, Any?>): StatusOr<Any?> {
        val status = statusValue(envelope["status"])
        status.orElse { failure ->
            return failure
        }
        return Ok(envelope["value"])
    }

    suspend fun await(future: Long): StatusOr<Any?> = suspendCancellableCoroutine { continuation ->
        pump
        val token = nextToken.getAndIncrement()
        callbacks[token] = { event ->
            continuation.resume(event.result) { _, _, _ -> releaseOwned(event.owned) }
        }
        val subscriptionLock = Any()
        continuation.invokeOnCancellation {
            synchronized(subscriptionLock) {
                callbacks.remove(token)
                status("future.unsubscribe", future, mapOf("token" to token))
                NativeControl.futureCancel(future)
            }
        }
        synchronized(subscriptionLock) {
            if (continuation.isActive) {
                val subscribed = status("future.subscribe", future, mapOf("token" to token))
                subscribed.orElse { status ->
                    if (callbacks.remove(token) != null) continuation.resume(status)
                    return@synchronized
                }
            }
        }
    }

    suspend fun async(
        operation: String,
        self: Long = 0,
        args: Map<String, Any?> = emptyMap(),
    ): StatusOr<Any?> {
        val future =
            call(operation, self, args).orElse { status ->
                return status
            }
        return awaitOwned(handle(future))
    }

    suspend fun awaitOwned(id: Long): StatusOr<Any?> =
        try {
            await(id)
        } finally {
            release(id)
        }

    fun callback(handler: suspend (Any?) -> Status): StatusOr<NativeReference> {
        pump
        val token = nextToken.getAndIncrement()
        handlers[token] = handler
        val result =
            call("callback.create", args = mapOf("token" to token)).orElse { status ->
                handlers.remove(token)
                return status
            }
        return Ok(NativeReference(handle(result)))
    }

    private fun dispatchCallback(event: NativeEvent.Invocation) {
        val promise = event.promise
        val handler = handlers[event.token]
        if (handler == null) {
            releaseOwned(event.owned)
            completeCallback(promise, failedPrecondition("JVM callback was released"))
            return
        }
        val entered = AtomicBoolean(false)
        val job =
            A11Runtime.scope.launch(start = CoroutineStart.LAZY) {
                entered.set(true)
                try {
                    completeCallback(promise, handler(event.arguments))
                } catch (error: CancellationException) {
                    completeCallback(promise, cancelled("JVM callback was cancelled"))
                    throw error
                } catch (error: Exception) {
                    completeCallback(promise, Status.fromException(error))
                } finally {
                    jobs.remove(promise)
                }
            }
        jobs[promise] = job
        job.invokeOnCompletion {
            if (!entered.get()) releaseOwned(event.owned)
            jobs.remove(promise)
        }
        job.start()
    }

    private fun completeCallback(promise: Long, status: Status) {
        val details =
            if (status.details.isEmpty()) ""
            else
                A11Json.encodeToString(status.details).orElse { error ->
                    completeCallbackNative(promise, error.code.value, error.message, "")
                    return
                }
        completeCallbackNative(promise, status.code.value, status.message, details)
    }

    private external fun completeCallbackNative(
        promise: Long,
        code: Int,
        message: String,
        details: String,
    ): Status

    fun release(id: Long) {
        if (id != 0L) NativeControl.release(id)
    }

    private fun releaseOwned(handles: LongArray) {
        for (id in handles) release(id)
    }

    fun fields(value: Any?): Map<String, Any?> {
        val source = value as? Map<*, *> ?: fatal("Expected native object fields")
        return source.entries.associate { (key, item) ->
            (key as? String ?: fatal("Native object key is not a string")) to item
        }
    }

    fun number(value: Any?): Long = (value as? Number)?.toLong() ?: fatal("Expected native integer")

    fun handle(value: Any?): Long = number(fields(value)["handle"])

    fun statusValue(value: Any?): Status {
        val fields = fields(value)
        val code =
            StatusCode.fromValue(number(fields["code"]).toInt())
                ?: fatal("Native status code is not canonical")
        val message = fields["message"] as? String ?: fatal("Native status message is not text")
        val details = fields["details"] as? List<*> ?: emptyList<Any?>()
        return Status(code, message, details)
    }
}

internal class NativeReference(id: Long) {
    private class Release(private val id: AtomicLong) : Runnable {
        override fun run() {
            NativeBridge.release(id.getAndSet(0))
        }
    }

    private val retained = AtomicLong(id)
    private val cleanable = cleaner.register(this, Release(retained))
    val id: Long
        get() = retained.get()

    fun release() {
        cleanable.clean()
    }

    companion object {
        private val cleaner: Cleaner = Cleaner.create()
    }
}
