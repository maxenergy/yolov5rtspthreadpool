#include "StreamHealthMonitor.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

StreamHealthMonitor::StreamHealthMonitor() 
    : shouldStop(false), eventListener(nullptr),
      totalChannels(0), healthyChannels(0), warningChannels(0), 
      criticalChannels(0), failedChannels(0) {
    
    // Start monitoring thread
    monitorThread = std::thread(&StreamHealthMonitor::monitorLoop, this);
    
    // Start alert processor thread
    alertProcessorThread = std::thread(&StreamHealthMonitor::alertProcessorLoop, this);
    
    LOGD("StreamHealthMonitor initialized");
}

StreamHealthMonitor::~StreamHealthMonitor() {
    cleanup();
}

bool StreamHealthMonitor::addChannel(int channelIndex) {
    auto lock = lockHealthData();
    
    if (healthDataMap.find(channelIndex) != healthDataMap.end()) {
        LOGW("Channel %d already exists in health monitor", channelIndex);
        return false;
    }
    
    auto healthData = std::make_unique<HealthData>(channelIndex);
    healthDataMap[channelIndex] = std::move(healthData);
    totalChannels++;
    
    LOGD("Added channel %d to health monitor", channelIndex);
    return true;
}

bool StreamHealthMonitor::removeChannel(int channelIndex) {
    auto lock = lockHealthData();
    
    auto it = healthDataMap.find(channelIndex);
    if (it == healthDataMap.end()) {
        return false;
    }
    
    healthDataMap.erase(it);
    totalChannels--;
    updateSystemStatistics();
    
    LOGD("Removed channel %d from health monitor", channelIndex);
    return true;
}

void StreamHealthMonitor::updateFrameRate(int channelIndex, float fps) {
    auto lock = lockHealthData();
    
    HealthData* healthData = getHealthData(channelIndex);
    if (!healthData) return;
    
    healthData->metrics[FRAME_RATE] = fps;
    healthData->averageFps = (healthData->averageFps + fps) / 2.0f;
    healthData->peakFps = std::max(healthData->peakFps, fps);
    healthData->minFps = (healthData->minFps == 0.0f) ? fps : std::min(healthData->minFps, fps);
    healthData->lastUpdate = std::chrono::steady_clock::now();
    
    // Assess frame rate health
    HealthStatus fpsStatus = assessMetricHealth(FRAME_RATE, fps);
    healthData->metricStatus[FRAME_RATE] = fpsStatus;
    
    if (fpsStatus != HEALTHY) {
        std::ostringstream oss;
        oss << "Frame rate " << std::fixed << std::setprecision(1) << fps 
            << " FPS below threshold " << thresholds.minFps << " FPS";
        addAlert(healthData, FRAME_RATE, oss.str());
    } else {
        removeAlert(healthData, FRAME_RATE);
    }
}

void StreamHealthMonitor::updateFrameDrops(int channelIndex, int dropped, int total) {
    auto lock = lockHealthData();
    
    HealthData* healthData = getHealthData(channelIndex);
    if (!healthData) return;
    
    healthData->droppedFrames += dropped;
    healthData->totalFrames += total;
    
    float dropRate = (total > 0) ? static_cast<float>(dropped) / total : 0.0f;
    healthData->metrics[FRAME_DROPS] = dropRate;
    healthData->lastUpdate = std::chrono::steady_clock::now();
    
    // Assess frame drop health
    HealthStatus dropStatus = assessMetricHealth(FRAME_DROPS, dropRate);
    healthData->metricStatus[FRAME_DROPS] = dropStatus;
    
    if (dropStatus != HEALTHY) {
        std::ostringstream oss;
        oss << "Frame drop rate " << std::fixed << std::setprecision(2) << (dropRate * 100) 
            << "% exceeds threshold " << (thresholds.maxDropRate * 100) << "%";
        addAlert(healthData, FRAME_DROPS, oss.str());
    } else {
        removeAlert(healthData, FRAME_DROPS);
    }
}

