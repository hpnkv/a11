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

import a11.Status
import a11.StatusOr
import a11.net.ByteChunkingOptions
import a11.net.BytePacket
import a11.net.ByteReassembler

/** Typed JNI access to C++ packet parsing and reassembly. */
internal object NativeFraming {
    init {
        NativeBridge.ensureLoaded()
    }

    external fun defaults(): LongArray

    external fun minimumPacketSize(): Int

    external fun validate(options: ByteChunkingOptions): Status

    external fun create(options: ByteChunkingOptions): StatusOr<Long>

    external fun split(
        bytes: ByteArray,
        transientId: Long,
        packetSize: Int,
    ): StatusOr<List<ByteArray>>

    external fun parse(bytes: ByteArray): StatusOr<BytePacket>

    external fun feed(self: ByteReassembler, bytes: ByteArray): StatusOr<ByteArray?>

    external fun clear(self: ByteReassembler)

    external fun pendingMessages(self: ByteReassembler): Long

    external fun pendingBytes(self: ByteReassembler): Long

    external fun packetCreate(
        type: Int,
        payload: ByteArray,
        transientId: Long,
        sequence: Long,
        count: Long,
    ): Long

    external fun packetCopy(self: BytePacket): Long

    external fun packetType(self: BytePacket): Int

    external fun packetTypeSet(self: BytePacket, value: Int)

    external fun packetPayload(self: BytePacket): ByteArray

    external fun packetPayloadSet(self: BytePacket, value: ByteArray)

    external fun packetTransientId(self: BytePacket): Long

    external fun packetTransientIdSet(self: BytePacket, value: Long)

    external fun packetSequence(self: BytePacket): Long

    external fun packetSequenceSet(self: BytePacket, value: Long)

    external fun packetCount(self: BytePacket): Long

    external fun packetCountSet(self: BytePacket, value: Long)
}
