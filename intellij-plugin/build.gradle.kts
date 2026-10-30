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

import org.jetbrains.intellij.platform.gradle.IntelliJPlatformType
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm") version "2.2.0"
    // IntelliJ Platform Gradle Plugin 2.x.
    id("org.jetbrains.intellij.platform") version "2.19.0"
}

group = providers.gradleProperty("pluginGroup").get()
version = providers.gradleProperty("pluginVersion").get()

repositories {
    mavenCentral()
    intellijPlatform {
        defaultRepositories()
    }
}

dependencies {
    intellijPlatform {
        // The compile and `runIde` target. The plugin depends only on
        // `com.intellij.modules.platform`, so it remains installable in other
        // JetBrains IDEs.
        create(
            IntelliJPlatformType.CLion,
            providers.gradleProperty("platformVersion").get(),
        )
        // No bundled plugins: everything used lives in the platform itself, and
        // a product-bundled dependency here is exactly what would make the
        // plugin uninstallable in IDEs that do not ship it.
        bundledPlugins(emptyList<String>())

        pluginVerifier()
        zipSigner()
        testFramework(org.jetbrains.intellij.platform.gradle.TestFrameworkType.Platform)
    }

    // The A11 Kotlin compatibility layer (composite build). Exclude its
    // transitive kotlinx-coroutines: the IntelliJ Platform ships its own
    // *patched* coroutines (with a custom EDT MainDispatcher). A second,
    // unpatched copy on the runtime/test classpath shadows it and breaks the
    // platform's coroutine dispatch — which manifests as the platform test
    // harness spinning forever in teardown (checkEditorsReleased). Coroutines
    // are provided by the platform at runtime, so we only need them to compile.
    implementation("dev.curiositystack.a11:a11-kotlin") {
        exclude(group = "org.jetbrains.kotlinx", module = "kotlinx-coroutines-core")
        exclude(group = "org.jetbrains.kotlinx", module = "kotlinx-coroutines-core-jvm")
    }
    compileOnly("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")

    testImplementation("junit:junit:4.13.2")
}

kotlin {
    jvmToolchain(21)
    compilerOptions { jvmTarget.set(JvmTarget.JVM_21) }
}

intellijPlatform {
    instrumentCode = false

    // CLion 2026.1 cannot run `traverseUI`, which is what this task does: CLion
    // overrides the starter to relaunch itself "with the Radler language
    // plugin" and builds the child command line with a `-D` flag where the
    // executable should be, so it dies in a second with Cannot run program
    // "-DactionSystem.update.actions.warn.dataRules.on.edt=false" from
    // CLionTraverseUIStarter — before any settings page is looked at, and
    // identically with or without this plugin's own extensions. Searchable
    // options only pre-index the Settings search field; every settings page
    // this plugin contributes is still there and still found by its name.
    // Re-enable when that starter is fixed, or when building against an IDE
    // that does not override traverseUI (`platformType=IC`).
    buildSearchableOptions = false

    pluginConfiguration {
        id = "dev.curiositystack.a11.clion"
        name = providers.gradleProperty("pluginName")
        version = providers.gradleProperty("pluginVersion")
        ideaVersion {
            sinceBuild = providers.gradleProperty("pluginSinceBuild")
            untilBuild = provider { null } // no upper bound; verified via pluginVerifier
        }
    }

    // Verify the "runs in any IDE" claim rather than asserting it: the verifier
    // checks the built plugin against the recommended IDE set for the declared
    // compatibility range, which spans products, not just CLion releases.
    pluginVerification {
        ides {
            recommended()
        }
    }
}

// Builds the JCEF webview bundle (chat + action explorer) into plugin
// resources.
//
// The UI itself is `../webview`, shared with the VSCode extension: the same
// chat, action explorer, conversation list and markdown renderer, behind the
// six-method bridge in `webview/src/bridge.ts`. This module provides the JCEF
// bridge entry point and bundling configuration.
//
// Three installs and a build, because the shared package owns the UI's own
// dependencies (`marked`, the TypeScript A11 client) and esbuild resolves them
// from where the importing file lives. Requires Node.js >= 20.
val buildWebview by tasks.registering(Exec::class) {
    group = "build"
    description = "Build the JCEF webview bundle into src/main/resources/webview/app.js."
    workingDir = layout.projectDirectory.asFile
    inputs.dir(layout.projectDirectory.dir("webview/src"))
    inputs.file(layout.projectDirectory.file("webview/package.json"))
    inputs.dir(layout.projectDirectory.dir("../webview/src"))
    inputs.file(layout.projectDirectory.file("../webview/package.json"))
    inputs.file(layout.projectDirectory.file("../webview/index.html"))
    inputs.dir(layout.projectDirectory.dir("../js/src"))
    inputs.file(layout.projectDirectory.file("../js/package.json"))
    outputs.file(layout.projectDirectory.file("src/main/resources/webview/app.js"))
    outputs.file(layout.projectDirectory.file("src/main/resources/webview/index.html"))
    // Install dependencies only when missing (first build / CI); otherwise just
    // run the local, offline builds so the task never needs the network.
    //
    // `index.html` is copied rather than duplicated: it carries the theme
    // variables both hosts substitute into, so it belongs with the UI it styles
    // and is placed here because `A11WebView` loads it off the classpath.
    commandLine(
        "bash", "-c",
        "[ -d ../js/node_modules ] || npm --prefix ../js ci; npm --prefix ../js run build && " +
            "{ [ -d ../webview/node_modules ] || npm --prefix ../webview ci; }; " +
            "{ [ -d webview/node_modules ] || npm --prefix webview ci; }; " +
            "npm --prefix webview run build && " +
            "mkdir -p src/main/resources/webview && " +
            "cp ../webview/index.html src/main/resources/webview/index.html",
    )
}