void StreamHealthMonitor::updateLatency(int channelIndex, double latencyMs) {
    auto lock = lockHealthData();
    
    HealthData* healthData = getHealthData(channelIndex);
    if (!healthData) return;
    
    healthData->metrics[LATENCY] = static_cast<float>(latencyMs);
    healthData->averageLatency = (healthData->averageLatency + latencyMs) / 2.0;
    healthData->peakLatency = std::max(healthData->peakLatency, latencyMs);
    healthData->lastUpdate = std::chrono::steady_clock::now();
    
    // Assess latency health
    HealthStatus latencyStatus = assessMetricHealth(LATENCY, static_cast<float>(latencyMs));
    healthData->metricStatus[LATENCY] = latencyStatus;
    
    if (latencyStatus != HEALTHY) {
        std::ostringstream oss;
        oss << "Latency " << std::fixed << std::setprecision(1) << latencyMs 
            << "ms exceeds threshold " << thresholds.maxLatency << "ms";
        addAlert(healthData, LATENCY, oss.str());
    } else {
        removeAlert(healthData, LATENCY);
    }
}

void StreamHealthMonitor::updateBandwidth(int channelIndex, long bytes) {
    auto lock = lockHealthData();
    
    HealthData* healthData = getHealthData(channelIndex);
    if (!healthData) return;
    
    healthData->totalBytes += bytes;
    
    // Calculate bandwidth in Mbps
    auto now = std::chrono::steady_clock::now();
    auto timeDiff = std::chrono::duration_cast<std::chrono::seconds>(now - healthData->lastUpdate);
    
    if (timeDiff.count() > 0) {
        float bandwidthMbps = (bytes * 8.0f) / (timeDiff.count() * 1024 * 1024);
        healthData->metrics[BANDWIDTH] = bandwidthMbps;
    }
    
    healthData->lastUpdate = now;
}

void StreamHealthMonitor::updateErrorRate(int channelIndex, int errors, int total) {
    auto lock = lockHealthData();
    
    HealthData* healthData = getHealthData(channelIndex);
    if (!healthData) return;
    
    float errorRate = (total > 0) ? static_cast<float>(errors) / total : 0.0f;
    healthData->metrics[ERROR_RATE] = errorRate;
    healthData->lastUpdate = std::chrono::steady_clock::now();
    
    // Assess error rate health
    HealthStatus errorStatus = assessMetricHealth(ERROR_RATE, errorRate);
    healthData->metricStatus[ERROR_RATE] = errorStatus;
    
    if (errorStatus != HEALTHY) {
        std::ostringstream oss;
        oss << "Error rate " << std::fixed << std::setprecision(2) << (errorRate * 100) 
            << "% exceeds threshold " << (thresholds.maxErrorRate * 100) << "%";
        addAlert(healthData, ERROR_RATE, oss.str());
    } else {
        removeAlert(healthData, ERROR_RATE);
    }
}

void StreamHealthMonitor::updateConnectionStatus(int channelIndex, bool connected) {
    auto lock = lockHealthData();
    
    HealthData* healthData = getHealthData(channelIndex);
    if (!healthData) return;
    
    healthData->metrics[CONNECTION_STABILITY] = connected ? 1.0f : 0.0f;
    healthData->lastUpdate = std::chrono::steady_clock::now();
    
    if (!connected) {
        healthData->consecutiveFailures++;
        healthData->reconnectCount++;
        addAlert(healthData, CONNECTION_STABILITY, "Connection lost");
    } else {
        healthData->consecutiveFailures = 0;
        healthData->lastHealthyTime = std::chrono::steady_clock::now();
        removeAlert(healthData, CONNECTION_STABILITY);
    }
    
    // Assess connection stability
    HealthStatus connectionStatus = connected ? HEALTHY : CRITICAL;
    healthData->metricStatus[CONNECTION_STABILITY] = connectionStatus;
}

