#include "MultiStreamDetectionIntegration.h"
#include <algorithm>
#include <sstream>

MultiStreamDetectionIntegration::MultiStreamDetectionIntegration()
    : multiStreamSystem(nullptr), statsThreadRunning(false) {
    LOGD("MultiStreamDetectionIntegration created");
}

MultiStreamDetectionIntegration::~MultiStreamDetectionIntegration() {
    cleanup();
    LOGD("MultiStreamDetectionIntegration destroyed");
}

bool MultiStreamDetectionIntegration::initialize(char* modelData, int modelSize, 
                                                MultiStreamIntegration* multiStreamSystem) {
    if (!modelData || modelSize <= 0) {
        LOGE("Invalid model data provided");
        return false;
    }
    
    // Initialize per-channel detection
    perChannelDetection = std::make_unique<PerChannelDetection>();
    if (!perChannelDetection->initialize(modelData, modelSize)) {
        LOGE("Failed to initialize per-channel detection");
        return false;
    }
    
    // Set this as the event listener
    perChannelDetection->setEventListener(this);
    
    // Initialize result manager
    resultManager = std::make_unique<DetectionResultManager>();
    
    // Set multi-stream system
    this->multiStreamSystem = multiStreamSystem;
    if (multiStreamSystem) {
        setupMultiStreamCallbacks();
    }
    
    // Start statistics thread
    statsThreadRunning = true;
    statsUpdateThread = std::thread(&MultiStreamDetectionIntegration::statisticsUpdateLoop, this);
    
    LOGD("MultiStreamDetectionIntegration initialized successfully");
    return true;
}

void MultiStreamDetectionIntegration::cleanup() {
    // Stop statistics thread
    statsThreadRunning = false;
    if (statsUpdateThread.joinable()) {
        statsUpdateThread.join();
    }
    
    // Cleanup per-channel detection
    if (perChannelDetection) {
        perChannelDetection->cleanup();
        perChannelDetection.reset();
    }
    
    // Cleanup result manager
    if (resultManager) {
        resultManager->clearAllResults();
        resultManager.reset();
    }
    
    // Clear configurations
    {
        std::lock_guard<std::mutex> lock(configMutex);
        channelConfigs.clear();
    }
    
    // Clear frame counters
    {
        std::lock_guard<std::mutex> lock(frameCounterMutex);
        channelFrameCounters.clear();
    }
    
    multiStreamSystem = nullptr;
    LOGD("MultiStreamDetectionIntegration cleanup completed");
}

bool MultiStreamDetectionIntegration::addDetectionChannel(int channelIndex, 
                                                         const DetectionChannelConfig& config) {
    if (!perChannelDetection) {
        LOGE("Per-channel detection not initialized");
        return false;
    }
    
    // Add to per-channel detection
    PerChannelDetection::DetectionConfig detectionConfig(channelIndex);
    detectionConfig.enabled = config.detectionEnabled;
    detectionConfig.confidenceThreshold = config.confidenceThreshold;
    detectionConfig.maxDetections = config.maxDetections;
    detectionConfig.enableNMS = config.enableNMS;
    detectionConfig.nmsThreshold = config.nmsThreshold;
    detectionConfig.enabledClasses = config.enabledClasses;
    
    if (!perChannelDetection->addChannel(channelIndex, detectionConfig)) {
        LOGE("Failed to add channel %d to per-channel detection", channelIndex);
        return false;
    }
    
    // Add to result manager
    if (!resultManager->addChannel(channelIndex)) {
        LOGE("Failed to add channel %d to result manager", channelIndex);
        perChannelDetection->removeChannel(channelIndex);
        return false;
    }
    
    // Store channel configuration
    {
        std::lock_guard<std::mutex> lock(configMutex);
        channelConfigs[channelIndex] = config;
        channelConfigs[channelIndex].channelIndex = channelIndex;
    }
    
    // Initialize frame counter
    {
        std::lock_guard<std::mutex> lock(frameCounterMutex);
        channelFrameCounters[channelIndex] = 0;
    }
    
    LOGD("Detection channel %d added successfully", channelIndex);
    return true;
}

