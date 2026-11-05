package com.pocketsplat.app

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.provider.OpenableColumns
import android.app.Activity
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.coroutines.coroutineContext
import kotlin.math.roundToInt

/** A scripted benchmark run parsed from intent extras (scripts/bench_android.py drives these). */
data class BenchRequest(
    val sceneName: String,
    val seconds: Int,
    val keyBits: Int,
    val minContribution: Float,
    val targetFps: Float, // 0 = uncapped
    val resolution: Pair<Int, Int>?,
    // SplatRenderer::DrawMode: 0 = 6 vertices/splat, 1 = instanced strip, 2 = indexed batch, 3 = triangle;
    // + 4 x a diagnostic DrawProbe (bench_android.py --draw-probe), decoded in the native host
    val drawMode: Int,
    val controller: Boolean, // thermal-adaptive quality controller (ADR-0009) instead of minContribution
    val voteFps: Boolean, // ask the compositor for targetFps (the panel follows, as in interactive use)
    val homeSwing: Boolean, // camera swings about the start view instead of the framing orbit
)

/** A probed .ply/.spz source waiting for import confirmation ([src] is always a local file). */
data class ImportCandidate(
    val src: File,
    val displayName: String,
    val count: Long,
    val shDegree: Long,
    val deleteAfter: Boolean, // true for SAF picks copied into cache/import
    val isSample: Boolean = false, // the bundled sample: importing or dismissing it marks it installed
)

