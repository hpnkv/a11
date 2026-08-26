import org.jetbrains.intellij.platform.gradle.IntelliJPlatformType
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm") version "2.2.0"
    // IntelliJ Platform Gradle Plugin 2.x.
    id("org.jetbrains.intellij.platform") version "2.2.1"
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
        // The IDE this is compiled and `runIde`-ed against. It is *not* a
        // restriction on where the plugin may be installed: nothing here depends
        // on a product-specific module or API, and plugin.xml asks only for
        // `com.intellij.modules.platform`, so the artifact installs in every
        // JetBrains IDE. CLion stays the development target because that is the
        // IDE this is developed in.
        create(
            IntelliJPlatformType.CLion,
            providers.gradleProperty("platformVersion").get(),
        )
        // No bundled plugins: everything used lives in the platform itself, and a
        // product-bundled dependency here is exactly what would make the plugin
        // uninstallable in IDEs that do not ship it.
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
    // overrides the starter to relaunch itself "with the Radler language plugin"
    // and builds the child command line with a `-D` flag where the executable
    // should be, so it dies in a second with
    //   Cannot run program "-DactionSystem.update.actions.warn.dataRules.on.edt=false"
    // from CLionTraverseUIStarter — before any settings page is looked at, and
    // identically with or without this plugin's own extensions. Searchable
    // options only pre-index the Settings search field; every settings page this
    // plugin contributes is still there and still found by its name. Re-enable
    // when that starter is fixed, or when building against an IDE that does not
    // override traverseUI (`platformType=IC`).
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

// Builds the JCEF webview bundle (chat + action explorer) into plugin resources.
//
// The UI itself is `../webview`, shared with the VSCode extension: the same chat,
// action explorer, conversation list and markdown renderer, behind the six-method
// bridge in `webview/src/bridge.ts`. What is in `webview/` *here* is this host's
// half — the entry point that installs the JCEF bridge — and the bundling.
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

tasks {
    processResources {
        dependsOn(buildWebview)

        // The flows the plugin can run live in `src/main/resources/flows`, so
        // they land on the classpath with everything else and
        // `A11WebView.readFlow` can read them back by name.

        // The language itself, per platform. `FlowEngine` runs one of these and
        // asks it what a flow means -- which is why there is no lexer, parser,
        // resolver, inspector or word list in this plugin any more.
        //
        // Whatever is in `bin/<os>-<arch>/a11-flow` is bundled, and a platform
        // nobody built for simply has no entry: the plugin then colours flows and
        // does not check them, with one notification saying so. Build one with
        //
        //   cmake --preset debug && cmake --build --preset debug --target a11_flow_tool
        //   mkdir -p intellij-plugin/bin/macos-aarch64
        //   cp build/debug/cpp/a11-flow intellij-plugin/bin/macos-aarch64/
        //
        // and a release fills the other platforms from their CI runners. Nothing
        // is checked in: a binary in git is a binary that goes stale.
        from(layout.projectDirectory.dir("bin")) {
            into("bin")
        }
    }

    // The platform fixture hangs in `setUp` without this, and the deadlock reads
    // like a slow test rather than a broken one.
    //
    // `BasePlatformTestCase` creates its project under a modal progress. That
    // runs as a coroutine, and this Kotlin's stdlib (2.2) writes a debug-metadata
    // version the platform's bundled kotlinx-coroutines refuses: the moment it
    // recovers a stack trace it throws `Debug metadata version mismatch. Expected:
    // 1, got 2`, which is fatal inside the dispatcher. Project init dies, the
    // modal progress backing it never completes, and the EDT pumps events for a
    // hierarchy that will never go away while the test thread waits on the EDT.
    //
    // Recovery is the only caller of the version check and is a debugging aid, so
    // turning it off costs nothing but coroutine frames in stack traces.
    withType<Test>().configureEach {
        systemProperty("kotlinx.coroutines.stacktrace.recovery", "false")

        // The language, if this checkout has built it. The tests that assert what
        // a *word means* need it and skip without it (see FlowLexerTest); the ones
        // that assert the platform's own contract run either way.
        for (candidate in listOf(
            "../build/ctests/cpp/a11-flow",
            "../build/debug/cpp/a11-flow",
            "../build/release/cpp/a11-flow",
            "bin/macos-aarch64/a11-flow",
        )) {
            val tool = layout.projectDirectory.file(candidate).asFile
            if (tool.canExecute()) {
                systemProperty("a11.flow.tool", tool.absolutePath)
                break
            }
        }
    }

    // Fast dev loop: `./gradlew runIde` launches a sandbox CLion with the plugin.
    // The platform auto-reloads the plugin on rebuild while runIde is running.
    // The chat needs a gateway to talk to: run `a11 gateway` alongside it.
}