bool MultiStreamDetectionIntegration::removeDetectionChannel(int channelIndex) {
    if (!perChannelDetection) {
        return false;
    }
    
    // Remove from per-channel detection
    perChannelDetection->removeChannel(channelIndex);
    
    // Remove from result manager
    resultManager->removeChannel(channelIndex);
    
    // Remove configuration
    {
        std::lock_guard<std::mutex> lock(configMutex);
        channelConfigs.erase(channelIndex);
    }
    
    // Remove frame counter
    {
        std::lock_guard<std::mutex> lock(frameCounterMutex);
        channelFrameCounters.erase(channelIndex);
    }
    
    LOGD("Detection channel %d removed successfully", channelIndex);
    return true;
}

bool MultiStreamDetectionIntegration::startChannelDetection(int channelIndex) {
    if (!perChannelDetection) {
        return false;
    }
    
    bool result = perChannelDetection->startDetection(channelIndex);
    if (result) {
        LOGD("Started detection for channel %d", channelIndex);
    }
    return result;
}

bool MultiStreamDetectionIntegration::stopChannelDetection(int channelIndex) {
    if (!perChannelDetection) {
        return false;
    }
    
    bool result = perChannelDetection->stopDetection(channelIndex);
    if (result) {
        LOGD("Stopped detection for channel %d", channelIndex);
    }
    return result;
}

bool MultiStreamDetectionIntegration::processFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData) {
    if (!perChannelDetection || !frameData) {
        return false;
    }
    
    // Check if detection is enabled for this channel
    auto config = getChannelConfigInternal(channelIndex);
    if (!config || !config->detectionEnabled) {
        return false;
    }
    
    // Update frame counter
    {
        std::lock_guard<std::mutex> lock(frameCounterMutex);
        auto it = channelFrameCounters.find(channelIndex);
        if (it != channelFrameCounters.end()) {
            it->second++;
        }
    }
    
    // Submit frame for detection
    return perChannelDetection->submitFrame(channelIndex, frameData);
}

bool MultiStreamDetectionIntegration::getChannelDetectionsNonBlocking(int channelIndex, 
                                                                     std::vector<Detection>& detections) {
    if (!perChannelDetection) {
        return false;
    }
    
    PerChannelDetection::DetectionResult result;
    if (perChannelDetection->getDetectionResultNonBlocking(channelIndex, result)) {
        detections = result.detections;
        
        // Store result in result manager
        resultManager->storeResult(channelIndex, result);
        
        // Notify callback if set
        if (detectionCallback) {
            notifyDetectionCallback(channelIndex, detections);
        }
        
        return true;
    }
    
    return false;
}

void MultiStreamDetectionIntegration::enableGlobalDetection(bool enabled) {
    if (perChannelDetection) {
        perChannelDetection->enableGlobalDetection(enabled);
        LOGD("Global detection %s", enabled ? "enabled" : "disabled");
    }
}

bool MultiStreamDetectionIntegration::isGlobalDetectionEnabled() const {
    return perChannelDetection ? perChannelDetection->isGlobalDetectionEnabled() : false;
}

void MultiStreamDetectionIntegration::setChannelConfig(int channelIndex, 
                                                      const DetectionChannelConfig& config) {
    {
        std::lock_guard<std::mutex> lock(configMutex);
        channelConfigs[channelIndex] = config;
        channelConfigs[channelIndex].channelIndex = channelIndex;
    }
    
    applyChannelConfig(channelIndex, config);
    LOGD("Updated configuration for channel %d", channelIndex);
}

MultiStreamDetectionIntegration::DetectionChannelConfig 
MultiStreamDetectionIntegration::getChannelConfig(int channelIndex) const {
    std::lock_guard<std::mutex> lock(configMutex);
    
    auto it = channelConfigs.find(channelIndex);
    if (it != channelConfigs.end()) {
        return it->second;
    }
    
    return DetectionChannelConfig(channelIndex);
}