class MainActivity : ComponentActivity() {
    private val engine = RenderEngine()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        engine.create()
        // Benchmarks run unattended for 10 minutes on battery; the screen must stay on.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val request = benchRequestFromIntent()
        // Scripted interactive open (no bench): e.g. idle-power checks of on-demand rendering.
        val openScene = intent.getStringExtra("open_scene")
        // Scripted backside info for open_scene (ADR-0015): "room", "object" or "cameras" with
        // backside_files = comma-separated paths the app can read (e.g. under files/cameras).
        val backsideRequest = when (intent.getStringExtra("backside")) {
            "cameras" -> 0
            "room" -> 1
            "object" -> 2
            else -> -1
        }
        val backsideFiles = intent.getStringExtra("backside_files")?.split(',')?.filter { it.isNotBlank() }
            ?.map { File(it) }.orEmpty()
        // Hand-held sessions: every interactive controller sample goes to files/trace/<name>.csv.
        intent.getStringExtra("interactive_trace")?.let { name ->
            engine.startTrace(File(File(filesDir, "trace").apply { mkdirs() }, "$name.csv").path)
        }
        val protocolRes = protocolResolution()
        // Backside fade (ADR-0015): interactive use fades at kDefaultBacksideFade while a gesture
        // moves the view; scripted runs (bench_android.py passes coverage_strength) fade at their
        // strength all the time, as every bench since ADR-0013. The overlay changes both.
        val scriptedFade = intent.hasExtra("coverage_strength")
        val coverageStrength =
            if (scriptedFade) intent.getFloatExtra("coverage_strength", 0f).coerceIn(0f, 1f) else kDefaultBacksideFade
        engine.setFadeWhileMoving(!scriptedFade)
        engine.setCoverageStrength(coverageStrength)
        setContent {
            MaterialTheme {
                ViewerScreen(
                    engine, ::sceneFiles, request, openScene, protocolRes, filesDir, coverageStrength,
                    !scriptedFade, backsideRequest, backsideFiles,
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        engine.resume()
    }

    override fun onPause() {
        engine.pause()
        super.onPause()
    }

    override fun onDestroy() {
        if (isFinishing) engine.destroy()
        super.onDestroy()
    }

    /** Scenes from the app dirs (adb push or past imports); any other file arrives via SAF. */
    private fun sceneFiles(): List<File> {
        val dirs = listOfNotNull(File(filesDir, "scenes"), getExternalFilesDir(null)?.resolve("scenes"))
        val extensions = setOf("pspk", "spz", "ply")
        return dirs.flatMap { dir -> dir.listFiles().orEmpty().toList() }
            .filter { it.isFile && it.extension.lowercase() in extensions }
            .sortedBy { it.name }
    }

    private fun benchRequestFromIntent(): BenchRequest? {
        val scene = intent.getStringExtra("bench_scene") ?: return null
        val w = intent.getIntExtra("bench_width", 0)
        val h = intent.getIntExtra("bench_height", 0)
        return BenchRequest(
            sceneName = scene,
            seconds = intent.getIntExtra("bench_seconds", 600),
            keyBits = intent.getIntExtra("bench_key_bits", 32),
            minContribution = intent.getFloatExtra("bench_min_contribution", 0f),
            targetFps = intent.getFloatExtra("bench_target_fps", 0f),
            resolution = if (w > 0 && h > 0) Pair(w, h) else null,
            drawMode = intent.getIntExtra("bench_draw_mode", 3), // default = SplatRenderer::kDefaultDrawMode (triangle)
            controller = intent.getBooleanExtra("bench_controller", false),
            voteFps = intent.getBooleanExtra("bench_vote_fps", false),
            homeSwing = intent.getStringExtra("bench_pose") == "home",
        )
    }

    /**
     * The benchmark-protocol internal resolution: 0.6x of the panel's native mode (the Phase 0
     * gate decision), independent of whatever display mode the system is currently in.
     */
    private fun protocolResolution(): Pair<Int, Int>? {
        val mode = display?.supportedModes?.maxByOrNull { it.physicalWidth.toLong() * it.physicalHeight }
            ?: return null
        return Pair((mode.physicalWidth * 0.6f).roundToInt(), (mode.physicalHeight * 0.6f).roundToInt())
    }
}

private fun importStageLabel(stage: Int): String = when (stage) {
    0 -> "reading scene…"
    1 -> "cleaning…"
    2 -> "removing floaters (SOR)…"
    3 -> "packing…"
    4 -> "writing pack…"
    else -> "importing…"
}

private fun sanitizeName(name: String): String = name.replace(Regex("[^A-Za-z0-9._-]"), "_")

/**
 * Copies a SAF pick into cache/import (content URIs are unreadable from native code) and probes
 * it. Returns null after reporting the reason through [onStatus]; cancellation deletes the copy.
 */
private suspend fun stageSafPick(
    context: Context, uri: Uri, onStatus: (String?) -> Unit,
): ImportCandidate? = withContext(Dispatchers.IO) {
    val resolver = context.contentResolver
    var name = "scene"
    var size = -1L
    resolver.query(uri, null, null, null, null)?.use { cursor ->
        if (cursor.moveToFirst()) {
            val nameIdx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            val sizeIdx = cursor.getColumnIndex(OpenableColumns.SIZE)
            if (nameIdx >= 0) cursor.getString(nameIdx)?.let { name = it }
            if (sizeIdx >= 0 && !cursor.isNull(sizeIdx)) size = cursor.getLong(sizeIdx)
        }
    }
    val ext = name.substringAfterLast('.', "").lowercase()
    if (ext != "ply" && ext != "spz") {
        onStatus("unsupported file type '.$ext' - pick a .ply or .spz")
        return@withContext null
    }
    val dir = File(context.cacheDir, "import").apply { mkdirs() }
    val copy = File(dir, sanitizeName(name))
    try {
        resolver.openInputStream(uri).use { input ->
            if (input == null) {
                onStatus("cannot open the picked file")
                return@withContext null
            }
            copy.outputStream().use { out ->
                val buffer = ByteArray(1 shl 20)
                var copied = 0L
                while (true) {
                    coroutineContext.ensureActive()
                    val n = input.read(buffer)
                    if (n < 0) break
                    out.write(buffer, 0, n)
                    copied += n
                    val progress = if (size > 0) "${copied shr 20} / ${size shr 20} MB"
                    else "${copied shr 20} MB"
                    onStatus("copying $progress")
                }
            }
        }
    } catch (e: CancellationException) {
        copy.delete()
        throw e
    } catch (e: Exception) {
        copy.delete()
        onStatus("copy failed: ${e.message}")
        return@withContext null
    }
    val probe = SplatEngine.nativeProbeScene(copy.absolutePath)
    if (probe == null) {
        copy.delete()
        onStatus("not a readable splat scene - see logcat")
        return@withContext null
    }
    onStatus(null)
    ImportCandidate(copy, name, probe[0], probe[1], deleteAfter = true)
}

/**
 * Copies picked capture-camera files (cameras.json, transforms.json, or COLMAP cameras + images)
 * into cache/import/cameras, replacing any earlier pick. Returns the copies (empty on failure,
 * reported through [onStatus]).
 */
private suspend fun stageCameraFiles(
    context: Context, uris: List<Uri>, onStatus: (String?) -> Unit,
): List<File> = withContext(Dispatchers.IO) {
    val dir = File(context.cacheDir, "import/cameras")
    dir.deleteRecursively()
    dir.mkdirs()
    val out = mutableListOf<File>()
    for (uri in uris) {
        var name = "cameras"
        context.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0) cursor.getString(idx)?.let { name = it }
            }
        }
        val copy = File(dir, sanitizeName(name))
        try {
            onStatus("copying $name")
            context.contentResolver.openInputStream(uri).use { input ->
                if (input == null) throw IllegalStateException("cannot open $name")
                copy.outputStream().use { input.copyTo(it) }
            }
            out += copy
        } catch (e: Exception) {
            onStatus("copy failed: ${e.message}")
            return@withContext emptyList<File>()
        }
    }
    onStatus(null)
    out
}