void StreamHealthMonitor::updateResourceUsage(int channelIndex, float cpuUsage, long memoryUsage) {
    auto lock = lockHealthData();
    
    HealthData* healthData = getHealthData(channelIndex);
    if (!healthData) return;
    
    healthData->metrics[CPU_USAGE] = cpuUsage;
    healthData->metrics[MEMORY_USAGE] = static_cast<float>(memoryUsage / (1024 * 1024)); // MB
    healthData->lastUpdate = std::chrono::steady_clock::now();
    
    // Assess resource usage health
    HealthStatus cpuStatus = (cpuUsage > 80.0f) ? WARNING : HEALTHY;
    HealthStatus memoryStatus = (memoryUsage > 100 * 1024 * 1024) ? WARNING : HEALTHY; // 100MB threshold
    
    healthData->metricStatus[CPU_USAGE] = cpuStatus;
    healthData->metricStatus[MEMORY_USAGE] = memoryStatus;
    
    if (cpuStatus != HEALTHY) {
        std::ostringstream oss;
        oss << "High CPU usage: " << std::fixed << std::setprecision(1) << cpuUsage << "%";
        addAlert(healthData, CPU_USAGE, oss.str());
    } else {
        removeAlert(healthData, CPU_USAGE);
    }
    
    if (memoryStatus != HEALTHY) {
        std::ostringstream oss;
        oss << "High memory usage: " << std::fixed << std::setprecision(1) 
            << (memoryUsage / (1024.0 * 1024.0)) << " MB";
        addAlert(healthData, MEMORY_USAGE, oss.str());
    } else {
        removeAlert(healthData, MEMORY_USAGE);
    }
}

StreamHealthMonitor::HealthStatus StreamHealthMonitor::getChannelHealth(int channelIndex) const {
    auto lock = const_cast<StreamHealthMonitor*>(this)->lockHealthData();
    
    const HealthData* healthData = getHealthData(channelIndex);
    return healthData ? healthData->overallStatus : UNKNOWN;
}

StreamHealthMonitor::HealthData StreamHealthMonitor::getChannelHealthData(int channelIndex) const {
    auto lock = const_cast<StreamHealthMonitor*>(this)->lockHealthData();
    
    const HealthData* healthData = getHealthData(channelIndex);
    return healthData ? *healthData : HealthData(channelIndex);
}

std::vector<int> StreamHealthMonitor::getChannelsByStatus(HealthStatus status) const {
    auto lock = const_cast<StreamHealthMonitor*>(this)->lockHealthData();
    
    std::vector<int> channels;
    for (const auto& pair : healthDataMap) {
        if (pair.second->overallStatus == status) {
            channels.push_back(pair.first);
        }
    }
    return channels;
}

std::vector<std::string> StreamHealthMonitor::getActiveAlerts(int channelIndex) const {
    auto lock = const_cast<StreamHealthMonitor*>(this)->lockHealthData();
    
    const HealthData* healthData = getHealthData(channelIndex);
    return healthData ? healthData->activeAlerts : std::vector<std::string>();
}

StreamHealthMonitor::HealthStatus StreamHealthMonitor::getSystemHealth() const {
    int total = totalChannels.load();
    if (total == 0) return UNKNOWN;
    
    int failed = failedChannels.load();
    int critical = criticalChannels.load();
    int warning = warningChannels.load();
    
    if (failed > total * 0.5f) return FAILED;
    if (critical > total * 0.3f) return CRITICAL;
    if (warning > total * 0.5f) return WARNING;
    
    return HEALTHY;
}

void StreamHealthMonitor::monitorLoop() {
    LOGD("Health monitor thread started");
    
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(monitorMutex);
        monitorCv.wait_for(lock, std::chrono::milliseconds(thresholds.healthCheckInterval));
        
        if (shouldStop) break;
        
        // Check health of all channels
        auto healthLock = lockHealthData();
        for (auto& pair : healthDataMap) {
            checkChannelHealth(pair.second.get());
        }
        healthLock.unlock();
        
        updateSystemStatistics();
    }
    
    LOGD("Health monitor thread stopped");
}

void StreamHealthMonitor::checkChannelHealth(HealthData* healthData) {
    if (!healthData) return;
    
    HealthStatus oldStatus = healthData->overallStatus;
    
    // Check for timeout
    auto now = std::chrono::steady_clock::now();
    auto timeSinceUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - healthData->lastUpdate);
    
    if (timeSinceUpdate.count() > thresholds.criticalThreshold) {
        healthData->overallStatus = FAILED;
        addAlert(healthData, CONNECTION_STABILITY, "Health data timeout");
    } else {
        updateOverallHealth(healthData);
        detectAnomalies(healthData);
    }
    
    // Notify if status changed
    if (oldStatus != healthData->overallStatus && eventListener) {
        eventListener->onHealthStatusChanged(healthData->channelIndex, oldStatus, healthData->overallStatus);
    }
}

