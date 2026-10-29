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

package a11

import a11.internal.NativeByteMap
import a11.internal.NativeData
import a11.internal.NativeList

/** Validate a native identifier using the C++ contract. */
fun validateName(name: String): Status = a11.internal.NativeControl.validateName(name)

/** The two wire payload variants accepted by a node fragment. */
sealed interface NodePayload {
    fun validate(): Status
}

/** Native metadata; member views retain the record containing them. */
class ChunkMetadata internal constructor(id: Long) : NativeObject(id) {
    constructor(
        mimetype: String = "",
        timestampMillis: Long? = null,
        attributes: Map<String, ByteArray> = emptyMap(),
    ) : this(NativeData.chunkMetadataNew()) {
        this.mimetype = mimetype
        this.timestampMillis = timestampMillis
        if (attributes.isNotEmpty()) attributesView.replaceContents(attributes)
    }

    var mimetype: String
        get() = NativeData.chunkMetadataMimetype(this)
        set(value) = NativeData.chunkMetadataMimetypeSet(this, value)

    var timestamp: Time?
        get() = NativeData.metadataTimestamp(this)
        set(value) = NativeData.metadataTimestampSet(this, value)

    var timestampMicros: Long?
        get() = NativeData.metadataMicros(this)
        set(value) = NativeData.metadataMicrosSet(this, value)

    var timestampMillis: Long?
        get() = NativeData.metadataMillis(this)
        set(value) = NativeData.metadataMillisSet(this, value)

    private val attributesView by
        lazy(LazyThreadSafetyMode.NONE) { NativeByteMap(NativeData.chunkMetadataMap(this)) }
    var attributes: ByteMap
        get() = attributesView
        set(value) {
            attributesView.replaceContents(value)
        }

    fun setAttribute(key: String, value: ByteArray): Status =
        NativeData.metadataSetAttribute(this, key, value)

    fun getAttribute(key: String): StatusOr<ByteArray> = NativeData.metadataGetAttribute(this, key)

    override fun equals(other: Any?): Boolean =
        other is ChunkMetadata && NativeData.chunkMetadataEquals(this, other)

    override fun hashCode(): Int = NativeData.chunkMetadataHash(this)

    fun copy(): ChunkMetadata = ChunkMetadata(NativeData.chunkMetadataCopy(this))

    fun validate(): Status = NativeData.chunkMetadataValidate(this)

    fun toMsgpack(): StatusOr<ByteArray> = NativeData.chunkMetadataEncode(this)

    /** Assign the native value, copying its owned contents. */
    fun copyFrom(other: ChunkMetadata) {
        NativeData.chunkMetadataCopyFrom(this, other)
    }

    /** Move native contents from [other]; it retains its C++ moved-from state. */
    fun moveFrom(other: ChunkMetadata) {
        NativeData.chunkMetadataMoveFrom(this, other)
    }

    /** Swap native contents without copying their buffers. */
    fun swap(other: ChunkMetadata) {
        NativeData.chunkMetadataSwap(this, other)
    }

    /** Native estimate used for bounded-buffer accounting. */
    val approxBytes: ULong
        get() = NativeData.chunkMetadataApproxBytes(this).toULong()

    fun debugString(): String = NativeData.chunkMetadataDebugString(this)

    override fun toString(): String = debugString()

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<ChunkMetadata> =
            NativeData.chunkMetadataDecode(bytes)
    }
}

/**
 * A native chunk. Reading [data] returns owned JVM bytes; assign [data] to replace them. Mutating a
 * returned byte array does not modify the native chunk, matching Python bytes.
 */
class Chunk internal constructor(id: Long) : NativeObject(id), NodePayload {
    constructor(
        metadata: ChunkMetadata? = null,
        ref: String = "",
        data: ByteArray = ByteArray(0),
    ) : this(NativeData.chunkNew()) {
        this.metadata = metadata
        this.ref = ref
        this.data = data
    }

    var metadata: ChunkMetadata?
        get() = NativeData.chunkMetadata(this)
        set(value) = NativeData.chunkMetadataSet(this, value)

    var ref: String
        get() = NativeData.chunkRef(this)
        set(value) = NativeData.chunkRefSet(this, value)

