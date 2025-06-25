#include "SystemPerformanceMonitor.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdio>

SystemPerformanceMonitor::SystemPerformanceMonitor()
    : eventListener(nullptr), monitorRunning(false), monitorIntervalMs(1000),
      optimizationIntervalMs(5000), historySize(300), enableAutoOptimization(true),
      enableDetailedLogging(false), systemCpuUsage(0.0f), systemMemoryUsage(0),
      systemGpuUsage(0.0f) {
    LOGD("SystemPerformanceMonitor created");
}

SystemPerformanceMonitor::~SystemPerformanceMonitor() {
    cleanup();
    LOGD("SystemPerformanceMonitor destroyed");
}

bool SystemPerformanceMonitor::initialize() {
    // Open performance log file if detailed logging is enabled
    if (enableDetailedLogging) {
        performanceLogFile.open("/data/data/com.wulala.myyolov5rtspthreadpool/performance.log", 
                               std::ios::app);
        if (!performanceLogFile.is_open()) {
            LOGW("Failed to open performance log file");
        }
    }
    
    LOGD("SystemPerformanceMonitor initialized");
    return true;
}

void SystemPerformanceMonitor::cleanup() {
    stopMonitoring();
    
    // Close log file
    if (performanceLogFile.is_open()) {
        performanceLogFile.close();
    }
    
    // Clear data
    {
        std::lock_guard<std::mutex> lock(metricsMutex);
        channelMetrics.clear();
        while (!metricsHistory.empty()) {
            metricsHistory.pop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(optimizationMutex);
        while (!optimizationQueue.empty()) {
            optimizationQueue.pop();
        }
    }
    
    LOGD("SystemPerformanceMonitor cleanup completed");
}

void SystemPerformanceMonitor::startMonitoring() {
    if (monitorRunning) {
        LOGW("Performance monitoring already running");
        return;
    }
    
    monitorRunning = true;
    monitorThread = std::thread(&SystemPerformanceMonitor::monitoringLoop, this);
    optimizationThread = std::thread(&SystemPerformanceMonitor::optimizationLoop, this);
    
    LOGD("Performance monitoring started");
}

void SystemPerformanceMonitor::stopMonitoring() {
    if (!monitorRunning) {
        return;
    }
    
    monitorRunning = false;
    monitorCv.notify_all();
    optimizationCv.notify_all();
    
    if (monitorThread.joinable()) {
        monitorThread.join();
    }
    
    if (optimizationThread.joinable()) {
        optimizationThread.join();
    }
    
    LOGD("Performance monitoring stopped");
}

bool SystemPerformanceMonitor::addChannel(int channelIndex) {
    if (!validateChannelIndex(channelIndex)) {
        LOGE("Invalid channel index: %d", channelIndex);
        return false;
    }
    
    std::lock_guard<std::mutex> lock(metricsMutex);
    
    if (channelMetrics.find(channelIndex) != channelMetrics.end()) {
        LOGW("Channel %d already being monitored", channelIndex);
        return false;
    }
    
    channelMetrics[channelIndex] = ChannelPerformanceMetrics(channelIndex);
    
    LOGD("Added channel %d to performance monitoring", channelIndex);
    return true;
}

bool SystemPerformanceMonitor::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    
    auto it = channelMetrics.find(channelIndex);
    if (it == channelMetrics.end()) {
        LOGW("Channel %d not found in performance monitoring", channelIndex);
        return false;
    }
    
    channelMetrics.erase(it);
    
    LOGD("Removed channel %d from performance monitoring", channelIndex);
    return true;
}

void SystemPerformanceMonitor::updateChannelMetrics(int channelIndex, float fps, float detectionFps, float renderFps) {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (!channelMetricsPtr) {
        return;
    }
    
    channelMetricsPtr->fps = fps;
    channelMetricsPtr->detectionFps = detectionFps;
    channelMetricsPtr->renderFps = renderFps;
    channelMetricsPtr->lastUpdate = std::chrono::steady_clock::now();
    
    // Update performance level
    updateChannelPerformanceLevel(channelIndex);
    
    LOGD("Updated channel %d metrics: FPS=%.2f, DetectionFPS=%.2f, RenderFPS=%.2f", 
         channelIndex, fps, detectionFps, renderFps);
}

