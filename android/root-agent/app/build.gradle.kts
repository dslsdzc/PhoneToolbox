plugins {
    id("com.android.application")
}

val keystorePath = project.findProperty("keystorePath") as String?
val keystorePass = project.findProperty("keystorePass") as String?

android {
    namespace = "com.local.service"
    compileSdk = 35

    defaultConfig {
        // 开源项目使用固定包名，方便 Provider/升级/配套工具识别；
        // 需要隐蔽安装时仍可用 -PappId / -PappLabel 覆盖。
        applicationId = (project.findProperty("appId") as String?)
            ?: "com.phonetoolbox.agent"
        resValue("string", "app_name",
            (project.findProperty("appLabel") as String?) ?: "PhoneToolbox Agent")
        minSdk = 21
        targetSdk = 35
        versionCode = 2
        versionName = "0.1.0-beta04"
    }

    buildFeatures {
        // app_name 由 resValue 按构建参数生成，避免固定应用名指纹。
        resValues = true
    }

    signingConfigs {
        if (keystorePath != null && keystorePass != null) {
            create("release") {
                storeFile = file(keystorePath)
                storePassword = keystorePass
                keyAlias = "agent"
                keyPassword = keystorePass
            }
        }
    }

    buildTypes {
        release {
            // release 不带 debuggable，且用 R8 混淆类名/裁剪资源，减少固定指纹。
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro")
            if (keystorePath != null && keystorePass != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        // 必须解压 native 库：agent 是可执行文件，要从 nativeLibraryDir 直接运行。
        jniLibs {
            useLegacyPackaging = true
        }
    }

    lint {
        abortOnError = false
    }
}
