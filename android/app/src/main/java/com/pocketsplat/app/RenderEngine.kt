package com.pocketsplat.app

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.view.Choreographer
import android.view.Surface
import java.util.concurrent.CountDownLatch
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * Owns the render thread. All SplatEngine calls are funneled onto one HandlerThread, and frames
 * are paced by that thread's Choreographer (vsync), at most at the native target rate (60 fps:
 * on a 120 Hz panel every other vsync is skipped). Frames are drawn on demand: a camera change,
 * a new surface, a scene or a resume requests a short burst of frames, and an idle viewer
 * presents nothing (the last image stays on screen). The activity pauses/resumes the loop; the
 * SurfaceView hands surfaces in ([setSurface] with null blocks until the native side released
 * the surface, as surfaceDestroyed requires).
 */
class RenderEngine {
    private val thread = HandlerThread("SplatRender").apply { start() }
    private val handler = Handler(thread.looper)
    private val mainHandler = Handler(Looper.getMainLooper())

    // Render-thread state only (mutated exclusively inside posted lambdas).
    private var handle = 0L
    private var surfaceReady = false
    private var rendering = false
    private var frameScheduled = false
    private var pendingFrames = 0 // frames still owed to the last change (on-demand rendering)
    private var lastFrameNanos = 0L // vsync time of the last interactive frame

    // Benchmark state (render thread). While active, frames free-run (handler.post instead of
    // Choreographer) - the native side switched the swapchain off vsync, so the GPU paces the loop.
    private var benchActive = false
    private var benchEndAt = 0L
    private var benchDone: ((String?) -> Unit)? = null

    // Trackball camera (render-thread state): the eye orbits [pivot] at [distance] along the
    // camera's +z; [orientation] is camera-to-world (w, x, y, z), renormalized after every change.
    // A drag turns it about the camera's own up and right axes (no fixed axis, no limits).
    private var pivot = floatArrayOf(0f, 0f, 0f)
    private var orientation = Quat.identity()
    private var distance = 5.0f
    private var sceneDiag = 1.0f
    // Across the view's long side (vertical in portrait): turning the phone keeps the scale.
    private val fovRad = Math.toRadians(60.0).toFloat()
    // Level orbit (HUD toggle): horizontal drags turn about the scene's fitted up axis instead of the
    // camera's, so circling keeps the horizon; vertical drags still tilt about the camera's right.
    private var levelOrbit = false
    private var sceneUp = floatArrayOf(0f, 1f, 0f)
    private var homePivot = floatArrayOf(0f, 0f, 0f)
    private var homeOrientation = Quat.identity()
    private var homeDistance = 5.0f

    // Double-tap pivot: a pick in flight, then a short turn of the camera toward the point hit.
    private var pickPending = false
    private var pivotAnimation: PivotAnimation? = null

    /** Eye fixed; the view turns to the new pivot and the distance becomes the eye-pivot distance. */
    private class PivotAnimation(
        val eye: FloatArray,
        val fromQ: FloatArray,
        val toQ: FloatArray,
        val fromDistance: Float,
        val toDistance: Float,
        val startNanos: Long,
    )

    fun create() = handler.post {
        if (handle == 0L) handle = SplatEngine.nativeCreate()
    }

    fun destroy() {
        handler.post {
            handler.removeCallbacks(thermalTick)
            if (handle != 0L) {
                SplatEngine.nativeDestroy(handle)
                handle = 0L
            }
        }
        thread.quitSafely()
    }

    fun resume() = handler.post {
        rendering = true
        handler.removeCallbacks(thermalTick)
        handler.postDelayed(thermalTick, THERMAL_TICK_MS)
        requestRender()
    }

    fun pause() = handler.post {
        rendering = false
        handler.removeCallbacks(thermalTick)
        if (handle != 0L) SplatEngine.nativeFlushTrace(handle)
    }

    /** Starts an interactive trace (bench-format CSV of every controller sample) at [path]. */
    fun startTrace(path: String) = handler.post {
        if (handle != 0L) SplatEngine.nativeStartTrace(handle, path)
    }

