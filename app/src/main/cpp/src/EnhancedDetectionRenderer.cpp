#include "EnhancedDetectionRenderer.h"
#include "logging.h"
#include <algorithm>
#include <chrono>

EnhancedDetectionRenderer::EnhancedDetectionRenderer() 
    : lastOptimization(std::chrono::steady_clock::now()) {
    LOGD("EnhancedDetectionRenderer created");
}

EnhancedDetectionRenderer::~EnhancedDetectionRenderer() {
    std::lock_guard<std::mutex> lock(statesMutex);
    channelStates.clear();
    LOGD("EnhancedDetectionRenderer destroyed");
}

bool EnhancedDetectionRenderer::addChannel(int channelIndex, int width, int height) {
    if (!validateChannelIndex(channelIndex)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(statesMutex);
    
    if (channelStates.find(channelIndex) != channelStates.end()) {
        LOGW("Channel %d already exists", channelIndex);
        return false;
    }

    auto state = std::make_unique<ChannelRenderState>();
    state->viewportWidth = width;
    state->viewportHeight = height;
    state->config = calculateViewportConfig(width, height, false);
    
    channelStates[channelIndex] = std::move(state);
    
    LOGD("Added channel %d with viewport %dx%d", channelIndex, width, height);
    return true;
}

bool EnhancedDetectionRenderer::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto it = channelStates.find(channelIndex);
    if (it == channelStates.end()) {
        return false;
    }
    
    channelStates.erase(it);
    LOGD("Removed channel %d", channelIndex);
    return true;
}

bool EnhancedDetectionRenderer::updateChannelViewport(int channelIndex, int width, int height) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->viewportWidth = width;
    state->viewportHeight = height;
    state->config = calculateViewportConfig(width, height, state->isActive);
    state->lastUpdate = std::chrono::steady_clock::now();
    
    LOGD("Updated viewport for channel %d: %dx%d", channelIndex, width, height);
    return true;
}

bool EnhancedDetectionRenderer::setChannelActive(int channelIndex, bool active) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->isActive = active;
    // Recalculate config when active state changes
    state->config = calculateViewportConfig(state->viewportWidth, state->viewportHeight, active);
    
    LOGD("Set channel %d active state: %s", channelIndex, active ? "true" : "false");
    return true;
}

bool EnhancedDetectionRenderer::setChannelVisible(int channelIndex, bool visible) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->isVisible = visible;
    return true;
}

bool EnhancedDetectionRenderer::setChannelRenderingMode(int channelIndex, RenderingMode mode) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->mode = mode;
    LOGD("Set rendering mode for channel %d: %d", channelIndex, mode);
    return true;
}

bool EnhancedDetectionRenderer::renderDetections(int channelIndex, uint8_t* frameData, 
                                                int width, int height, int stride,
                                                const std::vector<Detection>& detections) {
    if (!frameData || detections.empty()) {
        return false;
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::lock_guard<std::mutex> lock(statesMutex);
    auto state = getChannelStateInternal(channelIndex);
    if (!state || !state->isVisible) {
        return false;
    }

    // Update viewport if dimensions changed
    if (state->viewportWidth != width || state->viewportHeight != height) {
        state->viewportWidth = width;
        state->viewportHeight = height;
        state->config = createOptimizedConfig(channelIndex, width, height);
    }

    // Filter detections based on channel configuration and system load
    auto filteredDetections = filterDetectionsForChannel(channelIndex, detections);
    
    // Choose rendering method based on mode and system state
    if (adaptiveRenderingEnabled.load() && state->mode == ADAPTIVE) {
        DrawDetectionsAdaptive(frameData, width, height, stride, filteredDetections,
                              channelIndex, state->isActive, currentSystemLoad.load());
    } else if (state->mode == MINIMAL || state->mode == PERFORMANCE_FIRST) {
        // Use minimal rendering configuration
        ViewportRenderConfig minimalConfig = state->config;
        minimalConfig.showConfidenceInSmallViewport = false;
        minimalConfig.showClassNamesInSmallViewport = state->isActive;
        minimalConfig.minBoxThickness = 1;
        minimalConfig.maxBoxThickness = 2;
        
        DrawDetectionsOnRGBAViewportOptimized(frameData, width, height, stride, 
                                            filteredDetections, minimalConfig);
    } else {
        // Full detail rendering
        DrawDetectionsOnRGBAViewportOptimized(frameData, width, height, stride, 
                                            filteredDetections, state->config);
    }

    // Update metrics
    auto endTime = std::chrono::high_resolution_clock::now();
    float renderTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    updateChannelMetrics(channelIndex, renderTime, static_cast<int>(filteredDetections.size()));

    return true;
}

void EnhancedDetectionRenderer::updateSystemLoad(float load) {
    currentSystemLoad.store(load);
    
    // Trigger optimization if load is high
    if (performanceOptimizationEnabled.load() && load > systemLoadThreshold.load()) {
        auto now = std::chrono::steady_clock::now();
        auto timeSinceLastOptimization = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastOptimization).count();
        
        if (timeSinceLastOptimization > 1000) { // Optimize at most once per second
            applyPerformanceOptimizations();
            lastOptimization = now;
        }
    }
}