MultiStreamDetectionIntegration::DetectionSystemStats 
MultiStreamDetectionIntegration::getSystemStats() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return systemStats;
}

std::vector<int> MultiStreamDetectionIntegration::getActiveDetectionChannels() const {
    return perChannelDetection ? perChannelDetection->getActiveChannels() : std::vector<int>();
}

void MultiStreamDetectionIntegration::setDetectionCallback(DetectionCallback callback) {
    detectionCallback = callback;
}

void MultiStreamDetectionIntegration::setErrorCallback(ErrorCallback callback) {
    errorCallback = callback;
}

void MultiStreamDetectionIntegration::setStatsCallback(StatsCallback callback) {
    statsCallback = callback;
}

// PerChannelDetection::DetectionEventListener implementation
void MultiStreamDetectionIntegration::onDetectionCompleted(int channelIndex, 
                                                          const PerChannelDetection::DetectionResult& result) {
    processDetectionResult(channelIndex, result);
}

void MultiStreamDetectionIntegration::onDetectionError(int channelIndex, const std::string& error) {
    LOGE("Detection error on channel %d: %s", channelIndex, error.c_str());
    if (errorCallback) {
        notifyErrorCallback(channelIndex, error);
    }
}

void MultiStreamDetectionIntegration::onQueueOverflow(int channelIndex, int droppedFrames) {
    LOGW("Queue overflow on channel %d: %d frames dropped", channelIndex, droppedFrames);
}

void MultiStreamDetectionIntegration::onStateChanged(int channelIndex, 
                                                    PerChannelDetection::DetectionState oldState,
                                                    PerChannelDetection::DetectionState newState) {
    LOGD("Channel %d detection state changed: %d -> %d", channelIndex, oldState, newState);
}

// Private methods
void MultiStreamDetectionIntegration::updateSystemStatistics() {
    if (!perChannelDetection) return;
    
    std::lock_guard<std::mutex> lock(statsMutex);
    
    // Get all channel statistics
    auto allChannelStats = perChannelDetection->getAllChannelStats();
    systemStats.channelStats.clear();
    
    systemStats.totalChannels = allChannelStats.size();
    systemStats.activeDetectionChannels = perChannelDetection->getActiveChannelCount();
    systemStats.totalFramesProcessed = 0;
    systemStats.totalDetections = 0;
    
    for (const auto& channelStat : allChannelStats) {
        systemStats.channelStats[channelStat.channelIndex] = channelStat;
        systemStats.totalFramesProcessed += channelStat.totalFramesProcessed;
        systemStats.totalDetections += channelStat.totalDetections;
    }
    
    // Calculate averages
    if (systemStats.totalFramesProcessed > 0) {
        systemStats.averageDetectionsPerFrame = 
            static_cast<float>(systemStats.totalDetections) / systemStats.totalFramesProcessed;
    }
    
    // Calculate system detection FPS (simplified)
    systemStats.systemDetectionFps = systemStats.activeDetectionChannels * 30.0f; // Approximate
}

