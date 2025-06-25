#include "ChannelManager.h"
#include <chrono>
#include <algorithm>

// Global instance
std::unique_ptr<NativeChannelManager> g_channelManager = nullptr;

NativeChannelManager::NativeChannelManager() : 
    shouldStop(false),
    jvm(nullptr),
    javaChannelManager(nullptr),
    onFrameReceivedMethod(nullptr),
    onDetectionReceivedMethod(nullptr),
    onChannelStateChangedMethod(nullptr),
    onChannelErrorMethod(nullptr) {
    
    // Initialize all channels
    for (int i = 0; i < MAX_CHANNELS; i++) {
        channels[i] = std::make_unique<ChannelInfo>(i);
    }
    
    // Start performance monitoring thread
    performanceThread = std::thread(&NativeChannelManager::performanceMonitorLoop, this);
}

NativeChannelManager::~NativeChannelManager() {
    cleanup();
}

bool NativeChannelManager::initialize(char* modelData, int modelSize) {
    if (!modelData || modelSize <= 0) {
        LOGE("Invalid model data provided to ChannelManager");
        return false;
    }
    
    return initializeSharedResources(modelData, modelSize);
}

void NativeChannelManager::setJavaCallbacks(JNIEnv* env, jobject javaObject) {
    // Get JavaVM for later use
    env->GetJavaVM(&jvm);
    
    // Create global reference to Java object
    javaChannelManager = env->NewGlobalRef(javaObject);
    
    // Get method IDs for callbacks
    jclass clazz = env->GetObjectClass(javaObject);
    onFrameReceivedMethod = env->GetMethodID(clazz, "onNativeFrameReceived", "(I)V");
    onDetectionReceivedMethod = env->GetMethodID(clazz, "onNativeDetectionReceived", "(II)V");
    onChannelStateChangedMethod = env->GetMethodID(clazz, "onChannelStateChanged", "(II)V");
    onChannelErrorMethod = env->GetMethodID(clazz, "onChannelError", "(ILjava/lang/String;)V");
    
    env->DeleteLocalRef(clazz);
}

bool NativeChannelManager::createChannel(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    
    if (channelInfo && channelInfo->state == INACTIVE) {
        // Channel is ready to be configured
        return true;
    }
    
    return false;
}

bool NativeChannelManager::destroyChannel(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    
    if (channelInfo) {
        // Stop the player if active
        if (channelInfo->player) {
            channelInfo->player.reset();
        }
        
        // Release surface
        if (channelInfo->surface) {
            ANativeWindow_release(channelInfo->surface);
            channelInfo->surface = nullptr;
        }
        
        // Reset channel state
        channelInfo->state = INACTIVE;
        channelInfo->frameCount = 0;
        channelInfo->detectionCount = 0;
        channelInfo->fps = 0.0f;
        channelInfo->errorMessage.clear();
        channelInfo->retryCount = 0;
        
        performanceMetrics.activeChannelCount--;
        
        return true;
    }
    
    return false;
}

bool NativeChannelManager::startChannel(int channelIndex, const char* rtspUrl) {
    if (!isValidChannelIndex(channelIndex) || !rtspUrl) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    
    if (!channelInfo || !sharedResources.modelData) {
        LOGE("Channel %d not ready or shared resources not initialized", channelIndex);
        return false;
    }
    
    try {
        // Update state to connecting
        updateChannelState(channelIndex, CONNECTING);
        
        // Create new MultiChannelZLPlayer instance with shared model data
        channelInfo->player = std::make_unique<MultiChannelZLPlayer>(
            channelIndex,
            sharedResources.modelData,
            sharedResources.modelSize,
            this
        );

        // Configure RTSP URL
        channelInfo->rtspUrl = rtspUrl;
        channelInfo->player->setChannelRTSPUrl(rtspUrl);

        // Set surface if available
        if (channelInfo->surface) {
            channelInfo->player->setChannelSurface(channelInfo->surface);
        }

        // Configure detection
        channelInfo->player->setDetectionEnabled(channelInfo->detectionEnabled);
        
        // Update state to active
        updateChannelState(channelIndex, ACTIVE);
        performanceMetrics.activeChannelCount++;
        
        LOGD("Channel %d started successfully with URL: %s", channelIndex, rtspUrl);
        return true;
        
    } catch (const std::exception& e) {
        LOGE("Failed to start channel %d: %s", channelIndex, e.what());
        onChannelError(channelIndex, e.what());
        return false;
    }
}

