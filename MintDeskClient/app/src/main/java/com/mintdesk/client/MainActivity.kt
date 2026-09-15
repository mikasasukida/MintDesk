package com.mintdesk.client

import android.app.Activity
import android.graphics.SurfaceTexture
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView
import android.view.InputDevice
import android.view.View
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast

class MainActivity : Activity() {
    private lateinit var videoView: TextureView
    private lateinit var videoContainer: FrameLayout
    private lateinit var remoteCursorView: RemoteCursorView
    private lateinit var controlBar: LinearLayout
    private lateinit var hostInput: EditText
    private lateinit var portInput: EditText
    private lateinit var connectButton: Button
    private lateinit var saveDeviceButton: Button
    private lateinit var gnsTestButton: Button
    private lateinit var mouseTestButton: Button
    private lateinit var keyboardButton: Button
    private lateinit var gameModeButton: Button
    private lateinit var keyboardInput: EditText
    private lateinit var statusText: TextView
    private lateinit var devicePanel: LinearLayout
    private lateinit var deviceCard: LinearLayout
    private lateinit var deviceNameText: TextView
    private lateinit var deviceAddressText: TextView
    private lateinit var deviceStatusText: TextView
    private lateinit var deviceConnectButton: Button

    private val mainHandler = Handler(Looper.getMainLooper())
    private var surface: Surface? = null
    private var receiver: H264SocketReceiver? = null
    private var videoTouchActive = false
    private var leftButtonDown = false
    private var mousePrimaryDown = false
    private var mouseSecondaryDown = false
    private var sawMouseButtonPress = false
    private var gameModeEnabled = false
    private var longPressSent = false
    private var lastScrollY = 0f
    private var keyboardTextChanging = false
    private var longPressRunnable: Runnable? = null
    private var remotePointerX = 0.5f
    private var remotePointerY = 0.5f
    private var touchDownX = 0f
    private var touchDownY = 0f
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var touchMoved = false
    private var hasLastGameMouse = false
    private var lastGameMouseX = 0f
    private var lastGameMouseY = 0f

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        videoView = findViewById(R.id.videoView)
        videoContainer = findViewById(R.id.videoContainer)
        remoteCursorView = findViewById(R.id.remoteCursorView)
        controlBar = findViewById(R.id.controlBar)
        hostInput = findViewById(R.id.hostInput)
        portInput = findViewById(R.id.portInput)
        connectButton = findViewById(R.id.connectButton)
        saveDeviceButton = findViewById(R.id.saveDeviceButton)
        gnsTestButton = findViewById(R.id.gnsTestButton)
        mouseTestButton = findViewById(R.id.mouseTestButton)
        keyboardButton = findViewById(R.id.keyboardButton)
        gameModeButton = findViewById(R.id.gameModeButton)
        keyboardInput = findViewById(R.id.keyboardInput)
        statusText = findViewById(R.id.statusText)
        devicePanel = findViewById(R.id.devicePanel)
        deviceCard = findViewById(R.id.deviceCard)
        deviceNameText = findViewById(R.id.deviceNameText)
        deviceAddressText = findViewById(R.id.deviceAddressText)
        deviceStatusText = findViewById(R.id.deviceStatusText)
        deviceConnectButton = findViewById(R.id.deviceConnectButton)

        loadSavedDevice()

        runCatching { NativeGns.selfTest() }
            .onSuccess { setStatus("Native GNS: $it") }
            .onFailure { setStatus("Native GNS failed: ${it.message}") }

        videoView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(
                surfaceTexture: SurfaceTexture,
                width: Int,
                height: Int
            ) {
                surface?.release()
                surface = Surface(surfaceTexture)
                adjustVideoLayout()
                setStatus("Video surface ready. Start MintDeskHost, then tap Connect.")
                connectButton.isEnabled = true
            }

            override fun onSurfaceTextureSizeChanged(
                surfaceTexture: SurfaceTexture,
                width: Int,
                height: Int
            ) {
                adjustVideoLayout()
            }

            override fun onSurfaceTextureDestroyed(surfaceTexture: SurfaceTexture): Boolean {
                stopReceiver()
                surface?.release()
                surface = null
                connectButton.isEnabled = false
                setStatus("Video surface destroyed.")
                return true
            }

