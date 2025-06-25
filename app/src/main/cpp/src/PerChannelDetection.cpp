#include "PerChannelDetection.h"
#include <algorithm>
#include <sstream>

PerChannelDetection::PerChannelDetection() 
    : eventListener(nullptr), modelData(nullptr), modelDataSize(0),
      activeChannelCount(0), globalEnabled(true), statsThreadRunning(false) {
    LOGD("PerChannelDetection created");
}

PerChannelDetection::~PerChannelDetection() {
    cleanup();
    LOGD("PerChannelDetection destroyed");
}

bool PerChannelDetection::initialize(char* modelData, int modelSize) {
    if (!modelData || modelSize <= 0) {
        LOGE("Invalid model data provided");
        return false;
    }
    
    // Copy model data
    this->modelData = new char[modelSize];
    memcpy(this->modelData, modelData, modelSize);
    this->modelDataSize = modelSize;
    
    // Start statistics thread
    statsThreadRunning = true;
    statsThread = std::thread(&PerChannelDetection::statisticsLoop, this);
    
    LOGD("PerChannelDetection initialized with model size: %d", modelSize);
    return true;
}

void PerChannelDetection::cleanup() {
    // Stop statistics thread
    statsThreadRunning = false;
    statsCondition.notify_all();
    if (statsThread.joinable()) {
        statsThread.join();
    }
    
    // Stop all channels
    std::lock_guard<std::mutex> lock(channelsMutex);
    for (auto& pair : channels) {
        cleanupChannel(pair.second.get());
    }
    channels.clear();
    
    // Clean up model data
    if (modelData) {
        delete[] modelData;
        modelData = nullptr;
        modelDataSize = 0;
    }
    
    activeChannelCount = 0;
    LOGD("PerChannelDetection cleanup completed");
}

bool PerChannelDetection::addChannel(int channelIndex, const DetectionConfig& config) {
    if (!validateChannelIndex(channelIndex)) {
        LOGE("Invalid channel index: %d", channelIndex);
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    // Check if channel already exists
    if (channels.find(channelIndex) != channels.end()) {
        LOGW("Channel %d already exists", channelIndex);
        return false;
    }
    
    // Create new channel info
    auto channelInfo = std::make_unique<ChannelDetectionInfo>(channelIndex);
    channelInfo->config = config;
    channelInfo->config.channelIndex = channelIndex; // Ensure consistency
    
    // Initialize thread pool for this channel
    channelInfo->threadPool = std::make_unique<Yolov5ThreadPool>();
    if (channelInfo->threadPool->setUpWithModelData(config.threadPoolSize, 
                                                   modelData, modelDataSize) != NN_SUCCESS) {
        LOGE("Failed to initialize thread pool for channel %d", channelIndex);
        return false;
    }
    
    // Start processing thread
    channelInfo->processingThread = std::thread(&PerChannelDetection::channelProcessingLoop, 
                                              this, channelIndex);
    
    channels[channelIndex] = std::move(channelInfo);
    LOGD("Channel %d added successfully", channelIndex);
    return true;
}

bool PerChannelDetection::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    auto it = channels.find(channelIndex);
    if (it == channels.end()) {
        LOGW("Channel %d not found", channelIndex);
        return false;
    }
    
    // Cleanup and remove channel
    cleanupChannel(it->second.get());
    channels.erase(it);
    
    if (activeChannelCount > 0) {
        activeChannelCount--;
    }
    
    LOGD("Channel %d removed successfully", channelIndex);
    return true;
}

bool PerChannelDetection::startDetection(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        LOGE("Channel %d not found", channelIndex);
        return false;
    }
    
    if (channelInfo->state == ACTIVE) {
        LOGW("Channel %d detection already active", channelIndex);
        return true;
    }
    
    changeChannelState(channelIndex, ACTIVE);
    channelInfo->isProcessing = true;
    activeChannelCount++;
    
    LOGD("Detection started for channel %d", channelIndex);
    return true;
}