void MultiStreamDetectionIntegration::statisticsUpdateLoop() {
    while (statsThreadRunning) {
        updateSystemStatistics();
        
        // Notify stats callback
        if (statsCallback) {
            notifyStatsCallback();
        }
        
        // Sleep for 2 seconds
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void MultiStreamDetectionIntegration::processDetectionResult(int channelIndex, 
                                                           const PerChannelDetection::DetectionResult& result) {
    // Store result in result manager
    resultManager->storeResult(channelIndex, result);
    
    // Notify detection callback
    if (detectionCallback) {
        notifyDetectionCallback(channelIndex, result.detections);
    }
    
    LOGD("Processed detection result for channel %d: %zu detections", 
         channelIndex, result.detections.size());
}

MultiStreamDetectionIntegration::DetectionChannelConfig* 
MultiStreamDetectionIntegration::getChannelConfigInternal(int channelIndex) {
    std::lock_guard<std::mutex> lock(configMutex);
    
    auto it = channelConfigs.find(channelIndex);
    return (it != channelConfigs.end()) ? &it->second : nullptr;
}

void MultiStreamDetectionIntegration::applyChannelConfig(int channelIndex, 
                                                        const DetectionChannelConfig& config) {
    if (!perChannelDetection) return;
    
    // Convert to PerChannelDetection config
    PerChannelDetection::DetectionConfig detectionConfig(channelIndex);
    detectionConfig.enabled = config.detectionEnabled;
    detectionConfig.confidenceThreshold = config.confidenceThreshold;
    detectionConfig.maxDetections = config.maxDetections;
    detectionConfig.enableNMS = config.enableNMS;
    detectionConfig.nmsThreshold = config.nmsThreshold;
    detectionConfig.enabledClasses = config.enabledClasses;
    
    perChannelDetection->setChannelConfig(channelIndex, detectionConfig);
}

void MultiStreamDetectionIntegration::notifyDetectionCallback(int channelIndex, 
                                                            const std::vector<Detection>& detections) {
    try {
        if (detectionCallback) {
            detectionCallback(channelIndex, detections);
        }
    } catch (const std::exception& e) {
        LOGE("Exception in detection callback for channel %d: %s", channelIndex, e.what());
    }
}

void MultiStreamDetectionIntegration::notifyErrorCallback(int channelIndex, const std::string& error) {
    try {
        if (errorCallback) {
            errorCallback(channelIndex, error);
        }
    } catch (const std::exception& e) {
        LOGE("Exception in error callback for channel %d: %s", channelIndex, e.what());
    }
}

void MultiStreamDetectionIntegration::notifyStatsCallback() {
    try {
        if (statsCallback) {
            statsCallback(systemStats);
        }
    } catch (const std::exception& e) {
        LOGE("Exception in stats callback: %s", e.what());
    }
}

// Additional methods implementation
bool MultiStreamDetectionIntegration::isDetectionChannelActive(int channelIndex) const {
    return perChannelDetection ? perChannelDetection->isChannelActive(channelIndex) : false;
}

bool MultiStreamDetectionIntegration::pauseChannelDetection(int channelIndex) {
    return perChannelDetection ? perChannelDetection->pauseDetection(channelIndex) : false;
}

bool MultiStreamDetectionIntegration::resumeChannelDetection(int channelIndex) {
    return perChannelDetection ? perChannelDetection->resumeDetection(channelIndex) : false;
}

void MultiStreamDetectionIntegration::startAllDetection() {
    if (!perChannelDetection) return;

    std::lock_guard<std::mutex> lock(configMutex);
    for (const auto& pair : channelConfigs) {
        if (pair.second.detectionEnabled) {
            perChannelDetection->startDetection(pair.first);
        }
    }
    LOGD("Started detection for all enabled channels");
}

void MultiStreamDetectionIntegration::stopAllDetection() {
    if (!perChannelDetection) return;

    std::lock_guard<std::mutex> lock(configMutex);
    for (const auto& pair : channelConfigs) {
        perChannelDetection->stopDetection(pair.first);
    }
    LOGD("Stopped detection for all channels");
}

void MultiStreamDetectionIntegration::setGlobalConfidenceThreshold(float threshold) {
    if (perChannelDetection) {
        perChannelDetection->setGlobalConfidenceThreshold(threshold);
    }

    // Update all channel configs
    std::lock_guard<std::mutex> lock(configMutex);
    for (auto& pair : channelConfigs) {
        pair.second.confidenceThreshold = threshold;
    }

    LOGD("Set global confidence threshold to %.2f", threshold);
}

void MultiStreamDetectionIntegration::setGlobalMaxDetections(int maxDetections) {
    std::lock_guard<std::mutex> lock(configMutex);
    for (auto& pair : channelConfigs) {
        pair.second.maxDetections = maxDetections;
        applyChannelConfig(pair.first, pair.second);
    }
    LOGD("Set global max detections to %d", maxDetections);
}

bool MultiStreamDetectionIntegration::getChannelDetections(int channelIndex, std::vector<Detection>& detections) {
    if (!perChannelDetection) {
        return false;
    }

    PerChannelDetection::DetectionResult result;
    if (perChannelDetection->getDetectionResult(channelIndex, result)) {
        detections = result.detections;
        resultManager->storeResult(channelIndex, result);
        return true;
    }

    return false;
}

PerChannelDetection::DetectionStats MultiStreamDetectionIntegration::getChannelStats(int channelIndex) const {
    if (perChannelDetection) {
        return perChannelDetection->getChannelStats(channelIndex);
    }
    return PerChannelDetection::DetectionStats(channelIndex);
}

void MultiStreamDetectionIntegration::setMultiStreamSystem(MultiStreamIntegration* system) {
    multiStreamSystem = system;
    if (system) {
        setupMultiStreamCallbacks();
    }
}

bool MultiStreamDetectionIntegration::integrateWithMultiStream() {
    if (!multiStreamSystem) {
        LOGE("Multi-stream system not set");
        return false;
    }

    setupMultiStreamCallbacks();
    LOGD("Integrated with multi-stream system");
    return true;
}

void MultiStreamDetectionIntegration::disconnectFromMultiStream() {
    multiStreamSystem = nullptr;
    LOGD("Disconnected from multi-stream system");
}

bool MultiStreamDetectionIntegration::enableVisualization(int channelIndex, bool enabled) {
    auto config = getChannelConfigInternal(channelIndex);
    if (config) {
        config->visualizationEnabled = enabled;
        LOGD("Visualization %s for channel %d", enabled ? "enabled" : "disabled", channelIndex);
        return true;
    }
    return false;
}

bool MultiStreamDetectionIntegration::isVisualizationEnabled(int channelIndex) const {
    std::lock_guard<std::mutex> lock(configMutex);
    auto it = channelConfigs.find(channelIndex);
    return (it != channelConfigs.end()) ? it->second.visualizationEnabled : false;
}

void MultiStreamDetectionIntegration::setVisualizationStyle(int channelIndex, const std::string& style) {
    // This would integrate with DetectionVisualizationManager
    LOGD("Set visualization style '%s' for channel %d", style.c_str(), channelIndex);
}

void MultiStreamDetectionIntegration::optimizeForPerformance() {
    if (!perChannelDetection) return;

    // Apply performance optimizations
    std::lock_guard<std::mutex> lock(configMutex);
    for (auto& pair : channelConfigs) {
        // Reduce thread pool size for better resource utilization
        PerChannelDetection::DetectionConfig config = perChannelDetection->getChannelConfig(pair.first);
        config.threadPoolSize = std::min(config.threadPoolSize, 3);
        config.maxQueueSize = std::min(config.maxQueueSize, 30);
        perChannelDetection->setChannelConfig(pair.first, config);
    }

    LOGD("Applied performance optimizations");
}

void MultiStreamDetectionIntegration::setDetectionFrameSkip(int channelIndex, int skipFrames) {
    // This would be implemented to skip frames for performance
    LOGD("Set frame skip to %d for channel %d", skipFrames, channelIndex);
}

void MultiStreamDetectionIntegration::enableAdaptiveDetection(bool enabled) {
    // This would enable adaptive detection based on system load
    LOGD("Adaptive detection %s", enabled ? "enabled" : "disabled");
}

void MultiStreamDetectionIntegration::setupMultiStreamCallbacks() {
    if (!multiStreamSystem) return;

    // This would set up callbacks with the multi-stream system
    // to receive frame data and state changes
    LOGD("Set up multi-stream callbacks");
}

void MultiStreamDetectionIntegration::onMultiStreamFrameReceived(int channelIndex,
                                                                std::shared_ptr<frame_data_t> frameData) {
    // Process frame through detection pipeline
    processFrame(channelIndex, frameData);
}

void MultiStreamDetectionIntegration::onMultiStreamChannelStateChanged(int channelIndex,
                                                                      const std::string& state) {
    LOGD("Multi-stream channel %d state changed to: %s", channelIndex, state.c_str());

    // Adjust detection based on stream state
    if (state == "STREAMING") {
        startChannelDetection(channelIndex);
    } else if (state == "DISCONNECTED" || state == "ERROR") {
        stopChannelDetection(channelIndex);
    }
}

bool MultiStreamDetectionIntegration::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

// DetectionVisualizationManager implementation
DetectionVisualizationManager::DetectionVisualizationManager() {
    loadDefaultClassColors();
    LOGD("DetectionVisualizationManager created");
}

DetectionVisualizationManager::~DetectionVisualizationManager() {
    LOGD("DetectionVisualizationManager destroyed");
}

void DetectionVisualizationManager::setChannelVisualizationConfig(int channelIndex,
                                                                 const VisualizationConfig& config) {
    std::lock_guard<std::mutex> lock(configMutex);
    channelConfigs[channelIndex] = config;
    LOGD("Set visualization config for channel %d", channelIndex);
}

DetectionVisualizationManager::VisualizationConfig
DetectionVisualizationManager::getChannelVisualizationConfig(int channelIndex) const {
    std::lock_guard<std::mutex> lock(configMutex);

    auto it = channelConfigs.find(channelIndex);
    if (it != channelConfigs.end()) {
        return it->second;
    }

    return VisualizationConfig(); // Return default config
}

bool DetectionVisualizationManager::visualizeDetections(int channelIndex,
                                                       std::shared_ptr<frame_data_t> frameData,
                                                       const std::vector<Detection>& detections) {
    if (!frameData || !frameData->data) {
        return false;
    }

    auto config = getChannelVisualizationConfig(channelIndex);

    return drawDetectionsOnFrame(reinterpret_cast<uint8_t*>(frameData->data.get()),
                               frameData->screenW, frameData->screenH,
                               frameData->screenStride, detections, config);
}

bool DetectionVisualizationManager::drawDetectionsOnFrame(uint8_t* frameData, int width, int height,
                                                         int stride, const std::vector<Detection>& detections,
                                                         const VisualizationConfig& config) {
    if (!frameData || detections.empty()) {
        return false;
    }

    for (const auto& detection : detections) {
        if (config.showBoundingBoxes) {
            drawBoundingBox(frameData, width, height, stride, detection, config);
        }

        if (config.showConfidence) {
            drawConfidenceText(frameData, width, height, stride, detection, config);
        }

        if (config.showClassNames) {
            drawClassName(frameData, width, height, stride, detection, config);
        }
    }

    return true;
}

void DetectionVisualizationManager::setGlobalVisualizationStyle(VisualizationStyle style) {
    std::lock_guard<std::mutex> lock(configMutex);
    for (auto& pair : channelConfigs) {
        pair.second.style = style;
    }
    LOGD("Set global visualization style to %d", style);
}

void DetectionVisualizationManager::enableGlobalConfidenceDisplay(bool enabled) {
    std::lock_guard<std::mutex> lock(configMutex);
    for (auto& pair : channelConfigs) {
        pair.second.showConfidence = enabled;
    }
    LOGD("Global confidence display %s", enabled ? "enabled" : "disabled");
}

void DetectionVisualizationManager::enableGlobalClassNameDisplay(bool enabled) {
    std::lock_guard<std::mutex> lock(configMutex);
    for (auto& pair : channelConfigs) {
        pair.second.showClassNames = enabled;
    }
    LOGD("Global class name display %s", enabled ? "enabled" : "disabled");
}

void DetectionVisualizationManager::setClassColor(int classId, const std::string& color) {
    std::lock_guard<std::mutex> lock(configMutex);
    for (auto& pair : channelConfigs) {
        pair.second.classColors[classId] = color;
    }
    LOGD("Set color for class %d to %s", classId, color.c_str());
}

std::string DetectionVisualizationManager::getClassColor(int classId) const {
    std::lock_guard<std::mutex> lock(configMutex);

    // Try to find in any channel config
    for (const auto& pair : channelConfigs) {
        auto it = pair.second.classColors.find(classId);
        if (it != pair.second.classColors.end()) {
            return it->second;
        }
    }

    return getDefaultClassColor(classId);
}

void DetectionVisualizationManager::loadDefaultClassColors() {
    // Load default COCO class colors
    std::vector<std::string> defaultColors = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF",
        "#800000", "#008000", "#000080", "#808000", "#800080", "#008080",
        "#C0C0C0", "#808080", "#9999FF", "#993366", "#FFFFCC", "#CCFFFF",
        "#660066", "#FF8080", "#0066CC", "#CCCCFF", "#000080", "#FF00FF"
    };

    // This would be applied to default configs
    LOGD("Loaded default class colors");
}

void DetectionVisualizationManager::drawBoundingBox(uint8_t* frameData, int width, int height,
                                                   int stride, const Detection& detection,
                                                   const VisualizationConfig& config) {
    // Simplified bounding box drawing - would use actual graphics library
    int x1 = static_cast<int>(detection.box.x);
    int y1 = static_cast<int>(detection.box.y);
    int x2 = static_cast<int>(detection.box.x + detection.box.width);
    int y2 = static_cast<int>(detection.box.y + detection.box.height);

    // Clamp to frame boundaries
    x1 = std::max(0, std::min(x1, width - 1));
    y1 = std::max(0, std::min(y1, height - 1));
    x2 = std::max(0, std::min(x2, width - 1));
    y2 = std::max(0, std::min(y2, height - 1));

    // This would draw actual bounding box using graphics primitives
    LOGD("Drawing bounding box for class %d at (%d,%d)-(%d,%d)",
         detection.class_id, x1, y1, x2, y2);
}

void DetectionVisualizationManager::drawConfidenceText(uint8_t* frameData, int width, int height,
                                                      int stride, const Detection& detection,
                                                      const VisualizationConfig& config) {
    // This would draw confidence text using graphics library
    LOGD("Drawing confidence %.2f for class %d", detection.confidence, detection.class_id);
}

void DetectionVisualizationManager::drawClassName(uint8_t* frameData, int width, int height,
                                                 int stride, const Detection& detection,
                                                 const VisualizationConfig& config) {
    // This would draw class name using graphics library
    LOGD("Drawing class name '%s' for class %d", detection.className.c_str(), detection.class_id);
}

uint32_t DetectionVisualizationManager::parseColor(const std::string& colorStr) const {
    // Parse hex color string to RGBA value
    if (colorStr.length() == 7 && colorStr[0] == '#') {
        return std::stoul(colorStr.substr(1), nullptr, 16) | 0xFF000000; // Add alpha
    }
    return 0xFFFFFFFF; // Default white
}

std::string DetectionVisualizationManager::getDefaultClassColor(int classId) const {
    // Return default color based on class ID
    std::vector<std::string> colors = {
        "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"
    };
    return colors[classId % colors.size()];
}

// DetectionPerformanceMonitor implementation
DetectionPerformanceMonitor::DetectionPerformanceMonitor() : monitorRunning(false) {
    LOGD("DetectionPerformanceMonitor created");
}

DetectionPerformanceMonitor::~DetectionPerformanceMonitor() {
    stopMonitoring();
    LOGD("DetectionPerformanceMonitor destroyed");
}

void DetectionPerformanceMonitor::startMonitoring() {
    if (monitorRunning) {
        LOGW("Performance monitoring already running");
        return;
    }

    monitorRunning = true;
    monitorThread = std::thread(&DetectionPerformanceMonitor::monitoringLoop, this);
    LOGD("Performance monitoring started");
}

void DetectionPerformanceMonitor::stopMonitoring() {
    if (!monitorRunning) {
        return;
    }

    monitorRunning = false;
    if (monitorThread.joinable()) {
        monitorThread.join();
    }
    LOGD("Performance monitoring stopped");
}

void DetectionPerformanceMonitor::updateChannelMetrics(int channelIndex,
                                                      const PerformanceMetrics& metrics) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    channelMetrics[channelIndex] = metrics;
}

