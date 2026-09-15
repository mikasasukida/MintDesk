package com.mintdesk.client

object H264Nal {
    const val TYPE_NON_IDR = 1
    const val TYPE_IDR = 5
    const val TYPE_SEI = 6
    const val TYPE_SPS = 7
    const val TYPE_PPS = 8

    fun startCodeLength(bytes: ByteArray, offset: Int = 0): Int {
        if (offset + 2 < bytes.size &&
            bytes[offset] == 0.toByte() &&
            bytes[offset + 1] == 0.toByte() &&
            bytes[offset + 2] == 1.toByte()
        ) {
            return 3
        }

        if (offset + 3 < bytes.size &&
            bytes[offset] == 0.toByte() &&
            bytes[offset + 1] == 0.toByte() &&
            bytes[offset + 2] == 0.toByte() &&
            bytes[offset + 3] == 1.toByte()
        ) {
            return 4
        }

        return 0
    }

    fun typeOf(nal: ByteArray): Int {
        val headerOffset = startCodeLength(nal)
        if (headerOffset >= nal.size) return -1
        return nal[headerOffset].toInt() and 0x1F
    }

    fun typeName(type: Int): String {
        return when (type) {
            TYPE_NON_IDR -> "non-IDR"
            TYPE_IDR -> "IDR"
            TYPE_SEI -> "SEI"
            TYPE_SPS -> "SPS"
            TYPE_PPS -> "PPS"
            else -> "type-$type"
        }
    }
}