void StreamHealthMonitor::updateOverallHealth(HealthData* healthData) {
    std::vector<HealthStatus> statuses;
    
    for (const auto& pair : healthData->metricStatus) {
        statuses.push_back(pair.second);
    }
    
    healthData->overallStatus = combineHealthStatus(statuses);
}

void StreamHealthMonitor::detectAnomalies(HealthData* healthData) {
    // Simple anomaly detection based on consecutive failures
    if (healthData->consecutiveFailures >= thresholds.maxConsecutiveFailures) {
        healthData->overallStatus = FAILED;
        
        if (eventListener) {
            eventListener->onStreamFailure(healthData->channelIndex, "Too many consecutive failures");
        }
    }
}

StreamHealthMonitor::HealthStatus StreamHealthMonitor::assessMetricHealth(HealthMetric metric, float value) const {
    switch (metric) {
        case FRAME_RATE:
            if (value < thresholds.minFps * 0.5f) return CRITICAL;
            if (value < thresholds.minFps) return WARNING;
            return HEALTHY;
            
        case FRAME_DROPS:
            if (value > thresholds.maxDropRate * 2.0f) return CRITICAL;
            if (value > thresholds.maxDropRate) return WARNING;
            return HEALTHY;
            
        case LATENCY:
            if (value > thresholds.maxLatency * 2.0f) return CRITICAL;
            if (value > thresholds.maxLatency) return WARNING;
            return HEALTHY;
            
        case ERROR_RATE:
            if (value > thresholds.maxErrorRate * 2.0f) return CRITICAL;
            if (value > thresholds.maxErrorRate) return WARNING;
            return HEALTHY;
            
        case CONNECTION_STABILITY:
            return (value > 0.5f) ? HEALTHY : CRITICAL;
            
        default:
            return HEALTHY;
    }
}

StreamHealthMonitor::HealthStatus StreamHealthMonitor::combineHealthStatus(const std::vector<HealthStatus>& statuses) const {
    if (statuses.empty()) return UNKNOWN;
    
    bool hasFailed = false;
    bool hasCritical = false;
    bool hasWarning = false;
    
    for (HealthStatus status : statuses) {
        switch (status) {
            case FAILED:
                hasFailed = true;
                break;
            case CRITICAL:
                hasCritical = true;
                break;
            case WARNING:
                hasWarning = true;
                break;
            default:
                break;
        }
    }
    
    if (hasFailed) return FAILED;
    if (hasCritical) return CRITICAL;
    if (hasWarning) return WARNING;
    
    return HEALTHY;
}

void StreamHealthMonitor::updateSystemStatistics() {
    int healthy = 0, warning = 0, critical = 0, failed = 0;
    
    for (const auto& pair : healthDataMap) {
        switch (pair.second->overallStatus) {
            case HEALTHY:
                healthy++;
                break;
            case WARNING:
                warning++;
                break;
            case CRITICAL:
                critical++;
                break;
            case FAILED:
                failed++;
                break;
            default:
                break;
        }
    }
    
    healthyChannels.store(healthy);
    warningChannels.store(warning);
    criticalChannels.store(critical);
    failedChannels.store(failed);
}

// Alert management
void StreamHealthMonitor::alertProcessorLoop() {
    LOGD("Alert processor thread started");

    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(alertMutex);
        alertCv.wait(lock, [this] { return !alertQueue.empty() || shouldStop; });

        if (shouldStop) break;

        if (!alertQueue.empty()) {
            auto alert = alertQueue.front();
            alertQueue.pop();
            lock.unlock();

            processAlert(alert.first, alert.second);
        }
    }

    LOGD("Alert processor thread stopped");
}

void StreamHealthMonitor::processAlert(int channelIndex, const std::string& message) {
    LOGW("Health Alert - Channel %d: %s", channelIndex, message.c_str());

    // Additional alert processing logic can be added here
    // For example: logging to file, sending notifications, etc.
}

