#include "StreamHealthIntegration.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

StreamHealthIntegration::StreamHealthIntegration()
    : rtspManager(nullptr), streamProcessor(nullptr), decoderManager(nullptr),
      totalRecoveryActions(0), successfulRecoveries(0), failedRecoveries(0),
      optimizationThreadRunning(false) {
    LOGD("StreamHealthIntegration created");
}

StreamHealthIntegration::~StreamHealthIntegration() {
    cleanup();
    LOGD("StreamHealthIntegration destroyed");
}

bool StreamHealthIntegration::initialize(const HealthIntegrationConfig& config) {
    this->config = config;
    
    // Initialize health monitor
    healthMonitor = std::make_unique<StreamHealthMonitor>();
    healthMonitor->setEventListener(this);
    
    // Initialize anomaly detector
    anomalyDetector = std::make_unique<StreamAnomalyDetector>();
    
    // Initialize recovery manager
    recoveryManager = std::make_unique<StreamRecoveryManager>();
    
    // Apply health thresholds
    applyHealthThresholds();
    
    // Start performance optimization thread if enabled
    if (config.performanceOptimizationEnabled) {
        optimizationThreadRunning = true;
        optimizationThread = std::thread(&StreamHealthIntegration::performanceOptimizationLoop, this);
    }
    
    LOGD("StreamHealthIntegration initialized successfully");
    return true;
}

void StreamHealthIntegration::cleanup() {
    // Stop optimization thread
    optimizationThreadRunning = false;
    optimizationCv.notify_all();
    if (optimizationThread.joinable()) {
        optimizationThread.join();
    }
    
    // Cleanup health monitor
    if (healthMonitor) {
        healthMonitor->cleanup();
        healthMonitor.reset();
    }
    
    // Clear channel health status
    {
        std::lock_guard<std::mutex> lock(healthStatusMutex);
        channelHealthStatus.clear();
    }
    
    // Clear recovery data
    {
        std::lock_guard<std::mutex> lock(recoveryMutex);
        channelRecoveryAttempts.clear();
        lastRecoveryTime.clear();
    }
    
    LOGD("StreamHealthIntegration cleanup completed");
}

void StreamHealthIntegration::setRTSPStreamManager(RTSPStreamManager* manager) {
    rtspManager = manager;
    LOGD("RTSP Stream Manager set");
}

void StreamHealthIntegration::setMultiStreamProcessor(MultiStreamProcessor* processor) {
    streamProcessor = processor;
    LOGD("Multi-Stream Processor set");
}

void StreamHealthIntegration::setDecoderManager(DecoderManager* manager) {
    decoderManager = manager;
    LOGD("Decoder Manager set");
}

bool StreamHealthIntegration::addChannel(int channelIndex) {
    if (!validateChannelIndex(channelIndex)) {
        LOGE("Invalid channel index: %d", channelIndex);
        return false;
    }
    
    // Add to health monitor
    if (!healthMonitor->addChannel(channelIndex)) {
        LOGE("Failed to add channel %d to health monitor", channelIndex);
        return false;
    }
    
    // Create channel health status
    {
        std::lock_guard<std::mutex> lock(healthStatusMutex);
        channelHealthStatus[channelIndex] = std::make_unique<ChannelHealthStatus>(channelIndex);
    }
    
    // Initialize recovery attempts counter
    {
        std::lock_guard<std::mutex> lock(recoveryMutex);
        channelRecoveryAttempts[channelIndex] = 0;
        lastRecoveryTime[channelIndex] = std::chrono::steady_clock::now();
    }
    
    LOGD("Channel %d added to health integration", channelIndex);
    return true;
}

bool StreamHealthIntegration::removeChannel(int channelIndex) {
    // Remove from health monitor
    if (healthMonitor) {
        healthMonitor->removeChannel(channelIndex);
    }
    
    // Remove channel health status
    {
        std::lock_guard<std::mutex> lock(healthStatusMutex);
        channelHealthStatus.erase(channelIndex);
    }
    
    // Remove recovery data
    {
        std::lock_guard<std::mutex> lock(recoveryMutex);
        channelRecoveryAttempts.erase(channelIndex);
        lastRecoveryTime.erase(channelIndex);
    }
    
    LOGD("Channel %d removed from health integration", channelIndex);
    return true;
}

