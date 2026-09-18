package com.mintdesk.client

import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import kotlin.concurrent.thread

class ClipboardDataReceiver(
    private val context: Context,
    private val host: String,
    private val port: Int,
    private val onStatus: (String) -> Unit
) {
    @Volatile
    private var running = false

    @Volatile
    private var socket: Socket? = null

    private var worker: Thread? = null
    private val outputLock = Any()

    fun start() {
        if (running) return
        running = true
        worker = thread(name = "MintDesk-ClipboardReceiver") {
            runReceiver()
        }
    }

    fun stop() {
        running = false
        socket?.close()
        socket = null
        worker?.interrupt()
        worker = null
    }

    fun sendFile(uri: Uri, displayName: String, mimeType: String?) {
        val client = socket
        if (!running || client == null || client.isClosed) {
            onStatus("Clipboard channel is not connected.")
            return
        }

        thread(name = "MintDesk-ClipboardSender") {
            runCatching {
                val payload = context.contentResolver.openInputStream(uri)?.use { input ->
                    val output = ByteArrayOutputStream()
                    val buffer = ByteArray(64 * 1024)
                    var total = 0L
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count
                        output.write(buffer, 0, count)
                    }
                    output.toByteArray()
                } ?: error("Unable to open selected file")

                val cleanName = displayName
                    .replace(Regex("[\\\\/:*?\"<>|\\r\\n]"), "_")
                    .trim()
                    .ifBlank { "mintdesk-file.bin" }
                val nameBytes = cleanName.toByteArray(StandardCharsets.UTF_8)
                val type = if (mimeType?.startsWith("image/") == true) {
                    TYPE_IMAGE_PNG
                } else {
                    TYPE_FILE
                }

                val header = ByteBuffer.allocate(HEADER_SIZE)
                    .order(ByteOrder.LITTLE_ENDIAN)
                    .put(byteArrayOf('M'.code.toByte(), 'D'.code.toByte(), 'C'.code.toByte(), 'L'.code.toByte()))
                    .putShort(PROTOCOL_VERSION.toShort())
                    .putShort(type.toShort())
                    .putInt(nameBytes.size)
                    .putLong(payload.size.toLong())
                    .putInt(0)
                    .array()

                synchronized(outputLock) {
                    val output = client.getOutputStream()
                    writeAll(output, header)
                    writeAll(output, nameBytes)
                    writeAll(output, payload)
                    output.flush()
                }
                onStatus("File sent to PC: $cleanName")
                Log.i(TAG, "Sent file $cleanName (${payload.size} bytes)")
            }.onFailure { error ->
                Log.w(TAG, "Clipboard file send failed", error)
                onStatus("File send failed: ${error.message ?: error.javaClass.simpleName}")
            }
        }
    }

    private fun writeAll(output: java.io.OutputStream, bytes: ByteArray) {
        var offset = 0
        while (offset < bytes.size) {
            val count = minOf(64 * 1024, bytes.size - offset)
            output.write(bytes, offset, count)
            offset += count
        }
    }

    private fun runReceiver() {
        try {
            val client = Socket()
            socket = client
            client.tcpNoDelay = true
            client.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
            onStatus("Clipboard channel connected.")
            Log.i(TAG, "Clipboard connected to $host:$port")

            val input = client.getInputStream()
            while (running) {
                val header = input.readExact(HEADER_SIZE) ?: break
                if (header[0] != 'M'.code.toByte() ||
                    header[1] != 'D'.code.toByte() ||
                    header[2] != 'C'.code.toByte() ||
                    header[3] != 'L'.code.toByte()
                ) {
                    throw IllegalStateException("Clipboard packet magic mismatch")
                }

                val view = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
                view.position(4)
                val version = view.short.toInt() and 0xFFFF
                val type = view.short.toInt() and 0xFFFF
                val nameSize = view.int
                val payloadSize = view.long
                view.int

                if (version != PROTOCOL_VERSION ||
                    nameSize < 0 ||
                    nameSize > MAX_NAME_BYTES ||
                    payloadSize < 0 ||
                    payloadSize > Int.MAX_VALUE.toLong()
                ) {
                    throw IllegalStateException(
                        "Unsupported clipboard packet version=$version name=$nameSize size=$payloadSize"
                    )
                }

                val name = if (nameSize > 0) {
                    String(input.readExact(nameSize) ?: break, StandardCharsets.UTF_8)
                } else {
                    defaultNameForType(type)
                }

                val payload = input.readExact(payloadSize.toInt()) ?: break
                handlePacket(type, name, payload)
            }
        } catch (error: Exception) {
            if (running) {
                Log.w(TAG, "Clipboard receiver error", error)
                onStatus("Clipboard sync stopped: ${error.message ?: error.javaClass.simpleName}")
            }
        } finally {
            socket?.close()
            socket = null
            running = false
        }
    }

    private fun handlePacket(type: Int, name: String, payload: ByteArray) {
        when (type) {
            TYPE_TEXT -> {
                val text = String(payload, StandardCharsets.UTF_8)
                val clipboard = context.getSystemService(ClipboardManager::class.java)
                clipboard?.setPrimaryClip(ClipData.newPlainText("MintDesk", text))
                onStatus("Clipboard text received (${text.length} chars).")
            }
            TYPE_IMAGE_PNG -> {
                val savedName = savePayload(uniqueFileName(name.ifBlank { "clipboard.png" }), payload)
                onStatus("Image received: $savedName")
            }
            TYPE_FILE -> {
                val savedName = savePayload(uniqueFileName(name.ifBlank { "mintdesk-file.bin" }), payload)
                onStatus("File received: $savedName")
            }
            else -> {
                Log.w(TAG, "Unknown clipboard packet type=$type size=${payload.size}")
            }
        }
    }

    private fun savePayload(name: String, payload: ByteArray): String {
        val cleanName = sanitizeFileName(name)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val resolver = context.contentResolver
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, cleanName)
                put(MediaStore.Downloads.MIME_TYPE, mimeTypeForName(cleanName))
                put(MediaStore.Downloads.RELATIVE_PATH, "${Environment.DIRECTORY_DOWNLOADS}/MintDesk")
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri: Uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: error("Unable to create Downloads item")

            resolver.openOutputStream(uri)?.use { output ->
                output.write(payload)
            } ?: error("Unable to open Downloads item")

            values.clear()
            values.put(MediaStore.Downloads.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
            Log.i(TAG, "Saved clipboard payload to Downloads/MintDesk/$cleanName")
            return cleanName
        }

        val baseDir = File(
            context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS),
            "MintDesk"
        )
        baseDir.mkdirs()
        val file = File(baseDir, cleanName)
        file.outputStream().use { output ->
            output.write(payload)
        }
        Log.i(TAG, "Saved clipboard payload to ${file.absolutePath}")
        return file.name
    }

    private fun mimeTypeForName(name: String): String {
        val lower = name.lowercase()
        return when {
            lower.endsWith(".png") -> "image/png"
            lower.endsWith(".jpg") || lower.endsWith(".jpeg") -> "image/jpeg"
            lower.endsWith(".gif") -> "image/gif"
            lower.endsWith(".txt") -> "text/plain"
            lower.endsWith(".pdf") -> "application/pdf"
            lower.endsWith(".zip") -> "application/zip"
            else -> "application/octet-stream"
        }
    }

    private fun sanitizeFileName(name: String): String {
        val cleaned = name.replace(Regex("[\\\\/:*?\"<>|\\r\\n]"), "_").trim()
        return cleaned.ifEmpty { "mintdesk-file.bin" }
    }

    private fun uniqueFileName(name: String): String {
        val dot = name.lastIndexOf('.')
        val base = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        val suffix = System.currentTimeMillis()
        return "${base}_${suffix}${ext}"
    }

    private fun defaultNameForType(type: Int): String {
        return when (type) {
            TYPE_TEXT -> "clipboard.txt"
            TYPE_IMAGE_PNG -> "clipboard.png"
            else -> "mintdesk-file.bin"
        }
    }

    private fun java.io.InputStream.readExact(size: Int): ByteArray? {
        val buffer = ByteArray(size)
        var offset = 0
        while (offset < size) {
            val read = read(buffer, offset, size - offset)
            if (read < 0) {
                return null
            }
            offset += read
        }
        return buffer
    }

    companion object {
        private const val TAG = "MintDeskClipboard"
        private const val CONNECT_TIMEOUT_MS = 5000
        private const val HEADER_SIZE = 24
        private const val PROTOCOL_VERSION = 1
        private const val TYPE_TEXT = 1
        private const val TYPE_IMAGE_PNG = 2
        private const val TYPE_FILE = 3
        private const val MAX_NAME_BYTES = 4096
    }
}