    var data: ByteArray
        get() = NativeData.chunkData(this)
        set(value) = NativeData.chunkDataSet(this, value)

    val hasObject: Boolean
        get() = NativeData.chunkHasObject(this)

    fun materialize(): Status = NativeData.chunkMaterialize(this)

    val mimetype: String
        get() = metadata?.mimetype ?: ""

    val isEmpty: Boolean
        get() = NativeData.chunkIsEmpty(this)

    val isNull: Boolean
        get() = NativeData.chunkIsNull(this)

    override fun equals(other: Any?): Boolean =
        other is Chunk && NativeData.chunkEquals(this, other)

    override fun hashCode(): Int = NativeData.chunkHash(this)

    fun copy(): Chunk = Chunk(NativeData.chunkCopy(this))

    override fun validate(): Status = NativeData.chunkValidate(this)

    fun toMsgpack(): StatusOr<ByteArray> = NativeData.chunkEncode(this)

    /** Assign the native value, copying its owned contents. */
    fun copyFrom(other: Chunk) {
        NativeData.chunkCopyFrom(this, other)
    }

    /** Move native contents from [other]; it retains its C++ moved-from state. */
    fun moveFrom(other: Chunk) {
        NativeData.chunkMoveFrom(this, other)
    }

    /** Swap native contents without copying their buffers. */
    fun swap(other: Chunk) {
        NativeData.chunkSwap(this, other)
    }

    /** Native estimate used for bounded-buffer accounting. */
    val approxBytes: ULong
        get() = NativeData.chunkApproxBytes(this).toULong()

    fun debugString(): String = NativeData.chunkDebugString(this)

    override fun toString(): String = debugString()

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<Chunk> = NativeData.chunkDecode(bytes)
    }
}

/** Reference to a native node slice. Offsets and lengths follow the C++ unsigned ranges. */
class NodeRef internal constructor(handle: Long) : NativeObject(handle), NodePayload {
    constructor(
        id: String,
        offset: UInt = 0u,
        length: ULong? = null,
    ) : this(NativeData.nodeRefNew()) {
        this.id = id
        this.offset = offset
        this.length = length
    }

    var id: String
        get() = NativeData.nodeRefId(this)
        set(value) = NativeData.nodeRefIdSet(this, value)

    var offset: UInt
        get() = NativeData.nodeRefOffset(this).toUInt()
        set(value) = NativeData.nodeRefOffsetSet(this, value.toLong())

    var length: ULong?
        get() = NativeData.nodeRefLength(this)?.toULong()
        set(value) = NativeData.nodeRefLengthSet(this, value?.toLong())

    override fun equals(other: Any?): Boolean =
        other is NodeRef && NativeData.nodeRefEquals(this, other)

    override fun hashCode(): Int = NativeData.nodeRefHash(this)

    fun copy(
        id: String = this.id,
        offset: UInt = this.offset,
        length: ULong? = this.length,
    ): NodeRef = NodeRef(id, offset, length)

    override fun validate(): Status = NativeData.nodeRefValidate(this)

    fun toMsgpack(): StatusOr<ByteArray> = NativeData.nodeRefEncode(this)

    /** Assign the native value, copying its owned contents. */
    fun copyFrom(other: NodeRef) {
        NativeData.nodeRefCopyFrom(this, other)
    }

    /** Move native contents from [other]; it retains its C++ moved-from state. */
    fun moveFrom(other: NodeRef) {
        NativeData.nodeRefMoveFrom(this, other)
    }

    /** Swap native contents without copying their buffers. */
    fun swap(other: NodeRef) {
        NativeData.nodeRefSwap(this, other)
    }

    /** Native estimate used for bounded-buffer accounting. */
    val approxBytes: ULong
        get() = NativeData.nodeRefApproxBytes(this).toULong()

    fun debugString(): String = NativeData.nodeRefDebugString(this)

    override fun toString(): String = debugString()

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<NodeRef> = NativeData.nodeRefDecode(bytes)
    }
}