void StreamHealthIntegration::startHealthMonitoring() {
    if (healthMonitor) {
        // Health monitor starts automatically when created
        LOGD("Health monitoring started");
    }
}

void StreamHealthIntegration::stopHealthMonitoring() {
    if (healthMonitor) {
        healthMonitor->cleanup();
        LOGD("Health monitoring stopped");
    }
}

void StreamHealthIntegration::updateStreamHealth(int channelIndex, float fps, int droppedFrames, double latency) {
    if (!healthMonitor) return;
    
    healthMonitor->updateFrameRate(channelIndex, fps);
    healthMonitor->updateFrameDrops(channelIndex, droppedFrames, 100); // Assume 100 total frames
    healthMonitor->updateLatency(channelIndex, latency);
    
    LOGD("Updated stream health for channel %d: FPS=%.2f, Drops=%d, Latency=%.2fms", 
         channelIndex, fps, droppedFrames, latency);
}

void StreamHealthIntegration::updateConnectionHealth(int channelIndex, bool connected, int errorCount) {
    if (!healthMonitor) return;
    
    healthMonitor->updateConnectionStatus(channelIndex, connected);
    healthMonitor->updateErrorRate(channelIndex, errorCount, 100); // Assume 100 total operations
    
    LOGD("Updated connection health for channel %d: Connected=%s, Errors=%d", 
         channelIndex, connected ? "true" : "false", errorCount);
}

void StreamHealthIntegration::updateDecoderHealth(int channelIndex, float cpuUsage, long memoryUsage) {
    if (!healthMonitor) return;
    
    healthMonitor->updateResourceUsage(channelIndex, cpuUsage, memoryUsage);
    
    LOGD("Updated decoder health for channel %d: CPU=%.2f%%, Memory=%ldMB", 
         channelIndex, cpuUsage, memoryUsage / (1024 * 1024));
}

void StreamHealthIntegration::updateProcessingHealth(int channelIndex, float processingTime, int queueSize) {
    // Update processing-related health metrics
    // This could be extended to include more specific processing metrics
    LOGD("Updated processing health for channel %d: ProcessingTime=%.2fms, QueueSize=%d", 
         channelIndex, processingTime, queueSize);
}

bool StreamHealthIntegration::triggerManualRecovery(int channelIndex, RecoveryAction action) {
    if (!shouldAttemptRecovery(channelIndex)) {
        LOGW("Recovery not allowed for channel %d (too many attempts)", channelIndex);
        return false;
    }
    
    LOGD("Triggering manual recovery for channel %d, action: %d", channelIndex, action);
    
    bool success = executeRecoveryAction(channelIndex, action);
    updateRecoveryAttempts(channelIndex, success);
    
    if (recoveryActionCallback) {
        recoveryActionCallback(channelIndex, action, success);
    }
    
    return success;
}

void StreamHealthIntegration::enableAutoRecovery(int channelIndex, bool enabled) {
    auto channelStatus = getChannelHealthStatusInternal(channelIndex);
    if (channelStatus) {
        channelStatus->autoRecoveryEnabled = enabled;
        LOGD("Auto recovery %s for channel %d", enabled ? "enabled" : "disabled", channelIndex);
    }
}

StreamHealthIntegration::ChannelHealthStatus 
StreamHealthIntegration::getChannelHealthStatus(int channelIndex) const {
    std::lock_guard<std::mutex> lock(healthStatusMutex);
    
    auto it = channelHealthStatus.find(channelIndex);
    if (it != channelHealthStatus.end()) {
        return *it->second;
    }
    
    return ChannelHealthStatus(channelIndex);
}

std::vector<StreamHealthIntegration::ChannelHealthStatus> 
StreamHealthIntegration::getAllChannelHealthStatus() const {
    std::vector<ChannelHealthStatus> allStatus;
    std::lock_guard<std::mutex> lock(healthStatusMutex);
    
    for (const auto& pair : channelHealthStatus) {
        allStatus.push_back(*pair.second);
    }
    
    return allStatus;
}

StreamHealthMonitor::HealthStatus StreamHealthIntegration::getSystemHealthStatus() const {
    return healthMonitor ? healthMonitor->getSystemHealth() : StreamHealthMonitor::UNKNOWN;
}