void SystemPerformanceMonitor::updateChannelResourceUsage(int channelIndex, float cpuUsage, long memoryUsage) {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (!channelMetricsPtr) {
        return;
    }
    
    channelMetricsPtr->cpuUsage = cpuUsage;
    channelMetricsPtr->memoryUsage = memoryUsage;
    channelMetricsPtr->lastUpdate = std::chrono::steady_clock::now();
    
    // Check thresholds
    if (cpuUsage > thresholds.maxCpuUsage) {
        notifyResourceThresholdExceeded(CPU_USAGE, cpuUsage, thresholds.maxCpuUsage);
    }
    
    if (memoryUsage > thresholds.maxMemoryUsage) {
        notifyResourceThresholdExceeded(MEMORY_USAGE, memoryUsage, thresholds.maxMemoryUsage);
    }
    
    LOGD("Updated channel %d resource usage: CPU=%.2f%%, Memory=%ldMB", 
         channelIndex, cpuUsage, memoryUsage / (1024 * 1024));
}

void SystemPerformanceMonitor::updateChannelLatency(int channelIndex, float latency) {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (!channelMetricsPtr) {
        return;
    }
    
    // Update average latency (simple moving average)
    if (channelMetricsPtr->averageLatency == 0.0f) {
        channelMetricsPtr->averageLatency = latency;
    } else {
        channelMetricsPtr->averageLatency = (channelMetricsPtr->averageLatency * 0.9f) + (latency * 0.1f);
    }
    
    // Update peak latency
    if (latency > channelMetricsPtr->peakLatency) {
        channelMetricsPtr->peakLatency = latency;
    }
    
    channelMetricsPtr->lastUpdate = std::chrono::steady_clock::now();
    
    // Check threshold
    if (latency > thresholds.maxLatency) {
        notifyResourceThresholdExceeded(FRAME_RATE, latency, thresholds.maxLatency);
    }
}

void SystemPerformanceMonitor::reportDroppedFrames(int channelIndex, int droppedFrames) {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (!channelMetricsPtr) {
        return;
    }
    
    channelMetricsPtr->droppedFrames += droppedFrames;
    channelMetricsPtr->lastUpdate = std::chrono::steady_clock::now();
    
    LOGD("Channel %d dropped %d frames (total: %d)", 
         channelIndex, droppedFrames, channelMetricsPtr->droppedFrames);
}

void SystemPerformanceMonitor::updateSystemMetrics(const SystemMetrics& metrics) {
    {
        std::lock_guard<std::mutex> lock(metricsMutex);
        currentMetrics = metrics;
        addMetricsToHistory(metrics);
    }
    
    // Update system performance level
    updateSystemPerformanceLevel();
    
    // Notify event listener
    if (eventListener) {
        notifyPerformanceReport(metrics);
    }
    
    LOGD("Updated system metrics: FPS=%.2f, CPU=%.2f%%, Memory=%ldMB", 
         metrics.systemFps, metrics.cpuUsage, metrics.memoryUsage / (1024 * 1024));
}

void SystemPerformanceMonitor::monitoringLoop() {
    while (monitorRunning) {
        std::unique_lock<std::mutex> lock(threadMutex);
        monitorCv.wait_for(lock, std::chrono::milliseconds(monitorIntervalMs), 
                          [this] { return !monitorRunning; });
        
        if (!monitorRunning) break;
        
        // Collect system metrics
        collectSystemMetrics();
        
        // Collect channel metrics
        collectChannelMetrics();
        
        // Analyze performance
        analyzePerformance();
        
        // Detect performance issues
        detectPerformanceIssues();
    }
}

