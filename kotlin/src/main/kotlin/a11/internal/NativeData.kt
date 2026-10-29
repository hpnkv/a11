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

import a11.ActionMessage
import a11.Chunk
import a11.ChunkMetadata
import a11.ChunkStore
import a11.NativeObject
import a11.NodeFragment
import a11.NodeRef
import a11.Port
import a11.SessionWithRecv
import a11.Status
import a11.StatusOr
import a11.Time
import a11.WireMessage
import a11.net.NativeWireStream

/** Typed JNI access to the actual C++ wire records and their member views. */
internal object NativeData {
    external fun metadataSetAttribute(self: ChunkMetadata, key: String, value: ByteArray): Status

    external fun metadataGetAttribute(self: ChunkMetadata, key: String): StatusOr<ByteArray>

    external fun chunkMaterialize(self: Chunk): Status

    external fun chunkHasObject(self: Chunk): Boolean

    external fun streamSend(self: NativeWireStream, message: WireMessage): Status

    external fun sessionSend(self: SessionWithRecv, message: WireMessage): Status

    external fun storePut(self: ChunkStore, fragment: NodeFragment): StatusOr<Long>

    init {
        NativeBridge.ensureLoaded()
    }

    external fun chunkMetadataNew(): Long

    external fun chunkMetadataCopy(self: ChunkMetadata): Long

    external fun chunkMetadataValidate(self: ChunkMetadata): Status

    external fun chunkMetadataEncode(self: ChunkMetadata): StatusOr<ByteArray>

    external fun chunkMetadataDecode(bytes: ByteArray): StatusOr<ChunkMetadata>

    external fun chunkMetadataMimetype(self: ChunkMetadata): String

    external fun chunkMetadataMimetypeSet(self: ChunkMetadata, value: String): Unit

    external fun chunkNew(): Long

    external fun chunkCopy(self: Chunk): Long

    external fun chunkValidate(self: Chunk): Status

    external fun chunkEncode(self: Chunk): StatusOr<ByteArray>

    external fun chunkDecode(bytes: ByteArray): StatusOr<Chunk>

    external fun chunkRef(self: Chunk): String

    external fun chunkRefSet(self: Chunk, value: String): Unit

    external fun chunkData(self: Chunk): ByteArray

    external fun chunkDataSet(self: Chunk, value: ByteArray): Unit

    external fun nodeRefNew(): Long

    external fun nodeRefCopy(self: NodeRef): Long

    external fun nodeRefValidate(self: NodeRef): Status

    external fun nodeRefEncode(self: NodeRef): StatusOr<ByteArray>

    external fun nodeRefDecode(bytes: ByteArray): StatusOr<NodeRef>

    external fun nodeRefId(self: NodeRef): String

    external fun nodeRefIdSet(self: NodeRef, value: String): Unit

    external fun nodeRefOffset(self: NodeRef): Long

    external fun nodeRefOffsetSet(self: NodeRef, value: Long): Unit

    external fun nodeRefLength(self: NodeRef): Long?

    external fun nodeRefLengthSet(self: NodeRef, value: Long?): Unit

    external fun nodeFragmentNew(): Long

    external fun nodeFragmentCopy(self: NodeFragment): Long

    external fun nodeFragmentValidate(self: NodeFragment): Status

    external fun nodeFragmentEncode(self: NodeFragment): StatusOr<ByteArray>

    external fun nodeFragmentDecode(bytes: ByteArray): StatusOr<NodeFragment>

    external fun nodeFragmentId(self: NodeFragment): String

    external fun nodeFragmentIdSet(self: NodeFragment, value: String): Unit

    external fun nodeFragmentSeq(self: NodeFragment): Long?

    external fun nodeFragmentSeqSet(self: NodeFragment, value: Long?): Unit

    external fun nodeFragmentContinued(self: NodeFragment): Boolean

    external fun nodeFragmentContinuedSet(self: NodeFragment, value: Boolean): Unit

    external fun portNew(): Long

    external fun portCopy(self: Port): Long

    external fun portValidate(self: Port): Status

    external fun portEncode(self: Port): StatusOr<ByteArray>

    external fun portDecode(bytes: ByteArray): StatusOr<Port>

    external fun portName(self: Port): String

    external fun portNameSet(self: Port, value: String): Unit

    external fun portId(self: Port): String

    external fun portIdSet(self: Port, value: String): Unit

    external fun actionMessageNew(): Long

    external fun actionMessageCopy(self: ActionMessage): Long

    external fun actionMessageValidate(self: ActionMessage): Status

    external fun actionMessageEncode(self: ActionMessage): StatusOr<ByteArray>

