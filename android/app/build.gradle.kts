import java.io.File
import java.net.URI
import java.security.MessageDigest
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// The checkout this APK is built from, e.g. "ce02c09" or "ce02c09-dirty" (tracked files modified).
// Bench CSVs record it (apk=0.1-<hash>) and scripts/bench_android.py labels its files with it, so a
// file names the build that ran, not whatever the checkout is at when the driver script starts.
val gitHash: String = providers.exec {
    commandLine("git", "rev-parse", "--short", "HEAD")
    isIgnoreExitValue = true
}.standardOutput.asText.get().trim().ifEmpty { "unknown" }
val gitDirty: Boolean = providers.exec {
    commandLine("git", "status", "--porcelain", "--untracked-files=no")
    isIgnoreExitValue = true
}.standardOutput.asText.get().isNotBlank()

/**
 * Puts the bundled sample scene (assets/samples/hornedlizard.spz, imported on first launch) into a
 * generated assets dir. Scene files stay out of git (CLAUDE.md §2.4): the task takes a local copy
 * when one with the right hash exists (data/raw, or the root build's spz checkout), else downloads
 * the file from nianticlabs/spz at the tag cmake/deps.cmake pins. Either way the SHA-256 must match.
 */
abstract class SampleSceneTask : DefaultTask() {
    @get:Input abstract val url: Property<String>
    @get:Input abstract val sha256: Property<String>
    @get:Internal abstract val localCopies: ConfigurableFileCollection
    @get:Internal abstract val cacheFile: RegularFileProperty
    @get:OutputDirectory abstract val outputDir: DirectoryProperty

    private fun hashOf(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(1 shl 20)
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                digest.update(buffer, 0, n)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    @TaskAction
    fun run() {
        val cache = cacheFile.get().asFile
        if (!cache.isFile || hashOf(cache) != sha256.get()) {
            cache.parentFile.mkdirs()
            val local = localCopies.files.firstOrNull { it.isFile && hashOf(it) == sha256.get() }
            if (local != null) {
                local.copyTo(cache, overwrite = true)
            } else {
                logger.lifecycle("downloading ${url.get()}")
                URI(url.get()).toURL().openStream().use { input ->
                    cache.outputStream().use { input.copyTo(it) }
                }
                val got = hashOf(cache)
                if (got != sha256.get()) {
                    cache.delete()
                    throw GradleException("sample scene hash mismatch: got $got, want ${sha256.get()}")
                }
            }
        }
        val dir = outputDir.get().asFile.resolve("samples")
        dir.deleteRecursively()
        dir.mkdirs()
        cache.copyTo(dir.resolve("hornedlizard.spz"))
    }
}

val repoRoot = rootProject.layout.projectDirectory.dir("..")

// Release signing: android/keystore.properties (gitignored; keys storeFile, storePassword, keyAlias,
// keyPassword) or the POCKETSPLAT_KEYSTORE* environment variables. Without either, assembleRelease
// still builds and leaves an unsigned APK. android/README.md has the one-time keytool command.
val signingProps = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.isFile) file.inputStream().use { load(it) }
    System.getenv("POCKETSPLAT_KEYSTORE")?.let { setProperty("storeFile", it) }
    System.getenv("POCKETSPLAT_KEYSTORE_PASSWORD")?.let { setProperty("storePassword", it) }
    System.getenv("POCKETSPLAT_KEY_ALIAS")?.let { setProperty("keyAlias", it) }
    System.getenv("POCKETSPLAT_KEY_PASSWORD")?.let { setProperty("keyPassword", it) }
}
val hasReleaseKey = listOf("storeFile", "storePassword", "keyAlias", "keyPassword")
    .all { !signingProps.getProperty(it).isNullOrEmpty() }

androidComponents {
    onVariants { variant ->
        val task = tasks.register<SampleSceneTask>("${variant.name}SampleScene") {
            url.set("https://raw.githubusercontent.com/nianticlabs/spz/v3.0.0/samples/hornedlizard.spz")
            sha256.set("a076ca6aeb9f1a8fe52526f4a7c381fe8fffec60c0d7eb64b45532189b64bc6a")
            localCopies.from(
                repoRoot.file("data/raw/hornedlizard.spz"),
                repoRoot.file("build/_deps/spz-src/samples/hornedlizard.spz"),
            )
            cacheFile.set(layout.buildDirectory.file("sample-cache/hornedlizard.spz"))
        }
        variant.sources.assets?.addGeneratedSourceDirectory(task, SampleSceneTask::outputDir)
    }
}

android {
    namespace = "com.pocketsplat.app"
    compileSdk = 36

    // NDK LTS (CLAUDE.md §3). The SDK's CMake 4.1.2 is required: the repository root needs >= 3.24.
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.pocketsplat.app"
        minSdk = 33
        targetSdk = 36
        versionCode = 2
        versionName = "0.2.0-$gitHash" + if (gitDirty) "-dirty" else ""  // hash: bench_android.py labels runs by it
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                // 16 KB page alignment: Android 16 warns on (and future devices require)
                // 16 KB-aligned LOAD segments; NDK r27 opts in via this toolchain flag.
                arguments("-DANDROID_STL=c++_static", "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON")
                targets("pocketsplat") // just the JNI lib, not every root-project target
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    signingConfigs {
        if (hasReleaseKey) {
            create("release") {
                storeFile = rootProject.file(signingProps.getProperty("storeFile"))
                storePassword = signingProps.getProperty("storePassword")
                keyAlias = signingProps.getProperty("keyAlias")
                keyPassword = signingProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            // R8 cuts the APK's Compose dex from ~17 MB to a fraction. JNI resolves the
            // SplatEngine natives by class + method name; the default rules keep classes with
            // native methods and those names (native code never calls back into Kotlin).
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            if (hasReleaseKey) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    // SPZ is gzip inside; storing it uncompressed in the APK costs nothing and lets it stream.
    androidResources {
        noCompress += "spz"
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.lifecycle.runtime.compose)
}