    /** Coverage fade (ADR-0013): 0 = unchanged, 1 = splats seen from unobserved directions vanish. */
    fun setCoverageStrength(strength: Float) = handler.post {
        if (handle != 0L) {
            SplatEngine.nativeSetCoverageStrength(handle, strength)
            requestRender()
        }
    }

    fun setAdaptiveQuality(on: Boolean) = handler.post {
        if (handle != 0L) {
            SplatEngine.nativeSetAdaptive(handle, on)
            requestRender()
        }
    }

    /**
     * About once a second while resumed, drawn or not: the native quality controller reads the
     * platform's thermal headroom (Android drops its forecast history after 10 s without a call),
     * and a phone that cooled while idle steps back up - the still image is redrawn at the new rung.
     */
    private val thermalTick = object : Runnable {
        override fun run() {
            if (!rendering || handle == 0L) return
            if (SplatEngine.nativeThermalTick(handle)) requestRender()
            handler.postDelayed(this, THERMAL_TICK_MS)
        }
    }

    /** Blocking: after this returns with null the native side no longer touches the surface. */
    fun setSurface(surface: Surface?) {
        val latch = CountDownLatch(1)
        handler.post {
            if (handle != 0L) SplatEngine.nativeSetSurface(handle, surface)
            surfaceReady = surface != null
            if (surfaceReady) requestRender()
            latch.countDown()
        }
        latch.await()
    }

    fun openScene(path: String, onDone: (FloatArray?) -> Unit) = handler.post {
        val ok = handle != 0L && SplatEngine.nativeOpenScene(handle, path)
        val info = if (ok) SplatEngine.nativeSceneInfo(handle) else null
        if (info != null && info.size >= 16) {
            // Home view: near the capture, looking at the main object (splatcore::estimate_home_view).
            pivot = floatArrayOf(info[2], info[3], info[4])
            sceneDiag = info[5]
            orientation = Quat.normalize(floatArrayOf(info[9], info[10], info[11], info[12]))
            val dx = info[6] - pivot[0]
            val dy = info[7] - pivot[1]
            val dz = info[8] - pivot[2]
            distance = sqrt(dx * dx + dy * dy + dz * dz).coerceAtLeast(1e-4f * sceneDiag)
            sceneUp = floatArrayOf(info[13], info[14], info[15])
            homePivot = pivot.copyOf()
            homeOrientation = orientation.copyOf()
            homeDistance = distance
            pivotAnimation = null
            pickPending = false
            pushCamera()
        }
        requestRender() // a failed open still clears the old scene's image
        mainHandler.post { onDone(info) }
    }

    /** Trackball: a drag of (dx, dy) pixels turns the camera about its own up, then right axis. */
    fun orbit(dx: Float, dy: Float) = handler.post {
        stopPivotAnimation()
        // Local axes compose on the right, world axes on the left. The signs keep the old orbit's
        // feel: the scene follows the finger.
        val pitchStep = Quat.axisAngle(1f, 0f, 0f, -dy * ORBIT_RAD_PER_PX)
        orientation = if (levelOrbit) {
            val yawStep = Quat.axisAngle(sceneUp[0], sceneUp[1], sceneUp[2], -dx * ORBIT_RAD_PER_PX)
            Quat.normalize(Quat.mul(Quat.mul(yawStep, orientation), pitchStep))
        } else {
            val yawStep = Quat.axisAngle(0f, 1f, 0f, -dx * ORBIT_RAD_PER_PX)
            Quat.normalize(Quat.mul(Quat.mul(orientation, yawStep), pitchStep))
        }
        pushCamera()
    }

    fun setLevelOrbit(on: Boolean) = handler.post { levelOrbit = on }

    /** Back to the scene's home view. */
    fun resetHome() = handler.post {
        stopPivotAnimation()
        pivot = homePivot.copyOf()
        orientation = homeOrientation.copyOf()
        distance = homeDistance
        pushCamera()
    }