float StreamHealthIntegration::getRecoverySuccessRate() const {
    int total = totalRecoveryActions.load();
    if (total == 0) return 0.0f;
    
    return static_cast<float>(successfulRecoveries.load()) / total * 100.0f;
}

// StreamHealthMonitor::HealthEventListener implementation
void StreamHealthIntegration::onHealthStatusChanged(int channelIndex, 
                                                   StreamHealthMonitor::HealthStatus oldStatus,
                                                   StreamHealthMonitor::HealthStatus newStatus) {
    LOGD("Health status changed for channel %d: %d -> %d", channelIndex, oldStatus, newStatus);
    
    processHealthStatusChange(channelIndex, newStatus);
    
    if (healthStatusCallback) {
        healthStatusCallback(channelIndex, newStatus);
    }
}

void StreamHealthIntegration::onHealthAlert(int channelIndex, StreamHealthMonitor::HealthMetric metric, 
                                           const std::string& message) {
    LOGW("Health alert for channel %d, metric %d: %s", channelIndex, metric, message.c_str());
    
    processHealthAlert(channelIndex, metric, message);
}

void StreamHealthIntegration::onHealthRecovered(int channelIndex, StreamHealthMonitor::HealthMetric metric) {
    LOGD("Health recovered for channel %d, metric %d", channelIndex, metric);
    
    // Remove alert from channel status
    auto channelStatus = getChannelHealthStatusInternal(channelIndex);
    if (channelStatus) {
        // Remove alerts related to this metric
        channelStatus->activeAlerts.erase(
            std::remove_if(channelStatus->activeAlerts.begin(), channelStatus->activeAlerts.end(),
                [metric](const std::string& alert) {
                    return alert.find("metric:" + std::to_string(metric)) != std::string::npos;
                }),
            channelStatus->activeAlerts.end()
        );
    }
}

void StreamHealthIntegration::onStreamFailure(int channelIndex, const std::string& reason) {
    LOGE("Stream failure for channel %d: %s", channelIndex, reason.c_str());
    
    processStreamFailure(channelIndex, reason);
}

void StreamHealthIntegration::onRecoveryAction(int channelIndex, const std::string& action) {
    LOGD("Recovery action triggered for channel %d: %s", channelIndex, action.c_str());
}

// Private methods
void StreamHealthIntegration::processHealthStatusChange(int channelIndex, 
                                                       StreamHealthMonitor::HealthStatus newStatus) {
    updateChannelHealthStatus(channelIndex, newStatus);
    
    // Trigger auto recovery if enabled and status is critical/failed
    if (config.autoRecoveryEnabled && 
        (newStatus == StreamHealthMonitor::CRITICAL || newStatus == StreamHealthMonitor::FAILED)) {
        
        auto channelStatus = getChannelHealthStatusInternal(channelIndex);
        if (channelStatus && channelStatus->autoRecoveryEnabled && shouldAttemptRecovery(channelIndex)) {
            
            // Select appropriate recovery action
            RecoveryAction action = selectRecoveryAction(channelIndex, newStatus, channelStatus->recentAnomalies);
            
            LOGD("Auto-triggering recovery action %d for channel %d", action, channelIndex);
            bool success = executeRecoveryAction(channelIndex, action);
            updateRecoveryAttempts(channelIndex, success);
            
            if (recoveryActionCallback) {
                recoveryActionCallback(channelIndex, action, success);
            }
        }
    }
}

void StreamHealthIntegration::processHealthAlert(int channelIndex, StreamHealthMonitor::HealthMetric metric, 
                                                const std::string& message) {
    auto channelStatus = getChannelHealthStatusInternal(channelIndex);
    if (channelStatus) {
        std::string alertMessage = "metric:" + std::to_string(metric) + " - " + message;
        channelStatus->activeAlerts.push_back(alertMessage);
        
        // Limit alert history
        if (channelStatus->activeAlerts.size() > 10) {
            channelStatus->activeAlerts.erase(channelStatus->activeAlerts.begin());
        }
    }
}

