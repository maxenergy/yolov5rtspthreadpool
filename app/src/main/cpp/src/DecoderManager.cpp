#include "DecoderManager.h"
#include <algorithm>

// Static constant definitions
constexpr int DecoderManager::DEFAULT_MAX_DECODERS;
constexpr int DecoderManager::HEALTH_CHECK_INTERVAL_MS;
constexpr int DecoderManager::DECODER_TIMEOUT_MS;

DecoderManager::DecoderManager(int maxDecoders) 
    : maxDecoders(maxDecoders), activeDecoderCount(0), totalMemoryUsage(0),
      shouldStop(false), eventListener(nullptr) {
    
    // Start health monitoring thread
    healthMonitorThread = std::thread(&DecoderManager::healthMonitorLoop, this);
    
    LOGD("DecoderManager initialized with max %d decoders", maxDecoders);
}

DecoderManager::~DecoderManager() {
    cleanup();
}

bool DecoderManager::createDecoder(int channelIndex, rknn_app_context_t* context, 
                                  int codecType, int fps) {
    if (!context) {
        LOGE("Cannot create decoder for channel %d: null context", channelIndex);
        return false;
    }
    
    if (!hasCapacityForNewDecoder()) {
        LOGE("Cannot create decoder for channel %d: capacity exceeded", channelIndex);
        return false;
    }
    
    auto lock = lockDecoders();
    
    // Remove existing decoder if present
    auto it = decoders.find(channelIndex);
    if (it != decoders.end()) {
        LOGW("Replacing existing decoder for channel %d", channelIndex);
        destroyDecoder(channelIndex);
    }
    
    // Create new decoder info
    auto decoderInfo = std::make_unique<DecoderInfo>(channelIndex, context);
    decoderInfo->codecType = codecType;
    decoderInfo->fps = fps;
    
    // Create MPP decoder instance
    decoderInfo->decoder = std::make_unique<MppDecoder>();
    
    decoders[channelIndex] = std::move(decoderInfo);
    updateDecoderState(channelIndex, INITIALIZING);
    
    LOGD("Created decoder for channel %d (codec: %d, fps: %d)", channelIndex, codecType, fps);
    return true;
}

bool DecoderManager::destroyDecoder(int channelIndex) {
    auto lock = lockDecoders();
    
    auto it = decoders.find(channelIndex);
    if (it == decoders.end()) {
        return false;
    }
    
    DecoderInfo* decoderInfo = it->second.get();
    updateDecoderState(channelIndex, DESTROYED);
    
    // Cleanup decoder resources
    if (decoderInfo->decoder) {
        // MPP decoder cleanup is handled by destructor
        decoderInfo->decoder.reset();
    }
    
    activeDecoderCount--;
    decoders.erase(it);
    
    if (eventListener) {
        eventListener->onDecoderDestroyed(channelIndex);
    }
    
    LOGD("Destroyed decoder for channel %d", channelIndex);
    return true;
}

bool DecoderManager::initializeDecoder(int channelIndex) {
    auto lock = lockDecoders();
    
    DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    if (!decoderInfo || !decoderInfo->decoder) {
        LOGE("Decoder not found for channel %d", channelIndex);
        return false;
    }
    
    if (decoderInfo->state != INITIALIZING) {
        LOGW("Decoder for channel %d not in initializing state", channelIndex);
        return false;
    }
    
    // Initialize MPP decoder
    int ret = decoderInfo->decoder->Init(decoderInfo->codecType, decoderInfo->fps, decoderInfo->context);
    if (ret != 0) {
        LOGE("Failed to initialize decoder for channel %d: %d", channelIndex, ret);
        updateDecoderState(channelIndex, ERROR);
        return false;
    }
    
    updateDecoderState(channelIndex, READY);
    activeDecoderCount++;
    
    if (eventListener) {
        eventListener->onDecoderReady(channelIndex);
    }
    
    LOGD("Initialized decoder for channel %d", channelIndex);
    return true;
}