void SystemPerformanceMonitor::optimizationLoop() {
    while (monitorRunning) {
        std::unique_lock<std::mutex> lock(threadMutex);
        optimizationCv.wait_for(lock, std::chrono::milliseconds(optimizationIntervalMs), 
                               [this] { return !monitorRunning; });
        
        if (!monitorRunning) break;
        
        if (enableAutoOptimization) {
            // Generate optimization recommendations
            auto recommendations = generateOptimizationRecommendations();
            
            // Execute high-priority optimizations
            for (const auto& action : recommendations) {
                if (action.priority >= 8) { // High priority threshold
                    executeOptimizationAction(action);
                }
            }
        }
        
        // Process optimization queue
        std::lock_guard<std::mutex> optLock(optimizationMutex);
        while (!optimizationQueue.empty()) {
            auto action = optimizationQueue.front();
            optimizationQueue.pop();
            executeOptimizationAction(action);
        }
    }
}

void SystemPerformanceMonitor::collectSystemMetrics() {
    SystemMetrics metrics;
    
    // Collect system resource usage
    metrics.cpuUsage = collectCpuUsage();
    metrics.memoryUsage = collectMemoryUsage();
    metrics.gpuUsage = collectGpuUsage();
    metrics.networkBandwidth = collectNetworkBandwidth();
    metrics.diskIO = collectDiskIO();
    
    // Calculate system-wide frame rates
    float totalFps = 0.0f;
    float totalDetectionFps = 0.0f;
    float totalRenderFps = 0.0f;
    int activeChannels = 0;
    
    {
        std::lock_guard<std::mutex> lock(metricsMutex);
        for (const auto& pair : channelMetrics) {
            const auto& channelMetric = pair.second;
            if (channelMetric.fps > 0.0f) {
                totalFps += channelMetric.fps;
                totalDetectionFps += channelMetric.detectionFps;
                totalRenderFps += channelMetric.renderFps;
                activeChannels++;
            }
        }
        
        metrics.totalChannels = channelMetrics.size();
    }
    
    metrics.activeChannels = activeChannels;
    metrics.systemFps = activeChannels > 0 ? totalFps / activeChannels : 0.0f;
    metrics.detectionFps = activeChannels > 0 ? totalDetectionFps / activeChannels : 0.0f;
    metrics.renderFps = activeChannels > 0 ? totalRenderFps / activeChannels : 0.0f;
    
    updateSystemMetrics(metrics);
}

float SystemPerformanceMonitor::collectCpuUsage() {
    // Read from /proc/stat for system CPU usage
    std::ifstream statFile("/proc/stat");
    if (!statFile.is_open()) {
        return systemCpuUsage.load(); // Return cached value
    }
    
    std::string line;
    if (std::getline(statFile, line)) {
        // Parse CPU usage from /proc/stat
        // This is a simplified implementation
        static long lastIdle = 0, lastTotal = 0;
        
        std::istringstream iss(line);
        std::string cpu;
        long user, nice, system, idle, iowait, irq, softirq, steal;
        
        iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        
        long currentIdle = idle + iowait;
        long currentTotal = user + nice + system + idle + iowait + irq + softirq + steal;
        
        if (lastTotal != 0) {
            long totalDiff = currentTotal - lastTotal;
            long idleDiff = currentIdle - lastIdle;
            
            if (totalDiff > 0) {
                float cpuUsage = 100.0f * (1.0f - static_cast<float>(idleDiff) / totalDiff);
                systemCpuUsage.store(cpuUsage);
                return cpuUsage;
            }
        }
        
        lastIdle = currentIdle;
        lastTotal = currentTotal;
    }
    
    return systemCpuUsage.load();
}