bool PerChannelDetection::stopDetection(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        LOGE("Channel %d not found", channelIndex);
        return false;
    }
    
    changeChannelState(channelIndex, INACTIVE);
    channelInfo->isProcessing = false;
    
    if (activeChannelCount > 0) {
        activeChannelCount--;
    }
    
    // Clear queues
    {
        std::lock_guard<std::mutex> inputLock(channelInfo->inputMutex);
        std::queue<std::shared_ptr<frame_data_t>> empty;
        channelInfo->inputQueue.swap(empty);
    }
    
    {
        std::lock_guard<std::mutex> resultLock(channelInfo->resultMutex);
        std::queue<DetectionResult> empty;
        channelInfo->resultQueue.swap(empty);
    }
    
    LOGD("Detection stopped for channel %d", channelIndex);
    return true;
}

bool PerChannelDetection::submitFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData) {
    if (!frameData) {
        LOGE("Invalid frame data provided for channel %d", channelIndex);
        return false;
    }
    
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo || channelInfo->state != ACTIVE || !channelInfo->isProcessing) {
        LOGW("Channel %d not active for detection", channelIndex);
        return false;
    }
    
    if (!globalEnabled || !channelInfo->config.enabled) {
        return false; // Detection disabled
    }
    
    std::lock_guard<std::mutex> lock(channelInfo->inputMutex);
    
    // Check queue size limit
    if (channelInfo->inputQueue.size() >= channelInfo->config.maxQueueSize) {
        // Drop oldest frame
        channelInfo->inputQueue.pop();
        channelInfo->stats.droppedFrames++;
        
        if (eventListener) {
            notifyQueueOverflow(channelIndex, 1);
        }
        
        LOGW("Queue overflow for channel %d, dropped frame", channelIndex);
    }
    
    channelInfo->inputQueue.push(frameData);
    channelInfo->inputCondition.notify_one();
    
    return true;
}

bool PerChannelDetection::getDetectionResultNonBlocking(int channelIndex, DetectionResult& result) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelInfo->resultMutex);
    
    if (channelInfo->resultQueue.empty()) {
        return false;
    }
    
    result = channelInfo->resultQueue.front();
    channelInfo->resultQueue.pop();
    return true;
}

void PerChannelDetection::channelProcessingLoop(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        LOGE("Channel info not found for processing loop: %d", channelIndex);
        return;
    }
    
    LOGD("Processing loop started for channel %d", channelIndex);
    
    while (!channelInfo->shouldStop) {
        std::shared_ptr<frame_data_t> frameData;
        
        // Wait for frame data
        {
            std::unique_lock<std::mutex> lock(channelInfo->inputMutex);
            channelInfo->inputCondition.wait(lock, [&] {
                return !channelInfo->inputQueue.empty() || channelInfo->shouldStop;
            });
            
            if (channelInfo->shouldStop) {
                break;
            }
            
            if (channelInfo->inputQueue.empty()) {
                continue;
            }
            
            frameData = channelInfo->inputQueue.front();
            channelInfo->inputQueue.pop();
        }
        
        // Process frame if detection is active
        if (channelInfo->isProcessing && frameData) {
            processFrame(channelInfo, frameData);
        }
    }
    
    LOGD("Processing loop ended for channel %d", channelIndex);
}

void PerChannelDetection::processFrame(ChannelDetectionInfo* channelInfo, 
                                     std::shared_ptr<frame_data_t> frameData) {
    if (!channelInfo || !frameData) {
        return;
    }
    
    auto startTime = std::chrono::steady_clock::now();
    
    try {
        // Submit frame to thread pool
        if (channelInfo->threadPool->submitTask(frameData) != NN_SUCCESS) {
            LOGE("Failed to submit frame to thread pool for channel %d", channelInfo->channelIndex);
            return;
        }
        
        // Get detection results
        std::vector<Detection> detections;
        auto ret = channelInfo->threadPool->getTargetResultNonBlock(detections, frameData->frameId);
        
        if (ret == NN_SUCCESS) {
            auto endTime = std::chrono::steady_clock::now();
            float processingTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
            
            // Create detection result
            DetectionResult result(channelInfo->channelIndex, frameData->frameId);
            result.detections = detections;
            result.processingTime = processingTime;
            
            // Apply confidence threshold filtering
            if (channelInfo->config.confidenceThreshold > 0.0f) {
                result.detections.erase(
                    std::remove_if(result.detections.begin(), result.detections.end(),
                        [threshold = channelInfo->config.confidenceThreshold](const Detection& det) {
                            return det.confidence < threshold;
                        }),
                    result.detections.end()
                );
            }
            
            // Limit max detections
            if (result.detections.size() > channelInfo->config.maxDetections) {
                result.detections.resize(channelInfo->config.maxDetections);
            }
            
            // Store result
            {
                std::lock_guard<std::mutex> lock(channelInfo->resultMutex);
                channelInfo->resultQueue.push(result);
                
                // Limit result queue size
                while (channelInfo->resultQueue.size() > 50) {
                    channelInfo->resultQueue.pop();
                }
            }
            
            // Update statistics
            updateChannelStats(channelInfo, result);
            
            // Notify listener
            if (eventListener) {
                eventListener->onDetectionCompleted(channelInfo->channelIndex, result);
            }
            
            LOGD("Detection completed for channel %d, frame %d: %zu detections in %.2fms",
                 channelInfo->channelIndex, frameData->frameId, result.detections.size(), processingTime);
        }
        
    } catch (const std::exception& e) {
        LOGE("Exception in processFrame for channel %d: %s", channelInfo->channelIndex, e.what());
        if (eventListener) {
            notifyError(channelInfo->channelIndex, std::string("Processing exception: ") + e.what());
        }
    }
}