bool NativeChannelManager::stopChannel(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    
    if (channelInfo && channelInfo->player) {
        channelInfo->player.reset();
        updateChannelState(channelIndex, INACTIVE);
        performanceMetrics.activeChannelCount--;
        
        LOGD("Channel %d stopped", channelIndex);
        return true;
    }
    
    return false;
}

bool NativeChannelManager::setChannelSurface(int channelIndex, ANativeWindow* surface) {
    if (!isValidChannelIndex(channelIndex)) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    
    if (channelInfo) {
        // Release previous surface
        if (channelInfo->surface) {
            ANativeWindow_release(channelInfo->surface);
        }
        
        // Set new surface
        channelInfo->surface = surface;
        if (surface) {
            ANativeWindow_acquire(surface);
        }
        
        // Update player if active
        if (channelInfo->player) {
            channelInfo->player->setChannelSurface(surface);
        }
        
        return true;
    }
    
    return false;
}

bool NativeChannelManager::setChannelRTSPUrl(int channelIndex, const char* rtspUrl) {
    if (!isValidChannelIndex(channelIndex) || !rtspUrl) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    
    if (channelInfo) {
        channelInfo->rtspUrl = rtspUrl;
        
        if (channelInfo->player) {
            channelInfo->player->setChannelRTSPUrl(rtspUrl);
        }
        
        return true;
    }
    
    return false;
}

bool NativeChannelManager::setChannelDetectionEnabled(int channelIndex, bool enabled) {
    if (!isValidChannelIndex(channelIndex)) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    
    if (channelInfo) {
        channelInfo->detectionEnabled = enabled;
        
        if (channelInfo->player) {
            channelInfo->player->setDetectionEnabled(enabled);
        }
        
        return true;
    }
    
    return false;
}

// Callback implementations
void NativeChannelManager::onChannelFrameReceived(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return;
    }
    
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        channelInfo->frameCount++;
        channelInfo->lastFrameTime = std::chrono::steady_clock::now();
        performanceMetrics.totalFrameCount++;
        
        // Notify Java layer
        notifyJavaFrameReceived(channelIndex);
    }
}

void NativeChannelManager::onChannelDetectionReceived(int channelIndex, int detectionCount) {
    if (!isValidChannelIndex(channelIndex)) {
        return;
    }

    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        // Update detection statistics
        channelInfo->detectionCount += detectionCount;
        performanceMetrics.totalDetectionCount += detectionCount;

        // Log detection statistics
        if (detectionCount > 0) {
            LOGD("Channel %d: Received %d detections (total: %d)",
                 channelIndex, detectionCount, channelInfo->detectionCount.load());
        }

        // Notify Java layer
        notifyJavaDetectionReceived(channelIndex, detectionCount);
    }
}

void NativeChannelManager::onChannelFrameRendered(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return;
    }

    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        // Update render statistics
        channelInfo->renderCount++;
        performanceMetrics.totalRenderCount++;

        // Update channel state to active if it was inactive
        if (channelInfo->state == INACTIVE) {
            updateChannelState(channelIndex, ACTIVE);
        }

        LOGD("Channel %d: Frame rendered (total renders: %d)",
             channelIndex, channelInfo->renderCount.load());
    }
}