void EnhancedDetectionRenderer::optimizeRenderingPerformance() {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    LOGD("Optimizing rendering performance for %zu channels", channelStates.size());
    
    for (auto& pair : channelStates) {
        int channelIndex = pair.first;
        auto& state = pair.second;
        
        if (shouldOptimizeChannel(channelIndex)) {
            RenderingMode optimalMode = determineOptimalRenderingMode(channelIndex);
            state->mode = optimalMode;
            
            LOGD("Optimized channel %d to mode %d", channelIndex, optimalMode);
        }
    }
    
    updateSystemMetrics();
}

EnhancedDetectionRenderer::ChannelRenderState*
EnhancedDetectionRenderer::getChannelStateInternal(int channelIndex) const {
    auto it = channelStates.find(channelIndex);
    return (it != channelStates.end()) ? it->second.get() : nullptr;
}



void EnhancedDetectionRenderer::updateChannelMetrics(int channelIndex, float renderTime, int detectionCount) {
    auto state = getChannelStateInternal(channelIndex);
    if (state) {
        state->lastRenderTime = renderTime;
        state->detectionCount = detectionCount;
        state->lastUpdate = std::chrono::steady_clock::now();
    }
}

bool EnhancedDetectionRenderer::shouldOptimizeChannel(int channelIndex) const {
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    // Optimize if render time is too high or system load is high
    return (state->lastRenderTime > 16.67f) || // > 60 FPS threshold
           (currentSystemLoad.load() > systemLoadThreshold.load());
}

EnhancedDetectionRenderer::RenderingMode 
EnhancedDetectionRenderer::determineOptimalRenderingMode(int channelIndex) const {
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return ADAPTIVE;
    }
    
    float systemLoad = currentSystemLoad.load();
    
    if (systemLoad > 0.9f) {
        return PERFORMANCE_FIRST;
    } else if (systemLoad > 0.7f) {
        return state->isActive ? ADAPTIVE : MINIMAL;
    } else if (state->config.isSmallViewport && !state->isActive) {
        return MINIMAL;
    } else {
        return state->isActive ? FULL_DETAIL : ADAPTIVE;
    }
}

ViewportRenderConfig EnhancedDetectionRenderer::createOptimizedConfig(int channelIndex, 
                                                                     int width, int height) const {
    auto state = getChannelStateInternal(channelIndex);
    ViewportRenderConfig config = calculateViewportConfig(width, height, 
                                                         state ? state->isActive : false);
    
    // Apply system-wide optimizations
    float systemLoad = currentSystemLoad.load();
    if (systemLoad > 0.8f) {
        config.showConfidenceInSmallViewport = false;
        config.maxBoxThickness = std::min(config.maxBoxThickness, 3);
        config.maxTextScale = std::min(config.maxTextScale, 0.6f);
    }
    
    return config;
}

std::vector<Detection> EnhancedDetectionRenderer::filterDetectionsForChannel(int channelIndex,
                                                                           const std::vector<Detection>& detections) const {
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return detections;
    }
    
    std::vector<Detection> filtered;
    filtered.reserve(detections.size());
    
    int maxDetections = maxDetectionsPerChannel.load();
    float confidenceThreshold = state->config.isSmallViewport ? 0.6f : 0.4f;
    
    // Sort by confidence and take top detections
    std::vector<Detection> sorted = detections;
    std::sort(sorted.begin(), sorted.end(), 
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });
    
    for (const auto& detection : sorted) {
        if (filtered.size() >= maxDetections) {
            break;
        }
        
        if (detection.confidence >= confidenceThreshold) {
            filtered.push_back(detection);
        }
    }
    
    return filtered;
}