DetectionPerformanceMonitor::PerformanceMetrics
DetectionPerformanceMonitor::getChannelMetrics(int channelIndex) const {
    std::lock_guard<std::mutex> lock(metricsMutex);

    auto it = channelMetrics.find(channelIndex);
    if (it != channelMetrics.end()) {
        return it->second;
    }

    return PerformanceMetrics(); // Return default metrics
}

std::map<int, DetectionPerformanceMonitor::PerformanceMetrics>
DetectionPerformanceMonitor::getAllChannelMetrics() const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    return channelMetrics;
}

std::vector<int> DetectionPerformanceMonitor::identifyBottleneckChannels() const {
    std::vector<int> bottleneckChannels;
    std::lock_guard<std::mutex> lock(metricsMutex);

    for (const auto& pair : channelMetrics) {
        const auto& metrics = pair.second;

        // Identify bottlenecks based on various criteria
        if (metrics.averageDetectionTime > 100.0f ||  // > 100ms average
            metrics.queueUtilization > 80 ||          // > 80% queue utilization
            metrics.detectionFps < 15.0f) {           // < 15 FPS
            bottleneckChannels.push_back(pair.first);
        }
    }

    return bottleneckChannels;
}

std::vector<std::string> DetectionPerformanceMonitor::generateOptimizationRecommendations() const {
    std::vector<std::string> recommendations;
    std::lock_guard<std::mutex> lock(metricsMutex);

    for (const auto& pair : channelMetrics) {
        const auto& metrics = pair.second;
        int channelIndex = pair.first;

        if (metrics.averageDetectionTime > 100.0f) {
            recommendations.push_back("Channel " + std::to_string(channelIndex) +
                                    ": Consider reducing detection resolution or confidence threshold");
        }

        if (metrics.queueUtilization > 80) {
            recommendations.push_back("Channel " + std::to_string(channelIndex) +
                                    ": Consider increasing queue size or reducing input frame rate");
        }

        if (metrics.cpuUsage > 80.0f) {
            recommendations.push_back("Channel " + std::to_string(channelIndex) +
                                    ": High CPU usage detected, consider load balancing");
        }

        if (metrics.memoryUsage > 500 * 1024 * 1024) { // > 500MB
            recommendations.push_back("Channel " + std::to_string(channelIndex) +
                                    ": High memory usage detected, check for memory leaks");
        }
    }

    return recommendations;
}

bool DetectionPerformanceMonitor::shouldThrottleChannel(int channelIndex) const {
    auto metrics = getChannelMetrics(channelIndex);

    // Throttle if performance is poor
    return (metrics.averageDetectionTime > 150.0f ||  // > 150ms average
            metrics.queueUtilization > 90 ||          // > 90% queue utilization
            metrics.cpuUsage > 90.0f);                // > 90% CPU usage
}

void DetectionPerformanceMonitor::monitoringLoop() {
    while (monitorRunning) {
        collectSystemMetrics();
        analyzePerformance();

        // Sleep for 5 seconds
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void DetectionPerformanceMonitor::collectSystemMetrics() {
    // This would collect actual system metrics
    // For now, we'll just log that we're collecting metrics
    LOGD("Collecting system performance metrics for %zu channels", channelMetrics.size());
}

void DetectionPerformanceMonitor::analyzePerformance() {
    auto bottlenecks = identifyBottleneckChannels();
    if (!bottlenecks.empty()) {
        LOGW("Performance bottlenecks detected in %zu channels", bottlenecks.size());

        auto recommendations = generateOptimizationRecommendations();
        for (const auto& recommendation : recommendations) {
            LOGD("Recommendation: %s", recommendation.c_str());
        }
    }
}