void NativeChannelManager::onChannelError(int channelIndex, const std::string& errorMessage) {
    if (!isValidChannelIndex(channelIndex)) {
        return;
    }
    
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        channelInfo->errorMessage = errorMessage;
        updateChannelState(channelIndex, ERROR);
        
        // Notify Java layer
        notifyJavaChannelError(channelIndex, errorMessage);
    }
}

void NativeChannelManager::onChannelStateChanged(int channelIndex, ChannelState newState) {
    updateChannelState(channelIndex, newState);
}

// Getters
NativeChannelManager::ChannelState NativeChannelManager::getChannelState(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return INACTIVE;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    return channelInfo ? channelInfo->state : INACTIVE;
}

float NativeChannelManager::getChannelFps(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return 0.0f;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    return channelInfo ? channelInfo->fps : 0.0f;
}

int NativeChannelManager::getActiveChannelCount() {
    return performanceMetrics.activeChannelCount.load();
}

float NativeChannelManager::getSystemFps() {
    std::lock_guard<std::mutex> lock(performanceMutex);
    return performanceMetrics.systemFps;
}

int NativeChannelManager::getChannelFrameCount(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    return channelInfo ? channelInfo->frameCount.load() : 0;
}

int NativeChannelManager::getChannelDetectionCount(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    return channelInfo ? channelInfo->detectionCount.load() : 0;
}

std::string NativeChannelManager::getChannelError(int channelIndex) {
    if (!isValidChannelIndex(channelIndex)) {
        return "";
    }

    std::lock_guard<std::mutex> lock(channelsMutex);
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    return channelInfo ? channelInfo->errorMessage : "";
}

void NativeChannelManager::applyGlobalPerformanceOptimizations() {
    std::lock_guard<std::mutex> lock(channelsMutex);

    LOGD("Applying global performance optimizations");

    // Strategy 1: Reduce detection frequency for all channels
    for (auto& pair : channels) {
        ChannelInfo* channelInfo = pair.second.get();
        if (channelInfo && channelInfo->state == ACTIVE) {
            optimizeChannelPerformance(pair.first);
        }
    }

    // Strategy 2: Prioritize channels with better performance
    // Find the best performing channels and give them priority
    std::vector<std::pair<int, float>> channelPerformance;
    for (auto& pair : channels) {
        ChannelInfo* channelInfo = pair.second.get();
        if (channelInfo && channelInfo->state == ACTIVE) {
            channelPerformance.push_back({pair.first, channelInfo->fps});
        }
    }

    // Sort by FPS (descending)
    std::sort(channelPerformance.begin(), channelPerformance.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Apply different optimization levels based on performance ranking
    for (size_t i = 0; i < channelPerformance.size(); i++) {
        int channelIndex = channelPerformance[i].first;
        ChannelInfo* channelInfo = getChannelInfo(channelIndex);

        if (channelInfo) {
            if (i < channelPerformance.size() / 2) {
                // Top half: maintain normal processing
                LOGD("Channel %d: High priority (FPS: %.2f)", channelIndex, channelInfo->fps);
            } else {
                // Bottom half: reduce processing frequency
                LOGD("Channel %d: Reduced priority (FPS: %.2f)", channelIndex, channelInfo->fps);
                // Could disable detection or reduce render frequency
            }
        }
    }
}

void NativeChannelManager::optimizeChannelPerformance(int channelIndex) {
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return;
    }

    float channelFps = channelInfo->fps;

    LOGD("Optimizing performance for channel %d (current FPS: %.2f)", channelIndex, channelFps);

    // Strategy 1: Adjust frame interval based on performance
    if (channelFps < PerformanceMetrics::MIN_FPS_THRESHOLD) {
        // Increase frame interval to reduce load
        auto newInterval = std::chrono::microseconds(40000); // ~25 FPS
        channelInfo->frameInterval = newInterval;
        LOGD("Channel %d: Reduced target FPS to 25", channelIndex);
    } else if (channelFps > PerformanceMetrics::TARGET_FPS * 0.95f) {
        // Restore normal frame interval
        auto normalInterval = std::chrono::microseconds(33333); // ~30 FPS
        channelInfo->frameInterval = normalInterval;
    }

    // Strategy 2: Adaptive detection control
    if (channelFps < PerformanceMetrics::MIN_FPS_THRESHOLD * 0.8f) {
        // Temporarily disable detection for very poor performance
        channelInfo->detectionEnabled = false;
        LOGD("Channel %d: Detection temporarily disabled due to poor performance", channelIndex);
    } else if (channelFps > PerformanceMetrics::TARGET_FPS * 0.9f && !channelInfo->detectionEnabled) {
        // Re-enable detection when performance improves
        channelInfo->detectionEnabled = true;
        LOGD("Channel %d: Detection re-enabled", channelIndex);
    }

    // Strategy 3: Queue management
    // This would be implemented in the actual frame processing pipeline
    // to drop frames when queues get too large
}

