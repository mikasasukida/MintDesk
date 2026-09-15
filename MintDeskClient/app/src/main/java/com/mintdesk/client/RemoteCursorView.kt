package com.mintdesk.client

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

class RemoteCursorView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 3f
    }

    private var cursorX = 0f
    private var cursorY = 0f
    private var visibleCursor = false

    fun updatePosition(x: Float, y: Float) {
        cursorX = x
        cursorY = y
        visibleCursor = true
        invalidate()
    }

    fun hideCursor() {
        visibleCursor = false
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (!visibleCursor) return

        val radius = 9f
        canvas.drawCircle(cursorX, cursorY, radius, fillPaint)
        canvas.drawCircle(cursorX, cursorY, radius, strokePaint)
        canvas.drawLine(cursorX - 16f, cursorY, cursorX + 16f, cursorY, strokePaint)
        canvas.drawLine(cursorX, cursorY - 16f, cursorX, cursorY + 16f, strokePaint)
    }
}
