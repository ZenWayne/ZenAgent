plugins {
    kotlin("jvm") version "2.1.0"
}

repositories {
    mavenCentral()
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.1")
    testImplementation("org.junit.jupiter:junit-jupiter:5.10.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.8.1")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

kotlin {
    jvmToolchain(17)
}

// Build the native libagentflow_jni.so via Bazel before tests run, then
// expose it on java.library.path so System.loadLibrary("agentflow_jni")
// finds it.
val nativeLibsDir = layout.buildDirectory.dir("libs/native")

val buildNativeLib = tasks.register<Exec>("buildNativeLib") {
    workingDir = rootDir.parentFile  // zen/ (Bazel workspace root)
    val proxy = listOf(
        "--host_jvm_args=-Dhttps.proxyHost=127.0.0.1",
        "--host_jvm_args=-Dhttps.proxyPort=10809",
        "--host_jvm_args=-Dhttp.proxyHost=127.0.0.1",
        "--host_jvm_args=-Dhttp.proxyPort=10809",
    )
    commandLine("bazel", *proxy.toTypedArray(), "build", "//jni:libagentflow_jni.so")
    doLast {
        val src = rootDir.parentFile.resolve("bazel-bin/jni/libagentflow_jni.so")
        val dst = nativeLibsDir.get().asFile.resolve("libagentflow_jni.so")
        dst.parentFile.mkdirs()
        src.copyTo(dst, overwrite = true)
        // libagentflow_jni.so dynamically links libkissfft-float.so.131 (LiteRT-LM
        // audio dep). Stage it next to the JNI lib so the dynamic loader finds it.
        val kissfftSrc = rootDir.parentFile.resolve(
            "third_party/litert_lm/lib/libkissfft-float.so.131")
        if (kissfftSrc.exists()) {
            kissfftSrc.copyTo(
                dst.parentFile.resolve("libkissfft-float.so.131"),
                overwrite = true,
            )
        }
    }
}

tasks.test {
    dependsOn(buildNativeLib)
    useJUnitPlatform()
    val nativeDir = nativeLibsDir.get().asFile.absolutePath
    systemProperty("java.library.path", nativeDir)
    // LD_LIBRARY_PATH so the dynamic loader resolves libkissfft-float.so.131
    // that libagentflow_jni.so dlopens transitively. java.library.path alone
    // only helps with System.loadLibrary lookup, not transitive shared libs.
    environment("LD_LIBRARY_PATH", nativeDir)
    System.getenv("MODEL_PATH")?.let { environment("MODEL_PATH", it) }
    testLogging {
        events("passed", "skipped", "failed")
        showStandardStreams = true
    }
}