            override fun onSurfaceTextureUpdated(surfaceTexture: SurfaceTexture) = Unit
        }

        connectButton.setOnClickListener {
            if (receiver == null) {
                startReceiver()
            } else {
                stopReceiver()
                setStatus("Disconnected.")
            }
        }

        saveDeviceButton.setOnClickListener {
            saveCurrentDevice(showToast = true)
            updateSavedDeviceUi()
        }

        deviceConnectButton.setOnClickListener {
            if (receiver == null) {
                startReceiver()
            }
        }

        deviceCard.setOnClickListener {
            if (receiver == null) {
                startReceiver()
            }
        }

        gnsTestButton.setOnClickListener {
            startGnsHelloTest()
        }

        mouseTestButton.setOnClickListener {
            sendMouseTest()
        }

        keyboardButton.setOnClickListener {
            showRemoteKeyboard()
        }

        gameModeButton.setOnClickListener { toggleGameMode() }

        keyboardInput.setOnKeyListener { _, _, keyEvent ->
            val activeReceiver = receiver ?: return@setOnKeyListener false
            handleRemoteKeyEvent(activeReceiver, keyEvent)
        }
        keyboardInput.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit

            override fun afterTextChanged(editable: Editable) {
                if (keyboardTextChanging || editable.isEmpty()) {
                    return
                }

                val text = editable.toString()
                keyboardTextChanging = true
                editable.clear()
                keyboardTextChanging = false
                sendRemoteText(text)
            }
        })

        videoContainer.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            adjustVideoLayout()
        }

        videoContainer.setOnCapturedPointerListener { _, event ->
            val activeReceiver = receiver ?: return@setOnCapturedPointerListener false
            if (!gameModeEnabled || !isMouseLikeEvent(event)) {
                return@setOnCapturedPointerListener false
            }
            handleGameMouseEvent(event, activeReceiver)
        }
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        val activeReceiver = receiver

        if (activeReceiver != null && ::videoContainer.isInitialized) {
            if (gameModeEnabled) {
                if (isMouseLikeEvent(event)) {
                    if (handleGameMouseEvent(event, activeReceiver)) {
                        return true
                    }
                } else if (handleGameTouchEvent(event, activeReceiver)) {
                    return true
                }
            } else {
                if (isMouseLikeEvent(event) && handleMouseEvent(event, activeReceiver)) {
                    return true
                }
            }

            if (event.pointerCount >= 2) {
                return handleTwoFingerScroll(event, activeReceiver)
            }

            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    videoTouchActive = isRawPointInsideVideoContainer(event.rawX, event.rawY)
                    if (videoTouchActive) {
                        touchDownX = event.rawX
                        touchDownY = event.rawY
                        lastTouchX = event.rawX
                        lastTouchY = event.rawY
                        touchMoved = false
                        leftButtonDown = false
                        longPressSent = false
                        sendDirectTouch(event, H264SocketReceiver.POINTER_MOVE, activeReceiver)
                        scheduleRightClick(event.rawX, event.rawY, activeReceiver)
                        return true
                    }
                }
                MotionEvent.ACTION_MOVE -> {
                    if (videoTouchActive) {
                        val dx = event.rawX - lastTouchX
                        val dy = event.rawY - lastTouchY
                        val totalDx = event.rawX - touchDownX
                        val totalDy = event.rawY - touchDownY
                        if (kotlin.math.hypot(totalDx, totalDy) > TOUCH_TAP_SLOP_PX) {
                            touchMoved = true
                            cancelRightClick()
                            if (!leftButtonDown && !longPressSent) {
                                sendDirectTouch(
                                    event,
                                    H264SocketReceiver.POINTER_DOWN,
                                    activeReceiver
                                )
                                leftButtonDown = true
                            }
                        }
                        lastTouchX = event.rawX
                        lastTouchY = event.rawY
                        sendDirectTouch(event, H264SocketReceiver.POINTER_MOVE, activeReceiver)
                        return true
                    }
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    if (videoTouchActive) {
                        cancelRightClick()
                        if (event.actionMasked == MotionEvent.ACTION_UP) {
                            when {
                                leftButtonDown -> {
                                    sendDirectTouch(
                                        event,
                                        H264SocketReceiver.POINTER_UP,
                                        activeReceiver
                                    )
                                }
                                !longPressSent -> {
                                    sendDirectTouch(
                                        event,
                                        H264SocketReceiver.POINTER_DOWN,
                                        activeReceiver
                                    )
                                    sendDirectTouch(
                                        event,
                                        H264SocketReceiver.POINTER_UP,
                                        activeReceiver
                                    )
                                }
                            }
                        }
                        videoTouchActive = false
                        leftButtonDown = false
                        return true
                    }
                }
            }
        }

        return super.dispatchTouchEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val activeReceiver = receiver

        if (gameModeEnabled && activeReceiver != null && isMouseLikeEvent(event)) {
            if (handleGameMouseEvent(event, activeReceiver)) {
                return true
            }
        }

        if (activeReceiver != null &&
            ::videoContainer.isInitialized &&
            isMouseLikeEvent(event) &&
            isRawPointInsideVideoContainer(event.rawX, event.rawY)
        ) {
            if (handleMouseEvent(event, activeReceiver)) {
                return true
            }

            when (event.actionMasked) {
                MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_MOVE -> {
                    sendVideoTouch(event, H264SocketReceiver.POINTER_MOVE, activeReceiver)
                    return true
                }
            }
        }

        return super.dispatchGenericMotionEvent(event)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val activeReceiver = receiver

        if (activeReceiver != null && !isEditingConnectionFields()) {
            if (event.keyCode == KeyEvent.KEYCODE_FORWARD_DEL) {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    toggleGameMode()
                }
                return true
            }

            if (handleRemoteKeyEvent(activeReceiver, event)) {
                return true
            }
        }

        return super.dispatchKeyEvent(event)
    }

    override fun onBackPressed() {
        if (receiver != null) {
            stopReceiver()
            setStatus("Disconnected.")
            return
        }

        super.onBackPressed()
    }

    override fun onDestroy() {
        stopReceiver()
        super.onDestroy()
    }

    private fun startReceiver() {
        val targetSurface = surface
        if (targetSurface == null) {
            setStatus("Surface is not ready yet.")
            return
        }

        val host = hostInput.text.toString().trim()
        val port = portInput.text.toString().trim().toIntOrNull()

        if (host.isEmpty() || port == null || port !in 1..65535) {
            setStatus("Enter a valid host and port.")
            return
        }

        saveCurrentDevice(showToast = false)
        updateSavedDeviceUi()

        receiver = H264SocketReceiver(
            context = applicationContext,
            host = host,
            port = port,
            surface = targetSurface,
            onStatus = ::setStatus,
            onStopped = ::onReceiverStopped
        ).also { it.start() }

        connectButton.text = getString(R.string.disconnect)
        Log.i(TAG, "Starting receiver for $host:$port")
        setStatus("Connecting to $host:$port ...")
        enterRemoteMode()
    }

    private fun loadSavedDevice() {
        val preferences = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val host = preferences.getString(PREF_HOST, null)
        val port = preferences.getInt(PREF_PORT, 9000)

        if (!host.isNullOrBlank()) {
            hostInput.setText(host)
            portInput.setText(port.toString())
        }

        updateSavedDeviceUi()
    }

    private fun saveCurrentDevice(showToast: Boolean) {
        val host = hostInput.text.toString().trim()
        val port = portInput.text.toString().trim().toIntOrNull()

        if (host.isEmpty() || port == null || port !in 1..65535) {
            setStatus("Enter a valid host and port before saving.")
            return
        }

        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putString(PREF_HOST, host)
            .putInt(PREF_PORT, port)
            .putLong(PREF_LAST_SAVED_AT, System.currentTimeMillis())
            .apply()

        if (showToast) {
            Toast.makeText(this, "Device saved", Toast.LENGTH_SHORT).show()
        }
    }

    private fun updateSavedDeviceUi() {
        val preferences = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val host = preferences.getString(PREF_HOST, null)
        val port = preferences.getInt(PREF_PORT, 9000)

        if (host.isNullOrBlank()) {
            deviceNameText.text = getString(R.string.default_device_name)
            deviceAddressText.text = "No saved host"
            deviceStatusText.text = getString(R.string.device_status_empty)
            deviceConnectButton.isEnabled = false
            deviceCard.alpha = 0.65f
            return
        }

        deviceNameText.text = getString(R.string.default_device_name)
        deviceAddressText.text = "$host:$port"
        deviceStatusText.text = getString(R.string.device_status_saved)
        deviceConnectButton.isEnabled = true
        deviceCard.alpha = 1.0f
    }

    private fun stopReceiver() {
        Log.i(TAG, "Stopping receiver")
        receiver?.stop()
        receiver = null
        videoTouchActive = false
        leftButtonDown = false
        mousePrimaryDown = false
        mouseSecondaryDown = false
        sawMouseButtonPress = false
        longPressSent = false
        cancelRightClick()
        if (::remoteCursorView.isInitialized) {
        remoteCursorView.hideCursor()
        }
        if (::connectButton.isInitialized) {
            connectButton.text = getString(R.string.connect)
        }
        exitRemoteMode()
    }

    private fun startGnsHelloTest() {
        val host = hostInput.text.toString().trim()
        val port = portInput.text.toString().trim().toIntOrNull()

        if (host.isEmpty() || port == null || port !in 1..65535) {
            setStatus("Enter a valid GNS host and port.")
            return
        }

        gnsTestButton.isEnabled = false
        setStatus("GNS connecting to $host:$port ...")

        Thread {
            val result = runCatching { NativeGns.clientHello(host, port) }
                .getOrElse { "GNS failed: ${it.message}" }
            Log.i(TAG, "GNS result:\n$result")
            runOnUiThread {
                statusText.text = result
                gnsTestButton.isEnabled = true
            }
        }.start()
    }

    private fun sendMouseTest() {
        val activeReceiver = receiver
        if (activeReceiver == null) {
            setStatus("Connect before Mouse Test.")
            return
        }

        setStatus("Sending mouse test to center.")
        activeReceiver.sendPointer(H264SocketReceiver.POINTER_MOVE, 0.5f, 0.5f)
        Thread {
            activeReceiver.sendPointer(H264SocketReceiver.POINTER_DOWN, 0.5f, 0.5f)
            Thread.sleep(80)
            activeReceiver.sendPointer(H264SocketReceiver.POINTER_UP, 0.5f, 0.5f)
        }.start()
    }

    private fun setStatus(message: String) {
        Log.i(TAG, "status: $message")
        runOnUiThread {
            statusText.text = message
        }
    }

    private fun onReceiverStopped() {
        runOnUiThread {
            receiver = null
            videoTouchActive = false
            connectButton.text = getString(R.string.connect)
            exitRemoteMode()
        }
    }

    private fun enterRemoteMode() {
        hostInput.clearFocus()
        portInput.clearFocus()
        keyboardInput.clearFocus()
        videoContainer.requestFocus()

        getSystemService(InputMethodManager::class.java)?.hideSoftInputFromWindow(
            videoContainer.windowToken,
            0
        )

        controlBar.visibility = View.GONE
        devicePanel.visibility = View.GONE
        keyboardInput.visibility = View.GONE
        statusText.visibility = View.GONE
        window.decorView.systemUiVisibility = FULLSCREEN_FLAGS
        applyPointerCapture()
    }

    private fun exitRemoteMode() {
        if (!::controlBar.isInitialized) {
            return
        }

        controlBar.visibility = View.VISIBLE
        devicePanel.visibility = View.VISIBLE
        keyboardInput.visibility = View.VISIBLE
        statusText.visibility = View.VISIBLE
        window.decorView.systemUiVisibility = 0
        releasePointerCapture()
    }

    private fun updateGameModeUi() {
        gameModeButton.text = getString(
            if (gameModeEnabled) {
                R.string.game_mode_on
            } else {
                R.string.game_mode_off
            }
        )
    }

    private fun toggleGameMode() {
        gameModeEnabled = !gameModeEnabled
        hasLastGameMouse = false
        updateGameModeUi()

        if (receiver != null) {
            if (gameModeEnabled) {
                applyPointerCapture()
            } else {
                releasePointerCapture()
            }
        }

        Toast.makeText(
            this,
            if (gameModeEnabled) "Game Mode On" else "Game Mode Off",
            Toast.LENGTH_SHORT
        ).show()
    }

    private fun applyPointerCapture() {
        if (!gameModeEnabled || receiver == null) {
            releasePointerCapture()
            return
        }

        hasLastGameMouse = false
        videoContainer.requestFocus()
        videoContainer.requestPointerCapture()
        remoteCursorView.hideCursor()
    }

    private fun releasePointerCapture() {
        hasLastGameMouse = false
        if (::videoContainer.isInitialized && videoContainer.hasPointerCapture()) {
            videoContainer.releasePointerCapture()
        }
    }

    private fun sendVideoTouch(
        event: MotionEvent,
        action: Int,
        activeReceiver: H264SocketReceiver
    ) {
        val surfaceLocation = IntArray(2)
        videoContainer.getLocationOnScreen(surfaceLocation)
        val localX = event.rawX - surfaceLocation[0]
        val localY = event.rawY - surfaceLocation[1]
        val mapped = mapTouchToVideo(
            localX,
            localY,
            videoContainer.width,
            videoContainer.height
        ) ?: return

        setStatus("Input ${"%.3f".format(mapped.first)}, ${"%.3f".format(mapped.second)}")
        Log.d(
            TAG,
            "dispatch touch action=$action local=$localX,$localY mapped=${mapped.first},${mapped.second}"
        )
        activeReceiver.sendPointer(action, mapped.first, mapped.second)
    }

    private fun handleMouseEvent(
        event: MotionEvent,
        activeReceiver: H264SocketReceiver
    ): Boolean {
        if (!isRawPointInsideVideoContainer(event.rawX, event.rawY)) {
            return false
        }

        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_MOVE -> {
                sendDirectMouse(event, H264SocketReceiver.POINTER_MOVE, activeReceiver)
                return true
            }
            MotionEvent.ACTION_SCROLL -> {
                val scroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                if (scroll != 0f) {
                    val mapped = mapRawPointToVideo(event.rawX, event.rawY) ?: return true
                    val delta = (scroll * WHEEL_DELTA).toInt()
                        .coerceIn(-WHEEL_DELTA * 8, WHEEL_DELTA * 8)
                    activeReceiver.sendWheel(delta, mapped.first, mapped.second)
                    setStatus("Mouse wheel $delta")
                }
                return true
            }
            MotionEvent.ACTION_BUTTON_PRESS -> {
                val action = when (event.actionButton) {
                    MotionEvent.BUTTON_PRIMARY -> H264SocketReceiver.POINTER_DOWN
                    MotionEvent.BUTTON_SECONDARY -> H264SocketReceiver.POINTER_RIGHT_DOWN
                    else -> 0
                }
                if (action != 0) {
                    sawMouseButtonPress = true
                    if (action == H264SocketReceiver.POINTER_DOWN) {
                        if (mousePrimaryDown) return true
                        mousePrimaryDown = true
                    } else if (action == H264SocketReceiver.POINTER_RIGHT_DOWN) {
                        if (mouseSecondaryDown) return true
                        mouseSecondaryDown = true
                    }
                    sendDirectMouse(event, action, activeReceiver)
                    return true
                }
            }
            MotionEvent.ACTION_BUTTON_RELEASE -> {
                val action = when (event.actionButton) {
                    MotionEvent.BUTTON_PRIMARY -> H264SocketReceiver.POINTER_UP
                    MotionEvent.BUTTON_SECONDARY -> H264SocketReceiver.POINTER_RIGHT_UP
                    else -> 0
                }
                if (action != 0) {
                    sawMouseButtonPress = true
                    if (action == H264SocketReceiver.POINTER_UP) {
                        if (!mousePrimaryDown) return true
                        mousePrimaryDown = false
                    } else if (action == H264SocketReceiver.POINTER_RIGHT_UP) {
                        if (!mouseSecondaryDown) return true
                        mouseSecondaryDown = false
                    }
                    sendDirectMouse(event, action, activeReceiver)
                    return true
                }
            }
            MotionEvent.ACTION_DOWN -> {
                if (sawMouseButtonPress) {
                    return true
                }
                val action = if ((event.buttonState and MotionEvent.BUTTON_SECONDARY) != 0) {
                    H264SocketReceiver.POINTER_RIGHT_DOWN
                } else {
                    H264SocketReceiver.POINTER_DOWN
                }
                if (action == H264SocketReceiver.POINTER_DOWN) {
                    if (mousePrimaryDown) return true
                    mousePrimaryDown = true
                } else if (action == H264SocketReceiver.POINTER_RIGHT_DOWN) {
                    if (mouseSecondaryDown) return true
                    mouseSecondaryDown = true
                }
                sendDirectMouse(event, action, activeReceiver)
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (sawMouseButtonPress) {
                    sawMouseButtonPress = false
                    return true
                }
                val action = when {
                    mouseSecondaryDown -> H264SocketReceiver.POINTER_RIGHT_UP
                    mousePrimaryDown -> H264SocketReceiver.POINTER_UP
                    else -> H264SocketReceiver.POINTER_UP
                }
                if (action == H264SocketReceiver.POINTER_UP) {
                    mousePrimaryDown = false
                } else if (action == H264SocketReceiver.POINTER_RIGHT_UP) {
                    mouseSecondaryDown = false
                }
                sendDirectMouse(event, action, activeReceiver)
                return true
            }
        }

        return false
    }

    private fun handleGameMouseEvent(
        event: MotionEvent,
        activeReceiver: H264SocketReceiver
    ): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_MOVE -> {
                var dx = event.getAxisValue(MotionEvent.AXIS_RELATIVE_X)
                var dy = event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y)

                if (dx == 0f && dy == 0f) {
                    if (hasLastGameMouse) {
                        dx = event.x - lastGameMouseX
                        dy = event.y - lastGameMouseY
                    }
                    lastGameMouseX = event.x
                    lastGameMouseY = event.y
                    hasLastGameMouse = true
                }

                if (dx != 0f || dy != 0f) {
                    activeReceiver.sendRelativeMouse(
                        dx * GAME_MOUSE_SENSITIVITY,
                        dy * GAME_MOUSE_SENSITIVITY
                    )
                }
                return true
            }
            MotionEvent.ACTION_SCROLL -> {
                val scroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                if (scroll != 0f) {
                    val delta = (scroll * WHEEL_DELTA).toInt()
                        .coerceIn(-WHEEL_DELTA * 8, WHEEL_DELTA * 8)
                    activeReceiver.sendWheel(delta, 0.5f, 0.5f)
                }
                return true
            }
            MotionEvent.ACTION_DOWN -> {
                hasLastGameMouse = false
                val action = if ((event.buttonState and MotionEvent.BUTTON_SECONDARY) != 0) {
                    H264SocketReceiver.POINTER_RIGHT_DOWN
                } else {
                    H264SocketReceiver.POINTER_DOWN
                }
                if (action == H264SocketReceiver.POINTER_RIGHT_DOWN) {
                    mouseSecondaryDown = true
                } else {
                    mousePrimaryDown = true
                }
                activeReceiver.sendMouseButton(action)
                return true
            }
            MotionEvent.ACTION_UP -> {
                val action = if (mouseSecondaryDown) {
                    H264SocketReceiver.POINTER_RIGHT_UP
                } else {
                    H264SocketReceiver.POINTER_UP
                }
                mousePrimaryDown = false
                mouseSecondaryDown = false
                hasLastGameMouse = false
                activeReceiver.sendMouseButton(action)
                return true
            }
            MotionEvent.ACTION_BUTTON_PRESS -> {
                val action = when (event.actionButton) {
                    MotionEvent.BUTTON_PRIMARY -> H264SocketReceiver.POINTER_DOWN
                    MotionEvent.BUTTON_SECONDARY -> H264SocketReceiver.POINTER_RIGHT_DOWN
                    else -> 0
                }
                if (action != 0) {
                    if (action == H264SocketReceiver.POINTER_DOWN) {
                        mousePrimaryDown = true
                    } else if (action == H264SocketReceiver.POINTER_RIGHT_DOWN) {
                        mouseSecondaryDown = true
                    }
                    activeReceiver.sendMouseButton(action)
                    return true
                }
            }
            MotionEvent.ACTION_BUTTON_RELEASE -> {
                val action = when (event.actionButton) {
                    MotionEvent.BUTTON_PRIMARY -> H264SocketReceiver.POINTER_UP
                    MotionEvent.BUTTON_SECONDARY -> H264SocketReceiver.POINTER_RIGHT_UP
                    else -> 0
                }
                if (action != 0) {
                    if (action == H264SocketReceiver.POINTER_UP) {
                        mousePrimaryDown = false
                    } else if (action == H264SocketReceiver.POINTER_RIGHT_UP) {
                        mouseSecondaryDown = false
                    }
                    activeReceiver.sendMouseButton(action)
                    return true
                }
            }
        }

        return false
    }

    private fun handleGameTouchEvent(
        event: MotionEvent,
        activeReceiver: H264SocketReceiver
    ): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                videoTouchActive = isRawPointInsideVideoContainer(event.rawX, event.rawY)
                if (!videoTouchActive) {
                    return false
                }
                touchDownX = event.rawX
                touchDownY = event.rawY
                lastTouchX = event.rawX
                lastTouchY = event.rawY
                touchMoved = false
                cancelRightClick()
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (!videoTouchActive) {
                    return false
                }

                val dx = event.rawX - lastTouchX
                val dy = event.rawY - lastTouchY
                val totalDx = event.rawX - touchDownX
                val totalDy = event.rawY - touchDownY
                if (kotlin.math.hypot(totalDx, totalDy) > TOUCH_TAP_SLOP_PX) {
                    touchMoved = true
                }
                lastTouchX = event.rawX
                lastTouchY = event.rawY

                if (dx != 0f || dy != 0f) {
                    activeReceiver.sendRelativeMouse(
                        dx * GAME_TOUCH_SENSITIVITY,
                        dy * GAME_TOUCH_SENSITIVITY
                    )
                }
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (!videoTouchActive) {
                    return false
                }

                if (!touchMoved) {
                    activeReceiver.sendMouseButton(H264SocketReceiver.POINTER_DOWN)
                    activeReceiver.sendMouseButton(H264SocketReceiver.POINTER_UP)
                }
                videoTouchActive = false
                touchMoved = false
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                videoTouchActive = false
                touchMoved = false
                return true
            }
        }

        return false
    }

    private fun sendDirectMouse(
        event: MotionEvent,
        action: Int,
        activeReceiver: H264SocketReceiver
    ) {
        val mapped = mapRawPointToVideo(event.rawX, event.rawY) ?: return
        remotePointerX = mapped.first
        remotePointerY = mapped.second
        updateRemoteCursor(event.rawX, event.rawY)
        activeReceiver.sendPointer(action, mapped.first, mapped.second)
    }

    private fun sendDirectTouch(
        event: MotionEvent,
        action: Int,
        activeReceiver: H264SocketReceiver
    ) {
        val mapped = mapRawPointToVideo(event.rawX, event.rawY) ?: return

        remotePointerX = mapped.first
        remotePointerY = mapped.second

        updateRemoteCursor(event.rawX, event.rawY)

        val actionName = when (action) {
            H264SocketReceiver.POINTER_DOWN -> "down"
            H264SocketReceiver.POINTER_UP -> "up"
            H264SocketReceiver.POINTER_MOVE -> "move"
            else -> "unknown"
        }
        setStatus(
            "Direct $actionName ${"%.3f".format(mapped.first)}, ${"%.3f".format(mapped.second)}"
        )
        activeReceiver.sendPointer(action, mapped.first, mapped.second)
    }

    private fun updateRemoteCursor(rawX: Float, rawY: Float) {
        remoteCursorView.hideCursor()
    }

    private fun scheduleRightClick(rawX: Float, rawY: Float, activeReceiver: H264SocketReceiver) {
        cancelRightClick()
        longPressRunnable = Runnable {
            if (!videoTouchActive || touchMoved || leftButtonDown) {
                return@Runnable
            }

            val mapped = mapRawPointToVideo(rawX, rawY) ?: return@Runnable
            remotePointerX = mapped.first
            remotePointerY = mapped.second
            longPressSent = true
            activeReceiver.sendPointer(
                H264SocketReceiver.POINTER_MOVE,
                mapped.first,
                mapped.second
            )
            activeReceiver.sendPointer(
                H264SocketReceiver.POINTER_RIGHT_DOWN,
                mapped.first,
                mapped.second
            )
            activeReceiver.sendPointer(
                H264SocketReceiver.POINTER_RIGHT_UP,
                mapped.first,
                mapped.second
            )
            setStatus("Right click ${"%.3f".format(mapped.first)}, ${"%.3f".format(mapped.second)}")
        }
        mainHandler.postDelayed(longPressRunnable!!, LONG_PRESS_MS)
    }

    private fun cancelRightClick() {
        longPressRunnable?.let(mainHandler::removeCallbacks)
        longPressRunnable = null
    }

    private fun handleTwoFingerScroll(
        event: MotionEvent,
        activeReceiver: H264SocketReceiver
    ): Boolean {
        val midRawX = (event.getRawXCompat(0) + event.getRawXCompat(1)) / 2f
        val midRawY = (event.getRawYCompat(0) + event.getRawYCompat(1)) / 2f

        if (!isRawPointInsideVideoContainer(midRawX, midRawY)) {
            return false
        }

        when (event.actionMasked) {
            MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_DOWN -> {
                videoTouchActive = false
                leftButtonDown = false
                cancelRightClick()
                lastScrollY = midRawY
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dy = midRawY - lastScrollY
                if (kotlin.math.abs(dy) >= SCROLL_STEP_PX) {
                    val mapped = mapRawPointToVideo(midRawX, midRawY) ?: return true
                    val steps = (dy / SCROLL_STEP_PX).toInt()
                    val delta = (-steps * WHEEL_DELTA).coerceIn(-WHEEL_DELTA * 8, WHEEL_DELTA * 8)
                    lastScrollY = midRawY
                    activeReceiver.sendWheel(delta, mapped.first, mapped.second)
                    setStatus("Wheel $delta")
                }
                return true
            }
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                lastScrollY = 0f
                return true
            }
        }

        return true
    }

    private fun sendTouchpadMove(
        dx: Float,
        dy: Float,
        activeReceiver: H264SocketReceiver
    ) {
        val width = videoContainer.width.takeIf { it > 0 } ?: return
        val height = videoContainer.height.takeIf { it > 0 } ?: return

        remotePointerX = (remotePointerX + dx / width * TOUCHPAD_SENSITIVITY).coerceIn(0f, 1f)
        remotePointerY = (remotePointerY + dy / height * TOUCHPAD_SENSITIVITY).coerceIn(0f, 1f)

        remoteCursorView.updatePosition(
            remotePointerX * width,
            remotePointerY * height
        )
        setStatus("Touchpad ${"%.3f".format(remotePointerX)}, ${"%.3f".format(remotePointerY)}")
        activeReceiver.sendPointer(
            H264SocketReceiver.POINTER_MOVE,
            remotePointerX,
            remotePointerY
        )
    }

    private fun isRawPointInsideVideoContainer(rawX: Float, rawY: Float): Boolean {
        if (!::videoView.isInitialized ||
            videoView.width <= 0 ||
            videoView.height <= 0
        ) {
            return false
        }

        val videoLocation = IntArray(2)
        videoView.getLocationOnScreen(videoLocation)
        return rawX >= videoLocation[0] &&
            rawY >= videoLocation[1] &&
            rawX <= videoLocation[0] + videoView.width &&
            rawY <= videoLocation[1] + videoView.height
    }

    private fun isMouseLikeEvent(event: MotionEvent): Boolean {
        return event.isFromSource(InputDevice.SOURCE_MOUSE) ||
            event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE) ||
            event.isFromSource(InputDevice.SOURCE_TOUCHPAD) ||
            event.isFromSource(InputDevice.SOURCE_STYLUS)
    }

    private fun mapTouchToVideo(
        touchX: Float,
        touchY: Float,
        viewWidth: Int,
        viewHeight: Int
    ): Pair<Float, Float>? {
        if (viewWidth <= 0 || viewHeight <= 0) return null

        val scale = minOf(
            viewWidth.toFloat() / VIDEO_WIDTH,
            viewHeight.toFloat() / VIDEO_HEIGHT
        )
        val videoWidthOnScreen = VIDEO_WIDTH * scale
        val videoHeightOnScreen = VIDEO_HEIGHT * scale
        val offsetX = (viewWidth - videoWidthOnScreen) / 2f
        val offsetY = (viewHeight - videoHeightOnScreen) / 2f

        val videoX = touchX - offsetX
        val videoY = touchY - offsetY

        if (videoX < 0f ||
            videoY < 0f ||
            videoX > videoWidthOnScreen ||
            videoY > videoHeightOnScreen
        ) {
            return null
        }

        return Pair(
            (videoX / videoWidthOnScreen).coerceIn(0f, 1f),
            (videoY / videoHeightOnScreen).coerceIn(0f, 1f)
        )
    }

    private fun showRemoteKeyboard() {
        if (receiver == null) {
            setStatus("Connect before Keyboard.")
            return
        }

        keyboardInput.requestFocus()
        val inputMethodManager = getSystemService(InputMethodManager::class.java)
        inputMethodManager?.showSoftInput(keyboardInput, InputMethodManager.SHOW_IMPLICIT)
        setStatus("Keyboard ready.")
    }

    private fun isEditingConnectionFields(): Boolean {
        return hostInput.hasFocus() || portInput.hasFocus()
    }

    private fun handleRemoteKeyEvent(
        activeReceiver: H264SocketReceiver,
        event: KeyEvent
    ): Boolean {
        val virtualKey = androidKeyToWindowsVirtualKey(event.keyCode)

        if (virtualKey == 0) {
            return false
        }

        when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                if (event.repeatCount == 0) {
                    activeReceiver.sendKeyEvent(
                        H264SocketReceiver.KEY_ACTION_DOWN,
                        virtualKey
                    )
                }
                return true
            }
            KeyEvent.ACTION_UP -> {
                activeReceiver.sendKeyEvent(
                    H264SocketReceiver.KEY_ACTION_UP,
                    virtualKey
                )
                return true
            }
        }

        return false
    }

    private fun androidKeyToWindowsVirtualKey(keyCode: Int): Int {
        if (keyCode in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z) {
            return VK_A + (keyCode - KeyEvent.KEYCODE_A)
        }

        if (keyCode in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9) {
            return VK_0 + (keyCode - KeyEvent.KEYCODE_0)
        }

        return when (keyCode) {
            KeyEvent.KEYCODE_DEL -> VK_BACK
            KeyEvent.KEYCODE_ENTER -> VK_RETURN
            KeyEvent.KEYCODE_TAB -> VK_TAB
            KeyEvent.KEYCODE_ESCAPE -> VK_ESCAPE
            KeyEvent.KEYCODE_SHIFT_LEFT -> VK_LSHIFT
            KeyEvent.KEYCODE_SHIFT_RIGHT -> VK_RSHIFT
            KeyEvent.KEYCODE_CTRL_LEFT -> VK_LCONTROL
            KeyEvent.KEYCODE_CTRL_RIGHT -> VK_RCONTROL
            KeyEvent.KEYCODE_ALT_LEFT -> VK_LMENU
            KeyEvent.KEYCODE_ALT_RIGHT -> VK_RMENU
            KeyEvent.KEYCODE_META_LEFT -> VK_LWIN
            KeyEvent.KEYCODE_META_RIGHT -> VK_RWIN
            KeyEvent.KEYCODE_FORWARD_DEL -> VK_DELETE
            KeyEvent.KEYCODE_DPAD_LEFT -> VK_LEFT
            KeyEvent.KEYCODE_DPAD_UP -> VK_UP
            KeyEvent.KEYCODE_DPAD_RIGHT -> VK_RIGHT
            KeyEvent.KEYCODE_DPAD_DOWN -> VK_DOWN
            KeyEvent.KEYCODE_MOVE_HOME -> VK_HOME
            KeyEvent.KEYCODE_MOVE_END -> VK_END
            KeyEvent.KEYCODE_PAGE_UP -> VK_PRIOR
            KeyEvent.KEYCODE_PAGE_DOWN -> VK_NEXT
            KeyEvent.KEYCODE_INSERT -> VK_INSERT
            KeyEvent.KEYCODE_SPACE -> VK_SPACE
            KeyEvent.KEYCODE_F1 -> VK_F1
            KeyEvent.KEYCODE_F2 -> VK_F2
            KeyEvent.KEYCODE_F3 -> VK_F3
            KeyEvent.KEYCODE_F4 -> VK_F4
            KeyEvent.KEYCODE_F5 -> VK_F5
            KeyEvent.KEYCODE_F6 -> VK_F6
            KeyEvent.KEYCODE_F7 -> VK_F7
            KeyEvent.KEYCODE_F8 -> VK_F8
            KeyEvent.KEYCODE_F9 -> VK_F9
            KeyEvent.KEYCODE_F10 -> VK_F10
            KeyEvent.KEYCODE_F11 -> VK_F11
            KeyEvent.KEYCODE_F12 -> VK_F12
            KeyEvent.KEYCODE_MINUS -> VK_OEM_MINUS
            KeyEvent.KEYCODE_EQUALS -> VK_OEM_PLUS
            KeyEvent.KEYCODE_LEFT_BRACKET -> VK_OEM_4
            KeyEvent.KEYCODE_RIGHT_BRACKET -> VK_OEM_6
            KeyEvent.KEYCODE_BACKSLASH -> VK_OEM_5
            KeyEvent.KEYCODE_SEMICOLON -> VK_OEM_1
            KeyEvent.KEYCODE_APOSTROPHE -> VK_OEM_7
            KeyEvent.KEYCODE_COMMA -> VK_OEM_COMMA
            KeyEvent.KEYCODE_PERIOD -> VK_OEM_PERIOD
            KeyEvent.KEYCODE_SLASH -> VK_OEM_2
            KeyEvent.KEYCODE_GRAVE -> VK_OEM_3
            else -> 0
        }
    }

    private fun sendRemoteText(text: String) {
        val activeReceiver = receiver ?: return
        var index = 0

        while (index < text.length) {
            val codePoint = text.codePointAt(index)
            when (codePoint) {
                '\n'.code -> activeReceiver.sendKey(VK_RETURN)
                '\t'.code -> activeReceiver.sendKey(VK_TAB)
                else -> activeReceiver.sendText(codePoint)
            }
            index += Character.charCount(codePoint)
        }
    }

    private fun MotionEvent.getRawXCompat(pointerIndex: Int): Float {
        return rawX + getX(pointerIndex) - getX(0)
    }

    private fun MotionEvent.getRawYCompat(pointerIndex: Int): Float {
        return rawY + getY(pointerIndex) - getY(0)
    }

    private fun mapRawPointToVideo(rawX: Float, rawY: Float): Pair<Float, Float>? {
        if (!::videoView.isInitialized || videoView.width <= 0 || videoView.height <= 0) {
            return null
        }

        val videoLocation = IntArray(2)
        videoView.getLocationOnScreen(videoLocation)
        val localX = rawX - videoLocation[0]
        val localY = rawY - videoLocation[1]

        if (localX < 0f ||
            localY < 0f ||
            localX > videoView.width ||
            localY > videoView.height
        ) {
            return null
        }

        return Pair(
            (localX / videoView.width).coerceIn(0f, 1f),
            (localY / videoView.height).coerceIn(0f, 1f)
        )
    }

    private fun adjustVideoLayout() {
        if (!::videoContainer.isInitialized || !::videoView.isInitialized) return

        val containerWidth = videoContainer.width
        val containerHeight = videoContainer.height

        if (containerWidth <= 0 || containerHeight <= 0) return

        val scale = minOf(
            containerWidth.toFloat() / VIDEO_WIDTH,
            containerHeight.toFloat() / VIDEO_HEIGHT
        )
        val targetWidth = (VIDEO_WIDTH * scale).toInt().coerceAtLeast(1)
        val targetHeight = (VIDEO_HEIGHT * scale).toInt().coerceAtLeast(1)
        val params = videoView.layoutParams as FrameLayout.LayoutParams

        if (params.width == targetWidth && params.height == targetHeight) {
            return
        }

        params.width = targetWidth
        params.height = targetHeight
        params.gravity = android.view.Gravity.CENTER
        videoView.layoutParams = params
        Log.i(TAG, "Video layout ${targetWidth}x$targetHeight in ${containerWidth}x$containerHeight")
    }

    companion object {
        private const val TAG = "MintDeskActivity"
        private const val PREFS_NAME = "mintdesk_devices"
        private const val PREF_HOST = "host"
        private const val PREF_PORT = "port"
        private const val PREF_LAST_SAVED_AT = "last_saved_at"
        private const val VIDEO_WIDTH = 2560f
        private const val VIDEO_HEIGHT = 1600f
        private const val TOUCHPAD_SENSITIVITY = 1.35f
        private const val TOUCH_TAP_SLOP_PX = 12f
        private const val LONG_PRESS_MS = 550L
        private const val SCROLL_STEP_PX = 24f
        private const val WHEEL_DELTA = 120
        private const val GAME_MOUSE_SENSITIVITY = 1.35f
        private const val GAME_TOUCH_SENSITIVITY = 1.15f
        private const val VK_BACK = 0x08
        private const val VK_TAB = 0x09
        private const val VK_RETURN = 0x0D
        private const val VK_SPACE = 0x20
        private const val VK_ESCAPE = 0x1B
        private const val VK_PRIOR = 0x21
        private const val VK_NEXT = 0x22
        private const val VK_END = 0x23
        private const val VK_HOME = 0x24
        private const val VK_LEFT = 0x25
        private const val VK_UP = 0x26
        private const val VK_RIGHT = 0x27
        private const val VK_DOWN = 0x28
        private const val VK_INSERT = 0x2D
        private const val VK_DELETE = 0x2E
        private const val VK_LWIN = 0x5B
        private const val VK_RWIN = 0x5C
        private const val VK_LSHIFT = 0xA0
        private const val VK_RSHIFT = 0xA1
        private const val VK_LCONTROL = 0xA2
        private const val VK_RCONTROL = 0xA3
        private const val VK_LMENU = 0xA4
        private const val VK_RMENU = 0xA5
        private const val VK_F1 = 0x70
        private const val VK_F2 = 0x71
        private const val VK_F3 = 0x72
        private const val VK_F4 = 0x73
        private const val VK_F5 = 0x74
        private const val VK_F6 = 0x75
        private const val VK_F7 = 0x76
        private const val VK_F8 = 0x77
        private const val VK_F9 = 0x78
        private const val VK_F10 = 0x79
        private const val VK_F11 = 0x7A
        private const val VK_F12 = 0x7B
        private const val VK_0 = 0x30
        private const val VK_A = 0x41
        private const val VK_OEM_1 = 0xBA
        private const val VK_OEM_PLUS = 0xBB
        private const val VK_OEM_COMMA = 0xBC
        private const val VK_OEM_MINUS = 0xBD
        private const val VK_OEM_PERIOD = 0xBE
        private const val VK_OEM_2 = 0xBF
        private const val VK_OEM_3 = 0xC0
        private const val VK_OEM_4 = 0xDB
        private const val VK_OEM_5 = 0xDC
        private const val VK_OEM_6 = 0xDD
        private const val VK_OEM_7 = 0xDE
        private const val FULLSCREEN_FLAGS =
            View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
    }
}