// Private helper methods
bool NativeChannelManager::isValidChannelIndex(int channelIndex) {
    return channelIndex >= 0 && channelIndex < MAX_CHANNELS;
}

NativeChannelManager::ChannelInfo* NativeChannelManager::getChannelInfo(int channelIndex) {
    auto it = channels.find(channelIndex);
    return (it != channels.end()) ? it->second.get() : nullptr;
}

void NativeChannelManager::updateChannelState(int channelIndex, ChannelState newState) {
    ChannelInfo* channelInfo = getChannelInfo(channelIndex);
    if (channelInfo && channelInfo->state != newState) {
        channelInfo->state = newState;
        notifyJavaChannelStateChanged(channelIndex, newState);
    }
}

bool NativeChannelManager::initializeSharedResources(char* modelData, int modelSize) {
    std::lock_guard<std::mutex> lock(sharedResources.resourceMutex);
    
    // Copy model data
    sharedResources.modelData = new char[modelSize];
    memcpy(sharedResources.modelData, modelData, modelSize);
    sharedResources.modelSize = modelSize;
    
    // Create shared thread pool
    sharedResources.sharedThreadPool = std::make_shared<Yolov5ThreadPool>();
    if (sharedResources.sharedThreadPool->setUpWithModelData(SHARED_THREAD_POOL_SIZE, 
                                                           sharedResources.modelData, 
                                                           sharedResources.modelSize) != NN_SUCCESS) {
        LOGE("Failed to initialize shared YOLOv5 thread pool");
        return false;
    }
    
    LOGD("Shared resources initialized successfully");
    return true;
}

void NativeChannelManager::cleanup() {
    // Stop performance monitoring
    shouldStop = true;
    performanceCv.notify_all();
    if (performanceThread.joinable()) {
        performanceThread.join();
    }
    
    // Stop all channels
    for (int i = 0; i < MAX_CHANNELS; i++) {
        destroyChannel(i);
    }
    
    // Cleanup shared resources
    cleanupSharedResources();
    
    // Cleanup JNI references
    if (jvm && javaChannelManager) {
        JNIEnv* env;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            env->DeleteGlobalRef(javaChannelManager);
        }
        javaChannelManager = nullptr;
    }
}

void NativeChannelManager::cleanupSharedResources() {
    std::lock_guard<std::mutex> lock(sharedResources.resourceMutex);
    
    if (sharedResources.sharedThreadPool) {
        sharedResources.sharedThreadPool->stopAll();
        sharedResources.sharedThreadPool.reset();
    }
    
    // Destructor will handle modelData cleanup
}

void NativeChannelManager::performanceMonitorLoop() {
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(performanceMutex);
        performanceCv.wait_for(lock, std::chrono::milliseconds(PERFORMANCE_UPDATE_INTERVAL_MS));
        
        if (!shouldStop) {
            updatePerformanceMetrics();
        }
    }
}

