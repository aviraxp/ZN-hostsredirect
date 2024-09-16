import android.databinding.tool.ext.capitalizeUS
import org.apache.tools.ant.filters.FixCrLfFilter
import org.apache.tools.ant.filters.ReplaceTokens
import java.security.MessageDigest

plugins {
    alias(libs.plugins.agp.app)
}

val moduleId: String by rootProject.extra
val moduleName: String by rootProject.extra
val verCode: Int by rootProject.extra
val verName: String by rootProject.extra
val commitHash: String by rootProject.extra
val abiList: List<String> by rootProject.extra

android {
    buildFeatures {
        prefab = true
    }
    defaultConfig {
        ndk {
            abiFilters.addAll(abiList)
        }
        externalNativeBuild {
            cmake {
                cppFlags("-std=c++20")
                arguments(
                    "-DANDROID_STL=none",
                    "-DMODULE_NAME=$moduleId"
                )
            }
        }
    }
    externalNativeBuild {
        /*
        ndkBuild {
            path("src/main/cpp/Android.mk")
        }
        */
        cmake {
            path("src/main/cpp/CMakeLists.txt")
        }
    }
}

val abiMap = mapOf(
    "arm64-v8a" to "arm64",
    "armeabi-v7a" to "arm",
    "x86" to "x86",
    "x86_64" to "x64"
)

androidComponents.onVariants { variant ->
    afterEvaluate {
        val variantLowered = variant.name.lowercase()
        val variantCapped = variant.name.capitalizeUS()
        val buildTypeLowered = variant.buildType?.lowercase()
        val supportedAbis = abiList.map {
            abiMap[it] ?: error("unsupported abi $it")
        }.joinToString(" ")

        val moduleDir = layout.buildDirectory.file("outputs/module/$variantLowered")
        val zipFileName =
            "$moduleName-$verName-$verCode-$commitHash-$buildTypeLowered.zip".replace(' ', '-')

        val prepareModuleFilesTask = task<Sync>("prepareModuleFiles$variantCapped") {
            group = "module"
            dependsOn("assemble$variantCapped")
            into(moduleDir)
            from(rootProject.layout.projectDirectory.file("README.md"))
            from(layout.projectDirectory.file("template")) {
                exclude("module.prop", "customize.sh", "post-fs-data.sh", "service.sh", "zn_modules.txt")
                filter<FixCrLfFilter>("eol" to FixCrLfFilter.CrLf.newInstance("lf"))
            }
            from(layout.projectDirectory.file("template")) {
                include("module.prop", "zn_modules.txt")
                expand(
                    "moduleId" to moduleId,
                    "moduleName" to moduleName,
                    "versionName" to "$verName ($verCode-$commitHash-$variantLowered)",
                    "versionCode" to verCode
                )
            }
            from(layout.projectDirectory.file("template")) {
                include("customize.sh", "post-fs-data.sh", "service.sh")
                val tokens = mapOf(
                    "DEBUG" to if (buildTypeLowered == "debug") "true" else "false",
                    "SONAME" to moduleId,
                    "SUPPORTED_ABIS" to supportedAbis
                )
                filter<ReplaceTokens>("tokens" to tokens)
                filter<FixCrLfFilter>("eol" to FixCrLfFilter.CrLf.newInstance("lf"))
            }
            abiList.forEach { abi ->
                val arch = abiMap[abi]
                from(layout.buildDirectory.file("intermediates/stripped_native_libs/$variantLowered/strip${variantCapped}DebugSymbols/out/lib/$abi")) {
                    into("lib/$arch")
                }
            }

            doLast {
                fileTree(moduleDir).visit {
                    if (isDirectory) return@visit
                    val md = MessageDigest.getInstance("SHA-256")
                    file.forEachBlock(4096) { bytes, size ->
                        md.update(bytes, 0, size)
                    }
                    file(file.path + ".sha256").writeText(
                        org.apache.commons.codec.binary.Hex.encodeHexString(
                            md.digest()
                        )
                    )
                }
            }
        }

        val zipTask = task<Zip>("zip$variantCapped") {
            group = "module"
            dependsOn(prepareModuleFilesTask)
            archiveFileName.set(zipFileName)
            destinationDirectory.set(layout.projectDirectory.file("release").asFile)
            from(moduleDir)
        }

        val pushTask = task<Exec>("push$variantCapped") {
            group = "module"
            dependsOn(zipTask)
            commandLine("adb", "push", zipTask.outputs.files.singleFile.path, "/data/local/tmp")
        }

        val installKsuTask = task<Exec>("installKsu$variantCapped") {
            group = "module"
            dependsOn(pushTask)
            commandLine(
                "adb", "shell", "su", "-c",
                "/data/adb/ksud module install /data/local/tmp/$zipFileName"
            )
        }

        val installMagiskTask = task<Exec>("installMagisk$variantCapped") {
            group = "module"
            dependsOn(pushTask)
            commandLine(
                "adb",
                "shell",
                "su",
                "-M",
                "-c",
                "magisk --install-module /data/local/tmp/$zipFileName"
            )
        }

        task<Exec>("installKsuAndReboot$variantCapped") {
            group = "module"
            dependsOn(installKsuTask)
            commandLine("adb", "reboot")
        }

        task<Exec>("installMagiskAndReboot$variantCapped") {
            group = "module"
            dependsOn(installMagiskTask)
            commandLine("adb", "reboot")
        }
    }
}

dependencies {
    implementation(libs.cxx)
}