void StreamHealthIntegration::processStreamFailure(int channelIndex, const std::string& reason) {
    auto channelStatus = getChannelHealthStatusInternal(channelIndex);
    if (channelStatus) {
        channelStatus->recentAnomalies.push_back("Stream failure: " + reason);
        
        // Limit anomaly history
        if (channelStatus->recentAnomalies.size() > 5) {
            channelStatus->recentAnomalies.erase(channelStatus->recentAnomalies.begin());
        }
    }
    
    // Trigger emergency recovery
    if (config.autoRecoveryEnabled && shouldAttemptRecovery(channelIndex)) {
        RecoveryAction action = RECONNECT_STREAM; // Default action for stream failure
        
        LOGD("Emergency recovery triggered for channel %d", channelIndex);
        bool success = executeRecoveryAction(channelIndex, action);
        updateRecoveryAttempts(channelIndex, success);
        
        if (recoveryActionCallback) {
            recoveryActionCallback(channelIndex, action, success);
        }
    }
}

bool StreamHealthIntegration::executeRecoveryAction(int channelIndex, RecoveryAction action) {
    totalRecoveryActions++;
    
    bool success = false;
    
    switch (action) {
        case RECONNECT_STREAM:
            success = reconnectStream(channelIndex);
            break;
        case RESTART_DECODER:
            success = restartDecoder(channelIndex);
            break;
        case REDUCE_QUALITY:
            success = reduceStreamQuality(channelIndex);
            break;
        case INCREASE_BUFFER:
            success = increaseBufferSize(channelIndex);
            break;
        case RESET_CHANNEL:
            success = resetChannel(channelIndex);
            break;
        case THROTTLE_PROCESSING:
            success = throttleProcessing(channelIndex);
            break;
        case CLEAR_QUEUES:
            success = clearChannelQueues(channelIndex);
            break;
        case RESTART_THREAD_POOL:
            success = restartThreadPool(channelIndex);
            break;
        default:
            LOGE("Unknown recovery action: %d", action);
            break;
    }
    
    if (success) {
        successfulRecoveries++;
    } else {
        failedRecoveries++;
    }
    
    LOGD("Recovery action %d for channel %d: %s", action, channelIndex, success ? "SUCCESS" : "FAILED");
    return success;
}

// Recovery action implementations
bool StreamHealthIntegration::reconnectStream(int channelIndex) {
    if (!rtspManager) {
        LOGE("RTSP Manager not available for reconnection");
        return false;
    }

    // Disconnect and reconnect the stream
    rtspManager->disconnectStreamByIndex(channelIndex);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Wait 1 second
    return rtspManager->connectStreamByIndex(channelIndex);
}

bool StreamHealthIntegration::restartDecoder(int channelIndex) {
    if (!decoderManager) {
        LOGE("Decoder Manager not available for restart");
        return false;
    }

    // Reset the decoder for this channel
    return decoderManager->resetDecoder(channelIndex);
}

bool StreamHealthIntegration::reduceStreamQuality(int channelIndex) {
    // This would reduce stream quality/resolution to improve performance
    // Implementation depends on stream configuration capabilities
    LOGD("Reducing stream quality for channel %d", channelIndex);
    return true; // Placeholder implementation
}

bool StreamHealthIntegration::increaseBufferSize(int channelIndex) {
    // This would increase buffer sizes to handle network fluctuations
    LOGD("Increasing buffer size for channel %d", channelIndex);
    return true; // Placeholder implementation
}

bool StreamHealthIntegration::resetChannel(int channelIndex) {
    bool success = true;

    // Reset all components for this channel
    if (rtspManager) {
        rtspManager->disconnectStreamByIndex(channelIndex);
    }

    if (decoderManager) {
        success &= decoderManager->resetDecoder(channelIndex);
    }

    if (streamProcessor) {
        success &= streamProcessor->stopStream(channelIndex);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        success &= streamProcessor->startStream(channelIndex);
    }

    if (rtspManager) {
        success &= rtspManager->connectStreamByIndex(channelIndex);
    }

    return success;
}

bool StreamHealthIntegration::throttleProcessing(int channelIndex) {
    if (!streamProcessor) {
        return false;
    }

    // Reduce processing priority for this channel
    streamProcessor->setStreamPriority(channelIndex, MultiStreamProcessor::LOW);
    LOGD("Throttled processing for channel %d", channelIndex);
    return true;
}