    external fun actionMessageDecode(bytes: ByteArray): StatusOr<ActionMessage>

    external fun actionMessageId(self: ActionMessage): String

    external fun actionMessageIdSet(self: ActionMessage, value: String): Unit

    external fun actionMessageName(self: ActionMessage): String

    external fun actionMessageNameSet(self: ActionMessage, value: String): Unit

    external fun wireMessageNew(): Long

    external fun wireMessageCopy(self: WireMessage): Long

    external fun wireMessageValidate(self: WireMessage): Status

    external fun wireMessageEncode(self: WireMessage): StatusOr<ByteArray>

    external fun wireMessageDecode(bytes: ByteArray): StatusOr<WireMessage>

    external fun metadataTimestamp(self: ChunkMetadata): Time?

    external fun metadataTimestampSet(self: ChunkMetadata, value: Time?): Unit

    external fun metadataMicros(self: ChunkMetadata): Long?

    external fun metadataMicrosSet(self: ChunkMetadata, value: Long?): Unit

    external fun chunkMetadata(self: Chunk): ChunkMetadata?

    external fun chunkMetadataSet(self: Chunk, value: ChunkMetadata?): Unit

    external fun chunkIsEmpty(self: Chunk): Boolean

    external fun chunkIsNull(self: Chunk): Boolean

    external fun fragmentIsChunk(self: NodeFragment): Boolean

    external fun fragmentChunk(self: NodeFragment): StatusOr<Chunk>

    external fun fragmentChunkSet(self: NodeFragment, value: Chunk): Unit

    external fun fragmentNodeRef(self: NodeFragment): StatusOr<NodeRef>

    external fun fragmentNodeRefSet(self: NodeFragment, value: NodeRef): Unit

    external fun chunkMetadataMap(self: ChunkMetadata): Long

    external fun actionMessageMap(self: ActionMessage): Long

    external fun wireMessageMap(self: WireMessage): Long

    external fun mapKeys(id: NativeObject): Array<String>

    external fun mapGet(id: NativeObject, key: String): ByteArray?

    external fun mapPut(id: NativeObject, key: String, bytes: ByteArray?): ByteArray?

    external fun actionMessageInputsSize(self: ActionMessage): Int

    external fun actionMessageInputsGet(self: ActionMessage, index: Int): Port

    external fun actionMessageInputsSet(self: ActionMessage, index: Int, value: Port): Port

    external fun actionMessageInputsAdd(self: ActionMessage, index: Int, value: Port): Unit

    external fun actionMessageInputsRemove(self: ActionMessage, index: Int): Port

    external fun actionMessageOutputsSize(self: ActionMessage): Int

    external fun actionMessageOutputsGet(self: ActionMessage, index: Int): Port

    external fun actionMessageOutputsSet(self: ActionMessage, index: Int, value: Port): Port

    external fun actionMessageOutputsAdd(self: ActionMessage, index: Int, value: Port): Unit

    external fun actionMessageOutputsRemove(self: ActionMessage, index: Int): Port

    external fun wireMessageNodeFragmentsSize(self: WireMessage): Int

    external fun wireMessageNodeFragmentsGet(self: WireMessage, index: Int): NodeFragment

    external fun wireMessageNodeFragmentsSet(
        self: WireMessage,
        index: Int,
        value: NodeFragment,
    ): NodeFragment

    external fun wireMessageNodeFragmentsAdd(
        self: WireMessage,
        index: Int,
        value: NodeFragment,
    ): Unit

    external fun wireMessageNodeFragmentsRemove(self: WireMessage, index: Int): NodeFragment

    external fun wireMessageActionsSize(self: WireMessage): Int

    external fun wireMessageActionsGet(self: WireMessage, index: Int): ActionMessage

    external fun wireMessageActionsSet(
        self: WireMessage,
        index: Int,
        value: ActionMessage,
    ): ActionMessage

    external fun wireMessageActionsAdd(self: WireMessage, index: Int, value: ActionMessage): Unit

    external fun wireMessageActionsRemove(self: WireMessage, index: Int): ActionMessage

    external fun chunkMetadataCopyFrom(self: ChunkMetadata, other: ChunkMetadata)

    external fun chunkMetadataMoveFrom(self: ChunkMetadata, other: ChunkMetadata)

    external fun chunkMetadataSwap(self: ChunkMetadata, other: ChunkMetadata)

    external fun chunkCopyFrom(self: Chunk, other: Chunk)

    external fun chunkMoveFrom(self: Chunk, other: Chunk)

    external fun chunkSwap(self: Chunk, other: Chunk)

    external fun nodeRefCopyFrom(self: NodeRef, other: NodeRef)

