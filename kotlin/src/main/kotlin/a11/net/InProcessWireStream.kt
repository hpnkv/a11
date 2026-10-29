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

package a11.net

import a11.internal.NativeBridge
import a11.map
import a11.value

/** Directly connected native transports with the same lifecycle as network streams. */
class InProcessWireStream private constructor(id: Long) : NativeWireStream(id) {
    companion object {
        fun createPair(): Pair<InProcessWireStream, InProcessWireStream> =
            NativeBridge.call("stream.inProcessPair")
                .map { raw ->
                    val pair = raw as List<*>
                    InProcessWireStream(NativeBridge.handle(pair[0])) to
                        InProcessWireStream(NativeBridge.handle(pair[1]))
                }
                .value()
    }
}
