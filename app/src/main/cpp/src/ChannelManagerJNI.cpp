#include <jni.h>
#include <android/native_window_jni.h>
#include "ChannelManager.h"

// External declarations from native-lib.cpp
extern ANativeWindow *window;
extern pthread_mutex_t windowMutex;

extern "C" {

// Initialize the global channel manager
JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_initializeNativeChannelManager(
        JNIEnv *env, jobject instance, jbyteArray modelData) {

    LOGD("initializeNativeChannelManager called");

    if (!modelData) {
        LOGE("Model data is null");
        return JNI_FALSE;
    }
    
    // Get model data from Java byte array
    jsize modelSize = env->GetArrayLength(modelData);
    jbyte* modelBytes = env->GetByteArrayElements(modelData, nullptr);
    
    if (!modelBytes) {
        LOGE("Failed to get model data bytes");
        return JNI_FALSE;
    }
    
    // Create global channel manager if it doesn't exist
    if (!g_channelManager) {
        LOGD("Creating global channel manager");
        g_channelManager = std::make_unique<NativeChannelManager>();
    } else {
        LOGD("Global channel manager already exists");
    }

    // Initialize with model data
    LOGD("Copying model data, size: %d", modelSize);
    char* modelDataCopy = new char[modelSize];
    memcpy(modelDataCopy, modelBytes, modelSize);
    
    bool success = g_channelManager->initialize(modelDataCopy, modelSize);
    
    if (success) {
        // Set Java callbacks
        g_channelManager->setJavaCallbacks(env, instance);
    }
    
    // Release Java byte array
    env->ReleaseByteArrayElements(modelData, modelBytes, JNI_ABORT);
    
    return success ? JNI_TRUE : JNI_FALSE;
}

// Create a native player instance
JNIEXPORT jlong JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_createNativePlayer(
        JNIEnv *env, jobject instance, jbyteArray modelData, jint modelSize) {

    LOGD("createNativePlayer called with modelSize: %d", modelSize);

    if (!modelData || modelSize <= 0) {
        LOGE("Invalid model data parameters: modelData=%p, modelSize=%d", modelData, modelSize);
        return 0;
    }

    // Check if global channel manager is initialized
    if (!g_channelManager) {
        LOGE("Global channel manager not initialized");
        return 0;
    }

    LOGD("Global channel manager is available, proceeding with player creation");

    // Get model data from Java byte array
    jbyte* modelBytes = env->GetByteArrayElements(modelData, nullptr);
    if (!modelBytes) {
        LOGE("Failed to get model data bytes");
        return 0;
    }

    try {
        // Create new ZLPlayer instance with error checking
        char* modelDataCopy = new char[modelSize];
        memcpy(modelDataCopy, modelBytes, modelSize);

        // Add validation for model data
        if (modelSize < 1000) { // Basic sanity check for model file size
            LOGE("Model data size too small: %d bytes", modelSize);
            delete[] modelDataCopy;
            env->ReleaseByteArrayElements(modelData, modelBytes, JNI_ABORT);
            return 0;
        }

        LOGD("About to create ZLPlayer with model size: %d", modelSize);
        ZLPlayer* player = new ZLPlayer(modelDataCopy, modelSize);
        LOGD("ZLPlayer creation completed, player: %p", player);

        // Verify player was created successfully
        if (!player) {
            LOGE("Failed to create ZLPlayer instance");
            delete[] modelDataCopy;
            env->ReleaseByteArrayElements(modelData, modelBytes, JNI_ABORT);
            return 0;
        }

        // Release Java byte array
        env->ReleaseByteArrayElements(modelData, modelBytes, JNI_ABORT);

        LOGD("Successfully created native player: %p", player);
        return reinterpret_cast<jlong>(player);

    } catch (const std::exception& e) {
        LOGE("Exception creating native player: %s", e.what());
        env->ReleaseByteArrayElements(modelData, modelBytes, JNI_ABORT);
        return 0;
    } catch (...) {
        LOGE("Unknown exception creating native player");
        env->ReleaseByteArrayElements(modelData, modelBytes, JNI_ABORT);
        return 0;
    }
}

// Destroy a native player instance
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_destroyNativePlayer(
        JNIEnv *env, jobject instance, jlong nativePlayer) {
    
    if (nativePlayer != 0) {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        delete player;
    }
}

// Start a native player
JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_startNativePlayer(
        JNIEnv *env, jobject instance, jlong nativePlayer) {
    
    if (nativePlayer == 0) {
        return JNI_FALSE;
    }
    
    try {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        // The player starts automatically in its constructor
        // This method could be used to restart or resume playback
        return JNI_TRUE;
        
    } catch (const std::exception& e) {
        LOGE("Failed to start native player: %s", e.what());
        return JNI_FALSE;
    }
}

// Stop a native player
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_stopNativePlayer(
        JNIEnv *env, jobject instance, jlong nativePlayer) {
    
    if (nativePlayer != 0) {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        // The player will be stopped in its destructor
        // This method could be used to pause playback without destroying the player
    }
}

// Set RTSP URL for a channel
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_setChannelRTSPUrl(
        JNIEnv *env, jobject instance, jlong nativePlayer, jstring rtspUrl) {
    
    if (nativePlayer == 0 || !rtspUrl) {
        return;
    }
    
    const char* urlStr = env->GetStringUTFChars(rtspUrl, nullptr);
    if (urlStr) {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        
        // Set the RTSP URL
        size_t urlLen = strlen(urlStr);
        if (urlLen < 256) { // Assuming rtsp_url has enough space
            strcpy(player->rtsp_url, urlStr);
            LOGD("RTSP URL set to: %s", urlStr);
        } else {
            LOGE("RTSP URL too long: %zu characters", urlLen);
        }
        
        env->ReleaseStringUTFChars(rtspUrl, urlStr);
    }
}

// Set surface for a channel
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_setChannelSurfaceNative(
        JNIEnv *env, jobject instance, jlong nativePlayer, jobject surface) {

    LOGD("setChannelSurfaceNative called with nativePlayer: %ld, surface: %p", nativePlayer, surface);

    if (nativePlayer == 0) {
        LOGE("Native player pointer is null, cannot set surface");
        return;
    }

    ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
    LOGD("ZLPlayer pointer: %p", player);

    // Get ANativeWindow from Surface
    ANativeWindow* nativeWindow = nullptr;
    if (surface) {
        nativeWindow = ANativeWindow_fromSurface(env, surface);
        LOGD("ANativeWindow created from Surface: %p", nativeWindow);

        if (!nativeWindow) {
            LOGE("Failed to create ANativeWindow from Surface");
            return;
        }
    } else {
        LOGD("Surface is null, clearing native window");
    }

    // Use ZLPlayer's channel-specific surface management
    LOGD("Setting channel surface on ZLPlayer");
    player->setChannelSurface(nativeWindow);
    LOGD("Channel surface set on ZLPlayer");

    // Release our reference since setChannelSurface acquires its own
    if (nativeWindow) {
        ANativeWindow_release(nativeWindow);
        LOGD("Surface set for native player and reference released");
    } else {
        LOGD("Surface cleared for native player");
    }
}

// Set detection enabled for a channel
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_setChannelDetectionEnabled(
        JNIEnv *env, jobject instance, jlong nativePlayer, jboolean enabled) {
    
    if (nativePlayer == 0) {
        return;
    }
    
    ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
    
    // TODO: Implement detection enable/disable functionality
    // This would require modifying the ZLPlayer class to support this feature
    LOGD("Detection %s for native player", enabled ? "enabled" : "disabled");
}

// Channel Manager specific methods using the global instance
JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_createChannel(
        JNIEnv *env, jobject instance, jint channelIndex) {
    
    if (!g_channelManager) {
        LOGE("Channel manager not initialized");
        return JNI_FALSE;
    }
    
    return g_channelManager->createChannel(channelIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_destroyChannel(
        JNIEnv *env, jobject instance, jint channelIndex) {
    
    if (!g_channelManager) {
        return JNI_FALSE;
    }
    
    return g_channelManager->destroyChannel(channelIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_startChannel(
        JNIEnv *env, jobject instance, jint channelIndex, jstring rtspUrl) {
    
    if (!g_channelManager || !rtspUrl) {
        return JNI_FALSE;
    }
    
    const char* urlStr = env->GetStringUTFChars(rtspUrl, nullptr);
    if (!urlStr) {
        return JNI_FALSE;
    }
    
    bool result = g_channelManager->startChannel(channelIndex, urlStr);
    
    env->ReleaseStringUTFChars(rtspUrl, urlStr);
    
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_stopChannel(
        JNIEnv *env, jobject instance, jint channelIndex) {
    
    if (!g_channelManager) {
        return JNI_FALSE;
    }
    
    return g_channelManager->stopChannel(channelIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_setChannelSurfaceByIndex(
        JNIEnv *env, jobject instance, jint channelIndex, jobject surface) {
    
    if (!g_channelManager) {
        return;
    }
    
    ANativeWindow* window = nullptr;
    if (surface) {
        window = ANativeWindow_fromSurface(env, surface);
    }
    
    g_channelManager->setChannelSurface(channelIndex, window);
}

JNIEXPORT jint JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_getChannelState(
        JNIEnv *env, jobject instance, jint channelIndex) {
    
    if (!g_channelManager) {
        return 0; // INACTIVE
    }
    
    return static_cast<jint>(g_channelManager->getChannelState(channelIndex));
}

JNIEXPORT jfloat JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_getChannelFps(
        JNIEnv *env, jobject instance, jint channelIndex) {
    
    if (!g_channelManager) {
        return 0.0f;
    }
    
    return g_channelManager->getChannelFps(channelIndex);
}

JNIEXPORT jint JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_getActiveChannelCount(
        JNIEnv *env, jobject instance) {
    
    if (!g_channelManager) {
        return 0;
    }
    
    return g_channelManager->getActiveChannelCount();
}

JNIEXPORT jfloat JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_getSystemFps(
        JNIEnv *env, jobject instance) {
    
    if (!g_channelManager) {
        return 0.0f;
    }
    
    return g_channelManager->getSystemFps();
}

JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_cleanupNative(
        JNIEnv *env, jobject instance) {

    if (g_channelManager) {
        g_channelManager->cleanup();
        g_channelManager.reset();
    }
}

// Surface recovery monitoring methods
JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_isSurfaceRecoveryRequested(
        JNIEnv *env, jobject instance, jlong nativePlayer) {

    LOGD("isSurfaceRecoveryRequested called with nativePlayer: %ld", nativePlayer);

    if (nativePlayer == 0) {
        LOGE("isSurfaceRecoveryRequested: nativePlayer is null");
        return JNI_FALSE;
    }

    try {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        if (player) {
            bool recoveryRequested = player->isSurfaceRecoveryRequested();
            LOGD("Surface recovery requested for player: %s", recoveryRequested ? "true" : "false");
            return recoveryRequested ? JNI_TRUE : JNI_FALSE;
        } else {
            LOGE("isSurfaceRecoveryRequested: Failed to cast nativePlayer to ZLPlayer");
            return JNI_FALSE;
        }
    } catch (const std::exception& e) {
        LOGE("isSurfaceRecoveryRequested: Exception occurred: %s", e.what());
        return JNI_FALSE;
    }
}

JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_clearSurfaceRecoveryRequest(
        JNIEnv *env, jobject instance, jlong nativePlayer) {

    LOGD("clearSurfaceRecoveryRequest called with nativePlayer: %ld", nativePlayer);

    if (nativePlayer == 0) {
        LOGE("clearSurfaceRecoveryRequest: nativePlayer is null");
        return;
    }

    try {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        if (player) {
            player->clearSurfaceRecoveryRequest();
            LOGD("Surface recovery request cleared for player");
        } else {
            LOGE("clearSurfaceRecoveryRequest: Failed to cast nativePlayer to ZLPlayer");
        }
    } catch (const std::exception& e) {
        LOGE("clearSurfaceRecoveryRequest: Exception occurred: %s", e.what());
    }
}

JNIEXPORT jboolean JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_validateSurfaceHealth(
        JNIEnv *env, jobject instance, jlong nativePlayer) {

    LOGD("validateSurfaceHealth called with nativePlayer: %ld", nativePlayer);

    if (nativePlayer == 0) {
        LOGE("validateSurfaceHealth: nativePlayer is null");
        return JNI_FALSE;
    }

    try {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        if (player) {
            bool isHealthy = player->validateSurfaceHealth();
            LOGD("Surface health validation result for player: %s", isHealthy ? "healthy" : "unhealthy");
            return isHealthy ? JNI_TRUE : JNI_FALSE;
        } else {
            LOGE("validateSurfaceHealth: Failed to cast nativePlayer to ZLPlayer");
            return JNI_FALSE;
        }
    } catch (const std::exception& e) {
        LOGE("validateSurfaceHealth: Exception occurred: %s", e.what());
        return JNI_FALSE;
    }
}

JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_ChannelManager_forceSurfaceReset(
        JNIEnv *env, jobject instance, jlong nativePlayer) {

    LOGD("forceSurfaceReset called with nativePlayer: %ld", nativePlayer);

    if (nativePlayer == 0) {
        LOGE("forceSurfaceReset: nativePlayer is null");
        return;
    }

    try {
        ZLPlayer* player = reinterpret_cast<ZLPlayer*>(nativePlayer);
        if (player) {
            player->forceSurfaceReset();
            LOGD("Surface force reset completed for player");
        } else {
            LOGE("forceSurfaceReset: Failed to cast nativePlayer to ZLPlayer");
        }
    } catch (const std::exception& e) {
        LOGE("forceSurfaceReset: Exception occurred: %s", e.what());
    }
}

} // extern "C"