/** A native fragment; payload access refers to the fragment's actual C++ member. */
class NodeFragment internal constructor(handle: Long) : NativeObject(handle) {
    constructor(
        id: String = "",
        data: NodePayload = Chunk(),
        seq: UInt? = null,
        continued: Boolean = false,
    ) : this(NativeData.nodeFragmentNew()) {
        this.id = id
        this.data = data
        this.seq = seq
        this.continued = continued
    }

    var id: String
        get() = NativeData.nodeFragmentId(this)
        set(value) = NativeData.nodeFragmentIdSet(this, value)

    var seq: UInt?
        get() = NativeData.nodeFragmentSeq(this)?.toUInt()
        set(value) = NativeData.nodeFragmentSeqSet(this, value?.toLong())

    var continued: Boolean
        get() = NativeData.nodeFragmentContinued(this)
        set(value) = NativeData.nodeFragmentContinuedSet(this, value)

    var data: NodePayload
        get() = if (NativeData.fragmentIsChunk(this)) getChunk().value() else getNodeRef().value()
        set(value) {
            when (value) {
                is Chunk -> NativeData.fragmentChunkSet(this, value)
                is NodeRef -> NativeData.fragmentNodeRefSet(this, value)
            }
        }

    fun getChunk(): StatusOr<Chunk> = NativeData.fragmentChunk(this)

    fun getNodeRef(): StatusOr<NodeRef> = NativeData.fragmentNodeRef(this)

    override fun equals(other: Any?): Boolean =
        other is NodeFragment && NativeData.nodeFragmentEquals(this, other)

    override fun hashCode(): Int = NativeData.nodeFragmentHash(this)

    fun copy(): NodeFragment = NodeFragment(NativeData.nodeFragmentCopy(this))

    fun validate(): Status = NativeData.nodeFragmentValidate(this)

    fun toMsgpack(): StatusOr<ByteArray> = NativeData.nodeFragmentEncode(this)

    /** Assign the native value, copying its owned contents. */
    fun copyFrom(other: NodeFragment) {
        NativeData.nodeFragmentCopyFrom(this, other)
    }

    /** Move native contents from [other]; it retains its C++ moved-from state. */
    fun moveFrom(other: NodeFragment) {
        NativeData.nodeFragmentMoveFrom(this, other)
    }

    /** Swap native contents without copying their buffers. */
    fun swap(other: NodeFragment) {
        NativeData.nodeFragmentSwap(this, other)
    }

    /** Native estimate used for bounded-buffer accounting. */
    val approxBytes: ULong
        get() = NativeData.nodeFragmentApproxBytes(this).toULong()

    fun debugString(): String = NativeData.nodeFragmentDebugString(this)

    override fun toString(): String = debugString()

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<NodeFragment> =
            NativeData.nodeFragmentDecode(bytes)
    }
}

/** Native binding from a schema port name to a node id. */
class Port internal constructor(handle: Long) : NativeObject(handle) {
    constructor(name: String = "", id: String = "") : this(NativeData.portNew()) {
        this.name = name
        this.id = id
    }

    var name: String
        get() = NativeData.portName(this)
        set(value) = NativeData.portNameSet(this, value)

    var id: String
        get() = NativeData.portId(this)
        set(value) = NativeData.portIdSet(this, value)

    fun copy(name: String = this.name, id: String = this.id): Port = Port(name, id)

    override fun equals(other: Any?): Boolean = other is Port && NativeData.portEquals(this, other)

    override fun hashCode(): Int = NativeData.portHash(this)

    fun validate(): Status = NativeData.portValidate(this)

    fun toMsgpack(): StatusOr<ByteArray> = NativeData.portEncode(this)

    /** Assign the native value, copying its owned contents. */
    fun copyFrom(other: Port) {
        NativeData.portCopyFrom(this, other)
    }

    /** Move native contents from [other]; it retains its C++ moved-from state. */
    fun moveFrom(other: Port) {
        NativeData.portMoveFrom(this, other)
    }

    /** Swap native contents without copying their buffers. */
    fun swap(other: Port) {
        NativeData.portSwap(this, other)
    }

    /** Native estimate used for bounded-buffer accounting. */
    val approxBytes: ULong
        get() = NativeData.portApproxBytes(this).toULong()

    fun debugString(): String = NativeData.portDebugString(this)

    override fun toString(): String = debugString()

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<Port> = NativeData.portDecode(bytes)
    }
}