void StreamHealthMonitor::addAlert(HealthData* healthData, HealthMetric metric, const std::string& message) {
    if (!healthData) return;

    std::string alertKey = healthMetricToString(metric);

    // Check if alert already exists
    auto it = std::find_if(healthData->activeAlerts.begin(), healthData->activeAlerts.end(),
                          [&alertKey](const std::string& alert) {
                              return alert.find(alertKey) != std::string::npos;
                          });

    if (it == healthData->activeAlerts.end()) {
        std::string fullAlert = alertKey + ": " + message;
        healthData->activeAlerts.push_back(fullAlert);

        // Queue alert for processing
        std::lock_guard<std::mutex> lock(alertMutex);
        alertQueue.push({healthData->channelIndex, fullAlert});
        alertCv.notify_one();

        if (eventListener) {
            eventListener->onHealthAlert(healthData->channelIndex, metric, message);
        }
    }
}

void StreamHealthMonitor::removeAlert(HealthData* healthData, HealthMetric metric) {
    if (!healthData) return;

    std::string alertKey = healthMetricToString(metric);

    auto it = std::remove_if(healthData->activeAlerts.begin(), healthData->activeAlerts.end(),
                            [&alertKey](const std::string& alert) {
                                return alert.find(alertKey) != std::string::npos;
                            });

    if (it != healthData->activeAlerts.end()) {
        healthData->activeAlerts.erase(it, healthData->activeAlerts.end());

        if (eventListener) {
            eventListener->onHealthRecovered(healthData->channelIndex, metric);
        }
    }
}

// Configuration and utility methods
void StreamHealthMonitor::setHealthThresholds(const HealthThresholds& newThresholds) {
    thresholds = newThresholds;
    LOGD("Updated health thresholds");
}

void StreamHealthMonitor::setEventListener(HealthEventListener* listener) {
    eventListener = listener;
}

void StreamHealthMonitor::setMonitoringInterval(int intervalMs) {
    thresholds.healthCheckInterval = intervalMs;
    monitorCv.notify_one();
}

void StreamHealthMonitor::triggerRecoveryAction(int channelIndex, const std::string& action) {
    if (eventListener) {
        eventListener->onRecoveryAction(channelIndex, action);
    }

    LOGD("Recovery action triggered for channel %d: %s", channelIndex, action.c_str());
}

void StreamHealthMonitor::resetChannelHealth(int channelIndex) {
    auto lock = lockHealthData();

    HealthData* healthData = getHealthData(channelIndex);
    if (healthData) {
        healthData->overallStatus = HEALTHY;
        healthData->consecutiveFailures = 0;
        healthData->activeAlerts.clear();
        healthData->lastHealthyTime = std::chrono::steady_clock::now();
        healthData->metricStatus.clear();

        LOGD("Reset health for channel %d", channelIndex);
    }
}

void StreamHealthMonitor::acknowledgeAlert(int channelIndex, HealthMetric metric) {
    auto lock = lockHealthData();

    HealthData* healthData = getHealthData(channelIndex);
    if (healthData) {
        removeAlert(healthData, metric);
        LOGD("Acknowledged alert for channel %d, metric %s",
             channelIndex, healthMetricToString(metric).c_str());
    }
}

// Diagnostics and reporting
std::string StreamHealthMonitor::generateHealthReport() const {
    auto lock = const_cast<StreamHealthMonitor*>(this)->lockHealthData();

    std::ostringstream report;
    report << "=== Stream Health Report ===\n";
    report << "Total Channels: " << totalChannels.load() << "\n";
    report << "Healthy: " << healthyChannels.load() << "\n";
    report << "Warning: " << warningChannels.load() << "\n";
    report << "Critical: " << criticalChannels.load() << "\n";
    report << "Failed: " << failedChannels.load() << "\n";
    report << "System Health: " << healthStatusToString(getSystemHealth()) << "\n\n";

    for (const auto& pair : healthDataMap) {
        const HealthData* data = pair.second.get();
        report << "Channel " << data->channelIndex << ":\n";
        report << "  Status: " << healthStatusToString(data->overallStatus) << "\n";
        report << "  FPS: " << std::fixed << std::setprecision(1) << data->averageFps << "\n";
        report << "  Dropped Frames: " << data->droppedFrames << "/" << data->totalFrames << "\n";
        report << "  Reconnects: " << data->reconnectCount << "\n";
        report << "  Active Alerts: " << data->activeAlerts.size() << "\n";
        if (!data->activeAlerts.empty()) {
            for (const auto& alert : data->activeAlerts) {
                report << "    - " << alert << "\n";
            }
        }
        report << "\n";
    }

    return report.str();
}

