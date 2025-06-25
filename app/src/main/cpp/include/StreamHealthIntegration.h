#ifndef AIBOX_STREAM_HEALTH_INTEGRATION_H
#define AIBOX_STREAM_HEALTH_INTEGRATION_H

#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>

#include "StreamHealthMonitor.h"
#include "RTSPStreamManager.h"
#include "MultiStreamProcessor.h"
#include "DecoderManager.h"
#include "log4c.h"

/**
 * Stream Health Integration
 * Integrates Stream Health Monitoring with Multi-Stream Processing components
 * Provides comprehensive health monitoring, automatic recovery, and system optimization
 */
class StreamHealthIntegration : public StreamHealthMonitor::HealthEventListener {
public:
    enum RecoveryAction {
        RECONNECT_STREAM = 0,
        RESTART_DECODER = 1,
        REDUCE_QUALITY = 2,
        INCREASE_BUFFER = 3,
        RESET_CHANNEL = 4,
        THROTTLE_PROCESSING = 5,
        CLEAR_QUEUES = 6,
        RESTART_THREAD_POOL = 7
    };

    struct HealthIntegrationConfig {
        bool autoRecoveryEnabled;
        int maxRecoveryAttempts;
        int recoveryDelayMs;
        bool adaptiveQualityEnabled;
        bool performanceOptimizationEnabled;
        float healthCheckIntervalSec;
        
        HealthIntegrationConfig() : autoRecoveryEnabled(true), maxRecoveryAttempts(3),
                                  recoveryDelayMs(5000), adaptiveQualityEnabled(true),
                                  performanceOptimizationEnabled(true), healthCheckIntervalSec(2.0f) {}
    };

    struct ChannelHealthStatus {
        int channelIndex;
        StreamHealthMonitor::HealthStatus overallHealth;
        std::map<StreamHealthMonitor::HealthMetric, StreamHealthMonitor::HealthStatus> metricHealth;
        std::vector<std::string> activeAlerts;
        std::vector<std::string> recentAnomalies;
        int recoveryAttempts;
        std::chrono::steady_clock::time_point lastRecoveryTime;
        bool autoRecoveryEnabled;
        
        ChannelHealthStatus(int index) : channelIndex(index), 
                                       overallHealth(StreamHealthMonitor::UNKNOWN),
                                       recoveryAttempts(0), autoRecoveryEnabled(true) {
            lastRecoveryTime = std::chrono::steady_clock::now();
        }
    };

    // Event callbacks
    using HealthStatusCallback = std::function<void(int channelIndex, StreamHealthMonitor::HealthStatus status)>;
    using RecoveryActionCallback = std::function<void(int channelIndex, RecoveryAction action, bool success)>;
    using SystemHealthCallback = std::function<void(StreamHealthMonitor::HealthStatus systemHealth)>;

private:
    std::unique_ptr<StreamHealthMonitor> healthMonitor;
    std::unique_ptr<StreamAnomalyDetector> anomalyDetector;
    std::unique_ptr<StreamRecoveryManager> recoveryManager;
    
    // Integration components
    RTSPStreamManager* rtspManager;
    MultiStreamProcessor* streamProcessor;
    DecoderManager* decoderManager;
    
    // Channel health status
    std::map<int, std::unique_ptr<ChannelHealthStatus>> channelHealthStatus;
    mutable std::mutex healthStatusMutex;

    // Configuration
    HealthIntegrationConfig config;
    mutable std::mutex configMutex;

    // Recovery management
    std::map<int, std::atomic<int>> channelRecoveryAttempts;
    std::map<int, std::chrono::steady_clock::time_point> lastRecoveryTime;
    mutable std::mutex recoveryMutex;
    
    // Event callbacks
    HealthStatusCallback healthStatusCallback;
    RecoveryActionCallback recoveryActionCallback;
    SystemHealthCallback systemHealthCallback;
    