private fun backsideLabel(source: Int): String = when (source) {
    0 -> "from capture cameras"
    1 -> "guessed: room"
    2 -> "guessed: object"
    else -> "not set up"
}

/** Which guess fits which capture (ADR-0015: the wrong one costs many wrong reveals). */
private const val kBacksideTypeHint =
    "room: the capture walked through it - object: the capture went around it"

/** Interactive backside fade strength (applied while a gesture moves the view, ADR-0015). */
private const val kDefaultBacksideFade = 0.75f

private const val kSampleAsset = "samples/hornedlizard.spz"
private const val kPrefs = "pocketsplat"
private const val kSampleInstalled = "sample_installed"

/**
 * Copies the bundled sample scene (assets/samples, MIT, nianticlabs/spz; app/build.gradle.kts puts
 * it there) into cache/import and probes it, so the first launch has a scene to open.
 */
private suspend fun stageSample(context: Context, onStatus: (String?) -> Unit): ImportCandidate? =
    withContext(Dispatchers.IO) {
        val copy = File(File(context.cacheDir, "import").apply { mkdirs() }, kSampleAsset.substringAfterLast('/'))
        try {
            context.assets.open(kSampleAsset).use { input -> copy.outputStream().use { input.copyTo(it) } }
        } catch (e: Exception) {
            copy.delete()
            onStatus("sample scene unavailable: ${e.message}")
            return@withContext null
        }
        val probe = SplatEngine.nativeProbeScene(copy.absolutePath)
        if (probe == null) {
            copy.delete()
            onStatus("cannot read the sample scene - see logcat")
            return@withContext null
        }
        onStatus(null)
        ImportCandidate(copy, copy.name, probe[0], probe[1], deleteAfter = true, isSample = true)
    }

/**
 * Licence notices (assets/third_party_notices.txt, from scripts/third_party_notices.py) as paragraphs.
 * The texts keep their original hard wraps; for display, a line break (and the next line's indent)
 * joins into a space unless the next line starts a list item.
 */
private fun loadNotices(context: Context): List<String> =
    try {
        val softBreak = Regex("""\n[ \t]*(?![*•-] |\(?\d+[.)] )""")
        context.assets.open("third_party_notices.txt").bufferedReader().use { it.readText() }
            .split(Regex("\n{2,}")).map { it.trim() }.filter { it.isNotEmpty() }
            .map { if (it.startsWith("=====")) it else it.replace(softBreak, " ") }
    } catch (e: Exception) {
        listOf("licence notices missing from this build: ${e.message}")
    }

/** Probes an already-local .ply/.spz (scene-list entries) into an import candidate. */
private suspend fun probeLocalFile(file: File, onStatus: (String?) -> Unit): ImportCandidate? =
    withContext(Dispatchers.IO) {
        val probe = SplatEngine.nativeProbeScene(file.absolutePath)
        if (probe == null) {
            onStatus("cannot read '${file.name}' - see logcat")
            null
        } else {
            onStatus(null)
            ImportCandidate(file, file.name, probe[0], probe[1], deleteAfter = false)
        }
    }

