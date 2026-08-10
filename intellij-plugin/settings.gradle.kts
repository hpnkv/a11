pluginManagement {
    repositories {
        gradlePluginPortal()
        maven("https://cache-redirector.jetbrains.com/plugins.gradle.org")
    }
}

// The A11 Kotlin compatibility layer is consumed as a composite build, so the
// plugin always builds against the local library sources.
includeBuild("../kotlin")

rootProject.name = "a11-clion-plugin"
