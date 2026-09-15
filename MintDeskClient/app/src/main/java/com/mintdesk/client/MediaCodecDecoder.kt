package com.mintdesk.client

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.BufferOverflowException

class MediaCodecDecoder(
    private val surface: Surface,
    private val width: Int,
    private val height: Int,
    private val onStatus: (String) -> Unit
) {
    private var codec: MediaCodec? = null
    private var ptsUs = 0L
    private var queuedCount = 0L
    private var renderedCount = 0L
    private var sawSps = false
    private var sawPps = false
    private var sawIdr = false

    fun start() {
        val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height)
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 2 * 1024 * 1024)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }

        codec = MediaCodec.createDecoderByType(MIME_TYPE).also { decoder ->
            Log.i(TAG, "Configuring MediaCodec decoder $MIME_TYPE ${width}x$height")
            decoder.configure(format, surface, null, 0)
            decoder.start()
        }

        Log.i(TAG, "MediaCodec decoder started")
        onStatus("MediaCodec decoder started.")
    }

    fun queueNal(nal: ByteArray) {
        val decoder = codec ?: return
        val nalType = H264Nal.typeOf(nal)

        drainOutput(decoder, 0)

        val inputIndex = decoder.dequeueInputBuffer(INPUT_TIMEOUT_US)
        if (inputIndex < 0) {
            Log.w(TAG, "No input buffer available for ${H264Nal.typeName(nalType)}")
            drainOutput(decoder, INPUT_TIMEOUT_US)
            return
        }

        val inputBuffer = decoder.getInputBuffer(inputIndex) ?: return
        inputBuffer.clear()

        try {
            inputBuffer.put(nal)
        } catch (error: BufferOverflowException) {
            Log.e(TAG, "NAL too large for codec input buffer size=${nal.size}", error)
            decoder.queueInputBuffer(inputIndex, 0, 0, ptsUs, 0)
            return
        }

        var flags = 0
        when (nalType) {
            H264Nal.TYPE_SPS -> {
                sawSps = true
                flags = MediaCodec.BUFFER_FLAG_CODEC_CONFIG
                Log.i(TAG, "SPS detected size=${nal.size}")
            }
            H264Nal.TYPE_PPS -> {
                sawPps = true
                flags = MediaCodec.BUFFER_FLAG_CODEC_CONFIG
                Log.i(TAG, "PPS detected size=${nal.size}")
            }
            H264Nal.TYPE_IDR -> {
                sawIdr = true
                flags = MediaCodec.BUFFER_FLAG_KEY_FRAME
                Log.i(TAG, "IDR detected size=${nal.size} sps=$sawSps pps=$sawPps")
            }
        }

        decoder.queueInputBuffer(
            inputIndex,
            0,
            nal.size,
            ptsUs,
            flags
        )
        ptsUs += FRAME_DURATION_US
        queuedCount++

        if (queuedCount <= 12L ||
            nalType == H264Nal.TYPE_SPS ||
            nalType == H264Nal.TYPE_PPS ||
            nalType == H264Nal.TYPE_IDR ||
            queuedCount % 120L == 0L
        ) {
            Log.d(
                TAG,
                "queued input #$queuedCount ${H264Nal.typeName(nalType)} size=${nal.size} flags=$flags pts=$ptsUs"
            )
        }

        drainOutput(decoder, 0)
    }

    fun close() {
        val decoder = codec ?: return
        codec = null

        runCatching { decoder.stop() }
        decoder.release()
        Log.i(TAG, "MediaCodec decoder released. queued=$queuedCount rendered=$renderedCount")
    }

    private fun drainOutput(decoder: MediaCodec, timeoutUs: Long) {
        val info = MediaCodec.BufferInfo()

        while (true) {
            when (val outputIndex = decoder.dequeueOutputBuffer(info, timeoutUs)) {
                MediaCodec.INFO_TRY_AGAIN_LATER -> return
                MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    onStatus("Decoder format: ${decoder.outputFormat}")
                }
                MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED -> Unit
                else -> {
                    if (outputIndex >= 0) {
                        renderedCount++
                        decoder.releaseOutputBuffer(
                            outputIndex,
                            info.size > 0
                        )
                        if (renderedCount <= 12L || renderedCount % 120L == 0L) {
                            Log.d(
                                TAG,
                                "rendered output #$renderedCount size=${info.size} flags=${info.flags} pts=${info.presentationTimeUs}"
                            )
                        }
                    }
                }
            }
        }
    }

    companion object {
        private const val TAG = "MintDeskCodec"
        private const val MIME_TYPE = "video/avc"
        private const val INPUT_TIMEOUT_US = 10_000L
        private const val FRAME_DURATION_US = 16_666L
    }
}