bool DecoderManager::decodeFrame(int channelIndex, uint8_t* data, int size, int64_t timestamp) {
    if (!data || size <= 0) {
        return false;
    }
    
    auto lock = lockDecoders();
    
    DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    if (!decoderInfo || !decoderInfo->decoder) {
        return false;
    }
    
    if (decoderInfo->state != READY && decoderInfo->state != DECODING) {
        LOGW("Decoder for channel %d not ready for decoding (state: %d)", channelIndex, decoderInfo->state);
        return false;
    }
    
    updateDecoderState(channelIndex, DECODING);
    lock.unlock(); // Unlock during decoding to allow other operations
    
    try {
        // Perform decoding
        int ret = decoderInfo->decoder->Decode(data, size, timestamp);
        if (ret == 0) {
            decoderInfo->frameCount++;
            decoderInfo->lastFrameTime = std::chrono::steady_clock::now();
            
            // Update state back to ready
            lock.lock();
            updateDecoderState(channelIndex, READY);
            lock.unlock();
            
            return true;
        } else {
            decoderInfo->errorCount++;
            handleDecoderError(channelIndex, "Decode operation failed");
            return false;
        }
    } catch (const std::exception& e) {
        decoderInfo->errorCount++;
        handleDecoderError(channelIndex, std::string("Decode exception: ") + e.what());
        return false;
    }
}

void DecoderManager::setDecoderCallback(int channelIndex, MppDecoderFrameCallback callback) {
    auto lock = lockDecoders();
    
    DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    if (decoderInfo && decoderInfo->decoder) {
        decoderInfo->decoder->SetCallback(callback);
        LOGD("Set callback for decoder channel %d", channelIndex);
    }
}

bool DecoderManager::isDecoderReady(int channelIndex) const {
    auto lock = const_cast<DecoderManager*>(this)->lockDecoders();
    
    const DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    return decoderInfo && (decoderInfo->state == READY || decoderInfo->state == DECODING);
}

DecoderManager::DecoderState DecoderManager::getDecoderState(int channelIndex) const {
    auto lock = const_cast<DecoderManager*>(this)->lockDecoders();
    
    const DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    return decoderInfo ? decoderInfo->state : DESTROYED;
}

int DecoderManager::getFrameCount(int channelIndex) const {
    auto lock = const_cast<DecoderManager*>(this)->lockDecoders();
    
    const DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    return decoderInfo ? decoderInfo->frameCount.load() : 0;
}

int DecoderManager::getErrorCount(int channelIndex) const {
    auto lock = const_cast<DecoderManager*>(this)->lockDecoders();
    
    const DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    return decoderInfo ? decoderInfo->errorCount.load() : 0;
}

std::vector<int> DecoderManager::getActiveChannels() const {
    auto lock = const_cast<DecoderManager*>(this)->lockDecoders();
    
    std::vector<int> activeChannels;
    for (const auto& pair : decoders) {
        if (pair.second->state == READY || pair.second->state == DECODING) {
            activeChannels.push_back(pair.first);
        }
    }
    return activeChannels;
}

bool DecoderManager::hasCapacityForNewDecoder() const {
    return activeDecoderCount.load() < maxDecoders.load();
}

void DecoderManager::healthMonitorLoop() {
    LOGD("Decoder health monitor started");
    
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(healthMonitorMutex);
        healthMonitorCv.wait_for(lock, std::chrono::milliseconds(HEALTH_CHECK_INTERVAL_MS));
        
        if (shouldStop) break;
        
        // Check health of all decoders
        auto decodersLock = lockDecoders();
        for (auto& pair : decoders) {
            checkDecoderHealth(pair.second.get());
        }
        decodersLock.unlock();
        
        // Update memory usage
        updateMemoryUsage();
    }
    
    LOGD("Decoder health monitor stopped");
}