    // Statistics and monitoring
    std::atomic<int> totalRecoveryActions;
    std::atomic<int> successfulRecoveries;
    std::atomic<int> failedRecoveries;
    
    // Performance optimization
    std::thread optimizationThread;
    std::atomic<bool> optimizationThreadRunning;
    std::condition_variable optimizationCv;
    mutable std::mutex optimizationMutex;

public:
    StreamHealthIntegration();
    ~StreamHealthIntegration();
    
    // Initialization
    bool initialize(const HealthIntegrationConfig& config = HealthIntegrationConfig());
    void cleanup();
    
    // Component integration
    void setRTSPStreamManager(RTSPStreamManager* manager);
    void setMultiStreamProcessor(MultiStreamProcessor* processor);
    void setDecoderManager(DecoderManager* manager);
    
    // Channel management
    bool addChannel(int channelIndex);
    bool removeChannel(int channelIndex);
    bool isChannelMonitored(int channelIndex) const;
    
    // Health monitoring control
    void startHealthMonitoring();
    void stopHealthMonitoring();
    void pauseHealthMonitoring(int channelIndex);
    void resumeHealthMonitoring(int channelIndex);
    
    // Health data updates (called by stream components)
    void updateStreamHealth(int channelIndex, float fps, int droppedFrames, double latency);
    void updateConnectionHealth(int channelIndex, bool connected, int errorCount);
    void updateDecoderHealth(int channelIndex, float cpuUsage, long memoryUsage);
    void updateProcessingHealth(int channelIndex, float processingTime, int queueSize);
    
    // Recovery management
    void enableAutoRecovery(int channelIndex, bool enabled);
    bool triggerManualRecovery(int channelIndex, RecoveryAction action);
    void resetChannelRecovery(int channelIndex);
    
    // Configuration
    void setHealthIntegrationConfig(const HealthIntegrationConfig& config);
    HealthIntegrationConfig getHealthIntegrationConfig() const;
    void setChannelAutoRecovery(int channelIndex, bool enabled);
    
    // Status and statistics
    ChannelHealthStatus getChannelHealthStatus(int channelIndex) const;
    std::vector<ChannelHealthStatus> getAllChannelHealthStatus() const;
    StreamHealthMonitor::HealthStatus getSystemHealthStatus() const;
    
    // Statistics
    int getTotalRecoveryActions() const { return totalRecoveryActions.load(); }
    int getSuccessfulRecoveries() const { return successfulRecoveries.load(); }
    int getFailedRecoveries() const { return failedRecoveries.load(); }
    float getRecoverySuccessRate() const;
    
    // Event callbacks
    void setHealthStatusCallback(HealthStatusCallback callback);
    void setRecoveryActionCallback(RecoveryActionCallback callback);
    void setSystemHealthCallback(SystemHealthCallback callback);
    
    // Diagnostics and reporting
    std::string generateHealthReport() const;
    std::string generateRecoveryReport() const;
    std::vector<std::string> getSystemRecommendations() const;
    
    // Performance optimization
    void enablePerformanceOptimization(bool enabled);
    void optimizeSystemPerformance();
    void adaptChannelQuality(int channelIndex, StreamHealthMonitor::HealthStatus health);

protected:
    // StreamHealthMonitor::HealthEventListener implementation
    void onHealthStatusChanged(int channelIndex, StreamHealthMonitor::HealthStatus oldStatus, 
                              StreamHealthMonitor::HealthStatus newStatus) override;
    void onHealthAlert(int channelIndex, StreamHealthMonitor::HealthMetric metric, 
                      const std::string& message) override;
    void onHealthRecovered(int channelIndex, StreamHealthMonitor::HealthMetric metric) override;
    void onStreamFailure(int channelIndex, const std::string& reason) override;
    void onRecoveryAction(int channelIndex, const std::string& action) override;

private:
    // Internal processing
    void processHealthStatusChange(int channelIndex, StreamHealthMonitor::HealthStatus newStatus);
    void processHealthAlert(int channelIndex, StreamHealthMonitor::HealthMetric metric, 
                           const std::string& message);
    void processStreamFailure(int channelIndex, const std::string& reason);
    