void NativeChannelManager::updatePerformanceMetrics() {
    auto currentTime = std::chrono::steady_clock::now();
    auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        currentTime - performanceMetrics.lastUpdate).count();

    if (deltaTime >= PERFORMANCE_UPDATE_INTERVAL_MS) {
        // Calculate system FPS
        int frameCount = performanceMetrics.totalFrameCount.exchange(0);
        int renderCount = performanceMetrics.totalRenderCount.exchange(0);
        int detectionCount = performanceMetrics.totalDetectionCount.exchange(0);

        performanceMetrics.systemFps = (frameCount * 1000.0f) / deltaTime;
        performanceMetrics.lastUpdate = currentTime;

        // Log system performance
        LOGD("System Performance: FPS=%.2f, Renders=%d, Detections=%d",
             performanceMetrics.systemFps, renderCount, detectionCount);

        // Update individual channel FPS and apply performance optimizations
        std::lock_guard<std::mutex> lock(channelsMutex);
        int activeChannels = 0;

        for (auto& pair : channels) {
            ChannelInfo* channelInfo = pair.second.get();
            if (channelInfo && channelInfo->state == ACTIVE) {
                activeChannels++;

                // Update channel FPS
                int channelFrameCount = channelInfo->frameCount.exchange(0);
                int channelRenderCount = channelInfo->renderCount.exchange(0);

                channelInfo->fps = (channelFrameCount * 1000.0f) / deltaTime;
                channelInfo->renderFps = (channelRenderCount * 1000.0f) / deltaTime;

                // Adaptive performance optimization
                if (channelInfo->fps < PerformanceMetrics::MIN_FPS_THRESHOLD) {
                    // Reduce detection frequency for this channel
                    LOGD("Channel %d performance below threshold (%.2f FPS), optimizing...",
                         pair.first, channelInfo->fps);
                }
            }
        }

        performanceMetrics.activeChannelCount.store(activeChannels);

        // System-wide performance optimization
        if (performanceMetrics.systemFps < PerformanceMetrics::MIN_FPS_THRESHOLD) {
            LOGW("System FPS below threshold (%.2f), applying global optimizations",
                 performanceMetrics.systemFps);
            applyGlobalPerformanceOptimizations();
        }
    }
}

// JNI callback helper implementations
void NativeChannelManager::notifyJavaFrameReceived(int channelIndex) {
    if (!jvm || !javaChannelManager || !onFrameReceivedMethod) {
        return;
    }

    JNIEnv* env;
    if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        env->CallVoidMethod(javaChannelManager, onFrameReceivedMethod, channelIndex);
    }
}

void NativeChannelManager::notifyJavaDetectionReceived(int channelIndex, int detectionCount) {
    if (!jvm || !javaChannelManager || !onDetectionReceivedMethod) {
        return;
    }

    JNIEnv* env;
    if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        env->CallVoidMethod(javaChannelManager, onDetectionReceivedMethod, channelIndex, detectionCount);
    }
}

void NativeChannelManager::notifyJavaChannelStateChanged(int channelIndex, ChannelState newState) {
    if (!jvm || !javaChannelManager || !onChannelStateChangedMethod) {
        return;
    }

    JNIEnv* env;
    if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        env->CallVoidMethod(javaChannelManager, onChannelStateChangedMethod, channelIndex, (int)newState);
    }
}

void NativeChannelManager::notifyJavaChannelError(int channelIndex, const std::string& errorMessage) {
    if (!jvm || !javaChannelManager || !onChannelErrorMethod) {
        return;
    }

    JNIEnv* env;
    if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        jstring jErrorMessage = env->NewStringUTF(errorMessage.c_str());
        env->CallVoidMethod(javaChannelManager, onChannelErrorMethod, channelIndex, jErrorMessage);
        env->DeleteLocalRef(jErrorMessage);
    }
}