long SystemPerformanceMonitor::collectMemoryUsage() {
    // Read from /proc/meminfo
    std::ifstream meminfoFile("/proc/meminfo");
    if (!meminfoFile.is_open()) {
        return systemMemoryUsage.load();
    }
    
    std::string line;
    long totalMem = 0, freeMem = 0, buffers = 0, cached = 0;
    
    while (std::getline(meminfoFile, line)) {
        if (line.find("MemTotal:") == 0) {
            sscanf(line.c_str(), "MemTotal: %ld kB", &totalMem);
        } else if (line.find("MemFree:") == 0) {
            sscanf(line.c_str(), "MemFree: %ld kB", &freeMem);
        } else if (line.find("Buffers:") == 0) {
            sscanf(line.c_str(), "Buffers: %ld kB", &buffers);
        } else if (line.find("Cached:") == 0) {
            sscanf(line.c_str(), "Cached: %ld kB", &cached);
        }
    }
    
    if (totalMem > 0) {
        long usedMem = totalMem - freeMem - buffers - cached;
        long usedMemBytes = usedMem * 1024; // Convert to bytes
        systemMemoryUsage.store(usedMemBytes);
        return usedMemBytes;
    }
    
    return systemMemoryUsage.load();
}

float SystemPerformanceMonitor::collectGpuUsage() {
    // GPU usage collection would be platform-specific
    // For now, return cached value or estimate based on system load
    float estimatedGpuUsage = std::min(100.0f, systemCpuUsage.load() * 0.8f);
    systemGpuUsage.store(estimatedGpuUsage);
    return estimatedGpuUsage;
}

float SystemPerformanceMonitor::collectNetworkBandwidth() {
    // Network bandwidth collection from /proc/net/dev
    // This is a simplified implementation
    return 0.0f; // Placeholder
}

float SystemPerformanceMonitor::collectDiskIO() {
    // Disk I/O collection from /proc/diskstats
    // This is a simplified implementation
    return 0.0f; // Placeholder
}

void SystemPerformanceMonitor::updateChannelPerformanceLevel(int channelIndex) {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (!channelMetricsPtr) {
        return;
    }
    
    PerformanceLevel oldLevel = channelMetricsPtr->performanceLevel;
    PerformanceLevel newLevel = assessChannelPerformance(channelIndex);
    
    if (newLevel != oldLevel) {
        channelMetricsPtr->performanceLevel = newLevel;
        notifyPerformanceLevelChanged(channelIndex, oldLevel, newLevel);
    }
}

SystemPerformanceMonitor::PerformanceLevel SystemPerformanceMonitor::assessChannelPerformance(int channelIndex) const {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (!channelMetricsPtr) {
        return CRITICAL;
    }
    
    const auto& metrics = *channelMetricsPtr;
    int score = 100; // Start with perfect score
    
    // Assess frame rate
    if (metrics.fps < thresholds.minFps) {
        score -= 30;
    } else if (metrics.fps < thresholds.targetFps * 0.9f) {
        score -= 15;
    }
    
    // Assess CPU usage
    if (metrics.cpuUsage > thresholds.maxCpuUsage) {
        score -= 25;
    } else if (metrics.cpuUsage > thresholds.maxCpuUsage * 0.8f) {
        score -= 10;
    }
    
    // Assess memory usage
    if (metrics.memoryUsage > thresholds.maxMemoryUsage) {
        score -= 20;
    } else if (metrics.memoryUsage > thresholds.maxMemoryUsage * 0.8f) {
        score -= 10;
    }
    
    // Assess latency
    if (metrics.averageLatency > thresholds.maxLatency) {
        score -= 15;
    } else if (metrics.averageLatency > thresholds.maxLatency * 0.8f) {
        score -= 8;
    }
    
    // Assess queue size
    if (metrics.queueSize > thresholds.maxQueueSize) {
        score -= 10;
    }
    
    // Convert score to performance level
    if (score >= 90) return EXCELLENT;
    if (score >= 75) return GOOD;
    if (score >= 60) return FAIR;
    if (score >= 40) return POOR;
    return CRITICAL;
}

SystemPerformanceMonitor::ChannelPerformanceMetrics* 
SystemPerformanceMonitor::getChannelMetricsInternal(int channelIndex) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    auto it = channelMetrics.find(channelIndex);
    return (it != channelMetrics.end()) ? &it->second : nullptr;
}