void PerChannelDetection::updateChannelStats(ChannelDetectionInfo* channelInfo, 
                                            const DetectionResult& result) {
    if (!channelInfo) return;
    
    channelInfo->stats.totalFramesProcessed++;
    channelInfo->stats.totalDetections += result.detections.size();
    channelInfo->stats.averageDetectionsPerFrame = 
        static_cast<float>(channelInfo->stats.totalDetections) / channelInfo->stats.totalFramesProcessed;
    
    // Update processing time statistics
    float currentAvg = channelInfo->stats.averageProcessingTime;
    int frameCount = channelInfo->stats.totalFramesProcessed;
    channelInfo->stats.averageProcessingTime = 
        (currentAvg * (frameCount - 1) + result.processingTime) / frameCount;
    
    if (result.processingTime > channelInfo->stats.peakProcessingTime) {
        channelInfo->stats.peakProcessingTime = result.processingTime;
    }
    
    channelInfo->stats.lastUpdate = std::chrono::steady_clock::now();
}

PerChannelDetection::ChannelDetectionInfo* PerChannelDetection::getChannelInfo(int channelIndex) {
    std::lock_guard<std::mutex> lock(channelsMutex);
    auto it = channels.find(channelIndex);
    return (it != channels.end()) ? it->second.get() : nullptr;
}

const PerChannelDetection::ChannelDetectionInfo* PerChannelDetection::getChannelInfo(int channelIndex) const {
    std::lock_guard<std::mutex> lock(channelsMutex);
    auto it = channels.find(channelIndex);
    return (it != channels.end()) ? it->second.get() : nullptr;
}

void PerChannelDetection::changeChannelState(int channelIndex, DetectionState newState) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) return;
    
    DetectionState oldState = channelInfo->state;
    channelInfo->state = newState;
    
    if (eventListener && oldState != newState) {
        notifyStateChange(channelIndex, oldState, newState);
    }
}

bool PerChannelDetection::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

void PerChannelDetection::cleanupChannel(ChannelDetectionInfo* channelInfo) {
    if (!channelInfo) return;
    
    channelInfo->shouldStop = true;
    channelInfo->inputCondition.notify_all();
    
    if (channelInfo->processingThread.joinable()) {
        channelInfo->processingThread.join();
    }
    
    // Thread pool cleanup is handled by unique_ptr destructor
}

void PerChannelDetection::statisticsLoop() {
    while (statsThreadRunning) {
        std::unique_lock<std::mutex> lock(statsMutex);
        statsCondition.wait_for(lock, std::chrono::seconds(5), [this] { return !statsThreadRunning; });
        
        if (!statsThreadRunning) break;
        
        updateGlobalStatistics();
    }
}

void PerChannelDetection::updateGlobalStatistics() {
    // Update global statistics for all channels
    // This can be extended to collect system-wide metrics
    LOGD("Updated global detection statistics for %d active channels", activeChannelCount.load());
}

void PerChannelDetection::notifyError(int channelIndex, const std::string& error) {
    if (eventListener) {
        eventListener->onDetectionError(channelIndex, error);
    }
}

