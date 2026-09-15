package com.mintdesk.client

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.util.Log
import android.view.Surface
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlin.concurrent.thread

class H264SocketReceiver(
    private val context: Context,
    private val host: String,
    private val port: Int,
    private val surface: Surface,
    private val onStatus: (String) -> Unit,
    private val onStopped: () -> Unit
) {
    @Volatile
    private var running = false

    @Volatile
    private var socket: Socket? = null

    @Volatile
    private var controlSocket: Socket? = null

    private val outputLock = Any()
    private val controlExecutor: ExecutorService = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "MintDesk-ControlSender")
    }
    private var worker: Thread? = null

    fun start() {
        if (running) return
        running = true
        worker = thread(name = "MintDesk-H264Receiver") {
            runReceiver()
        }
    }

    fun stop() {
        running = false
        socket?.close()
        controlSocket?.close()
        socket = null
        controlSocket = null
        controlExecutor.shutdownNow()
        worker?.interrupt()
        worker = null
    }

    fun sendPointer(action: Int, normalizedX: Float, normalizedY: Float) {
        if (!running) return

        val packet = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN)
        writeHeader(packet, TYPE_POINTER, POINTER_PAYLOAD_SIZE)
        packet.putInt(action)
        packet.putFloat(normalizedX.coerceIn(0f, 1f))
        packet.putFloat(normalizedY.coerceIn(0f, 1f))

        sendControlPacket(packet.array()) {
            val actionName = when (action) {
                POINTER_MOVE -> "move"
                POINTER_DOWN -> "down"
                POINTER_UP -> "up"
                POINTER_RIGHT_DOWN -> "right down"
                POINTER_RIGHT_UP -> "right up"
                else -> "unknown"
            }
            "Sent input $actionName ${"%.3f".format(normalizedX)}, ${"%.3f".format(normalizedY)}"
        }
    }

    fun sendWheel(delta: Int, normalizedX: Float, normalizedY: Float) {
        if (!running) return

        val packet = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN)
        writeHeader(packet, TYPE_WHEEL, WHEEL_PAYLOAD_SIZE)
        packet.putInt(delta)
        packet.putFloat(normalizedX.coerceIn(0f, 1f))
        packet.putFloat(normalizedY.coerceIn(0f, 1f))

        sendControlPacket(packet.array()) {
            "Sent wheel $delta"
        }
    }

    fun sendText(codePoint: Int) {
        if (!running || codePoint <= 0) return

        val packet = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
        writeHeader(packet, TYPE_TEXT, TEXT_PAYLOAD_SIZE)
        packet.putInt(codePoint)

        sendControlPacket(packet.array()) {
            "Sent text U+${codePoint.toString(16).uppercase()}"
        }
    }

    fun sendKey(virtualKey: Int) {
        if (!running || virtualKey <= 0) return

        val packet = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
        writeHeader(packet, TYPE_KEY, KEY_PAYLOAD_SIZE)
        packet.putInt(virtualKey)

        sendControlPacket(packet.array()) {
            "Sent key VK $virtualKey"
        }
    }

    fun sendKeyEvent(action: Int, virtualKey: Int) {
        if (!running || virtualKey <= 0) return

        val packet = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
        writeHeader(packet, TYPE_KEY, KEY_EVENT_PAYLOAD_SIZE)
        packet.putInt(action)
        packet.putInt(virtualKey)

        sendControlPacket(packet.array()) {
            val name = when (action) {
                KEY_ACTION_DOWN -> "down"
                KEY_ACTION_UP -> "up"
                else -> "press"
            }
            "Sent key $name VK $virtualKey"
        }
    }

    fun sendRelativeMouse(deltaX: Float, deltaY: Float) {
        if (!running) return

        val packet = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
        writeHeader(packet, TYPE_RELATIVE_MOUSE, RELATIVE_MOUSE_PAYLOAD_SIZE)
        packet.putFloat(deltaX)
        packet.putFloat(deltaY)

        sendControlPacket(packet.array()) {
            "Sent relative mouse ${"%.1f".format(deltaX)}, ${"%.1f".format(deltaY)}"
        }
    }

    fun sendMouseButton(action: Int) {
        if (!running) return

        val packet = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
        writeHeader(packet, TYPE_MOUSE_BUTTON, MOUSE_BUTTON_PAYLOAD_SIZE)
        packet.putInt(action)

        sendControlPacket(packet.array()) {
            "Sent mouse button $action"
        }
    }

    private fun writeHeader(packet: ByteBuffer, type: Int, payloadSize: Int) {
        packet.put(byteArrayOf('M'.code.toByte(), 'D'.code.toByte(), 'I'.code.toByte(), 'N'.code.toByte()))
        packet.putShort(PROTOCOL_VERSION.toShort())
        packet.putShort(type.toShort())
        packet.putInt(payloadSize)
    }

    private fun sendControlPacket(bytes: ByteArray, statusMessage: () -> String) {
        controlExecutor.execute {
            val client = controlSocket ?: run {
                Log.w(TAG, "send control skipped: control socket is not connected")
                return@execute
            }
            if (!running || client.isClosed) return@execute

            runCatching {
                synchronized(outputLock) {
                    client.getOutputStream().write(bytes)
                    client.getOutputStream().flush()
                }
                onStatus(statusMessage())
            }.onFailure { error ->
                Log.w(TAG, "send control failed: ${error.message}")
                onStatus("send control failed: ${error.message ?: error.javaClass.simpleName}")
            }
        }
    }

    private fun runReceiver() {
        var decoder: MediaCodecDecoder? = null
        val connectivityManager = context.getSystemService(
            ConnectivityManager::class.java
        )
        var boundToWifi = false

        try {
            val wifiNetwork = findWifiNetwork()
            if (wifiNetwork != null && connectivityManager != null) {
                boundToWifi = connectivityManager.bindProcessToNetwork(wifiNetwork)
                Log.i(TAG, "bindProcessToNetwork($wifiNetwork) result=$boundToWifi")
            } else {
                Log.w(TAG, "No Wi-Fi network found; using default socket route")
            }

            val client = Socket()

            socket = client
            client.tcpNoDelay = true
            Log.i(TAG, "Connecting to $host:$port")
            client.connect(InetSocketAddress(host, port), 5000)

            Log.i(TAG, "TCP connected to $host:$port")
            onStatus("Connected. Waiting for H.264...")
            connectControlSocket()

            decoder = MediaCodecDecoder(
                surface = surface,
                width = 2560,
                height = 1600,
                onStatus = onStatus
            )
            decoder.start()

            var nalCount = 0L
            var bytesTotal = 0L
            var lastByteLogTimeMs = 0L
            val parser = H264AnnexBParser { nal ->
                if (!running) return@H264AnnexBParser
                decoder.queueNal(nal)
                nalCount++
                val type = H264Nal.typeOf(nal)
                if (type == H264Nal.TYPE_SPS ||
                    type == H264Nal.TYPE_PPS ||
                    type == H264Nal.TYPE_IDR ||
                    nalCount <= 8L
                ) {
                    Log.d(
                        TAG,
                        "NAL #$nalCount ${H264Nal.typeName(type)} size=${nal.size}"
                    )
                }
                if (nalCount % 120L == 0L) {
                    onStatus("Decoding H.264 NAL units: $nalCount")
                }
            }

            val input = client.getInputStream()
            val buffer = ByteArray(64 * 1024)

            while (running) {
                val bytesRead = input.read(buffer)
                if (bytesRead < 0) break
                bytesTotal += bytesRead
                val now = System.currentTimeMillis()
                if (now - lastByteLogTimeMs >= 1000L) {
                    Log.d(TAG, "received bytes total=$bytesTotal last=$bytesRead")
                    lastByteLogTimeMs = now
                }
                parser.append(buffer, bytesRead)
            }

            parser.flush()
            Log.i(TAG, "Stream ended. bytes=$bytesTotal nal=$nalCount")
            onStatus("Stream ended.")
        } catch (error: Exception) {
            if (running) {
                Log.e(TAG, "Receiver error", error)
                onStatus("Receiver error: ${error.message ?: error.javaClass.simpleName}")
            }
        } finally {
            decoder?.close()
            controlSocket?.close()
            socket?.close()
            if (boundToWifi) {
                connectivityManager?.bindProcessToNetwork(null)
                Log.i(TAG, "Unbound process from Wi-Fi network")
            }
            socket = null
            controlSocket = null
            running = false
            onStopped()
        }
    }

    private fun connectControlSocket() {
        val controlPort = port + 1
        val client = Socket()
        client.tcpNoDelay = true
        Log.i(TAG, "Connecting control to $host:$controlPort")
        client.connect(InetSocketAddress(host, controlPort), 5000)
        controlSocket = client
        Log.i(TAG, "Control connected to $host:$controlPort")
        onStatus("Video + control connected.")
    }

    private fun findWifiNetwork(): Network? {
        val connectivityManager = context.getSystemService(
            ConnectivityManager::class.java
        ) ?: return null

        return connectivityManager.allNetworks.firstOrNull { network ->
            val capabilities = connectivityManager.getNetworkCapabilities(network)
            capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }
    }

    companion object {
        private const val TAG = "MintDeskReceiver"
        const val POINTER_MOVE = 0
        const val POINTER_DOWN = 1
        const val POINTER_UP = 2
        const val POINTER_RIGHT_DOWN = 3
        const val POINTER_RIGHT_UP = 4
        const val KEY_ACTION_PRESS = 0
        const val KEY_ACTION_DOWN = 1
        const val KEY_ACTION_UP = 2
        private const val PROTOCOL_VERSION = 1
        private const val TYPE_POINTER = 1
        private const val TYPE_WHEEL = 2
        private const val TYPE_TEXT = 3
        private const val TYPE_KEY = 4
        private const val TYPE_RELATIVE_MOUSE = 5
        private const val TYPE_MOUSE_BUTTON = 6
        private const val POINTER_PAYLOAD_SIZE = 12
        private const val WHEEL_PAYLOAD_SIZE = 12
        private const val TEXT_PAYLOAD_SIZE = 4
        private const val KEY_PAYLOAD_SIZE = 4
        private const val KEY_EVENT_PAYLOAD_SIZE = 8
        private const val RELATIVE_MOUSE_PAYLOAD_SIZE = 8
        private const val MOUSE_BUTTON_PAYLOAD_SIZE = 4
    }
}