/** Native ActionMessage with mutable views over its C++ collections. */
class ActionMessage internal constructor(handle: Long) : NativeObject(handle) {
    constructor(
        id: String = "",
        name: String = "",
        inputs: List<Port> = emptyList(),
        outputs: List<Port> = emptyList(),
        headers: Map<String, ByteArray> = emptyMap(),
    ) : this(NativeData.actionMessageNew()) {
        this.id = id
        this.name = name
        if (inputs.isNotEmpty()) NativeData.actionMessageInputsReplace(this, inputs.toTypedArray())
        if (outputs.isNotEmpty())
            NativeData.actionMessageOutputsReplace(this, outputs.toTypedArray())
        if (headers.isNotEmpty()) headersView.replaceContents(headers)
    }

    var id: String
        get() = NativeData.actionMessageId(this)
        set(value) = NativeData.actionMessageIdSet(this, value)

    var name: String
        get() = NativeData.actionMessageName(this)
        set(value) = NativeData.actionMessageNameSet(this, value)

    private val inputsView by
        lazy(LazyThreadSafetyMode.NONE) {
            NativeList(
                { NativeData.actionMessageInputsSize(this) },
                { index -> NativeData.actionMessageInputsGet(this, index) },
                { index, value -> NativeData.actionMessageInputsSet(this, index, value) },
                { index, value -> NativeData.actionMessageInputsAdd(this, index, value) },
                { index -> NativeData.actionMessageInputsRemove(this, index) },
                { NativeData.actionMessageInputsClear(this) },
            )
        }
    var inputs: MutableList<Port>
        get() = inputsView
        set(value) {
            NativeData.actionMessageInputsReplace(this, value.toTypedArray())
        }

    private val outputsView by
        lazy(LazyThreadSafetyMode.NONE) {
            NativeList(
                { NativeData.actionMessageOutputsSize(this) },
                { index -> NativeData.actionMessageOutputsGet(this, index) },
                { index, value -> NativeData.actionMessageOutputsSet(this, index, value) },
                { index, value -> NativeData.actionMessageOutputsAdd(this, index, value) },
                { index -> NativeData.actionMessageOutputsRemove(this, index) },
                { NativeData.actionMessageOutputsClear(this) },
            )
        }
    var outputs: MutableList<Port>
        get() = outputsView
        set(value) {
            NativeData.actionMessageOutputsReplace(this, value.toTypedArray())
        }

    private val headersView by
        lazy(LazyThreadSafetyMode.NONE) { NativeByteMap(NativeData.actionMessageMap(this)) }
    var headers: ByteMap
        get() = headersView
        set(value) {
            headersView.replaceContents(value)
        }

    fun copy(): ActionMessage = ActionMessage(NativeData.actionMessageCopy(this))

    fun validate(): Status = NativeData.actionMessageValidate(this)

    fun toMsgpack(): StatusOr<ByteArray> = NativeData.actionMessageEncode(this)

    override fun equals(other: Any?): Boolean =
        other is ActionMessage && NativeData.actionMessageEquals(this, other)

    override fun hashCode(): Int = NativeData.actionMessageHash(this)

    /** Assign the native value, copying its owned contents. */
    fun copyFrom(other: ActionMessage) {
        NativeData.actionMessageCopyFrom(this, other)
    }

    /** Move native contents from [other]; it retains its C++ moved-from state. */
    fun moveFrom(other: ActionMessage) {
        NativeData.actionMessageMoveFrom(this, other)
    }

    /** Swap native contents without copying their buffers. */
    fun swap(other: ActionMessage) {
        NativeData.actionMessageSwap(this, other)
    }

    /** Native estimate used for bounded-buffer accounting. */
    val approxBytes: ULong
        get() = NativeData.actionMessageApproxBytes(this).toULong()

    fun debugString(): String = NativeData.actionMessageDebugString(this)

    override fun toString(): String = debugString()

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<ActionMessage> =
            NativeData.actionMessageDecode(bytes)
    }
}