    /** Pinch: the eye moves toward (factor < 1) or away from the pivot. */
    fun zoom(factor: Float) = handler.post {
        stopPivotAnimation()
        distance = (distance * factor).coerceIn(1e-4f * sceneDiag, 10f * sceneDiag)
        pushCamera()
    }

    /**
     * Two-finger drag by (dx, dy) in view long sides (the FOV's span): the pivot moves in the camera plane so that the
     * point at the pivot's depth stays under the fingers.
     */
    fun pan(dx: Float, dy: Float) = handler.post {
        stopPivotAnimation()
        val worldPerLongSide = 2f * distance * tan(0.5f * fovRad)
        val right = Quat.rotate(orientation, 1f, 0f, 0f)
        val up = Quat.rotate(orientation, 0f, 1f, 0f)
        for (c in 0..2) pivot[c] += (-right[c] * dx + up[c] * dy) * worldPerLongSide
        pushCamera()
    }

    /** Double tap at (u, v) of the view (0..1, y down): pick the point there and orbit around it. */
    fun pickPivot(u: Float, v: Float) = handler.post {
        if (handle == 0L || benchActive) return@post
        stopPivotAnimation()
        SplatEngine.nativeRequestPick(handle, u, v)
        pickPending = true
        requestRender() // the pick runs on the next frames
    }

    private fun eye(): FloatArray {
        val back = Quat.rotate(orientation, 0f, 0f, 1f)
        return FloatArray(3) { pivot[it] + distance * back[it] }
    }

