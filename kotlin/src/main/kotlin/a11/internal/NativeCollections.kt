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

import a11.NativeObject

/** Mutable collection protocols over native fields; no shadow collection is retained. */
internal class NativeList<T>(
    private val count: () -> Int,
    private val read: (Int) -> T,
    private val replace: (Int, T) -> T,
    private val insert: (Int, T) -> Unit,
    private val erase: (Int) -> T,
    private val clearContents: () -> Unit,
) : AbstractMutableList<T>() {
    override val size: Int
        get() = count()

    override fun get(index: Int): T {
        checkElement(index)
        return read(index)
    }

    override fun set(index: Int, element: T): T {
        checkElement(index)
        return replace(index, element)
    }

    override fun add(index: Int, element: T) {
        if (index !in 0..size) throw IndexOutOfBoundsException("Index: $index, size: $size")
        insert(index, element)
        modCount++
    }

    override fun clear() {
        clearContents()
        modCount++
    }

    override fun removeAt(index: Int): T {
        checkElement(index)
        val removed = erase(index)
        modCount++
        return removed
    }

    private fun checkElement(index: Int) {
        if (index !in 0 until size) throw IndexOutOfBoundsException("Index: $index, size: $size")
    }
}

internal class NativeByteMap(id: Long) : AbstractMutableMap<String, ByteArray>() {
    private val reference = object : NativeObject(id) {}

    override val size: Int
        get() = NativeData.mapSize(reference)

    override fun containsKey(key: String): Boolean = NativeData.mapContainsKey(reference, key)

    override fun clear() {
        NativeData.mapClear(reference)
    }

    override fun get(key: String): ByteArray? = NativeData.mapGet(reference, key)

    override fun put(key: String, value: ByteArray): ByteArray? =
        NativeData.mapPut(reference, key, value)

    override fun remove(key: String): ByteArray? = NativeData.mapPut(reference, key, null)

    fun replaceContents(from: Map<String, ByteArray>) {
        if (from is NativeByteMap) NativeData.mapAssignNative(reference, from.reference)
        else {
            val entries = from.entries.toList()
            NativeData.mapAssign(
                reference,
                entries.map { entry -> entry.key }.toTypedArray(),
                entries.map { entry -> entry.value }.toTypedArray(),
            )
        }
    }

    override fun putAll(from: Map<out String, ByteArray>) {
        if (from is NativeByteMap) NativeData.mapPutAllNative(reference, from.reference)
        else {
            val entries = from.entries.toList()
            NativeData.mapPutAll(
                reference,
                entries.map { entry -> entry.key }.toTypedArray(),
                entries.map { entry -> entry.value }.toTypedArray(),
            )
        }
    }

    override val entries: MutableSet<MutableMap.MutableEntry<String, ByteArray>>
        get() =
            object : AbstractMutableSet<MutableMap.MutableEntry<String, ByteArray>>() {
                override val size: Int
                    get() = this@NativeByteMap.size

                override fun add(element: MutableMap.MutableEntry<String, ByteArray>): Boolean =
                    throw UnsupportedOperationException("Use put")

                override fun iterator():
                    MutableIterator<MutableMap.MutableEntry<String, ByteArray>> {
                    val keys = NativeData.mapKeys(reference).iterator()
                    var current: String? = null
                    return object : MutableIterator<MutableMap.MutableEntry<String, ByteArray>> {
                        override fun hasNext(): Boolean = keys.hasNext()

                        override fun next(): MutableMap.MutableEntry<String, ByteArray> {
                            val key = keys.next()
                            current = key
                            return object : MutableMap.MutableEntry<String, ByteArray> {
                                override val key: String = key
                                override val value: ByteArray
                                    get() =
                                        get(key)
                                            ?: throw ConcurrentModificationException(
                                                "Native map entry was removed"
                                            )

                                override fun setValue(newValue: ByteArray): ByteArray =
                                    put(key, newValue)
                                        ?: throw ConcurrentModificationException(
                                            "Native map entry was removed"
                                        )

                                override fun equals(other: Any?): Boolean =
                                    other is Map.Entry<*, *> &&
                                        key == other.key &&
                                        value == other.value

                                override fun hashCode(): Int = key.hashCode() xor value.hashCode()
                            }
                        }

                        override fun remove() {
                            val key =
                                current ?: throw IllegalStateException("Call next before remove")
                            NativeData.mapErase(reference, key)
                            current = null
                        }
                    }
                }
            }
}
