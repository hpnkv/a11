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

plugins { base }

val kotlinLibrary = gradle.includedBuild("a11-kotlin")
val intellijPlugin = gradle.includedBuild("a11-clion-plugin")
val kotlinEcho = gradle.includedBuild("a11-websocket-echo-kotlin")

tasks.assemble {
    dependsOn(
        kotlinLibrary.task(":assemble"),
        intellijPlugin.task(":assemble"),
        kotlinEcho.task(":installDist"),
    )
}

tasks.check { dependsOn(kotlinLibrary.task(":check"), intellijPlugin.task(":check")) }

tasks.register("kotlinFormat") {
    group = "formatting"
    dependsOn(kotlinLibrary.task(":kotlinFormat"))
}

tasks.register("kotlinFormatCheck") {
    group = "verification"
    dependsOn(kotlinLibrary.task(":kotlinFormatCheck"))
}
