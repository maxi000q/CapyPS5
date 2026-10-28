// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// The engine's lang/ folder (repository root) is the single source of the interface
// translations; every build copies its JSON files into the APK assets, where the
// Localization object reads them. assets/lang is generated - edit the root folder.
val syncLangFiles = tasks.register<Copy>("syncLangFiles") {
    from(rootProject.projectDir.parentFile.resolve("lang")) {
        include("*.json")
    }
    into(layout.projectDirectory.dir("src/main/assets/lang"))
}
tasks.named("preBuild") { dependsOn(syncLangFiles) }

android {
    namespace = "com.consolefivepro"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.consolefivepro"
        minSdk = 31
        targetSdk = 34
        // versionCode 18: v0.1.0pro - the first beta. The CFP MAX v2 performance
        // mode, the windowed-flicker and language-switch fixes, the redesigned
        // controls editor, the occlusion-query and ZArchive rounds, and the
        // improved Kurdish translation.
        versionCode = 18
        versionName = "0.1.0pro"

        ndk {
            abiFilters.add("arm64-v8a")
        }

        // The app's interface strings live in the engine tree's lang/ folder as JSON (one
        // file per language, English the default); the Android resources stay English-only
        // as the final fallback. Keep locale resources from androidx/Compose out of the APK.
        resourceConfigurations += listOf("en")
    }

    lint {
        // MainActivity and GameActivity extend the abstract CapyActivity (a ComponentActivity);
        // lint does not follow it and reports them as not being activities.
        disable += "Instantiatable"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            isDebuggable = true
            applicationIdSuffix = ".debug"
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

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.12.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.documentfile:documentfile:1.0.1")

    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