void EnhancedDetectionRenderer::applyPerformanceOptimizations() {
    LOGD("Applying performance optimizations due to high system load");
    
    std::lock_guard<std::mutex> lock(statesMutex);
    
    for (auto& pair : channelStates) {
        auto& state = pair.second;
        
        if (!state->isActive) {
            // Reduce rendering quality for inactive channels
            state->mode = MINIMAL;
        } else if (state->config.isSmallViewport) {
            // Use adaptive mode for small active channels
            state->mode = ADAPTIVE;
        }
    }
}

bool EnhancedDetectionRenderer::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

void EnhancedDetectionRenderer::updateSystemMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex);

    systemMetrics.activeChannels = 0;
    systemMetrics.totalDetections = 0;
    float totalRenderTime = 0.0f;
    int renderingChannels = 0;

    for (const auto& pair : channelStates) {
        const auto& state = pair.second;
        if (state->isActive) {
            systemMetrics.activeChannels++;
        }
        if (state->isVisible && state->lastRenderTime > 0) {
            totalRenderTime += state->lastRenderTime;
            renderingChannels++;
            systemMetrics.totalDetections += state->detectionCount;
        }
    }

    systemMetrics.averageRenderTime = (renderingChannels > 0) ?
        totalRenderTime / renderingChannels : 0.0f;
    systemMetrics.totalRenderLoad = totalRenderTime;
    systemMetrics.lastUpdate = std::chrono::steady_clock::now();
}

EnhancedDetectionRenderer::SystemRenderMetrics EnhancedDetectionRenderer::getSystemMetrics() const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    return systemMetrics;
}

EnhancedDetectionRenderer::ChannelRenderState EnhancedDetectionRenderer::getChannelState(int channelIndex) const {
    std::lock_guard<std::mutex> lock(statesMutex);
    auto state = getChannelStateInternal(channelIndex);
    return state ? *state : ChannelRenderState();
}

std::vector<int> EnhancedDetectionRenderer::getActiveChannels() const {
    std::lock_guard<std::mutex> lock(statesMutex);
    std::vector<int> activeChannels;

    for (const auto& pair : channelStates) {
        if (pair.second->isActive) {
            activeChannels.push_back(pair.first);
        }
    }

    return activeChannels;
}

std::vector<int> EnhancedDetectionRenderer::getOverloadedChannels() const {
    std::lock_guard<std::mutex> lock(statesMutex);
    std::vector<int> overloadedChannels;

    for (const auto& pair : channelStates) {
        if (pair.second->lastRenderTime > 16.67f) { // > 60 FPS threshold
            overloadedChannels.push_back(pair.first);
        }
    }

    return overloadedChannels;
}

void EnhancedDetectionRenderer::setGlobalRenderingMode(RenderingMode mode) {
    std::lock_guard<std::mutex> lock(statesMutex);

    for (auto& pair : channelStates) {
        pair.second->mode = mode;
    }

    LOGD("Set global rendering mode to %d for all channels", mode);
}

void EnhancedDetectionRenderer::resetChannelConfigurations() {
    std::lock_guard<std::mutex> lock(statesMutex);

    for (auto& pair : channelStates) {
        auto& state = pair.second;
        state->mode = ADAPTIVE;
        state->config = calculateViewportConfig(state->viewportWidth, state->viewportHeight, state->isActive);
    }

    LOGD("Reset all channel configurations to default");
}

void EnhancedDetectionRenderer::setAdaptiveRenderingEnabled(bool enabled) {
    adaptiveRenderingEnabled.store(enabled);
    LOGD("Adaptive rendering %s", enabled ? "enabled" : "disabled");
}

void EnhancedDetectionRenderer::setPerformanceOptimizationEnabled(bool enabled) {
    performanceOptimizationEnabled.store(enabled);
    LOGD("Performance optimization %s", enabled ? "enabled" : "disabled");
}

void EnhancedDetectionRenderer::setSystemLoadThreshold(float threshold) {
    systemLoadThreshold.store(threshold);
    LOGD("System load threshold set to %.2f", threshold);
}

void EnhancedDetectionRenderer::setMaxDetectionsPerChannel(int maxDetections) {
    maxDetectionsPerChannel.store(maxDetections);
    LOGD("Max detections per channel set to %d", maxDetections);
}

// DetectionRenderingMonitor implementation
DetectionRenderingMonitor::DetectionRenderingMonitor() {
    LOGD("DetectionRenderingMonitor created");
}

