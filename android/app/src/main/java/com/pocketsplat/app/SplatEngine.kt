package com.pocketsplat.app

import android.view.Surface

/**
 * JNI surface of the native engine (jni_bridge.cpp). A handle is one AndroidHost. Every call
 * except [nativeGetStats] must run on the render thread — [RenderEngine] owns that contract.
 * The handle-free import/probe calls are the other exception: they touch no AndroidHost and run
 * on a background coroutine ([nativeImportScene] blocks for the whole pipeline).
 */
object SplatEngine {
    init {
        System.loadLibrary("pocketsplat")
    }

    external fun nativeCreate(): Long
    external fun nativeDestroy(handle: Long)
    external fun nativeOpenScene(handle: Long, path: String): Boolean

    /**
     * [splat count, SH degree, pivot x, y, z, framing diagonal, eye x, y, z, orientation w, x, y, z,
     *  up x, y, z, has backside info, its source (0 cameras, 1 room, 2 object, -1 none)]:
     * the scene's home view (near the capture, looking at the main object) as a trackball pose.
     */
    external fun nativeSceneInfo(handle: Long): FloatArray
    external fun nativeSetSurface(handle: Long, surface: Surface?): Boolean

    /**
     * Camera pose, RUB world: eye and camera-to-world orientation (w, x, y, z; looks down -z); the
     * FOV spans the view's long side.
     */
    external fun nativeSetCameraPose(
        handle: Long, ex: Float, ey: Float, ez: Float, qw: Float, qx: Float, qy: Float, qz: Float, fovRad: Float,
    )

    /** Depth pick at a point of the view (0..1, y down); the result comes from [nativeTakePick]. */
    external fun nativeRequestPick(handle: Long, u: Float, v: Float)

    /** null = no result yet (keep drawing) or none requested; [x, y, z] = the world point hit; [] = miss. */
    external fun nativeTakePick(handle: Long): FloatArray?

    external fun nativeRender(handle: Long)

    /** Thermal-adaptive quality controller in interactive use (ADR-0009). */
    external fun nativeSetAdaptive(handle: Long, on: Boolean)

    /** Coverage fade strength 0..1 (ADR-0013); kept across scenes, bench runs included. */
    external fun nativeSetCoverageStrength(handle: Long, strength: Float)

    /** Platform thermal sample, about once a second while resumed; true = the quality rung changed. */
    external fun nativeThermalTick(handle: Long): Boolean

    /** Rate interactive frames are paced at (vsyncs in between are skipped); 0 = free-running. */
    external fun nativeTargetFps(handle: Long): Float

    /** Records every interactive controller sample to a bench-format CSV (`splatconv replay`). */
    external fun nativeStartTrace(handle: Long, path: String): Boolean

    /** Appends the trace rows buffered since the last flush (also done every ~10 s natively). */
    external fun nativeFlushTrace(handle: Long)

    external fun nativeStartBench(
        handle: Long, csvPath: String, meta: String, sceneName: String, keyBits: Int,
        minContribution: Float, targetFps: Float, drawMode: Int, controller: Boolean, voteFps: Boolean,
        homeSwing: Boolean,
    ): Boolean

    /** Writes the CSV and returns a one-line summary ("" = nothing was recorded). */
    external fun nativeStopBench(handle: Long): String

    /**
     * [gpu ms, cull ms, preprocess ms, prepare+sort ms, draw ms, visible, visible chunks, frames,
     *  contribution threshold px, adaptive-quality rung (-1 = off)]
     */
    external fun nativeGetStats(handle: Long): FloatArray

    /**
     * Backside info (ADR-0015): writes `<scene stem>.pscov` next to [scene] from capture camera
     * files ([source] 0: cameras.json, transforms.json, or COLMAP cameras + images) or virtual
     * cameras for a scene type (1 room, 2 object). Blocks for seconds - call off the main thread.
     * Returns "ok: <summary>" or the error.
     */
    external fun nativeComputeCoverage(scene: String, source: Int, cameraFiles: Array<String>): String

    /** Header-only probe: [splat count, SH degree, file bytes]; null = not a loadable scene. */
    external fun nativeProbeScene(path: String): LongArray?

    /**
     * Runs the whole import pipeline (read, clean, pack, write) synchronously — call from a
     * background coroutine only. Returns "" on success, the error message otherwise
     * ("import cancelled" after [nativeImportCancel]). maxSplats <= 0 keeps every splat.
     */
    external fun nativeImportScene(
        src: String, dst: String, shTarget: Int, deepClean: Boolean, maxSplats: Long,
    ): String

    /** Stage of the running import (0 read, 1 clean, 2 SOR, 3 pack, 4 write); -1 = idle. */
    external fun nativeImportStage(): Int

    /** Asks the running import to stop at its next stage boundary. */
    external fun nativeImportCancel()
}