bool StreamHealthIntegration::clearChannelQueues(int channelIndex) {
    // This would clear processing queues for the channel
    LOGD("Clearing queues for channel %d", channelIndex);
    return true; // Placeholder implementation
}

bool StreamHealthIntegration::restartThreadPool(int channelIndex) {
    // This would restart thread pool for the channel
    LOGD("Restarting thread pool for channel %d", channelIndex);
    return true; // Placeholder implementation
}

StreamHealthIntegration::RecoveryAction
StreamHealthIntegration::selectRecoveryAction(int channelIndex, StreamHealthMonitor::HealthStatus health,
                                             const std::vector<std::string>& anomalies) {
    // Select recovery action based on health status and anomalies
    if (health == StreamHealthMonitor::FAILED) {
        // For failed streams, try reconnection first
        return RECONNECT_STREAM;
    } else if (health == StreamHealthMonitor::CRITICAL) {
        // For critical issues, check anomalies
        for (const auto& anomaly : anomalies) {
            if (anomaly.find("Connection") != std::string::npos) {
                return RECONNECT_STREAM;
            } else if (anomaly.find("Decoder") != std::string::npos) {
                return RESTART_DECODER;
            } else if (anomaly.find("Memory") != std::string::npos) {
                return CLEAR_QUEUES;
            } else if (anomaly.find("CPU") != std::string::npos) {
                return THROTTLE_PROCESSING;
            }
        }

        // Default critical action
        return RESET_CHANNEL;
    } else {
        // For warning status, try lighter actions
        return CLEAR_QUEUES;
    }
}

bool StreamHealthIntegration::shouldAttemptRecovery(int channelIndex) const {
    std::lock_guard<std::mutex> lock(recoveryMutex);

    auto it = channelRecoveryAttempts.find(channelIndex);
    if (it == channelRecoveryAttempts.end()) {
        return true; // No attempts yet
    }

    int attempts = it->second.load();
    if (attempts >= config.maxRecoveryAttempts) {
        return false; // Too many attempts
    }

    // Check time since last recovery
    auto timeIt = lastRecoveryTime.find(channelIndex);
    if (timeIt != lastRecoveryTime.end()) {
        auto timeSinceLastRecovery = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - timeIt->second);

        if (timeSinceLastRecovery.count() < config.recoveryDelayMs) {
            return false; // Too soon since last recovery
        }
    }

    return true;
}

void StreamHealthIntegration::updateRecoveryAttempts(int channelIndex, bool success) {
    std::lock_guard<std::mutex> lock(recoveryMutex);

    if (success) {
        // Reset attempts on success
        channelRecoveryAttempts[channelIndex] = 0;
    } else {
        // Increment attempts on failure
        channelRecoveryAttempts[channelIndex]++;
    }

    lastRecoveryTime[channelIndex] = std::chrono::steady_clock::now();
}

void StreamHealthIntegration::performanceOptimizationLoop() {
    while (optimizationThreadRunning) {
        std::unique_lock<std::mutex> lock(optimizationMutex);
        optimizationCv.wait_for(lock, std::chrono::seconds(10), [this] { return !optimizationThreadRunning; });

        if (!optimizationThreadRunning) break;

        analyzeSystemPerformance();
        balanceSystemLoad();
    }
}

void StreamHealthIntegration::analyzeSystemPerformance() {
    if (!healthMonitor) return;

    auto systemHealth = healthMonitor->getSystemHealth();

    // Analyze overall system performance
    if (systemHealth == StreamHealthMonitor::WARNING || systemHealth == StreamHealthMonitor::CRITICAL) {
        LOGW("System performance degraded, health status: %d", systemHealth);

        // Optimize individual channels
        std::lock_guard<std::mutex> lock(healthStatusMutex);
        for (const auto& pair : channelHealthStatus) {
            if (pair.second->overallHealth != StreamHealthMonitor::HEALTHY) {
                optimizeChannelPerformance(pair.first);
            }
        }
    }
}