void PerChannelDetection::notifyQueueOverflow(int channelIndex, int droppedFrames) {
    if (eventListener) {
        eventListener->onQueueOverflow(channelIndex, droppedFrames);
    }
}

void PerChannelDetection::notifyStateChange(int channelIndex, DetectionState oldState, DetectionState newState) {
    if (eventListener) {
        eventListener->onStateChanged(channelIndex, oldState, newState);
    }
}

// DetectionResultManager implementation
DetectionResultManager::DetectionResultManager() {
    LOGD("DetectionResultManager created");
}

DetectionResultManager::~DetectionResultManager() {
    clearAllResults();
    LOGD("DetectionResultManager destroyed");
}

bool DetectionResultManager::addChannel(int channelIndex, int maxResults) {
    std::lock_guard<std::mutex> lock(managerMutex);
    
    if (channelResults.find(channelIndex) != channelResults.end()) {
        LOGW("Channel %d already exists in result manager", channelIndex);
        return false;
    }
    
    channelResults[channelIndex] = std::make_unique<ChannelResults>(channelIndex, maxResults);
    LOGD("Added channel %d to result manager with max results: %d", channelIndex, maxResults);
    return true;
}

bool DetectionResultManager::storeResult(int channelIndex, const PerChannelDetection::DetectionResult& result) {
    std::lock_guard<std::mutex> lock(managerMutex);
    
    auto it = channelResults.find(channelIndex);
    if (it == channelResults.end()) {
        LOGW("Channel %d not found in result manager", channelIndex);
        return false;
    }
    
    std::lock_guard<std::mutex> resultLock(it->second->resultsMutex);
    
    // Limit queue size
    while (it->second->results.size() >= it->second->maxResults) {
        it->second->results.pop();
    }
    
    it->second->results.push(result);
    return true;
}

bool DetectionResultManager::getLatestResult(int channelIndex, PerChannelDetection::DetectionResult& result) {
    std::lock_guard<std::mutex> lock(managerMutex);

    auto it = channelResults.find(channelIndex);
    if (it == channelResults.end()) {
        return false;
    }

    std::lock_guard<std::mutex> resultLock(it->second->resultsMutex);

    if (it->second->results.empty()) {
        return false;
    }

    result = it->second->results.back();
    return true;
}

// Additional PerChannelDetection methods
bool PerChannelDetection::isChannelActive(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    return channelInfo && channelInfo->state == ACTIVE;
}

bool PerChannelDetection::pauseDetection(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }

    if (channelInfo->state == ACTIVE) {
        changeChannelState(channelIndex, PAUSED);
        channelInfo->isProcessing = false;
        LOGD("Detection paused for channel %d", channelIndex);
        return true;
    }

    return false;
}

bool PerChannelDetection::resumeDetection(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }

    if (channelInfo->state == PAUSED) {
        changeChannelState(channelIndex, ACTIVE);
        channelInfo->isProcessing = true;
        LOGD("Detection resumed for channel %d", channelIndex);
        return true;
    }

    return false;
}

bool PerChannelDetection::getDetectionResult(int channelIndex, DetectionResult& result) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }

    std::unique_lock<std::mutex> lock(channelInfo->resultMutex);

    // Wait for result with timeout
    auto timeout = std::chrono::milliseconds(100);
    auto start = std::chrono::steady_clock::now();

    while (channelInfo->resultQueue.empty()) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            return false; // Timeout
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    result = channelInfo->resultQueue.front();
    channelInfo->resultQueue.pop();
    return true;
}

void PerChannelDetection::setChannelConfig(int channelIndex, const DetectionConfig& config) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        channelInfo->config = config;
        channelInfo->config.channelIndex = channelIndex; // Ensure consistency
        LOGD("Updated config for channel %d", channelIndex);
    }
}

PerChannelDetection::DetectionConfig PerChannelDetection::getChannelConfig(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        return channelInfo->config;
    }
    return DetectionConfig(channelIndex);
}

void PerChannelDetection::setEventListener(DetectionEventListener* listener) {
    eventListener = listener;
}

PerChannelDetection::DetectionStats PerChannelDetection::getChannelStats(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        return channelInfo->stats;
    }
    return DetectionStats(channelIndex);
}