/** Native WireMessage with mutable views over its C++ collections. */
class WireMessage internal constructor(handle: Long) : NativeObject(handle) {
    constructor(
        nodeFragments: List<NodeFragment> = emptyList(),
        actions: List<ActionMessage> = emptyList(),
        headers: Map<String, ByteArray> = emptyMap(),
    ) : this(NativeData.wireMessageNew()) {
        if (nodeFragments.isNotEmpty())
            NativeData.wireMessageNodeFragmentsReplace(this, nodeFragments.toTypedArray())
        if (actions.isNotEmpty()) NativeData.wireMessageActionsReplace(this, actions.toTypedArray())
        if (headers.isNotEmpty()) headersView.replaceContents(headers)
    }

    private val nodeFragmentsView by
        lazy(LazyThreadSafetyMode.NONE) {
            NativeList(
                { NativeData.wireMessageNodeFragmentsSize(this) },
                { index -> NativeData.wireMessageNodeFragmentsGet(this, index) },
                { index, value -> NativeData.wireMessageNodeFragmentsSet(this, index, value) },
                { index, value -> NativeData.wireMessageNodeFragmentsAdd(this, index, value) },
                { index -> NativeData.wireMessageNodeFragmentsRemove(this, index) },
                { NativeData.wireMessageNodeFragmentsClear(this) },
            )
        }
    var nodeFragments: MutableList<NodeFragment>
        get() = nodeFragmentsView
        set(value) {
            NativeData.wireMessageNodeFragmentsReplace(this, value.toTypedArray())
        }

    private val actionsView by
        lazy(LazyThreadSafetyMode.NONE) {
            NativeList(
                { NativeData.wireMessageActionsSize(this) },
                { index -> NativeData.wireMessageActionsGet(this, index) },
                { index, value -> NativeData.wireMessageActionsSet(this, index, value) },
                { index, value -> NativeData.wireMessageActionsAdd(this, index, value) },
                { index -> NativeData.wireMessageActionsRemove(this, index) },
                { NativeData.wireMessageActionsClear(this) },
            )
        }
    var actions: MutableList<ActionMessage>
        get() = actionsView
        set(value) {
            NativeData.wireMessageActionsReplace(this, value.toTypedArray())
        }

    private val headersView by
        lazy(LazyThreadSafetyMode.NONE) { NativeByteMap(NativeData.wireMessageMap(this)) }
    var headers: ByteMap
        get() = headersView
        set(value) {
            headersView.replaceContents(value)
        }

    fun copy(): WireMessage = WireMessage(NativeData.wireMessageCopy(this))

    fun validate(): Status = NativeData.wireMessageValidate(this)

    fun toMsgpack(): StatusOr<ByteArray> = NativeData.wireMessageEncode(this)

    val isHalfClose: Boolean
        get() = actions.isEmpty() && nodeFragments.isEmpty()

    override fun equals(other: Any?): Boolean =
        other is WireMessage && NativeData.wireMessageEquals(this, other)

    override fun hashCode(): Int = NativeData.wireMessageHash(this)

    /** Assign the native value, copying its owned contents. */
    fun copyFrom(other: WireMessage) {
        NativeData.wireMessageCopyFrom(this, other)
    }

    /** Move native contents from [other]; it retains its C++ moved-from state. */
    fun moveFrom(other: WireMessage) {
        NativeData.wireMessageMoveFrom(this, other)
    }

    /** Swap native contents without copying their buffers. */
    fun swap(other: WireMessage) {
        NativeData.wireMessageSwap(this, other)
    }

    /** Native estimate used for bounded-buffer accounting. */
    val approxBytes: ULong
        get() = NativeData.wireMessageApproxBytes(this).toULong()

    fun debugString(): String = NativeData.wireMessageDebugString(this)

    override fun toString(): String = debugString()

    companion object {
        const val VERSION = 1

        fun fromMsgpack(bytes: ByteArray): StatusOr<WireMessage> =
            NativeData.wireMessageDecode(bytes)
    }
}

/** Build an empty lifecycle message carrying terminal headers. */
fun makeHalfCloseMessage(headers: Map<String, ByteArray> = emptyMap()): WireMessage =
    WireMessage(headers = headers)

/** Build the octet-stream null chunk used to terminate unary data. */
fun makeNullChunk(): Chunk = Chunk(metadata = ChunkMetadata(mimetype = "application/octet-stream"))