void StreamHealthIntegration::optimizeChannelPerformance(int channelIndex) {
    auto channelStatus = getChannelHealthStatusInternal(channelIndex);
    if (!channelStatus) return;

    // Apply performance optimizations based on health status
    if (channelStatus->overallHealth == StreamHealthMonitor::WARNING) {
        // Light optimizations
        if (streamProcessor) {
            streamProcessor->setStreamPriority(channelIndex, MultiStreamProcessor::NORMAL);
        }
    } else if (channelStatus->overallHealth == StreamHealthMonitor::CRITICAL) {
        // Aggressive optimizations
        if (streamProcessor) {
            streamProcessor->setStreamPriority(channelIndex, MultiStreamProcessor::LOW);
        }

        // Consider reducing quality
        adaptChannelQuality(channelIndex, channelStatus->overallHealth);
    }

    LOGD("Applied performance optimizations for channel %d", channelIndex);
}

void StreamHealthIntegration::balanceSystemLoad() {
    if (!streamProcessor) return;

    // Get system statistics
    float systemCpuUsage = streamProcessor->getSystemCpuUsage();
    long systemMemoryUsage = streamProcessor->getSystemMemoryUsage();

    // Balance load if system is overloaded
    if (systemCpuUsage > 80.0f || systemMemoryUsage > 1024 * 1024 * 1024) { // 1GB threshold
        LOGW("System overloaded, balancing load. CPU: %.2f%%, Memory: %ldMB",
             systemCpuUsage, systemMemoryUsage / (1024 * 1024));

        // Trigger load balancing
        streamProcessor->triggerLoadBalancing();
    }
}

void StreamHealthIntegration::adaptChannelQuality(int channelIndex, StreamHealthMonitor::HealthStatus health) {
    // Adapt channel quality based on health status
    if (health == StreamHealthMonitor::CRITICAL || health == StreamHealthMonitor::FAILED) {
        reduceStreamQuality(channelIndex);
    }
}

StreamHealthIntegration::ChannelHealthStatus*
StreamHealthIntegration::getChannelHealthStatusInternal(int channelIndex) {
    std::lock_guard<std::mutex> lock(healthStatusMutex);

    auto it = channelHealthStatus.find(channelIndex);
    return (it != channelHealthStatus.end()) ? it->second.get() : nullptr;
}

void StreamHealthIntegration::updateChannelHealthStatus(int channelIndex, StreamHealthMonitor::HealthStatus status) {
    auto channelStatus = getChannelHealthStatusInternal(channelIndex);
    if (channelStatus) {
        channelStatus->overallHealth = status;
    }
}

bool StreamHealthIntegration::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

bool StreamHealthIntegration::validateComponentIntegration() const {
    return rtspManager != nullptr && streamProcessor != nullptr && decoderManager != nullptr;
}

void StreamHealthIntegration::applyHealthThresholds() {
    if (!healthMonitor) return;

    StreamHealthMonitor::HealthThresholds thresholds;
    thresholds.minFps = 15.0f;
    thresholds.maxDropRate = 0.05f;
    thresholds.maxLatency = 500.0;
    thresholds.maxErrorRate = 0.02f;
    thresholds.maxConsecutiveFailures = 3;
    thresholds.healthCheckInterval = static_cast<int>(config.healthCheckIntervalSec * 1000);

    healthMonitor->setHealthThresholds(thresholds);
}

std::string StreamHealthIntegration::generateHealthReport() const {
    std::ostringstream report;

    report << "=== Stream Health Integration Report ===\n";
    report << "System Health: " << getSystemHealthStatus() << "\n";
    report << "Total Recovery Actions: " << totalRecoveryActions.load() << "\n";
    report << "Successful Recoveries: " << successfulRecoveries.load() << "\n";
    report << "Failed Recoveries: " << failedRecoveries.load() << "\n";
    report << "Recovery Success Rate: " << std::fixed << std::setprecision(2)
           << getRecoverySuccessRate() << "%\n\n";

    auto allChannelStatus = getAllChannelHealthStatus();
    report << "Channel Status:\n";
    for (const auto& status : allChannelStatus) {
        report << "  Channel " << status.channelIndex
               << ": Health=" << status.overallHealth
               << ", Recovery Attempts=" << status.recoveryAttempts
               << ", Auto Recovery=" << (status.autoRecoveryEnabled ? "ON" : "OFF") << "\n";

        if (!status.activeAlerts.empty()) {
            report << "    Active Alerts: " << status.activeAlerts.size() << "\n";
        }
    }

    return report.str();
}