    external fun nodeRefMoveFrom(self: NodeRef, other: NodeRef)

    external fun nodeRefSwap(self: NodeRef, other: NodeRef)

    external fun nodeFragmentCopyFrom(self: NodeFragment, other: NodeFragment)

    external fun nodeFragmentMoveFrom(self: NodeFragment, other: NodeFragment)

    external fun nodeFragmentSwap(self: NodeFragment, other: NodeFragment)

    external fun portCopyFrom(self: Port, other: Port)

    external fun portMoveFrom(self: Port, other: Port)

    external fun portSwap(self: Port, other: Port)

    external fun actionMessageCopyFrom(self: ActionMessage, other: ActionMessage)

    external fun actionMessageMoveFrom(self: ActionMessage, other: ActionMessage)

    external fun actionMessageSwap(self: ActionMessage, other: ActionMessage)

    external fun wireMessageCopyFrom(self: WireMessage, other: WireMessage)

    external fun wireMessageMoveFrom(self: WireMessage, other: WireMessage)

    external fun wireMessageSwap(self: WireMessage, other: WireMessage)

    external fun mapSize(self: NativeObject): Int

    external fun mapContainsKey(self: NativeObject, key: String): Boolean

    external fun mapClear(self: NativeObject)

    external fun mapErase(self: NativeObject, key: String)

    external fun chunkMetadataEquals(self: ChunkMetadata, other: ChunkMetadata): Boolean

    external fun chunkMetadataHash(self: ChunkMetadata): Int

    external fun chunkEquals(self: Chunk, other: Chunk): Boolean

    external fun chunkHash(self: Chunk): Int

    external fun nodeRefEquals(self: NodeRef, other: NodeRef): Boolean

    external fun nodeRefHash(self: NodeRef): Int

    external fun nodeFragmentEquals(self: NodeFragment, other: NodeFragment): Boolean

    external fun nodeFragmentHash(self: NodeFragment): Int

    external fun portEquals(self: Port, other: Port): Boolean

    external fun portHash(self: Port): Int

    external fun actionMessageEquals(self: ActionMessage, other: ActionMessage): Boolean

    external fun actionMessageHash(self: ActionMessage): Int

    external fun wireMessageEquals(self: WireMessage, other: WireMessage): Boolean

    external fun wireMessageHash(self: WireMessage): Int

    external fun actionMessageInputsReplace(self: ActionMessage, values: Array<Port>)

    external fun actionMessageInputsClear(self: ActionMessage)

    external fun actionMessageOutputsReplace(self: ActionMessage, values: Array<Port>)

    external fun actionMessageOutputsClear(self: ActionMessage)

    external fun wireMessageNodeFragmentsReplace(self: WireMessage, values: Array<NodeFragment>)

    external fun wireMessageNodeFragmentsClear(self: WireMessage)

    external fun wireMessageActionsReplace(self: WireMessage, values: Array<ActionMessage>)

    external fun wireMessageActionsClear(self: WireMessage)

    external fun mapAssignNative(self: NativeObject, other: NativeObject)

    external fun mapAssign(self: NativeObject, keys: Array<String>, values: Array<ByteArray>)

    external fun mapPutAllNative(self: NativeObject, other: NativeObject)

    external fun mapPutAll(self: NativeObject, keys: Array<String>, values: Array<ByteArray>)

    external fun metadataMillis(self: ChunkMetadata): Long?

    external fun metadataMillisSet(self: ChunkMetadata, value: Long?)

    external fun streamSendMoved(self: NativeWireStream, message: WireMessage): Status

    external fun sessionSendMoved(self: SessionWithRecv, message: WireMessage): Status

    external fun storePutMoved(self: ChunkStore, fragment: NodeFragment): StatusOr<Long>

    external fun chunkMetadataDebugString(self: ChunkMetadata): String

    external fun chunkMetadataApproxBytes(self: ChunkMetadata): Long

    external fun chunkDebugString(self: Chunk): String

    external fun chunkApproxBytes(self: Chunk): Long

    external fun nodeRefDebugString(self: NodeRef): String

    external fun nodeRefApproxBytes(self: NodeRef): Long

    external fun nodeFragmentDebugString(self: NodeFragment): String

    external fun nodeFragmentApproxBytes(self: NodeFragment): Long

    external fun portDebugString(self: Port): String

    external fun portApproxBytes(self: Port): Long

    external fun actionMessageDebugString(self: ActionMessage): String

    external fun actionMessageApproxBytes(self: ActionMessage): Long

    external fun wireMessageDebugString(self: WireMessage): String

    external fun wireMessageApproxBytes(self: WireMessage): Long
}
