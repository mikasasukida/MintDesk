package com.mintdesk.client

class H264AnnexBParser(
    private val onNalUnit: (ByteArray) -> Unit
) {
    private var pending = ByteArray(0)

    fun append(data: ByteArray, length: Int) {
        if (length <= 0) return

        val combined = ByteArray(pending.size + length)
        pending.copyInto(combined)
        data.copyInto(combined, destinationOffset = pending.size, endIndex = length)

        var currentStart = findStartCode(combined, 0)
        if (currentStart < 0) {
            pending = combined.takeLastBytes(4)
            return
        }

        var searchFrom = startCodeEnd(combined, currentStart)
        while (true) {
            val nextStart = findStartCode(combined, searchFrom)
            if (nextStart < 0) {
                break
            }

            emitIfNonEmpty(combined.copyOfRange(currentStart, nextStart))
            currentStart = nextStart
            searchFrom = startCodeEnd(combined, currentStart)
        }

        pending = combined.copyOfRange(currentStart, combined.size)
    }

    fun flush() {
        emitIfNonEmpty(pending)
        pending = ByteArray(0)
    }

    private fun emitIfNonEmpty(nal: ByteArray) {
        if (nal.size > H264Nal.startCodeLength(nal)) {
            onNalUnit(nal)
        }
    }

    private fun findStartCode(bytes: ByteArray, offset: Int): Int {
        var i = offset.coerceAtLeast(0)
        while (i + 3 < bytes.size) {
            if (bytes[i] == 0.toByte() && bytes[i + 1] == 0.toByte()) {
                if (bytes[i + 2] == 1.toByte()) {
                    return i
                }
                if (i + 4 < bytes.size &&
                    bytes[i + 2] == 0.toByte() &&
                    bytes[i + 3] == 1.toByte()
                ) {
                    return i
                }
            }
            i++
        }
        return -1
    }

    private fun startCodeEnd(bytes: ByteArray, offset: Int): Int {
        return offset + startCodeLength(bytes, offset)
    }

    private fun startCodeLength(bytes: ByteArray, offset: Int): Int {
        return H264Nal.startCodeLength(bytes, offset)
    }

    private fun ByteArray.takeLastBytes(count: Int): ByteArray {
        if (size <= count) return this
        return copyOfRange(size - count, size)
    }
}