std::string StreamHealthIntegration::generateRecoveryReport() const {
    std::ostringstream report;

    report << "=== Recovery Report ===\n";
    report << "Total Recovery Actions: " << totalRecoveryActions.load() << "\n";
    report << "Successful Recoveries: " << successfulRecoveries.load() << "\n";
    report << "Failed Recoveries: " << failedRecoveries.load() << "\n";
    report << "Success Rate: " << std::fixed << std::setprecision(2)
           << getRecoverySuccessRate() << "%\n";

    return report.str();
}

std::vector<std::string> StreamHealthIntegration::getSystemRecommendations() const {
    std::vector<std::string> recommendations;

    auto systemHealth = getSystemHealthStatus();
    if (systemHealth == StreamHealthMonitor::WARNING) {
        recommendations.push_back("System performance is degraded. Consider reducing stream quality or count.");
    } else if (systemHealth == StreamHealthMonitor::CRITICAL) {
        recommendations.push_back("System is in critical state. Immediate action required.");
        recommendations.push_back("Consider stopping non-essential streams.");
    } else if (systemHealth == StreamHealthMonitor::FAILED) {
        recommendations.push_back("System failure detected. Restart required.");
    }

    float successRate = getRecoverySuccessRate();
    if (successRate < 50.0f && totalRecoveryActions.load() > 10) {
        recommendations.push_back("Low recovery success rate. Check system configuration.");
    }

    return recommendations;
}

void StreamHealthIntegration::setHealthStatusCallback(HealthStatusCallback callback) {
    healthStatusCallback = callback;
}

void StreamHealthIntegration::setRecoveryActionCallback(RecoveryActionCallback callback) {
    recoveryActionCallback = callback;
}

void StreamHealthIntegration::setSystemHealthCallback(SystemHealthCallback callback) {
    systemHealthCallback = callback;
}

// StreamHealthDashboard implementation
StreamHealthDashboard::StreamHealthDashboard(StreamHealthIntegration* integration)
    : healthIntegration(integration), updateThreadRunning(false) {
    LOGD("StreamHealthDashboard created");
}

StreamHealthDashboard::~StreamHealthDashboard() {
    stopDashboard();
    LOGD("StreamHealthDashboard destroyed");
}

void StreamHealthDashboard::startDashboard() {
    if (updateThreadRunning) {
        LOGW("Dashboard already running");
        return;
    }

    updateThreadRunning = true;
    updateThread = std::thread(&StreamHealthDashboard::updateLoop, this);
    LOGD("Dashboard started");
}

void StreamHealthDashboard::stopDashboard() {
    if (!updateThreadRunning) {
        return;
    }

    updateThreadRunning = false;
    updateCv.notify_all();

    if (updateThread.joinable()) {
        updateThread.join();
    }

    LOGD("Dashboard stopped");
}

StreamHealthDashboard::DashboardData StreamHealthDashboard::getDashboardData() const {
    std::lock_guard<std::mutex> lock(dashboardMutex);
    return dashboardData;
}

std::string StreamHealthDashboard::generateDashboardReport() const {
    auto data = getDashboardData();
    std::ostringstream report;

    report << "=== Stream Health Dashboard ===\n";
    report << "System Health: " << data.systemHealth << "\n";
    report << "Total Channels: " << data.totalChannels << "\n";
    report << "Healthy: " << data.healthyChannels << "\n";
    report << "Warning: " << data.warningChannels << "\n";
    report << "Critical: " << data.criticalChannels << "\n";
    report << "Failed: " << data.failedChannels << "\n";
    report << "Average FPS: " << std::fixed << std::setprecision(2) << data.averageSystemFps << "\n";
    report << "Total Bandwidth: " << data.totalBandwidthMbps << " Mbps\n";
    report << "Recovery Actions: " << data.totalRecoveryActions << "\n";
    report << "Recovery Success Rate: " << data.recoverySuccessRate << "%\n";

    if (!data.systemAlerts.empty()) {
        report << "\nSystem Alerts:\n";
        for (const auto& alert : data.systemAlerts) {
            report << "  - " << alert << "\n";
        }
    }

    if (!data.recommendations.empty()) {
        report << "\nRecommendations:\n";
        for (const auto& recommendation : data.recommendations) {
            report << "  - " << recommendation << "\n";
        }
    }

    return report.str();
}