@Composable
private fun ViewerScreen(
    engine: RenderEngine,
    refreshScenes: () -> List<File>,
    benchRequest: BenchRequest?,
    openSceneName: String?,
    protocolRes: Pair<Int, Int>?,
    filesDir: File,
    initialCoverageStrength: Float,
    initialFadeWhileMoving: Boolean,
    backsideRequest: Int = -1,
    backsideRequestFiles: List<File> = emptyList(),
) {
    var surfaceView by remember { mutableStateOf<SplatSurfaceView?>(null) }
    var sceneName by remember { mutableStateOf<String?>(null) }
    var sceneInfo by remember { mutableStateOf<FloatArray?>(null) }
    var currentFile by remember { mutableStateOf<File?>(null) }
    var stats by remember { mutableStateOf<FloatArray?>(null) }
    var openFailed by remember { mutableStateOf(false) }
    var openError by remember { mutableStateOf<String?>(null) }
    var levelOrbit by remember { mutableStateOf(false) }
    var coverageStrength by remember { mutableStateOf(initialCoverageStrength) }
    var fadeWhileMoving by remember { mutableStateOf(initialFadeWhileMoving) }
    // Hidden overlay = full-screen view: system bars hidden too (an edge swipe shows them for a
    // moment); a small knob in the corner brings both back.
    var overlayVisible by remember { mutableStateOf(true) }
    var benchRunning by remember { mutableStateOf(false) }
    var benchSeconds by remember { mutableStateOf(0) }
    var benchStartMs by remember { mutableLongStateOf(0L) }
    var nowMs by remember { mutableLongStateOf(0L) }
    var fpsEstimate by remember { mutableStateOf(0f) }
    var benchMessage by remember { mutableStateOf<String?>(null) }

    var sceneList by remember { mutableStateOf(refreshScenes()) }
    var importCandidate by remember { mutableStateOf<ImportCandidate?>(null) }
    var staging by remember { mutableStateOf(false) }       // SAF copy + probe in flight
    var importRunning by remember { mutableStateOf(false) } // native import in flight
    var importStatus by remember { mutableStateOf<String?>(null) }
    var deepClean by remember { mutableStateOf(false) }
    var budget by remember { mutableLongStateOf(1_000_000L) }
    var stagingJob by remember { mutableStateOf<Job?>(null) }
    var showLicences by remember { mutableStateOf(false) }
    // Backside info (ADR-0015): -1 off, 0 capture camera files, 1 room, 2 object.
    var backside by remember { mutableStateOf(-1) }
    var cameraFiles by remember { mutableStateOf<List<File>>(emptyList()) }
    var cameraTarget by remember { mutableStateOf<File?>(null) } // open pack the picked cameras are for
    var backsideBusy by remember { mutableStateOf(false) }
    var backsideMessage by remember { mutableStateOf<String?>(null) }

    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val prefs = remember { context.getSharedPreferences(kPrefs, Context.MODE_PRIVATE) }
    fun markSampleInstalled() = prefs.edit().putBoolean(kSampleInstalled, true).apply()

    /** Back to the scene list; the open scene keeps rendering behind it until another one opens. */
    fun showSceneList() {
        sceneInfo = null
        sceneName = null
        currentFile = null
        stats = null
        sceneList = refreshScenes()
    }

    fun openPack(file: File) {
        openFailed = false
        engine.openScene(file.absolutePath) { result ->
            sceneInfo = result
            sceneName = if (result != null) file.name else null
            currentFile = if (result != null) file else null
            openFailed = result == null
            openError = if (result == null) "${file.name}: ${engine.lastOpenError ?: "see logcat"}" else null
        }
    }

    val openDoc = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            importStatus = null
            staging = true
            val job = scope.launch {
                importCandidate = stageSafPick(context, uri) { importStatus = it }
            }
            job.invokeOnCompletion { cause ->
                staging = false
                if (cause is CancellationException) importStatus = "cancelled"
            }
            stagingJob = job
        }
    }

    /** Computes the backside sidecar of [file] in the background, then (re)opens it. */
    fun computeBackside(file: File, source: Int, files: List<File>, keepFiles: Boolean = false) {
        backsideBusy = true
        backsideMessage = "computing backside info…"
        scope.launch {
            val result = withContext(Dispatchers.IO) {
                SplatEngine.nativeComputeCoverage(file.absolutePath, source, files.map { it.absolutePath }.toTypedArray())
            }
            if (!keepFiles) files.forEach { it.delete() }
            if (source == 0 && !keepFiles) cameraFiles = emptyList()
            backsideBusy = false
            backsideMessage = if (result.startsWith("ok")) "backside: ${result.removePrefix("ok: ")}"
            else "backside info failed: $result"
            openPack(file)
        }
    }

    val pickCameras = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        val target = cameraTarget
        if (uris.isEmpty()) {
            if (target == null && cameraFiles.isEmpty()) backside = -1
            return@rememberLauncherForActivityResult
        }
        scope.launch {
            val files = stageCameraFiles(context, uris) { backsideMessage = it }
            if (target != null) {
                cameraTarget = null
                if (files.isNotEmpty()) computeBackside(target, 0, files)
            } else {
                cameraFiles = files
                if (files.isEmpty()) backside = -1
            }
        }
    }

    fun startImport(candidate: ImportCandidate) {
        val scenesDir = File(filesDir, "scenes").apply { mkdirs() }
        val dst = File(scenesDir, sanitizeName(candidate.displayName.substringBeforeLast('.')) + ".pspk")
        importRunning = true
        importStatus = "starting import…"
        scope.launch {
            val poll = launch {
                while (true) {
                    val stage = SplatEngine.nativeImportStage()
                    if (stage >= 0) importStatus = importStageLabel(stage)
                    delay(250)
                }
            }
            val error = withContext(Dispatchers.IO) {
                SplatEngine.nativeImportScene(
                    candidate.src.absolutePath, dst.absolutePath, 1, deepClean, budget,
                )
            }
            poll.cancel()
            importRunning = false
            if (error.isEmpty()) {
                if (candidate.isSample) markSampleInstalled()
                if (candidate.deleteAfter) candidate.src.delete()
                importCandidate = null
                importStatus = null
                sceneList = refreshScenes()
                val kind = backside
                backside = -1
                if (kind >= 0) computeBackside(dst, kind, if (kind == 0) cameraFiles else emptyList()) else openPack(dst)
            } else {
                importStatus = error // candidate (and any cache copy) stays for a retry
            }
        }
    }

    fun runBench(
        file: File, seconds: Int, keyBits: Int, minContribution: Float, targetFps: Float,
        resolution: Pair<Int, Int>?, drawMode: Int = 3, controller: Boolean = false, voteFps: Boolean = false,
        homeSwing: Boolean = false,
    ) {
        benchMessage = null
        engine.openScene(file.absolutePath) { info ->
            sceneInfo = info
            sceneName = if (info != null) file.name else null
            currentFile = if (info != null) file else null
            openFailed = info == null
            if (info == null) return@openScene
            val csv = File(File(filesDir, "bench").apply { mkdirs() },
                "bench-${file.nameWithoutExtension}-${keyBits}bit.csv")
            csv.delete() // the native side writes the fresh one atomically on stop
            // apk = versionName, which carries the build's git hash (app/build.gradle.kts): bench
            // files are labelled by what ran, not by the checkout the driver script runs from.
            val apk = context.packageManager.getPackageInfo(context.packageName, 0).versionName
            val meta = "model=${Build.MODEL}, soc=${Build.SOC_MODEL}, android=${Build.VERSION.RELEASE}, apk=$apk"
            val start: () -> Unit = {
                benchRunning = true
                benchSeconds = seconds
                benchStartMs = SystemClock.elapsedRealtime()
                engine.startBench(
                    csv.absolutePath, meta, file.name, keyBits, minContribution, targetFps, drawMode, controller,
                    voteFps, homeSwing, seconds,
                ) { summary ->
                    benchRunning = false
                    benchMessage = summary ?: "bench failed - see logcat"
                    surfaceView?.setInternalSize(null) // back to the interactive scale
                }
            }
            val view = surfaceView
            if (resolution != null && view != null) view.setInternalSize(resolution, start) else start()
        }
    }

    LaunchedEffect(Unit) {
        if (benchRequest != null) {
            val file = sceneList.find { it.name == benchRequest.sceneName }
            if (file == null) {
                benchMessage = "bench scene not found: ${benchRequest.sceneName}"
            } else {
                runBench(file, benchRequest.seconds, benchRequest.keyBits, benchRequest.minContribution,
                    benchRequest.targetFps, benchRequest.resolution ?: protocolRes, benchRequest.drawMode,
                    benchRequest.controller, benchRequest.voteFps, benchRequest.homeSwing)
            }
        } else if (openSceneName != null) {
            sceneList.find { it.name == openSceneName }?.let {
                // Scripted: the camera files stay where the caller put them (computeBackside
                // deletes only its own staged copies).
                if (backsideRequest >= 0) computeBackside(it, backsideRequest, backsideRequestFiles, keepFiles = true)
                else openPack(it)
            }
        } else if (!prefs.getBoolean(kSampleInstalled, false)) {
            // First launch: import the bundled sample (same pipeline and defaults as a picked file)
            // and open it. Scripted launches above skip this, so bench runs never import.
            importStatus = "preparing the sample scene…"
            staging = true
            val job = scope.launch {
                val candidate = stageSample(context) { importStatus = it }
                staging = false
                if (candidate != null) startImport(candidate)
            }
            job.invokeOnCompletion { staging = false }
            stagingJob = job
        }
    }

    BackHandler(enabled = showLicences || (sceneInfo != null && !benchRunning)) {
        if (showLicences) showLicences = false else showSceneList()
    }

    LaunchedEffect(sceneInfo) {
        var lastFrames = 0f
        var lastAtMs = 0L
        while (sceneInfo != null) {
            engine.requestStats { frame ->
                val at = SystemClock.elapsedRealtime()
                if (lastAtMs > 0 && at > lastAtMs) {
                    fpsEstimate = (frame[7] - lastFrames) * 1000f / (at - lastAtMs)
                }
                lastFrames = frame[7]
                lastAtMs = at
                stats = frame
                nowMs = at
            }
            delay(500)
        }
    }

    LaunchedEffect(overlayVisible) {
        val controller = (context as? Activity)?.window?.insetsController ?: return@LaunchedEffect
        if (overlayVisible) {
            controller.show(WindowInsets.Type.systemBars())
        } else {
            controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsets.Type.systemBars())
        }
    }

    Box(Modifier.fillMaxSize()) {
        AndroidView(
            factory = { viewContext -> SplatSurfaceView(viewContext, engine).also { surfaceView = it } },
            Modifier.fillMaxSize(),
        )
        if (showLicences) {
            LicencesScreen(context) { showLicences = false }
            return@Box
        }
        if (!overlayVisible) {
            Box(
                Modifier
                    .safeDrawingPadding()
                    .padding(12.dp)
                    .size(36.dp)
                    .clip(CircleShape)
                    .background(Color(0x55000000))
                    .clickable { overlayVisible = true },
                contentAlignment = Alignment.Center,
            ) { Text("i", color = Color(0xCCFFFFFF)) }
            return@Box
        }
        Column(
            Modifier
                .safeDrawingPadding()
                .padding(12.dp)
                .verticalScroll(rememberScrollState()) // a long scene list must not push the buttons off-screen
                .background(Color(0x99000000))
                .padding(8.dp),
        ) {
            val info = sceneInfo
            val frame = stats
            if (info == null) {
                Text("PocketSplat", color = Color.White, style = MaterialTheme.typography.titleMedium)
                val candidate = importCandidate
                if (staging || importRunning) {
                    Text(importStatus ?: "working…", color = Color.Yellow)
                    TextButton(onClick = {
                        if (staging) stagingJob?.cancel() else SplatEngine.nativeImportCancel()
                    }) { Text("Cancel", color = Color.Red) }
                } else if (candidate != null) {
                    val mb = candidate.src.length() shr 20
                    Text(
                        "${candidate.displayName}\n${candidate.count} splats, SH ${candidate.shDegree}, $mb MB",
                        color = Color.White,
                    )
                    Row {
                        Text("Budget: ", modifier = Modifier.padding(top = 12.dp), color = Color.White)
                        listOf(500_000L to "500K", 1_000_000L to "1M", 2_000_000L to "2M", 0L to "all")
                            .forEach { (value, label) ->
                                TextButton(onClick = { budget = value }) {
                                    Text(label, color = if (budget == value) Color.Yellow else Color.Cyan)
                                }
                            }
                    }
                    TextButton(onClick = { deepClean = !deepClean }) {
                        Text("Deep clean (floater removal): ${if (deepClean) "on" else "off"}",
                            color = Color.Cyan)
                    }
                    // Backside info: lets the fade hide what the capture never saw from the viewer's side.
                    Text("Backside fade (see through what the capture never saw):", color = Color.White)
                    Row {
                        listOf(-1 to "off", 1 to "guess: room", 2 to "guess: object").forEach { (value, label) ->
                            TextButton(onClick = { backside = value }) {
                                Text(label, color = if (backside == value) Color.Yellow else Color.Cyan)
                            }
                        }
                    }
                    TextButton(onClick = {
                        backside = 0
                        cameraTarget = null
                        pickCameras.launch(arrayOf("*/*"))
                    }) { Text("from capture camera files…", color = if (backside == 0) Color.Yellow else Color.Cyan) }
                    if (backside == 0) {
                        Text(
                            if (cameraFiles.isEmpty()) "pick cameras.json, transforms.json, or COLMAP cameras + images"
                            else cameraFiles.joinToString { it.name },
                            color = Color.White,
                        )
                    } else if (backside > 0) {
                        Text(kBacksideTypeHint, color = Color(0xFFBBBBBB), style = MaterialTheme.typography.bodySmall)
                    }
                    Row {
                        TextButton(onClick = { startImport(candidate) }) {
                            Text("Import", color = Color.Green)
                        }
                        TextButton(onClick = {
                            if (candidate.isSample) markSampleInstalled()
                            if (candidate.deleteAfter) candidate.src.delete()
                            importCandidate = null
                            importStatus = null
                        }) { Text("Dismiss", color = Color.Red) }
                    }
                    importStatus?.let { Text(it, color = Color.Yellow) }
                } else {
                    if (sceneList.isEmpty()) {
                        Text(
                            "No scenes yet. Open a downloaded .ply/.spz below,\nor push files into the app's files/scenes dir (README).",
                            color = Color.White,
                        )
                    } else {
                        Text("Pick a scene:", color = Color.White)
                        sceneList.forEach { file ->
                            TextButton(onClick = {
                                if (file.extension.lowercase() == "pspk") {
                                    openPack(file)
                                } else {
                                    // Convert once in the background; the .pspk lands in the list.
                                    importStatus = null
                                    staging = true
                                    val job = scope.launch {
                                        importCandidate = probeLocalFile(file) { importStatus = it }
                                    }
                                    job.invokeOnCompletion { staging = false }
                                    stagingJob = job
                                }
                            }) { Text(file.name, color = Color.Cyan) }
                        }
                    }
                    TextButton(onClick = { openDoc.launch(arrayOf("*/*")) }) {
                        Text("Open PLY/SPZ file…", color = Color.Green)
                    }
                    TextButton(onClick = { showLicences = true }) {
                        Text("About & licences", color = Color.Cyan)
                    }
                    if (openFailed) Text("Open failed - ${openError ?: "see logcat."}", color = Color.Red)
                    importStatus?.let { Text(it, color = Color.Yellow) }
                    benchMessage?.let { Text(it, color = Color.Yellow) }
                }
            } else {
                Text(
                    "$sceneName - ${info[0].toInt()} splats, SH ${info[1].toInt()}",
                    color = Color.White,
                )
                if (frame != null) {
                    Text(
                        "gpu %.2f ms (cull %.2f, pre %.2f, sort %.2f, draw %.2f)"
                            .format(frame[0], frame[1], frame[2], frame[3], frame[4]),
                        color = Color.White,
                    )
                    Text(
                        "visible ${frame[5].toInt()} splats, ${frame[6].toInt()} chunks, frame ${frame[7].toInt()}",
                        color = Color.White,
                    )
                    if (!benchRunning) {
                        val rung = frame[9].toInt()
                        Text(
                            "cull %.1f px - ".format(frame[8]) +
                                if (rung >= 0) "adaptive quality, rung $rung" else "fixed",
                            color = Color.White,
                        )
                        TextButton(onClick = { engine.setAdaptiveQuality(rung < 0) }) {
                            Text("Adaptive quality: ${if (rung >= 0) "on" else "off"}", color = Color.Cyan)
                        }
                        TextButton(onClick = {
                            levelOrbit = !levelOrbit
                            engine.setLevelOrbit(levelOrbit)
                        }) {
                            Text("Orbit: ${if (levelOrbit) "level" else "free trackball"}", color = Color.Cyan)
                        }
                        val source = if (info.size >= 18 && info[16] > 0f) info[17].toInt() else -1
                        val isPack = currentFile?.extension?.lowercase() == "pspk"
                        val guessRoom = { currentFile?.let { computeBackside(it, 1, emptyList()) } }
                        val guessObject = { currentFile?.let { computeBackside(it, 2, emptyList()) } }
                        val useCameras = {
                            cameraTarget = currentFile
                            pickCameras.launch(arrayOf("*/*"))
                        }
                        if (source >= 0) {
                            Text("Backside fade %.2f - %s".format(coverageStrength, backsideLabel(source)), color = Color.White)
                            Slider(
                                value = coverageStrength,
                                onValueChange = {
                                    coverageStrength = it
                                    engine.setCoverageStrength(it)
                                },
                                modifier = Modifier.width(280.dp),
                            )
                            TextButton(onClick = {
                                fadeWhileMoving = !fadeWhileMoving
                                engine.setFadeWhileMoving(fadeWhileMoving)
                            }) {
                                Text("Fade: ${if (fadeWhileMoving) "while moving" else "always"}", color = Color.Cyan)
                            }
                            if (isPack && !backsideBusy) {
                                Row {
                                    Text("Try another guess: ", modifier = Modifier.padding(top = 12.dp), color = Color.White)
                                    TextButton(onClick = { guessRoom() }) { Text("room", color = Color.Cyan) }
                                    TextButton(onClick = { guessObject() }) { Text("object", color = Color.Cyan) }
                                    TextButton(onClick = { useCameras() }) { Text("cameras…", color = Color.Cyan) }
                                }
                            }
                        } else if (isPack && !backsideBusy) {
                            Text("Backside fade: see through what the capture never saw", color = Color.White)
                            Text(
                                "Not set up for this scene. Guess it from how it was captured:",
                                color = Color(0xFFBBBBBB), style = MaterialTheme.typography.bodySmall,
                            )
                            TextButton(onClick = { guessRoom() }) {
                                Text("Guess: room (walked through)", color = Color.Green)
                            }
                            TextButton(onClick = { guessObject() }) {
                                Text("Guess: object (went around it)", color = Color.Green)
                            }
                            TextButton(onClick = { useCameras() }) {
                                Text("Use capture camera files…", color = Color.Cyan)
                            }
                        }
                        if (backsideBusy) Text("Backside fade: computing…", color = Color.Yellow)
                        backsideMessage?.let { Text(it, color = Color.Yellow) }
                        Row {
                            TextButton(onClick = { engine.resetHome() }) { Text("Home view", color = Color.Cyan) }
                            TextButton(onClick = { overlayVisible = false }) { Text("Hide overlay", color = Color.Cyan) }
                            TextButton(onClick = { showSceneList() }) { Text("Scenes", color = Color.Cyan) }
                        }
                    }
                }
                if (benchRunning) {
                    val elapsed = ((nowMs - benchStartMs) / 1000L).coerceAtLeast(0)
                    Text(
                        "bench %d:%02d / %d:%02d - %.1f fps"
                            .format(elapsed / 60, elapsed % 60, benchSeconds / 60, benchSeconds % 60, fpsEstimate),
                        color = Color.Yellow,
                    )
                    TextButton(onClick = { engine.stopBench() }) { Text("Stop bench", color = Color.Red) }
                } else {
                    benchMessage?.let { Text(it, color = Color.Yellow) }
                    currentFile?.let { file ->
                        TextButton(onClick = { runBench(file, 600, 32, 0f, 0f, protocolRes) }) {
                            Text("Benchmark 10 min", color = Color.Cyan)
                        }
                    }
                }
            }
        }
    }
}

/** Version, source link and the licence texts of everything shipped in the APK. */
@Composable
private fun LicencesScreen(context: Context, onBack: () -> Unit) {
    val paragraphs = remember { loadNotices(context) }
    val version = remember { context.packageManager.getPackageInfo(context.packageName, 0).versionName }
    Column(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .safeDrawingPadding()
            .padding(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("< Back", color = Color.Cyan) }
            Text("PocketSplat $version", color = Color.White, style = MaterialTheme.typography.titleMedium)
        }
        Text(
            "Source, method and measurements: github.com/Artemarius/pocketsplat",
            color = Color.White,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        LazyColumn(Modifier.fillMaxSize()) {
            items(paragraphs) { paragraph ->
                val rule = paragraph.startsWith("=====")
                // Section banners ("=====" / title / "=====") show as just the title.
                Text(
                    if (rule) paragraph.lines().filterNot { it.startsWith("=====") }.joinToString(" ") else paragraph,
                    color = if (rule) Color.Cyan else Color(0xFFDDDDDD),
                    style = if (rule) MaterialTheme.typography.titleSmall else MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(vertical = 4.dp),
                )
            }
        }
    }
}