void DecoderManager::checkDecoderHealth(DecoderInfo* decoderInfo) {
    if (!decoderInfo) return;
    
    // Check for timeout
    if (isDecoderTimedOut(decoderInfo)) {
        LOGW("Decoder timeout detected for channel %d", decoderInfo->channelIndex);
        handleDecoderError(decoderInfo->channelIndex, "Decoder timeout");
    }
    
    // Check error rate
    if (decoderInfo->frameCount > 0) {
        float errorRate = static_cast<float>(decoderInfo->errorCount) / decoderInfo->frameCount;
        if (errorRate > 0.1f) { // 10% error rate threshold
            LOGW("High error rate detected for channel %d: %.2f%%", 
                 decoderInfo->channelIndex, errorRate * 100);
        }
    }
}

bool DecoderManager::isDecoderTimedOut(const DecoderInfo* decoderInfo) const {
    if (!decoderInfo || decoderInfo->state != READY) return false;
    
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - decoderInfo->lastFrameTime);
    
    return timeSinceLastFrame.count() > DECODER_TIMEOUT_MS;
}

void DecoderManager::updateMemoryUsage() {
    long totalMemory = 0;
    
    auto lock = lockDecoders();
    for (const auto& pair : decoders) {
        totalMemory += estimateDecoderMemoryUsage(pair.second.get());
    }
    
    totalMemoryUsage.store(totalMemory);
}

long DecoderManager::estimateDecoderMemoryUsage(const DecoderInfo* decoderInfo) const {
    if (!decoderInfo) return 0;
    
    // Rough estimation: base decoder memory + frame buffers
    long baseMemory = 10 * 1024 * 1024; // 10MB base
    long frameMemory = decoderInfo->width * decoderInfo->height * 3; // RGB frame
    
    return baseMemory + frameMemory * 4; // Assume 4 frame buffers
}

void DecoderManager::handleDecoderError(int channelIndex, const std::string& error) {
    auto lock = lockDecoders();
    
    DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    if (decoderInfo) {
        decoderInfo->lastError = error;
        updateDecoderState(channelIndex, ERROR);
    }
    
    if (eventListener) {
        eventListener->onDecoderError(channelIndex, error);
    }
    
    LOGE("Decoder error for channel %d: %s", channelIndex, error.c_str());
}

// Utility methods
DecoderManager::DecoderInfo* DecoderManager::getDecoderInfo(int channelIndex) {
    auto it = decoders.find(channelIndex);
    return (it != decoders.end()) ? it->second.get() : nullptr;
}

const DecoderManager::DecoderInfo* DecoderManager::getDecoderInfo(int channelIndex) const {
    auto it = decoders.find(channelIndex);
    return (it != decoders.end()) ? it->second.get() : nullptr;
}

void DecoderManager::updateDecoderState(int channelIndex, DecoderState newState) {
    DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    if (decoderInfo) {
        decoderInfo->state = newState;
    }
}

void DecoderManager::setEventListener(DecoderEventListener* listener) {
    eventListener = listener;
}

void DecoderManager::setMaxDecoders(int maxDecoders) {
    this->maxDecoders.store(maxDecoders);
    LOGD("Updated max decoders to %d", maxDecoders);
}

void DecoderManager::cleanup() {
    LOGD("Cleaning up DecoderManager");
    
    // Stop health monitor
    shouldStop = true;
    healthMonitorCv.notify_all();
    
    if (healthMonitorThread.joinable()) {
        healthMonitorThread.join();
    }
    
    // Destroy all decoders
    auto lock = lockDecoders();
    for (auto& pair : decoders) {
        updateDecoderState(pair.first, DESTROYED);
    }
    decoders.clear();
    activeDecoderCount = 0;
    
    LOGD("DecoderManager cleanup complete");
}