std::string StreamHealthMonitor::generateChannelDiagnostics(int channelIndex) const {
    auto lock = const_cast<StreamHealthMonitor*>(this)->lockHealthData();

    const HealthData* data = getHealthData(channelIndex);
    if (!data) {
        return "Channel not found";
    }

    std::ostringstream diag;
    diag << "=== Channel " << channelIndex << " Diagnostics ===\n";
    diag << "Overall Status: " << healthStatusToString(data->overallStatus) << "\n";
    diag << "Last Update: " << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - data->lastUpdate).count() << " seconds ago\n";
    diag << "Consecutive Failures: " << data->consecutiveFailures << "\n\n";

    diag << "Performance Metrics:\n";
    diag << "  Average FPS: " << std::fixed << std::setprecision(1) << data->averageFps << "\n";
    diag << "  Peak FPS: " << data->peakFps << "\n";
    diag << "  Min FPS: " << data->minFps << "\n";
    diag << "  Total Frames: " << data->totalFrames << "\n";
    diag << "  Dropped Frames: " << data->droppedFrames << "\n";
    diag << "  Average Latency: " << std::fixed << std::setprecision(1) << data->averageLatency << "ms\n";
    diag << "  Peak Latency: " << data->peakLatency << "ms\n";
    diag << "  Total Bytes: " << data->totalBytes << "\n";
    diag << "  Reconnect Count: " << data->reconnectCount << "\n\n";

    diag << "Current Metrics:\n";
    for (const auto& metric : data->metrics) {
        diag << "  " << healthMetricToString(metric.first) << ": "
             << std::fixed << std::setprecision(2) << metric.second << "\n";
    }

    if (!data->activeAlerts.empty()) {
        diag << "\nActive Alerts:\n";
        for (const auto& alert : data->activeAlerts) {
            diag << "  - " << alert << "\n";
        }
    }

    return diag.str();
}

// Utility methods
StreamHealthMonitor::HealthData* StreamHealthMonitor::getHealthData(int channelIndex) {
    auto it = healthDataMap.find(channelIndex);
    return (it != healthDataMap.end()) ? it->second.get() : nullptr;
}

const StreamHealthMonitor::HealthData* StreamHealthMonitor::getHealthData(int channelIndex) const {
    auto it = healthDataMap.find(channelIndex);
    return (it != healthDataMap.end()) ? it->second.get() : nullptr;
}

std::string StreamHealthMonitor::healthStatusToString(HealthStatus status) const {
    switch (status) {
        case HEALTHY: return "HEALTHY";
        case WARNING: return "WARNING";
        case CRITICAL: return "CRITICAL";
        case FAILED: return "FAILED";
        case UNKNOWN: return "UNKNOWN";
        default: return "INVALID";
    }
}

std::string StreamHealthMonitor::healthMetricToString(HealthMetric metric) const {
    switch (metric) {
        case FRAME_RATE: return "Frame Rate";
        case FRAME_DROPS: return "Frame Drops";
        case LATENCY: return "Latency";
        case BANDWIDTH: return "Bandwidth";
        case ERROR_RATE: return "Error Rate";
        case CONNECTION_STABILITY: return "Connection";
        case MEMORY_USAGE: return "Memory Usage";
        case CPU_USAGE: return "CPU Usage";
        default: return "Unknown";
    }
}

void StreamHealthMonitor::cleanup() {
    LOGD("Cleaning up StreamHealthMonitor");

    // Stop threads
    shouldStop = true;
    monitorCv.notify_all();
    alertCv.notify_all();

    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    if (alertProcessorThread.joinable()) {
        alertProcessorThread.join();
    }

    // Clear data
    auto lock = lockHealthData();
    healthDataMap.clear();

    LOGD("StreamHealthMonitor cleanup complete");
}

// StreamAnomalyDetector implementation
StreamAnomalyDetector::StreamAnomalyDetector() {
    initializeBuiltInPatterns();
}