    private fun startPivotAnimation(point: FloatArray) {
        val eye = eye()
        val to = FloatArray(3) { point[it] - eye[it] }
        val toDistance = sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2])
        if (toDistance < 1e-5f * sceneDiag) return
        val forward = Quat.rotate(orientation, 0f, 0f, -1f)
        val toDir = FloatArray(3) { to[it] / toDistance }
        val toQ = Quat.normalize(Quat.mul(Quat.fromTo(forward, toDir), orientation))
        pivotAnimation = PivotAnimation(eye, orientation, toQ, distance, toDistance, System.nanoTime())
    }

    /** Advances the pivot animation; the eye stays where it was when the pick landed. */
    private fun stepPivotAnimation() {
        val a = pivotAnimation ?: return
        val t = ((System.nanoTime() - a.startNanos) / PIVOT_ANIMATION_NS.toFloat()).coerceIn(0f, 1f)
        val s = t * t * (3f - 2f * t) // smoothstep
        orientation = Quat.slerp(a.fromQ, a.toQ, s)
        distance = a.fromDistance + s * (a.toDistance - a.fromDistance)
        val back = Quat.rotate(orientation, 0f, 0f, 1f)
        pivot = FloatArray(3) { a.eye[it] - distance * back[it] }
        pushCamera()
        if (t >= 1f) pivotAnimation = null
    }

    private fun stopPivotAnimation() {
        pivotAnimation = null // the camera stays where the animation had taken it
    }

    /**
     * Starts the native benchmark (auto-orbit, vsync off, per-frame recording) and runs it for
     * [seconds] of wall clock; [onDone] gets the summary on the main thread (null = failed or
     * nothing recorded). [targetFps] > 0 caps the frame rate (the native side paces the loop).
     * [controller] runs the thermal-adaptive quality controller (ADR-0009) instead of the fixed
     * [minContribution]. [voteFps] asks the compositor for [targetFps] (the panel rate follows, as
     * it does for interactive frames); without it the panel stays at the compositor's choice, as
     * in every bench run before S21. [homeSwing] swings the camera about the scene's start view
     * instead of the framing orbit (AndroidHost::BenchConfig::home_swing). Touch camera input is ignored for the duration.
     */
    fun startBench(
        csvPath: String, meta: String, sceneName: String, keyBits: Int, minContribution: Float,
        targetFps: Float, drawMode: Int, controller: Boolean, voteFps: Boolean, homeSwing: Boolean, seconds: Int,
        onDone: (String?) -> Unit,
    ) = handler.post {
        val ok = !benchActive && handle != 0L &&
            SplatEngine.nativeStartBench(
                handle, csvPath, meta, sceneName, keyBits, minContribution, targetFps, drawMode, controller,
                voteFps, homeSwing,
            )
        if (!ok) {
            mainHandler.post { onDone(null) }
            return@post
        }
        benchActive = true
        benchEndAt = SystemClock.elapsedRealtime() + seconds * 1000L
        benchDone = onDone
        scheduleFrame()
    }

    /** Ends a running benchmark early; the CSV still covers everything recorded so far. */
    fun stopBench() = handler.post { benchEndAt = 0L }

    fun requestStats(onStats: (FloatArray) -> Unit) = handler.post {
        if (handle != 0L) {
            val stats = SplatEngine.nativeGetStats(handle)
            mainHandler.post { onStats(stats) }
        }
    }

    private fun pushCamera() {
        if (handle != 0L) {
            val eye = eye()
            SplatEngine.nativeSetCameraPose(
                handle, eye[0], eye[1], eye[2],
                orientation[0], orientation[1], orientation[2], orientation[3], fovRad,
            )
        }
        requestRender()
    }

    /**
     * Something on screen changed: draw a few frames. One would show the change; the extra ones
     * let the HUD's GPU timers and visible counts (which trail by the frames in flight) resolve,
     * and retry a frame the native side abandoned for a swapchain recreate.
     */
    private fun requestRender() {
        pendingFrames = SETTLE_FRAMES
        scheduleFrame()
    }

    private fun scheduleFrame() {
        if (frameScheduled || !rendering || !surfaceReady) return
        if (!benchActive && pendingFrames == 0) return // idle: nothing changed, present nothing
        frameScheduled = true
        if (benchActive) {
            handler.post {
                frameScheduled = false
                renderFrame()
            }
        } else {
            Choreographer.getInstance().postFrameCallback { frameTimeNanos ->
                frameScheduled = false
                if (tooSoon(frameTimeNanos)) {
                    scheduleFrame()
                } else {
                    lastFrameNanos = frameTimeNanos
                    renderFrame()
                }
            }
        }
    }

    /**
     * True when this vsync comes earlier than the native target rate allows. Vsync times are exact
     * multiples of the display period, so half a 120 Hz period of slack renders every other vsync
     * at 120 Hz and every one at 60 Hz.
     */
    private fun tooSoon(frameTimeNanos: Long): Boolean {
        if (handle == 0L) return false
        val fps = SplatEngine.nativeTargetFps(handle)
        if (fps <= 0f) return false
        return frameTimeNanos - lastFrameNanos < (1e9f / fps).toLong() - PACE_SLACK_NS
    }

    private fun renderFrame() {
        if (!rendering || !surfaceReady || handle == 0L) return
        if (benchActive && SystemClock.elapsedRealtime() >= benchEndAt) {
            benchActive = false
            val summary = SplatEngine.nativeStopBench(handle)
            val done = benchDone
            benchDone = null
            mainHandler.post { done?.invoke(summary.ifEmpty { null }) }
            pendingFrames = SETTLE_FRAMES // the vsync-on swapchain gets fresh frames, then idles
        }
        SplatEngine.nativeRender(handle)
        if (pendingFrames > 0) pendingFrames--
        if (pickPending) {
            val hit = SplatEngine.nativeTakePick(handle)
            if (hit == null) {
                pendingFrames = maxOf(pendingFrames, 1) // the result needs a few more frames
            } else {
                pickPending = false
                if (hit.size == 3) startPivotAnimation(hit)
            }
        }
        if (pivotAnimation != null) stepPivotAnimation() // also requests the next frame
        scheduleFrame()
    }

    private companion object {
        const val SETTLE_FRAMES = 3 // frames in flight (2) + 1
        const val THERMAL_TICK_MS = 1000L
        const val PACE_SLACK_NS = 4_000_000L // half a 120 Hz vsync period
        const val ORBIT_RAD_PER_PX = 0.005f
        const val PIVOT_ANIMATION_NS = 300_000_000L
    }
}