bool DecoderManager::resetDecoder(int channelIndex) {
    auto lock = lockDecoders();

    DecoderInfo* decoderInfo = getDecoderInfo(channelIndex);
    if (!decoderInfo) {
        return false;
    }

    LOGD("Resetting decoder for channel %d", channelIndex);

    // Store configuration
    int codecType = decoderInfo->codecType;
    int fps = decoderInfo->fps;
    rknn_app_context_t* context = decoderInfo->context;

    // Destroy current decoder
    destroyDecoder(channelIndex);

    // Recreate decoder
    if (createDecoder(channelIndex, context, codecType, fps)) {
        return initializeDecoder(channelIndex);
    }

    return false;
}

void DecoderManager::optimizeMemoryUsage() {
    auto lock = lockDecoders();

    LOGD("Optimizing decoder memory usage");

    // Find decoders with high error rates or low usage
    std::vector<int> candidatesForReset;

    for (const auto& pair : decoders) {
        const DecoderInfo* decoderInfo = pair.second.get();

        if (decoderInfo->frameCount > 100) {
            float errorRate = static_cast<float>(decoderInfo->errorCount) / decoderInfo->frameCount;
            if (errorRate > 0.05f) { // 5% error rate
                candidatesForReset.push_back(pair.first);
            }
        }
    }

    // Reset problematic decoders
    for (int channelIndex : candidatesForReset) {
        lock.unlock();
        resetDecoder(channelIndex);
        lock.lock();
    }
}

void DecoderManager::cleanupIdleDecoders() {
    auto lock = lockDecoders();

    auto now = std::chrono::steady_clock::now();
    std::vector<int> idleDecoders;

    for (const auto& pair : decoders) {
        const DecoderInfo* decoderInfo = pair.second.get();

        auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::minutes>(
            now - decoderInfo->lastFrameTime);

        if (timeSinceLastFrame.count() > 5) { // 5 minutes idle
            idleDecoders.push_back(pair.first);
        }
    }

    // Remove idle decoders
    for (int channelIndex : idleDecoders) {
        LOGD("Cleaning up idle decoder for channel %d", channelIndex);
        destroyDecoder(channelIndex);
    }
}

// DecoderPool implementation
DecoderPool::DecoderPool(int maxSize) : poolSize(0), maxPoolSize(maxSize) {
    LOGD("DecoderPool initialized with max size %d", maxSize);
}

DecoderPool::~DecoderPool() {
    cleanup();
}

MppDecoder* DecoderPool::acquireDecoder() {
    std::lock_guard<std::mutex> lock(poolMutex);

    PooledDecoder* pooledDecoder = findAvailableDecoder();

    if (!pooledDecoder) {
        // Create new decoder if pool not full
        if (poolSize < maxPoolSize) {
            createNewDecoder();
            pooledDecoder = findAvailableDecoder();
        }
    }

    if (pooledDecoder) {
        pooledDecoder->inUse = true;
        pooledDecoder->usageCount++;
        pooledDecoder->lastUsed = std::chrono::steady_clock::now();
        return pooledDecoder->decoder.get();
    }

    return nullptr;
}

void DecoderPool::releaseDecoder(MppDecoder* decoder) {
    std::lock_guard<std::mutex> lock(poolMutex);

    PooledDecoder* pooledDecoder = findDecoderByInstance(decoder);
    if (pooledDecoder) {
        pooledDecoder->inUse = false;
        pooledDecoder->lastUsed = std::chrono::steady_clock::now();
    }
}

void DecoderPool::createNewDecoder() {
    auto pooledDecoder = std::make_unique<PooledDecoder>();
    pooledDecoder->decoder = std::make_unique<MppDecoder>();

    decoderPool.push_back(std::move(pooledDecoder));
    poolSize++;

    LOGD("Created new decoder in pool (size: %d)", poolSize);
}

DecoderPool::PooledDecoder* DecoderPool::findAvailableDecoder() {
    for (auto& pooledDecoder : decoderPool) {
        if (!pooledDecoder->inUse) {
            return pooledDecoder.get();
        }
    }
    return nullptr;
}

DecoderPool::PooledDecoder* DecoderPool::findDecoderByInstance(MppDecoder* decoder) {
    for (auto& pooledDecoder : decoderPool) {
        if (pooledDecoder->decoder.get() == decoder) {
            return pooledDecoder.get();
        }
    }
    return nullptr;
}