void StreamAnomalyDetector::addPattern(const AnomalyPattern& pattern) {
    std::lock_guard<std::mutex> lock(patternsMutex);
    patterns.push_back(pattern);
}

void StreamAnomalyDetector::removePattern(const std::string& name) {
    std::lock_guard<std::mutex> lock(patternsMutex);

    patterns.erase(std::remove_if(patterns.begin(), patterns.end(),
                                 [&name](const AnomalyPattern& pattern) {
                                     return pattern.name == name;
                                 }), patterns.end());
}

std::vector<std::string> StreamAnomalyDetector::detectAnomalies(const StreamHealthMonitor::HealthData& healthData) {
    std::lock_guard<std::mutex> lock(patternsMutex);

    std::vector<std::string> detectedAnomalies;

    for (const auto& pattern : patterns) {
        if (pattern.detector(healthData)) {
            detectedAnomalies.push_back(pattern.name + ": " + pattern.description);
        }
    }

    return detectedAnomalies;
}

bool StreamAnomalyDetector::hasAnomalies(const StreamHealthMonitor::HealthData& healthData) {
    return !detectAnomalies(healthData).empty();
}

void StreamAnomalyDetector::initializeBuiltInPatterns() {
    // Frame rate fluctuation pattern
    addPattern(AnomalyPattern(
        "FrameRateFluctuation",
        "Significant frame rate variations detected",
        [this](const StreamHealthMonitor::HealthData& data) {
            return detectFrameRateFluctuation(data);
        },
        StreamHealthMonitor::WARNING
    ));

    // High latency spikes pattern
    addPattern(AnomalyPattern(
        "LatencySpikes",
        "High latency spikes detected",
        [this](const StreamHealthMonitor::HealthData& data) {
            return detectHighLatencySpikes(data);
        },
        StreamHealthMonitor::CRITICAL
    ));

    // Connection instability pattern
    addPattern(AnomalyPattern(
        "ConnectionInstability",
        "Frequent connection drops detected",
        [this](const StreamHealthMonitor::HealthData& data) {
            return detectConnectionInstability(data);
        },
        StreamHealthMonitor::CRITICAL
    ));

    // Memory leak pattern
    addPattern(AnomalyPattern(
        "MemoryLeak",
        "Potential memory leak detected",
        [this](const StreamHealthMonitor::HealthData& data) {
            return detectMemoryLeak(data);
        },
        StreamHealthMonitor::WARNING
    ));
}

bool StreamAnomalyDetector::detectFrameRateFluctuation(const StreamHealthMonitor::HealthData& data) {
    // Check if there's significant difference between peak and min FPS
    if (data.peakFps > 0 && data.minFps > 0) {
        float variation = (data.peakFps - data.minFps) / data.averageFps;
        return variation > 0.5f; // 50% variation threshold
    }
    return false;
}

bool StreamAnomalyDetector::detectHighLatencySpikes(const StreamHealthMonitor::HealthData& data) {
    // Check if peak latency is significantly higher than average
    if (data.peakLatency > 0 && data.averageLatency > 0) {
        return data.peakLatency > data.averageLatency * 3.0; // 3x average threshold
    }
    return false;
}

bool StreamAnomalyDetector::detectConnectionInstability(const StreamHealthMonitor::HealthData& data) {
    // Check for frequent reconnections
    return data.reconnectCount > 5; // More than 5 reconnects
}

bool StreamAnomalyDetector::detectMemoryLeak(const StreamHealthMonitor::HealthData& data) {
    // Simple heuristic: check if memory usage metric exists and is high
    auto it = data.metrics.find(StreamHealthMonitor::MEMORY_USAGE);
    if (it != data.metrics.end()) {
        return it->second > 200.0f; // 200MB threshold
    }
    return false;
}

// StreamRecoveryManager implementation
StreamRecoveryManager::StreamRecoveryManager() {
    initializeBuiltInStrategies();
}

void StreamRecoveryManager::addRecoveryStrategy(StreamHealthMonitor::HealthStatus status, const RecoveryStrategy& strategy) {
    std::lock_guard<std::mutex> lock(recoveryMutex);
    strategies[status] = strategy;
}

