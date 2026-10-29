/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm") version "2.2.0"
    `java-library`
}

group = "dev.curiositystack.a11"

version = rootDir.resolve("../VERSION").let { if (it.exists()) it.readText().trim() else "0.0.0" }

repositories { mavenCentral() }

dependencies {
    api("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    // Raw MessagePack so A11's concatenated-field framing is byte-controlled.
    implementation("org.msgpack:msgpack-core:0.9.8")

    testImplementation(kotlin("test"))
    testImplementation("org.junit.jupiter:junit-jupiter:5.11.3")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
}

kotlin {
    jvmToolchain(21)
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_21)
        // Java 21 virtual threads back the blocking transport edges.
        freeCompilerArgs.add("-Xjsr305=strict")
    }
}

tasks.test {
    useJUnitPlatform()
    systemProperty("a11.test.version", version.toString())
    val runtimeClasspath = sourceSets.test.get().runtimeClasspath
    doFirst { systemProperty("a11.test.classpath", runtimeClasspath.asPath) }
}

val nativeOs = if (System.getProperty("os.name").startsWith("Mac")) "macos" else "linux"
val nativeArch =
    when (System.getProperty("os.arch")) {
        "aarch64",
        "arm64" -> "aarch64"
        else -> "x86_64"
    }
val nativeLibraryName = System.mapLibraryName("a11_jvm")
val nativeLibrary = rootDir.resolve("../cmake-build-debug/cpp/$nativeLibraryName")
val nativePrefix =
    System.getenv("A11_DEPS_PREFIX")
        ?: System.getProperty("user.home") +
            "/.cache/a11-deps/" +
            if (nativeOs == "macos" && nativeArch == "aarch64") "arm64" else nativeArch
val nativeToolPath =
    providers.environmentVariable("PATH").orElse("").map { path ->
        (path.split(File.pathSeparator) +
                listOf("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"))
            .distinct()
            .joinToString(File.pathSeparator)
    }
val cmakeExecutable =
    providers
        .gradleProperty("a11.cmake")
        .orElse(providers.environmentVariable("A11_CMAKE"))
        .orElse(
            nativeToolPath.map { path ->
                path
                    .split(File.pathSeparator)
                    .map { directory -> File(directory, "cmake") }
                    .firstOrNull { candidate -> candidate.canExecute() }
                    ?.absolutePath
                    ?: error("Install CMake or set -Pa11.cmake=/absolute/path/to/cmake")
            }
        )

val configureNativeJvm =
    tasks.register<Exec>("configureNativeJvm") {
        workingDir(rootDir.resolve(".."))
        environment("A11_DEPS_PREFIX", nativePrefix)
        environment("JAVA_HOME", System.getProperty("java.home"))
        commandLine("cmake", "--preset", "debug", "-DA11_BUILD_JVM=ON")
        environment("PATH", nativeToolPath.get())
        inputs.property("nativePrefix", nativePrefix)
        doFirst { executable = cmakeExecutable.get() }
        inputs.files(
            rootDir.resolve("../CMakePresets.json"),
            rootDir.resolve("../cpp/CMakeLists.txt"),
            rootDir.resolve("../cpp/jvm/CMakeLists.txt"),
        )
        outputs.file(rootDir.resolve("../cmake-build-debug/CMakeCache.txt"))
    }
val buildNativeJvm =
    tasks.register<Exec>("buildNativeJvm") {
        dependsOn(configureNativeJvm)
        workingDir(rootDir.resolve(".."))
        commandLine("cmake", "--build", "--preset", "debug", "--target", "a11_jvm", "-j", "8")
        environment("PATH", nativeToolPath.get())
        doFirst { executable = cmakeExecutable.get() }
    }

tasks.processResources {
    dependsOn(buildNativeJvm)
    from(nativeLibrary) { into("a11/native/$nativeOs-$nativeArch") }
}

// Match the published Google Kotlin guide (four spaces and 100 columns).
val kotlinFormatter = configurations.create("kotlinFormatter")

dependencies { add(kotlinFormatter.name, "com.facebook:ktfmt:0.54") }

fun kotlinFormatFiles(): List<String> =
    (listOf(rootDir.resolve("../settings.gradle.kts"), rootDir.resolve("../build.gradle.kts")) +
            fileTree(projectDir) { include("src/**/*.kt", "*.gradle.kts") }.files +
            fileTree(rootDir.resolve("../examples/010-websocket-echo-kotlin")) {
                    include("src/**/*.kt", "*.gradle.kts")
                }
                .files)
        .sortedBy { it.path }
        .map { it.absolutePath }

tasks.register<JavaExec>("kotlinFormat") {
    group = "formatting"
    description = "Format Kotlin using the shared Google Kotlin conventions."
    classpath = kotlinFormatter
    mainClass.set("com.facebook.ktfmt.cli.Main")
    doFirst { args = listOf("--kotlinlang-style") + kotlinFormatFiles() }
}

tasks.register<JavaExec>("kotlinFormatCheck") {
    group = "verification"
    description = "Check Kotlin formatting without changing files."
    classpath = kotlinFormatter
    mainClass.set("com.facebook.ktfmt.cli.Main")
    doFirst {
        args =
            listOf("--kotlinlang-style", "--dry-run", "--set-exit-if-changed") + kotlinFormatFiles()
    }
}

tasks.check { dependsOn("kotlinFormatCheck") }
