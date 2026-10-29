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

package a11.flow

import a11.NativeObject
import a11.StatusOr
import a11.internal.NativeBridge
import a11.map

/** Native language service shared with Python, the CLI, and editor frontends. */
object Flow {
    fun request(request: Map<String, Any?>): StatusOr<Map<String, Any?>> =
        NativeBridge.call("flow.request", args = mapOf("value" to request))
            .map(NativeBridge::fields)

    fun check(source: String, sourceName: String = "-"): StatusOr<Map<String, Any?>> =
        request(mapOf("method" to "check", "source" to source, "path" to sourceName))

    fun format(source: String): StatusOr<Map<String, Any?>> =
        request(mapOf("method" to "format", "source" to source))

    fun complete(source: String, offset: Long): StatusOr<Map<String, Any?>> =
        request(mapOf("method" to "complete", "source" to source, "offset" to offset))

    fun compile(source: String, sourceName: String = ""): StatusOr<Program> =
        NativeBridge.call("flow.compile", args = mapOf("source" to source, "path" to sourceName))
            .map { value -> Program(NativeBridge.handle(value)) }
}

/** Native compiled program; graphs and their syntax tree share ownership. */
class Program internal constructor(id: Long) : NativeObject(id) {
    fun plan(): StatusOr<Map<String, Any?>> =
        NativeBridge.call("flow.plan", this).map(NativeBridge::fields)
}