std::vector<PerChannelDetection::DetectionStats> PerChannelDetection::getAllChannelStats() const {
    std::vector<DetectionStats> allStats;
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (const auto& pair : channels) {
        allStats.push_back(pair.second->stats);
    }

    return allStats;
}

std::vector<int> PerChannelDetection::getActiveChannels() const {
    std::vector<int> activeChannels;
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (const auto& pair : channels) {
        if (pair.second->state == ACTIVE) {
            activeChannels.push_back(pair.first);
        }
    }

    return activeChannels;
}

void PerChannelDetection::enableGlobalDetection(bool enabled) {
    globalEnabled = enabled;
    LOGD("Global detection %s", enabled ? "enabled" : "disabled");
}

void PerChannelDetection::setGlobalConfidenceThreshold(float threshold) {
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (auto& pair : channels) {
        pair.second->config.confidenceThreshold = threshold;
    }

    LOGD("Set global confidence threshold to %.2f", threshold);
}

int PerChannelDetection::getChannelQueueSize(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        std::lock_guard<std::mutex> lock(channelInfo->inputMutex);
        return channelInfo->inputQueue.size();
    }
    return 0;
}

void PerChannelDetection::clearChannelQueue(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        {
            std::lock_guard<std::mutex> inputLock(channelInfo->inputMutex);
            std::queue<std::shared_ptr<frame_data_t>> empty;
            channelInfo->inputQueue.swap(empty);
        }

        {
            std::lock_guard<std::mutex> resultLock(channelInfo->resultMutex);
            std::queue<DetectionResult> empty;
            channelInfo->resultQueue.swap(empty);
        }

        LOGD("Cleared queues for channel %d", channelIndex);
    }
}

void PerChannelDetection::clearAllQueues() {
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (auto& pair : channels) {
        clearChannelQueue(pair.first);
    }

    LOGD("Cleared all channel queues");
}

// Additional DetectionResultManager methods
bool DetectionResultManager::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(managerMutex);

    auto it = channelResults.find(channelIndex);
    if (it == channelResults.end()) {
        return false;
    }

    channelResults.erase(it);
    LOGD("Removed channel %d from result manager", channelIndex);
    return true;
}

bool DetectionResultManager::getAllResults(int channelIndex, std::vector<PerChannelDetection::DetectionResult>& results) {
    std::lock_guard<std::mutex> lock(managerMutex);

    auto it = channelResults.find(channelIndex);
    if (it == channelResults.end()) {
        return false;
    }

    std::lock_guard<std::mutex> resultLock(it->second->resultsMutex);

    results.clear();
    auto tempQueue = it->second->results;

    while (!tempQueue.empty()) {
        results.push_back(tempQueue.front());
        tempQueue.pop();
    }

    return true;
}

int DetectionResultManager::getResultCount(int channelIndex) const {
    std::lock_guard<std::mutex> lock(managerMutex);

    auto it = channelResults.find(channelIndex);
    if (it == channelResults.end()) {
        return 0;
    }

    std::lock_guard<std::mutex> resultLock(it->second->resultsMutex);
    return it->second->results.size();
}

void DetectionResultManager::clearChannelResults(int channelIndex) {
    std::lock_guard<std::mutex> lock(managerMutex);

    auto it = channelResults.find(channelIndex);
    if (it != channelResults.end()) {
        std::lock_guard<std::mutex> resultLock(it->second->resultsMutex);
        std::queue<PerChannelDetection::DetectionResult> empty;
        it->second->results.swap(empty);
        LOGD("Cleared results for channel %d", channelIndex);
    }
}

void DetectionResultManager::clearAllResults() {
    std::lock_guard<std::mutex> lock(managerMutex);

    for (auto& pair : channelResults) {
        std::lock_guard<std::mutex> resultLock(pair.second->resultsMutex);
        std::queue<PerChannelDetection::DetectionResult> empty;
        pair.second->results.swap(empty);
    }

    LOGD("Cleared all channel results");
}

std::vector<int> DetectionResultManager::getActiveChannels() const {
    std::vector<int> channels;
    std::lock_guard<std::mutex> lock(managerMutex);

    for (const auto& pair : channelResults) {
        channels.push_back(pair.first);
    }

    return channels;
}