std::string StreamHealthDashboard::generateJsonStatus() const {
    auto data = getDashboardData();
    std::ostringstream json;

    json << "{\n";
    json << "  \"systemHealth\": " << data.systemHealth << ",\n";
    json << "  \"totalChannels\": " << data.totalChannels << ",\n";
    json << "  \"healthyChannels\": " << data.healthyChannels << ",\n";
    json << "  \"warningChannels\": " << data.warningChannels << ",\n";
    json << "  \"criticalChannels\": " << data.criticalChannels << ",\n";
    json << "  \"failedChannels\": " << data.failedChannels << ",\n";
    json << "  \"averageSystemFps\": " << data.averageSystemFps << ",\n";
    json << "  \"totalBandwidthMbps\": " << data.totalBandwidthMbps << ",\n";
    json << "  \"totalRecoveryActions\": " << data.totalRecoveryActions << ",\n";
    json << "  \"recoverySuccessRate\": " << data.recoverySuccessRate << ",\n";
    json << "  \"lastUpdate\": \"" << std::chrono::duration_cast<std::chrono::seconds>(
        data.lastUpdate.time_since_epoch()).count() << "\"\n";
    json << "}";

    return json.str();
}

void StreamHealthDashboard::setUpdateInterval(int intervalMs) {
    // This would set the update interval for the dashboard
    LOGD("Dashboard update interval set to %d ms", intervalMs);
}

void StreamHealthDashboard::forceUpdate() {
    updateCv.notify_one();
}

void StreamHealthDashboard::updateLoop() {
    while (updateThreadRunning) {
        std::unique_lock<std::mutex> lock(updateMutex);
        updateCv.wait_for(lock, std::chrono::seconds(5), [this] { return !updateThreadRunning; });

        if (!updateThreadRunning) break;

        updateDashboardData();
    }
}

void StreamHealthDashboard::updateDashboardData() {
    if (!healthIntegration) return;

    std::lock_guard<std::mutex> lock(dashboardMutex);

    // Update basic system data
    dashboardData.systemHealth = healthIntegration->getSystemHealthStatus();
    dashboardData.totalRecoveryActions = healthIntegration->getTotalRecoveryActions();
    dashboardData.recoverySuccessRate = healthIntegration->getRecoverySuccessRate();

    // Update channel data
    auto allChannelStatus = healthIntegration->getAllChannelHealthStatus();
    dashboardData.totalChannels = allChannelStatus.size();
    dashboardData.healthyChannels = 0;
    dashboardData.warningChannels = 0;
    dashboardData.criticalChannels = 0;
    dashboardData.failedChannels = 0;

    for (const auto& status : allChannelStatus) {
        switch (status.overallHealth) {
            case StreamHealthMonitor::HEALTHY:
                dashboardData.healthyChannels++;
                break;
            case StreamHealthMonitor::WARNING:
                dashboardData.warningChannels++;
                break;
            case StreamHealthMonitor::CRITICAL:
                dashboardData.criticalChannels++;
                break;
            case StreamHealthMonitor::FAILED:
                dashboardData.failedChannels++;
                break;
            default:
                break;
        }
    }

    dashboardData.channelStatus = allChannelStatus;

    // Collect system metrics
    collectSystemMetrics();

    // Generate recommendations
    generateSystemRecommendations();

    dashboardData.lastUpdate = std::chrono::steady_clock::now();
}

void StreamHealthDashboard::collectSystemMetrics() {
    // This would collect actual system metrics
    // For now, we'll use placeholder values
    dashboardData.averageSystemFps = 25.0f; // Placeholder
    dashboardData.totalBandwidthMbps = 50.0f; // Placeholder
}

void StreamHealthDashboard::generateSystemRecommendations() {
    if (!healthIntegration) return;

    dashboardData.recommendations = healthIntegration->getSystemRecommendations();

    // Add dashboard-specific recommendations
    if (dashboardData.failedChannels > 0) {
        dashboardData.recommendations.push_back("Failed channels detected. Check network connectivity.");
    }

    if (dashboardData.criticalChannels > dashboardData.totalChannels * 0.3f) {
        dashboardData.recommendations.push_back("High number of critical channels. System overload suspected.");
    }
}