int DecoderPool::getAvailableDecoders() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(poolMutex));

    int available = 0;
    for (const auto& pooledDecoder : decoderPool) {
        if (!pooledDecoder->inUse) {
            available++;
        }
    }
    return available;
}

int DecoderPool::getUsedDecoders() const {
    return poolSize - getAvailableDecoders();
}

void DecoderPool::cleanup() {
    std::lock_guard<std::mutex> lock(poolMutex);

    decoderPool.clear();
    poolSize = 0;

    LOGD("DecoderPool cleanup complete");
}

// EnhancedMultiChannelDecoder implementation
EnhancedMultiChannelDecoder::EnhancedMultiChannelDecoder(int maxChannels) {
    decoderManager = std::make_unique<DecoderManager>(maxChannels);
    decoderPool = std::make_unique<DecoderPool>(maxChannels / 2); // Pool size is half of max channels

    LOGD("EnhancedMultiChannelDecoder initialized for %d channels", maxChannels);
}

EnhancedMultiChannelDecoder::~EnhancedMultiChannelDecoder() {
    cleanup();
}

bool EnhancedMultiChannelDecoder::addChannel(int channelIndex, rknn_app_context_t* context) {
    if (!decoderManager->createDecoder(channelIndex, context)) {
        return false;
    }

    return decoderManager->initializeDecoder(channelIndex);
}

bool EnhancedMultiChannelDecoder::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(callbacksMutex);

    frameCallbacks.erase(channelIndex);
    return decoderManager->destroyDecoder(channelIndex);
}

bool EnhancedMultiChannelDecoder::decodeFrame(int channelIndex, uint8_t* data, int size, int64_t timestamp) {
    return decoderManager->decodeFrame(channelIndex, data, size, timestamp);
}

bool EnhancedMultiChannelDecoder::isChannelReady(int channelIndex) const {
    return decoderManager->isDecoderReady(channelIndex);
}

void EnhancedMultiChannelDecoder::setFrameCallback(int channelIndex,
                                                  std::function<void(int, void*, int, int)> callback) {
    std::lock_guard<std::mutex> lock(callbacksMutex);
    frameCallbacks[channelIndex] = callback;

    // Set decoder callback to forward to our callback
    auto frameCallbackWrapper = [this, channelIndex](void* userdata, int width_stride, int height_stride,
                                                     int width, int height, int format, int fd, void* data) {
        handleFrameDecoded(channelIndex, data, width, height);
    };

    // Note: This is a simplified approach. In practice, we'd need to store the wrapper
    // and manage its lifetime properly
}

void EnhancedMultiChannelDecoder::removeFrameCallback(int channelIndex) {
    std::lock_guard<std::mutex> lock(callbacksMutex);
    frameCallbacks.erase(channelIndex);
}

int EnhancedMultiChannelDecoder::getActiveChannelCount() const {
    return decoderManager->getActiveDecoderCount();
}

std::vector<int> EnhancedMultiChannelDecoder::getActiveChannels() const {
    return decoderManager->getActiveChannels();
}

void EnhancedMultiChannelDecoder::optimizeResources() {
    decoderManager->optimizeMemoryUsage();
    decoderManager->cleanupIdleDecoders();
}

void EnhancedMultiChannelDecoder::handleFrameDecoded(int channelIndex, void* frameData, int width, int height) {
    std::lock_guard<std::mutex> lock(callbacksMutex);

    auto it = frameCallbacks.find(channelIndex);
    if (it != frameCallbacks.end()) {
        it->second(channelIndex, frameData, width, height);
    }
}

void EnhancedMultiChannelDecoder::cleanup() {
    std::lock_guard<std::mutex> lock(callbacksMutex);

    frameCallbacks.clear();
    decoderManager.reset();
    decoderPool.reset();

    LOGD("EnhancedMultiChannelDecoder cleanup complete");
}