    // Recovery actions
    bool executeRecoveryAction(int channelIndex, RecoveryAction action);
    bool reconnectStream(int channelIndex);
    bool restartDecoder(int channelIndex);
    bool reduceStreamQuality(int channelIndex);
    bool increaseBufferSize(int channelIndex);
    bool resetChannel(int channelIndex);
    bool throttleProcessing(int channelIndex);
    bool clearChannelQueues(int channelIndex);
    bool restartThreadPool(int channelIndex);
    
    // Recovery strategy selection
    RecoveryAction selectRecoveryAction(int channelIndex, StreamHealthMonitor::HealthStatus health,
                                       const std::vector<std::string>& anomalies);
    bool shouldAttemptRecovery(int channelIndex) const;
    void updateRecoveryAttempts(int channelIndex, bool success);
    
    // Performance optimization
    void performanceOptimizationLoop();
    void analyzeSystemPerformance();
    void optimizeChannelPerformance(int channelIndex);
    void balanceSystemLoad();
    
    // Utility methods
    ChannelHealthStatus* getChannelHealthStatusInternal(int channelIndex);
    void updateChannelHealthStatus(int channelIndex, StreamHealthMonitor::HealthStatus status);
    void notifyHealthStatusCallback(int channelIndex, StreamHealthMonitor::HealthStatus status);
    void notifyRecoveryActionCallback(int channelIndex, RecoveryAction action, bool success);
    void notifySystemHealthCallback(StreamHealthMonitor::HealthStatus systemHealth);
    
    // Configuration helpers
    void applyHealthThresholds();
    void setupHealthMonitorCallbacks();
    
    // Validation
    bool validateChannelIndex(int channelIndex) const;
    bool validateComponentIntegration() const;
};

/**
 * Stream Health Dashboard
 * Provides a comprehensive view of system health status and metrics
 */
class StreamHealthDashboard {
public:
    struct DashboardData {
        StreamHealthMonitor::HealthStatus systemHealth;
        int totalChannels;
        int healthyChannels;
        int warningChannels;
        int criticalChannels;
        int failedChannels;
        float averageSystemFps;
        float totalBandwidthMbps;
        int totalRecoveryActions;
        float recoverySuccessRate;
        std::vector<StreamHealthIntegration::ChannelHealthStatus> channelStatus;
        std::vector<std::string> systemAlerts;
        std::vector<std::string> recommendations;
        std::chrono::steady_clock::time_point lastUpdate;
        
        DashboardData() : systemHealth(StreamHealthMonitor::UNKNOWN), totalChannels(0),
                         healthyChannels(0), warningChannels(0), criticalChannels(0),
                         failedChannels(0), averageSystemFps(0.0f), totalBandwidthMbps(0.0f),
                         totalRecoveryActions(0), recoverySuccessRate(0.0f) {
            lastUpdate = std::chrono::steady_clock::now();
        }
    };

private:
    StreamHealthIntegration* healthIntegration;
    DashboardData dashboardData;
    mutable std::mutex dashboardMutex;

    std::thread updateThread;
    std::atomic<bool> updateThreadRunning;
    std::condition_variable updateCv;
    mutable std::mutex updateMutex;

public:
    StreamHealthDashboard(StreamHealthIntegration* integration);
    ~StreamHealthDashboard();
    
    // Dashboard control
    void startDashboard();
    void stopDashboard();
    
    // Data access
    DashboardData getDashboardData() const;
    std::string generateDashboardReport() const;
    std::string generateJsonStatus() const;
    
    // Update control
    void setUpdateInterval(int intervalMs);
    void forceUpdate();

private:
    void updateLoop();
    void updateDashboardData();
    void collectSystemMetrics();
    void generateSystemRecommendations();
};

#endif // AIBOX_STREAM_HEALTH_INTEGRATION_H