DetectionRenderingMonitor::~DetectionRenderingMonitor() {
    LOGD("DetectionRenderingMonitor destroyed");
}

void DetectionRenderingMonitor::recordRenderingEvent(int channelIndex, float renderTime, int detectionCount) {
    if (!monitoringEnabled.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(metricsMutex);

    auto& metrics = channelMetrics[channelIndex];

    // Update metrics
    metrics.totalFramesRendered++;
    metrics.totalDetectionsRendered += detectionCount;

    // Update average render time (exponential moving average)
    float alpha = 0.1f; // Smoothing factor
    metrics.averageRenderTime = (metrics.averageRenderTime * (1.0f - alpha)) + (renderTime * alpha);

    // Update peak render time
    metrics.peakRenderTime = std::max(metrics.peakRenderTime, renderTime);

    // Update detection density
    if (metrics.totalFramesRendered > 0) {
        metrics.detectionDensity = static_cast<float>(metrics.totalDetectionsRendered) / metrics.totalFramesRendered;
    }

    metrics.lastUpdate = std::chrono::steady_clock::now();
}

void DetectionRenderingMonitor::startMonitoring() {
    monitoringEnabled.store(true);
    LOGD("Detection rendering monitoring started");
}

void DetectionRenderingMonitor::stopMonitoring() {
    monitoringEnabled.store(false);
    LOGD("Detection rendering monitoring stopped");
}

void DetectionRenderingMonitor::resetMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex);
    channelMetrics.clear();
    LOGD("Detection rendering metrics reset");
}

DetectionRenderingMonitor::RenderingMetrics DetectionRenderingMonitor::getChannelMetrics(int channelIndex) const {
    std::lock_guard<std::mutex> lock(metricsMutex);

    auto it = channelMetrics.find(channelIndex);
    return (it != channelMetrics.end()) ? it->second : RenderingMetrics();
}

std::vector<int> DetectionRenderingMonitor::identifySlowChannels(float thresholdMs) const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    std::vector<int> slowChannels;

    for (const auto& pair : channelMetrics) {
        if (pair.second.averageRenderTime > thresholdMs) {
            slowChannels.push_back(pair.first);
        }
    }

    return slowChannels;
}

std::vector<int> DetectionRenderingMonitor::identifyHighDensityChannels(float thresholdDensity) const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    std::vector<int> highDensityChannels;

    for (const auto& pair : channelMetrics) {
        if (pair.second.detectionDensity > thresholdDensity) {
            highDensityChannels.push_back(pair.first);
        }
    }

    return highDensityChannels;
}

float DetectionRenderingMonitor::calculateSystemRenderingLoad() const {
    std::lock_guard<std::mutex> lock(metricsMutex);

    float totalLoad = 0.0f;
    int activeChannels = 0;

    for (const auto& pair : channelMetrics) {
        if (pair.second.totalFramesRendered > 0) {
            totalLoad += pair.second.averageRenderTime;
            activeChannels++;
        }
    }

    return (activeChannels > 0) ? totalLoad / activeChannels : 0.0f;
}

std::vector<std::string> DetectionRenderingMonitor::generateOptimizationRecommendations() const {
    std::vector<std::string> recommendations;

    auto slowChannels = identifySlowChannels(16.67f); // 60 FPS threshold
    auto highDensityChannels = identifyHighDensityChannels(10.0f);

    if (!slowChannels.empty()) {
        recommendations.push_back("Slow rendering detected on " + std::to_string(slowChannels.size()) +
                                " channels. Consider reducing rendering quality or detection frequency.");
    }

    if (!highDensityChannels.empty()) {
        recommendations.push_back("High detection density on " + std::to_string(highDensityChannels.size()) +
                                " channels. Consider filtering low-confidence detections.");
    }

    float systemLoad = calculateSystemRenderingLoad();
    if (systemLoad > 50.0f) {
        recommendations.push_back("High system rendering load (" + std::to_string(systemLoad) +
                                "ms avg). Enable adaptive rendering mode.");
    }

    return recommendations;
}

bool DetectionRenderingMonitor::shouldReduceRenderingQuality(int channelIndex) const {
    auto metrics = getChannelMetrics(channelIndex);
    return metrics.averageRenderTime > 20.0f || metrics.detectionDensity > 15.0f;
}

bool DetectionRenderingMonitor::shouldSkipFrameRendering(int channelIndex) const {
    auto metrics = getChannelMetrics(channelIndex);
    return metrics.averageRenderTime > 33.33f; // > 30 FPS threshold
}
