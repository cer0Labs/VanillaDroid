plugins {
    id("com.android.application")
}

// Optional: -PvdDepsDir=<dir> builds the native code against C++ dependencies that are already on disk, as
// <dir>/<name>-src (the FetchContent sources of a desktop build, e.g. "<repo>/build/windows/_deps"), instead
// of downloading them again into a fresh .cxx folder. Without the property nothing changes.
val vdDepsDir: String? = providers.gradleProperty("vdDepsDir").orNull
val vdDepsArguments: List<String> = vdDepsDir?.let { dir ->
    val arguments = mutableListOf("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
    for (name in listOf("vulkan_headers", "volk", "vma", "imgui", "lz4", "glm", "mbedtls", "miniaudio", "tracy")) {
        val source = file(dir).resolve("$name-src")
        if (name == "tracy" && !source.isDirectory) {
            continue // only the profile variant needs it
        }
        arguments += "-DFETCHCONTENT_SOURCE_DIR_${name.uppercase()}=${source.invariantSeparatorsPath}"
    }
    arguments
} ?: emptyList()

// Phase 22 (distribution, ADR-0005): -PvdVersion=X.Y.Z (the release's git tag vX.Y.Z; 0.1.0 in development) sets
// versionName, versionCode (X * 1,000,000 + Y * 1,000 + Z) and the native VD_VERSION (core/Version.h).
val vdVersion: String = providers.gradleProperty("vdVersion").orNull ?: "0.1.0"
val vdVersionCode: Int = vdVersion.split(".").let { parts ->
    require(parts.size == 3) { "vdVersion must be X.Y.Z, not '$vdVersion'" }
    parts[0].toInt() * 1_000_000 + parts[1].toInt() * 1_000 + parts[2].toInt()
}
// The release key of the dist build lives outside the repository: VD_KEYSTORE (path), VD_KEYSTORE_PASSWORD,
// VD_KEY_ALIAS and VD_KEY_PASSWORD from the environment (the CI's secrets) or ~/.gradle/gradle.properties. Without
// them dist is signed with the debug key: a local test build that cannot update a published one.
fun vdSecret(name: String): String? = System.getenv(name) ?: providers.gradleProperty(name).orNull
val vdKeystore: String? = vdSecret("VD_KEYSTORE")
// Phase 22 (apppack, ADR-0005 §2): app.vdpk (our texts, art and font: vd_baker bake --only app, without the WoW) goes
// into the APK's assets from this generated folder (not in git); scripts/android.ps1 bakes it there before calling
// Gradle. The app copies it to its internal storage (apps/android/native/AppPackInstall.cpp).
val vdAssetsDir = layout.buildDirectory.dir("generated/vdassets")

android {
    namespace = "dev.cer.vanilladroid"
    compileSdk = 35
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "dev.cer.vanilladroid"
        minSdk = 30
        targetSdk = 35
        versionCode = vdVersionCode
        versionName = vdVersion
        // The development builds; dist is "VanillaDroid" (both can be installed side by side: other applicationId).
        manifestPlaceholders["appLabel"] = "VanillaDroid Dev"
        // Phase 22 (updates): plain HTTP only in development (VD_UPDATE_URL: a release folder served on the LAN); dist
        // reads its releases over HTTPS alone.
        manifestPlaceholders["vdCleartext"] = "true"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                // GameActivity's prefab package is built against the shared STL.
                arguments += listOf("-DANDROID_STL=c++_shared")
                arguments += listOf("-DVD_VERSION=$vdVersion")
                arguments += vdDepsArguments
            }
        }
    }

    signingConfigs {
        if (vdKeystore != null) {
            create("vdRelease") {
                storeFile = file(vdKeystore)
                storePassword = vdSecret("VD_KEYSTORE_PASSWORD")
                keyAlias = vdSecret("VD_KEY_ALIAS")
                keyPassword = vdSecret("VD_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Personal sideloading only: sign release builds with the debug key.
            signingConfig = signingConfigs.getByName("debug")
        }
        // Release + Tracy profiler client (on-demand, port 8086). Use it to profile; use plain
        // release for final numbers.
        create("profile") {
            initWith(getByName("release"))
            matchingFallbacks += listOf("release")
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DVD_ENABLE_TRACY=ON")
                }
            }
        }
        // Phase 22: what is published (ADR-0005): its own applicationId (androidComponents below), the release key,
        // the hardened native build (VD_DIST: no ImGui panels, galleries or VD_* debug switches).
        create("dist") {
            initWith(getByName("release"))
            matchingFallbacks += listOf("release")
            signingConfig = signingConfigs.getByName(if (vdKeystore != null) "vdRelease" else "debug")
            manifestPlaceholders["appLabel"] = "VanillaDroid"
            manifestPlaceholders["vdCleartext"] = "false" // phase 22 (updates)
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DVD_DIST=ON")
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            // The whole engine is built from the repository root CMake project.
            path = file("../../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefab = true
    }

    sourceSets {
        getByName("main") {
            assets.srcDir(vdAssetsDir) // phase 22 (apppack): app.vdpk
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

androidComponents {
    // Phase 22: the published app is com.cer0labs.vanilladroid; the development builds stay dev.cer.vanilladroid
    // (scripts/, docs/ANDROID.md), so both live on the test phones without uninstalling (which deletes the packs).
    onVariants(selector().withBuildType("dist")) { variant ->
        variant.applicationId.set("com.cer0labs.vanilladroid")
    }
}

// Phase 22 (apppack): without app.vdpk the game shows the keys of our texts and none of our icons: the dist APK refuses
// to build, the development ones warn.
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("Assets") && !name.contains("Test")) {
        val isDist = name.contains("Dist")
        doFirst {
            val pack = vdAssetsDir.get().file("app.vdpk").asFile
            if (!pack.isFile) {
                val message = "app.vdpk is missing from ${pack.parentFile}: bake it there with " +
                    "'vd_baker bake --only app --out <that folder>' (scripts/android.ps1 does it)"
                if (isDist) {
                    throw GradleException(message)
                }
                logger.warn("warning: $message")
            }
        }
    }
}

dependencies {
    // games-activity drags kotlin-stdlib-jdk7/jdk8 1.6 while androidx pulls kotlin-stdlib 1.8,
    // which duplicates classes; the BOM aligns them all.
    implementation(platform("org.jetbrains.kotlin:kotlin-bom:1.8.22"))
    implementation("androidx.games:games-activity:4.4.2")
    implementation("androidx.games:games-frame-pacing:2.1.3")
    // Paddleboat: game controllers (native library + its Java GameControllerManager).
    implementation("androidx.games:games-controller:2.0.2")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.core:core:1.13.1")
}

// Phase 22 (phonebake, ROADMAP 22.7, ADR-0005 §8): the phone bakes its own packs with the same console vd_baker the PC
// runs, built for Android (VD_ANDROID_BAKER: the baker alone, libc++ static) and shipped as
// lib/arm64-v8a/libvdbaker.so: an executable named like a library. The installer extracts it into the app's native
// library folder, the one place an app may still run a program of its own from (Android 10+ forbids running files of
// its writable folders, W^X); that needs the libraries stored compressed and extracted at install
// (useLegacyPackaging, android:extractNativeLibs="true"; the game's .so then takes its room twice on the phone, a few
// MB). BakeService (process :baker) runs it with ProcessBuilder. A second CMake configuration of the repository in
// .cxx/vdbaker (one build for every variant; the CI keeps its dependency sources with the game's), with the SDK's
// CMake 3.22.1 and Ninja and this module's NDK; the C++ dependencies from -PvdDepsDir, else this checkout's desktop
// build when it has their sources, else CMake downloads them.
abstract class VdBakerTask : DefaultTask() {
    @get:javax.inject.Inject abstract val exec: org.gradle.process.ExecOperations
    @get:Internal abstract val sdkDir: DirectoryProperty
    @get:Internal abstract val ndkDir: DirectoryProperty
    @get:Internal abstract val sourceDir: DirectoryProperty
    @get:Internal abstract val cmakeBuildDir: DirectoryProperty
    @get:Internal abstract val depsDir: Property<String>
    @get:Internal abstract val version: Property<String>
    @get:OutputDirectory abstract val outputDir: DirectoryProperty

    @TaskAction
    fun build() {
        val windows = System.getProperty("os.name").lowercase().contains("windows")
        val suffix = if (windows) ".exe" else ""
        val cmakeBin = sdkDir.get().asFile.resolve("cmake/3.22.1/bin")
        val cmake = cmakeBin.resolve("cmake$suffix")
        if (!cmake.isFile) {
            throw GradleException("The SDK's CMake 3.22.1 is missing: $cmake (sdkmanager \"cmake;3.22.1\")")
        }
        val ndk = ndkDir.get().asFile
        val build = cmakeBuildDir.get().asFile
        val configure = mutableListOf(cmake.path, "-S", sourceDir.get().asFile.path, "-B", build.path, "-G", "Ninja",
            "-DCMAKE_MAKE_PROGRAM=${cmakeBin.resolve("ninja$suffix").invariantSeparatorsPath}",
            "-DCMAKE_TOOLCHAIN_FILE=${ndk.resolve("build/cmake/android.toolchain.cmake").invariantSeparatorsPath}",
            "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=30", "-DANDROID_STL=c++_static",
            "-DCMAKE_BUILD_TYPE=Release", "-DVD_ANDROID_BAKER=ON", "-DVD_VERSION=${version.get()}")
        depsDir.orNull?.let { dir ->
            for (name in listOf("vulkan_headers", "volk", "vma", "imgui", "lz4", "glm", "mbedtls", "miniaudio",
                                "stormlib", "astcenc", "qrcodegen")) {
                val source = File(dir, "$name-src")
                if (source.isDirectory) { // what is missing is downloaded
                    configure += "-DFETCHCONTENT_SOURCE_DIR_${name.uppercase()}=${source.invariantSeparatorsPath}"
                }
            }
        }
        exec.exec { commandLine(configure) }
        exec.exec { commandLine(cmake.path, "--build", build.path, "--target", "vd_baker") }
        // Without its debug information (about 46 -> 2 MB); the built one keeps it, for reading a crash's addresses.
        val prebuilt = ndk.resolve("toolchains/llvm/prebuilt").listFiles()?.firstOrNull { it.isDirectory }
            ?: throw GradleException("No LLVM toolchain in $ndk")
        val abiDir = outputDir.get().asFile.resolve("arm64-v8a")
        abiDir.mkdirs()
        exec.exec {
            commandLine(prebuilt.resolve("bin/llvm-strip$suffix").path, "--strip-all", "-o",
                        abiDir.resolve("libvdbaker.so").path, build.resolve("tools/baker/vd_baker").path)
        }
    }
}

android {
    packaging {
        jniLibs {
            useLegacyPackaging = true // phase 22 (phonebake): libvdbaker.so must be a file on the phone
        }
    }
}

androidComponents {
    onVariants { variant ->
        val task = tasks.register<VdBakerTask>("vdBaker${variant.name.replaceFirstChar { it.uppercase() }}") {
            sdkDir.set(androidComponents.sdkComponents.sdkDirectory)
            ndkDir.set(androidComponents.sdkComponents.ndkDirectory)
            sourceDir.set(layout.projectDirectory.dir("../../.."))
            cmakeBuildDir.set(layout.projectDirectory.dir(".cxx/vdbaker"))
            val checkoutDeps = layout.projectDirectory.dir("../../../build/windows/_deps").asFile
            depsDir.set(vdDepsDir ?: checkoutDeps.takeIf { it.resolve("stormlib-src").isDirectory }?.path)
            version.set(vdVersion)
            outputs.upToDateWhen { false } // Ninja knows what changed
        }
        variant.sources.jniLibs?.addGeneratedSourceDirectory(task, VdBakerTask::outputDir)
    }
}
