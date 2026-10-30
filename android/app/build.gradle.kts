import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.windrop.app"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.windrop.app"
        minSdk = 29
        targetSdk = 36
        // One version for the whole project: the VERSION file at the repo root (also read by CMake).
        versionName = rootProject.file("../VERSION").readText().trim()
        versionCode = versionName!!.split(".").map { it.toInt() }.let { (a, b, c) -> a * 10000 + b * 100 + c }
    }

    buildFeatures { buildConfig = true }

    // Release signing key lives outside the repo: ~/.windrop-signing/keystore.properties
    // (storeFile, storePassword, keyAlias, keyPassword), or the file named by WINDROP_SIGNING.
    // Without it, release builds fall back to the debug key so anyone can still build.
    val signingProps = (System.getenv("WINDROP_SIGNING")
        ?: "${System.getProperty("user.home")}/.windrop-signing/keystore.properties").let(::file)
    signingConfigs {
        if (signingProps.exists()) {
            create("release") {
                val p = Properties().apply { signingProps.inputStream().use(::load) }
                storeFile = file(p.getProperty("storeFile"))
                storePassword = p.getProperty("storePassword")
                keyAlias = p.getProperty("keyAlias")
                keyPassword = p.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        disable += "ExpiredTargetSdkVersion"
    }
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}