const SystemPerformanceMonitor::ChannelPerformanceMetrics* 
SystemPerformanceMonitor::getChannelMetricsInternal(int channelIndex) const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    auto it = channelMetrics.find(channelIndex);
    return (it != channelMetrics.end()) ? &it->second : nullptr;
}

bool SystemPerformanceMonitor::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

void SystemPerformanceMonitor::addMetricsToHistory(const SystemMetrics& metrics) {
    metricsHistory.push(metrics);
    
    // Limit history size
    while (metricsHistory.size() > historySize) {
        metricsHistory.pop();
    }
}

std::string SystemPerformanceMonitor::performanceLevelToString(PerformanceLevel level) const {
    switch (level) {
        case EXCELLENT: return "EXCELLENT";
        case GOOD: return "GOOD";
        case FAIR: return "FAIR";
        case POOR: return "POOR";
        case CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

// Event notification methods
void SystemPerformanceMonitor::notifyPerformanceLevelChanged(int channelIndex, PerformanceLevel oldLevel, PerformanceLevel newLevel) {
    if (eventListener) {
        eventListener->onPerformanceLevelChanged(channelIndex, oldLevel, newLevel);
    }
}

void SystemPerformanceMonitor::notifyResourceThresholdExceeded(ResourceType resource, float value, float threshold) {
    if (eventListener) {
        eventListener->onResourceThresholdExceeded(resource, value, threshold);
    }
}

void SystemPerformanceMonitor::notifyPerformanceReport(const SystemMetrics& metrics) {
    if (eventListener) {
        eventListener->onPerformanceReport(metrics);
    }
}

// Additional methods implementation
void SystemPerformanceMonitor::collectChannelMetrics() {
    // Channel metrics are updated externally via updateChannelMetrics calls
    // This method can be used for any additional channel-specific data collection
}

void SystemPerformanceMonitor::analyzePerformance() {
    // Analyze overall system performance
    auto systemLevel = assessSystemPerformance();

    if (systemLevel == POOR || systemLevel == CRITICAL) {
        std::string message = "System performance is " + performanceLevelToString(systemLevel);
        notifySystemPerformanceAlert(systemLevel, message);
    }
}

void SystemPerformanceMonitor::detectPerformanceIssues() {
    std::lock_guard<std::mutex> lock(metricsMutex);

    for (auto& pair : channelMetrics) {
        auto& channelMetric = pair.second;
        channelMetric.performanceIssues.clear();

        // Detect various performance issues
        if (channelMetric.fps < thresholds.minFps) {
            channelMetric.performanceIssues.push_back("Low frame rate: " + std::to_string(channelMetric.fps) + " FPS");
        }

        if (channelMetric.cpuUsage > thresholds.maxCpuUsage) {
            channelMetric.performanceIssues.push_back("High CPU usage: " + std::to_string(channelMetric.cpuUsage) + "%");
        }

        if (channelMetric.memoryUsage > thresholds.maxMemoryUsage) {
            channelMetric.performanceIssues.push_back("High memory usage: " + std::to_string(channelMetric.memoryUsage / (1024 * 1024)) + "MB");
        }

        if (channelMetric.averageLatency > thresholds.maxLatency) {
            channelMetric.performanceIssues.push_back("High latency: " + std::to_string(channelMetric.averageLatency) + "ms");
        }

        if (channelMetric.queueSize > thresholds.maxQueueSize) {
            channelMetric.performanceIssues.push_back("Queue overflow: " + std::to_string(channelMetric.queueSize) + " items");
        }
    }
}

SystemPerformanceMonitor::PerformanceLevel SystemPerformanceMonitor::assessSystemPerformance() const {
    std::lock_guard<std::mutex> lock(metricsMutex);

    if (channelMetrics.empty()) {
        return EXCELLENT;
    }

    int totalScore = 0;
    int channelCount = 0;

    for (const auto& pair : channelMetrics) {
        PerformanceLevel channelLevel = assessChannelPerformance(pair.first);

        // Convert level to score
        int score = 0;
        switch (channelLevel) {
            case EXCELLENT: score = 100; break;
            case GOOD: score = 80; break;
            case FAIR: score = 60; break;
            case POOR: score = 40; break;
            case CRITICAL: score = 20; break;
        }

        totalScore += score;
        channelCount++;
    }

    if (channelCount == 0) return EXCELLENT;

    int averageScore = totalScore / channelCount;

    // Also consider system-wide metrics
    if (currentMetrics.cpuUsage > 90.0f || currentMetrics.memoryUsage > 1024 * 1024 * 1024) {
        averageScore -= 20;
    }

    // Convert average score back to performance level
    if (averageScore >= 90) return EXCELLENT;
    if (averageScore >= 75) return GOOD;
    if (averageScore >= 60) return FAIR;
    if (averageScore >= 40) return POOR;
    return CRITICAL;
}

std::vector<int> SystemPerformanceMonitor::getBottleneckChannels() const {
    std::vector<int> bottleneckChannels;
    std::lock_guard<std::mutex> lock(metricsMutex);

    for (const auto& pair : channelMetrics) {
        PerformanceLevel level = assessChannelPerformance(pair.first);
        if (level == POOR || level == CRITICAL) {
            bottleneckChannels.push_back(pair.first);
        }
    }

    return bottleneckChannels;
}

std::vector<std::string> SystemPerformanceMonitor::getPerformanceIssues(int channelIndex) const {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (!channelMetricsPtr) {
        return {};
    }

    return channelMetricsPtr->performanceIssues;
}

SystemPerformanceMonitor::SystemMetrics SystemPerformanceMonitor::getSystemMetrics() const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    return currentMetrics;
}

SystemPerformanceMonitor::ChannelPerformanceMetrics
SystemPerformanceMonitor::getChannelMetrics(int channelIndex) const {
    auto channelMetricsPtr = getChannelMetricsInternal(channelIndex);
    if (channelMetricsPtr) {
        return *channelMetricsPtr;
    }
    return ChannelPerformanceMetrics(channelIndex);
}

std::vector<SystemPerformanceMonitor::ChannelPerformanceMetrics>
SystemPerformanceMonitor::getAllChannelMetrics() const {
    std::vector<ChannelPerformanceMetrics> allMetrics;
    std::lock_guard<std::mutex> lock(metricsMutex);

    for (const auto& pair : channelMetrics) {
        allMetrics.push_back(pair.second);
    }

    return allMetrics;
}

void SystemPerformanceMonitor::setPerformanceThresholds(const PerformanceThresholds& newThresholds) {
    std::lock_guard<std::mutex> lock(thresholdsMutex);
    thresholds = newThresholds;
    LOGD("Performance thresholds updated");
}

SystemPerformanceMonitor::PerformanceThresholds SystemPerformanceMonitor::getPerformanceThresholds() const {
    std::lock_guard<std::mutex> lock(thresholdsMutex);
    return thresholds;
}

void SystemPerformanceMonitor::setEventListener(PerformanceEventListener* listener) {
    eventListener = listener;
}

void SystemPerformanceMonitor::scheduleOptimization(const OptimizationAction& action) {
    std::lock_guard<std::mutex> lock(optimizationMutex);
    optimizationQueue.push(action);
    optimizationCv.notify_one();

    LOGD("Scheduled optimization action for channel %d: %s", action.channelIndex, action.description.c_str());
}

std::vector<SystemPerformanceMonitor::OptimizationAction>
SystemPerformanceMonitor::generateOptimizationRecommendations() const {
    std::vector<OptimizationAction> recommendations;
    std::lock_guard<std::mutex> lock(metricsMutex);

    for (const auto& pair : channelMetrics) {
        int channelIndex = pair.first;
        const auto& metrics = pair.second;

        // Generate recommendations based on performance issues
        if (metrics.fps < thresholds.minFps) {
            recommendations.emplace_back(channelIndex, "reduce_quality",
                                       "Reduce stream quality to improve frame rate", 8);
        }

        if (metrics.cpuUsage > thresholds.maxCpuUsage) {
            recommendations.emplace_back(channelIndex, "reduce_detection_frequency",
                                       "Reduce detection frequency to lower CPU usage", 7);
        }

        if (metrics.memoryUsage > thresholds.maxMemoryUsage) {
            recommendations.emplace_back(channelIndex, "clear_buffers",
                                       "Clear buffers to reduce memory usage", 6);
        }

        if (metrics.queueSize > thresholds.maxQueueSize) {
            recommendations.emplace_back(channelIndex, "increase_processing_speed",
                                       "Increase processing speed to reduce queue size", 9);
        }
    }

    // Sort by priority (descending)
    std::sort(recommendations.begin(), recommendations.end(),
              [](const OptimizationAction& a, const OptimizationAction& b) {
                  return a.priority > b.priority;
              });

    return recommendations;
}

void SystemPerformanceMonitor::executeOptimizationAction(const OptimizationAction& action) {
    LOGD("Executing optimization action for channel %d: %s",
         action.channelIndex, action.description.c_str());

    if (action.actionType == "reduce_quality") {
        optimizeChannelFrameRate(action.channelIndex);
    } else if (action.actionType == "reduce_detection_frequency") {
        optimizeChannelDetection(action.channelIndex);
    } else if (action.actionType == "clear_buffers") {
        // This would clear channel buffers
    } else if (action.actionType == "increase_processing_speed") {
        // This would increase processing priority
    }

    notifyOptimizationApplied(action);
}

void SystemPerformanceMonitor::optimizeChannelFrameRate(int channelIndex) {
    // This would implement frame rate optimization
    LOGD("Optimizing frame rate for channel %d", channelIndex);
}

void SystemPerformanceMonitor::optimizeChannelDetection(int channelIndex) {
    // This would implement detection optimization
    LOGD("Optimizing detection for channel %d", channelIndex);
}

void SystemPerformanceMonitor::optimizeChannelRendering(int channelIndex) {
    // This would implement rendering optimization
    LOGD("Optimizing rendering for channel %d", channelIndex);
}

std::string SystemPerformanceMonitor::generatePerformanceReport() const {
    std::ostringstream report;

    report << "=== System Performance Report ===\n";

    auto systemMetrics = getSystemMetrics();
    report << "System Overview:\n";
    report << "  System FPS: " << std::fixed << std::setprecision(2) << systemMetrics.systemFps << "\n";
    report << "  CPU Usage: " << systemMetrics.cpuUsage << "%\n";
    report << "  Memory Usage: " << systemMetrics.memoryUsage / (1024 * 1024) << "MB\n";
    report << "  Active Channels: " << systemMetrics.activeChannels << "/" << systemMetrics.totalChannels << "\n";
    report << "  Performance Level: " << performanceLevelToString(assessSystemPerformance()) << "\n\n";

    auto allChannelMetrics = getAllChannelMetrics();
    report << "Channel Performance:\n";

    for (const auto& channelMetric : allChannelMetrics) {
        report << "  Channel " << channelMetric.channelIndex << ":\n";
        report << "    FPS: " << channelMetric.fps << "\n";
        report << "    CPU: " << channelMetric.cpuUsage << "%\n";
        report << "    Memory: " << channelMetric.memoryUsage / (1024 * 1024) << "MB\n";
        report << "    Latency: " << channelMetric.averageLatency << "ms\n";
        report << "    Level: " << performanceLevelToString(channelMetric.performanceLevel) << "\n";

        if (!channelMetric.performanceIssues.empty()) {
            report << "    Issues:\n";
            for (const auto& issue : channelMetric.performanceIssues) {
                report << "      - " << issue << "\n";
            }
        }
        report << "\n";
    }

    return report.str();
}

void SystemPerformanceMonitor::updateSystemPerformanceLevel() {
    // This method updates the overall system performance level
    // and can trigger system-wide optimizations if needed
}

void SystemPerformanceMonitor::notifySystemPerformanceAlert(PerformanceLevel level, const std::string& message) {
    if (eventListener) {
        eventListener->onSystemPerformanceAlert(level, message);
    }
}

void SystemPerformanceMonitor::notifyOptimizationApplied(const OptimizationAction& action) {
    if (eventListener) {
        eventListener->onOptimizationApplied(action);
    }
}

// PerformanceAnalyticsEngine implementation
PerformanceAnalyticsEngine::PerformanceAnalyticsEngine(SystemPerformanceMonitor* monitor)
    : performanceMonitor(monitor) {
    LOGD("PerformanceAnalyticsEngine created");
}

PerformanceAnalyticsEngine::~PerformanceAnalyticsEngine() {
    LOGD("PerformanceAnalyticsEngine destroyed");
}

std::vector<PerformanceAnalyticsEngine::PerformanceTrend>
PerformanceAnalyticsEngine::analyzePerformanceTrends() const {
    std::vector<PerformanceTrend> trends;

    if (!performanceMonitor) {
        return trends;
    }

    auto metricsHistory = performanceMonitor->getMetricsHistory();
    if (metricsHistory.size() < 10) {
        return trends; // Need more data for trend analysis
    }

    // Analyze CPU usage trend
    std::vector<float> cpuValues;
    for (const auto& metrics : metricsHistory) {
        cpuValues.push_back(metrics.cpuUsage);
    }

    PerformanceTrend cpuTrend(SystemPerformanceMonitor::CPU_USAGE);
    cpuTrend.currentValue = cpuValues.back();
    cpuTrend.trendSlope = calculateTrendSlope(cpuValues);
    cpuTrend.confidenceLevel = calculateConfidenceLevel(cpuValues);

    if (cpuTrend.trendSlope > 0.5f) {
        cpuTrend.trendDescription = "CPU usage is increasing";
    } else if (cpuTrend.trendSlope < -0.5f) {
        cpuTrend.trendDescription = "CPU usage is decreasing";
    } else {
        cpuTrend.trendDescription = "CPU usage is stable";
    }

    trends.push_back(cpuTrend);

    return trends;
}

float PerformanceAnalyticsEngine::calculateTrendSlope(const std::vector<float>& values) const {
    if (values.size() < 2) return 0.0f;

    // Simple linear regression slope calculation
    float n = values.size();
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;

    for (size_t i = 0; i < values.size(); i++) {
        float x = i;
        float y = values[i];
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }

    float slope = (n * sumXY - sumX * sumY) / (n * sumX2 - sumX * sumX);
    return slope;
}

int PerformanceAnalyticsEngine::calculateConfidenceLevel(const std::vector<float>& values) const {
    if (values.size() < 5) return 0;

    // Calculate variance to determine confidence
    float mean = 0;
    for (float value : values) {
        mean += value;
    }
    mean /= values.size();

    float variance = 0;
    for (float value : values) {
        variance += (value - mean) * (value - mean);
    }
    variance /= values.size();

    // Lower variance = higher confidence
    if (variance < 10.0f) return 90;
    if (variance < 50.0f) return 70;
    if (variance < 100.0f) return 50;
    return 30;
}

std::vector<SystemPerformanceMonitor::SystemMetrics> SystemPerformanceMonitor::getMetricsHistory() const {
    std::lock_guard<std::mutex> lock(metricsMutex);

    std::vector<SystemMetrics> history;

    // Return a copy of the metrics history
    // For simplicity, we'll return the last 100 metrics entries
    // In a real implementation, you might want to store historical data

    // Since we don't have a historical storage in the current implementation,
    // we'll return the current metrics as a single entry
    SystemMetrics currentMetrics = getSystemMetrics();
    history.push_back(currentMetrics);

    return history;
}