void StreamRecoveryManager::removeRecoveryStrategy(StreamHealthMonitor::HealthStatus status) {
    std::lock_guard<std::mutex> lock(recoveryMutex);
    strategies.erase(status);
}

bool StreamRecoveryManager::executeRecovery(int channelIndex, StreamHealthMonitor::HealthStatus status) {
    std::lock_guard<std::mutex> lock(recoveryMutex);

    auto it = strategies.find(status);
    if (it == strategies.end()) {
        LOGW("No recovery strategy found for status %d", status);
        return false;
    }

    const RecoveryStrategy& strategy = it->second;
    int& attempts = recoveryAttempts[channelIndex];

    if (attempts >= strategy.maxAttempts) {
        LOGE("Max recovery attempts (%d) reached for channel %d", strategy.maxAttempts, channelIndex);
        return false;
    }

    LOGD("Executing recovery strategy '%s' for channel %d (attempt %d/%d)",
         strategy.name.c_str(), channelIndex, attempts + 1, strategy.maxAttempts);

    bool success = true;
    for (RecoveryAction action : strategy.actions) {
        if (!executeRecoveryAction(channelIndex, action)) {
            success = false;
            break;
        }

        // Delay between actions
        if (strategy.delayBetweenAttempts > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(strategy.delayBetweenAttempts / strategy.actions.size()));
        }
    }

    attempts++;

    if (success) {
        LOGD("Recovery strategy executed successfully for channel %d", channelIndex);
    } else {
        LOGE("Recovery strategy failed for channel %d", channelIndex);
    }

    return success;
}

void StreamRecoveryManager::resetRecoveryAttempts(int channelIndex) {
    std::lock_guard<std::mutex> lock(recoveryMutex);
    recoveryAttempts[channelIndex] = 0;
}

int StreamRecoveryManager::getRecoveryAttempts(int channelIndex) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(recoveryMutex));

    auto it = recoveryAttempts.find(channelIndex);
    return (it != recoveryAttempts.end()) ? it->second : 0;
}

void StreamRecoveryManager::initializeBuiltInStrategies() {
    // Warning level recovery
    addRecoveryStrategy(StreamHealthMonitor::WARNING, RecoveryStrategy(
        "Warning Recovery",
        {ADJUST_BITRATE, INCREASE_BUFFER},
        2, 3000
    ));

    // Critical level recovery
    addRecoveryStrategy(StreamHealthMonitor::CRITICAL, RecoveryStrategy(
        "Critical Recovery",
        {REDUCE_QUALITY, RESET_DECODER, RECONNECT},
        3, 5000
    ));

    // Failed level recovery
    addRecoveryStrategy(StreamHealthMonitor::FAILED, RecoveryStrategy(
        "Failed Recovery",
        {RESTART_STREAM, CLEAR_CACHE, RECONNECT},
        5, 10000
    ));
}

bool StreamRecoveryManager::executeRecoveryAction(int channelIndex, RecoveryAction action) {
    LOGD("Executing recovery action '%s' for channel %d",
         recoveryActionToString(action).c_str(), channelIndex);

    // In a real implementation, these would call actual recovery functions
    switch (action) {
        case RESTART_STREAM:
            // Restart the stream
            return true;

        case REDUCE_QUALITY:
            // Reduce stream quality/bitrate
            return true;

        case INCREASE_BUFFER:
            // Increase buffer size
            return true;

        case RESET_DECODER:
            // Reset the decoder
            return true;

        case RECONNECT:
            // Reconnect to stream
            return true;

        case CLEAR_CACHE:
            // Clear stream cache
            return true;

        case ADJUST_BITRATE:
            // Adjust bitrate
            return true;

        default:
            LOGE("Unknown recovery action: %d", action);
            return false;
    }
}

std::string StreamRecoveryManager::recoveryActionToString(RecoveryAction action) const {
    switch (action) {
        case RESTART_STREAM: return "Restart Stream";
        case REDUCE_QUALITY: return "Reduce Quality";
        case INCREASE_BUFFER: return "Increase Buffer";
        case RESET_DECODER: return "Reset Decoder";
        case RECONNECT: return "Reconnect";
        case CLEAR_CACHE: return "Clear Cache";
        case ADJUST_BITRATE: return "Adjust Bitrate";
        default: return "Unknown";
    }
}