val flowToolPlatform = run {
    val os = System.getProperty("os.name").lowercase().let {
        when {
            it.startsWith("mac") -> "macos"
            it.startsWith("windows") -> "windows"
            else -> "linux"
        }
    }
    val arch = when (System.getProperty("os.arch").lowercase()) {
        "aarch64", "arm64" -> "aarch64"
        else -> "x86_64"
    }
    "$os-$arch"
}
val flowToolName = if (flowToolPlatform.startsWith("windows")) "a11-flow.exe" else "a11-flow"
val flowToolBuildDirectory = providers.gradleProperty("flowToolBuildDir").map { file(it) }
    .orElse(providers.provider {
        listOf("../cmake-build-debug", "../cmake-build-release", "../build/debug", "../build/release")
            .map { file(it) }
            .firstOrNull { it.resolve("CMakeCache.txt").isFile }
            ?: file("../cmake-build-debug")
    })
val suppliedFlowTool = providers.gradleProperty("flowToolBinary").map { file(it) }
val builtFlowTool = providers.provider {
    flowToolBuildDirectory.get().resolve("cpp/$flowToolName")
}
val buildFlowTool = tasks.register<Exec>("buildFlowTool") {
    group = "build"
    description = "Build the current native Flow language service for this platform."
    onlyIf { !suppliedFlowTool.isPresent }
    workingDir(layout.projectDirectory.dir(".."))
    doFirst {
        check(flowToolBuildDirectory.get().resolve("CMakeCache.txt").isFile) {
            "Configure the native tree with cmake --preset debug first, or supply " +
                "-PflowToolBuildDir=<configured CMake directory> or -PflowToolBinary=<a11-flow>."
        }
    }
    commandLine("cmake", "--build", flowToolBuildDirectory.get().absolutePath,
        "--target", "a11_flow_tool", "--parallel", "8")
}
val prepareFlowTool = tasks.register<Sync>("prepareFlowTool") {
    dependsOn(buildFlowTool)
    val tool = suppliedFlowTool.orElse(builtFlowTool)
    from(tool)
    into(layout.buildDirectory.dir("generated/flow-tool/bin/$flowToolPlatform"))
    doFirst {
        check(tool.get().isFile) { "The Flow language-service binary is missing: ${tool.get()}" }
    }
    rename { flowToolName }
}

tasks {
    processResources {
        dependsOn(buildWebview, prepareFlowTool)

        // The flows the plugin can run live in `src/main/resources/flows`, so
        // they land on the classpath with everything else and
        // `A11WebView.readFlow` can read them back by name.

        // Refresh the host service from CMake. Release builds supply additional
        // platforms in bin/; an old host copy there cannot override this build.
        from(layout.projectDirectory.dir("bin")) {
            exclude("$flowToolPlatform/**")
            into("bin")
        }
        from(layout.buildDirectory.dir("generated/flow-tool"))
    }

    // Disable coroutine stack-trace recovery in platform tests. Kotlin 2.2
    // emits debug metadata version 2, while the platform's bundled coroutines
    // expects version 1; recovery fails during modal project initialization and
    // leaves `BasePlatformTestCase.setUp` waiting indefinitely. The setting
    // removes coroutine frames from recovered test stack traces only.
    withType<Test>().configureEach {
        systemProperty("kotlinx.coroutines.stacktrace.recovery", "false")

        // Exercise the bundled resource through FlowEngine's normal extraction
        // path, so tests and the installed plugin use the same language service.
        systemProperty("a11.flow.bundled.resource", "bin/$flowToolPlatform/$flowToolName")
        systemProperty("a11.flow.built.tool", suppliedFlowTool.orElse(builtFlowTool).get().absolutePath)
    }

    // Fast dev loop: `./gradlew runIde` launches a sandbox CLion with the
    // plugin. The platform auto-reloads the plugin on rebuild while runIde is
    // running. The chat needs a gateway to talk to: run `a11 gateway`
    // alongside it.
}
