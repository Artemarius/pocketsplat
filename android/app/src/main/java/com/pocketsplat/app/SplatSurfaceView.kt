package com.pocketsplat.app

import android.annotation.SuppressLint
import android.content.Context
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewConfiguration
import kotlin.math.hypot

/**
 * SurfaceView hosting the Vulkan swapchain, with the touch camera: one finger turns the trackball,
 * two fingers pinch (zoom about the pivot) and pan, a double tap moves the pivot to the point under
 * the finger. Renders at a reduced internal resolution via setFixedSize (the compositor's scaler
 * does the upscale for free — the 1080p-class internal resolution the Phase 3 plan calls for on
 * 1440p panels).
 */
class SplatSurfaceView(context: Context, private val engine: RenderEngine) :
    SurfaceView(context), SurfaceHolder.Callback {

    // One-finger drags follow one pointer by id. Pointer indices shift when a finger lifts: reading
    // index 0 after the other finger of a pinch went up made the remaining finger's position read as
    // a drag across the gap between the two, and the camera jumped.
    private var activeId = MotionEvent.INVALID_POINTER_ID
    private var lastX = 0f
    private var lastY = 0f
    private var scaling = false
    private var lastFocusX = 0f
    private var lastFocusY = 0f
    // A gesture "moves" once a finger travels past the touch slop or a pinch starts: taps and the
    // double-tap pick never show the backside fade (RenderEngine.setGesture).
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
    private var downX = 0f
    private var downY = 0f
    private var moving = false

    private fun startMoving() {
        if (!moving) {
            moving = true
            engine.setGesture(true)
        }
    }
    private val scaleDetector = ScaleGestureDetector(
        context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                if (detector.scaleFactor > 0f) engine.zoom(1f / detector.scaleFactor)
                // Two-finger drag pans by the focal point's motion (zoom and pan compose), in view
                // long sides (the FOV's span) so the content under the fingers follows them.
                val side = maxOf(width, height).coerceAtLeast(1).toFloat()
                engine.pan((detector.focusX - lastFocusX) / side, (detector.focusY - lastFocusY) / side)
                lastFocusX = detector.focusX
                lastFocusY = detector.focusY
                return true
            }

            override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
                scaling = true
                startMoving()
                lastFocusX = detector.focusX
                lastFocusY = detector.focusY
                return true
            }

            override fun onScaleEnd(detector: ScaleGestureDetector) {
                scaling = false
            }
        },
    )

    private val tapDetector = GestureDetector(
        context,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                if (width > 0 && height > 0) engine.pickPivot(e.x / width, e.y / height)
                return true
            }
        },
    )

    // Exact-resolution override (benchmarks pin the protocol resolution regardless of the
    // display mode the panel happens to be in); null = INTERNAL_SCALE of the view size.
    private var internalOverride: Pair<Int, Int>? = null
    private var onSizeApplied: (() -> Unit)? = null

    init {
        holder.addCallback(this)
    }

    /**
     * Forces the internal resolution to [size] (null returns to the default scale). [onApplied]
     * runs on the main thread once the native swapchain is live at the requested size.
     */
    fun setInternalSize(size: Pair<Int, Int>?, onApplied: () -> Unit = {}) {
        internalOverride = size
        val (iw, ih) = desiredSize()
        val frame = holder.surfaceFrame
        if (frame.width() == iw && frame.height() == ih) {
            onApplied()
        } else {
            onSizeApplied = onApplied
            holder.setFixedSize(iw, ih)
        }
    }

    private fun desiredSize(): Pair<Int, Int> = internalOverride
        ?: Pair(
            (width * INTERNAL_SCALE).toInt().coerceAtLeast(1),
            (height * INTERNAL_SCALE).toInt().coerceAtLeast(1),
        )

    override fun surfaceCreated(holder: SurfaceHolder) = Unit // wait for surfaceChanged (size known)

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) {
        val (iw, ih) = desiredSize()
        if (w != iw || h != ih) {
            holder.setFixedSize(iw, ih) // triggers surfaceChanged again at the internal size
            return
        }
        engine.setSurface(holder.surface) // blocking: the swapchain is live when this returns
        onSizeApplied?.invoke()
        onSizeApplied = null
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        engine.setSurface(null) // blocks until the native side released the surface
    }

    @SuppressLint("ClickableViewAccessibility") // viewer canvas: gestures only, no click actions
    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(event)
        tapDetector.onTouchEvent(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                anchor(event, 0)
                downX = event.x
                downY = event.y
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // The tracked finger lifted: follow one that stays, from where it is now.
                val up = event.actionIndex
                if (event.getPointerId(up) == activeId) anchor(event, if (up == 0) 1 else 0)
            }
            MotionEvent.ACTION_MOVE -> {
                val i = event.findPointerIndex(activeId)
                if (i >= 0) {
                    val x = event.getX(i)
                    val y = event.getY(i)
                    if (!moving && hypot(event.x - downX, event.y - downY) > touchSlop) startMoving()
                    if (!scaling && event.pointerCount == 1) engine.orbit(x - lastX, y - lastY)
                    lastX = x
                    lastY = y
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                activeId = MotionEvent.INVALID_POINTER_ID
                if (moving) engine.setGesture(false)
                moving = false
            }
        }
        return true
    }

    private fun anchor(event: MotionEvent, index: Int) {
        activeId = event.getPointerId(index)
        lastX = event.getX(index)
        lastY = event.getY(index)
    }

    private companion object {
        // 0.6x of the 3120x1440 panel ~ 1872x864 internal; PROJECT.md Phase 0 gate decision.
        const val INTERNAL_SCALE = 0.6f
    }
}
