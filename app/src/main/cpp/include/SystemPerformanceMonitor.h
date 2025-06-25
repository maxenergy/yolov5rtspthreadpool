#ifndef AIBOX_SYSTEM_PERFORMANCE_MONITOR_H
#define AIBOX_SYSTEM_PERFORMANCE_MONITOR_H

#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <queue>
#include <fstream>

#include "log4c.h"

/**
 * System Performance Monitor
 * Comprehensive performance monitoring and optimization for multi-channel system
 */
class SystemPerformanceMonitor {
public:
    enum PerformanceLevel {
        EXCELLENT = 0,
        GOOD = 1,
        FAIR = 2,
        POOR = 3,
        CRITICAL = 4
    };

    enum ResourceType {
        CPU_USAGE = 0,
        MEMORY_USAGE = 1,
        GPU_USAGE = 2,
        NETWORK_BANDWIDTH = 3,
        DISK_IO = 4,
        FRAME_RATE = 5,
        DETECTION_RATE = 6,
        RENDER_RATE = 7
    };

    struct SystemMetrics {
        float cpuUsage;
        long memoryUsage;
        float gpuUsage;
        float networkBandwidth;
        float diskIO;
        float systemFps;
        float detectionFps;
        float renderFps;
        int activeChannels;
        int totalChannels;
        std::chrono::steady_clock::time_point timestamp;
        
        SystemMetrics() : cpuUsage(0.0f), memoryUsage(0), gpuUsage(0.0f),
                         networkBandwidth(0.0f), diskIO(0.0f), systemFps(0.0f),
                         detectionFps(0.0f), renderFps(0.0f), activeChannels(0),
                         totalChannels(0) {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    struct ChannelPerformanceMetrics {
        int channelIndex;
        float fps;
        float detectionFps;
        float renderFps;
        float cpuUsage;
        long memoryUsage;
        int queueSize;
        int droppedFrames;
        float averageLatency;
        float peakLatency;
        PerformanceLevel performanceLevel;
        std::vector<std::string> performanceIssues;
        std::chrono::steady_clock::time_point lastUpdate;
        
        ChannelPerformanceMetrics() : channelIndex(-1), fps(0.0f),
                                     detectionFps(0.0f), renderFps(0.0f),
                                     cpuUsage(0.0f), memoryUsage(0),
                                     queueSize(0), droppedFrames(0),
                                     averageLatency(0.0f), peakLatency(0.0f),
                                     performanceLevel(EXCELLENT) {
            lastUpdate = std::chrono::steady_clock::now();
        }

        ChannelPerformanceMetrics(int index) : channelIndex(index), fps(0.0f),
                                             detectionFps(0.0f), renderFps(0.0f),
                                             cpuUsage(0.0f), memoryUsage(0),
                                             queueSize(0), droppedFrames(0),
                                             averageLatency(0.0f), peakLatency(0.0f),
                                             performanceLevel(EXCELLENT) {
            lastUpdate = std::chrono::steady_clock::now();
        }
    };

    struct PerformanceThresholds {
        float targetFps;
        float minFps;
        float maxCpuUsage;
        long maxMemoryUsage;
        float maxLatency;
        int maxQueueSize;
        float maxDropRate;
        
        PerformanceThresholds() : targetFps(30.0f), minFps(25.0f), maxCpuUsage(80.0f),
                                maxMemoryUsage(512 * 1024 * 1024), maxLatency(100.0f),
                                maxQueueSize(50), maxDropRate(0.05f) {}
    };

    struct OptimizationAction {
        int channelIndex;
        std::string actionType;
        std::string description;
        int priority;
        std::chrono::steady_clock::time_point timestamp;
        
        OptimizationAction(int channel, const std::string& type, const std::string& desc, int prio)
            : channelIndex(channel), actionType(type), description(desc), priority(prio) {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    // Event listener interface
    class PerformanceEventListener {
    public:
        virtual ~PerformanceEventListener() = default;
        virtual void onPerformanceLevelChanged(int channelIndex, PerformanceLevel oldLevel, PerformanceLevel newLevel) = 0;
        virtual void onSystemPerformanceAlert(PerformanceLevel level, const std::string& message) = 0;
        virtual void onOptimizationApplied(const OptimizationAction& action) = 0;
        virtual void onResourceThresholdExceeded(ResourceType resource, float value, float threshold) = 0;
        virtual void onPerformanceReport(const SystemMetrics& metrics) = 0;
    };

private:
    // Performance data
    SystemMetrics currentMetrics;
    std::map<int, ChannelPerformanceMetrics> channelMetrics;
    std::queue<SystemMetrics> metricsHistory;
    mutable std::mutex metricsMutex;

    // Configuration
    PerformanceThresholds thresholds;
    mutable std::mutex thresholdsMutex;
    
    // Monitoring threads
    std::thread monitorThread;
    std::thread optimizationThread;
    std::atomic<bool> monitorRunning;
    std::condition_variable monitorCv;
    std::condition_variable optimizationCv;
    std::mutex threadMutex;
    
    // Optimization queue
    std::queue<OptimizationAction> optimizationQueue;
    std::mutex optimizationMutex;
    
    // Event listener
    PerformanceEventListener* eventListener;
    
    // Configuration
    int monitorIntervalMs;
    int optimizationIntervalMs;
    int historySize;
    bool enableAutoOptimization;
    bool enableDetailedLogging;
    
    // System resource monitoring
    std::atomic<float> systemCpuUsage;
    std::atomic<long> systemMemoryUsage;
    std::atomic<float> systemGpuUsage;
    
    // Performance logging
    std::ofstream performanceLogFile;
    std::mutex logMutex;

public:
    SystemPerformanceMonitor();
    ~SystemPerformanceMonitor();
    
    // Initialization
    bool initialize();
    void cleanup();
    
    // Monitoring control
    void startMonitoring();
    void stopMonitoring();
    void pauseMonitoring();
    void resumeMonitoring();
    
    // Channel management
    bool addChannel(int channelIndex);
    bool removeChannel(int channelIndex);
    bool isChannelMonitored(int channelIndex) const;
    
    // Metrics updates
    void updateChannelMetrics(int channelIndex, float fps, float detectionFps, float renderFps);
    void updateChannelResourceUsage(int channelIndex, float cpuUsage, long memoryUsage);
    void updateChannelLatency(int channelIndex, float latency);
    void updateChannelQueueSize(int channelIndex, int queueSize);
    void reportDroppedFrames(int channelIndex, int droppedFrames);
    
    // System metrics updates
    void updateSystemMetrics(const SystemMetrics& metrics);
    void updateSystemResourceUsage(float cpuUsage, long memoryUsage, float gpuUsage);
    
    // Performance assessment
    PerformanceLevel assessChannelPerformance(int channelIndex) const;
    PerformanceLevel assessSystemPerformance() const;
    std::vector<int> getBottleneckChannels() const;
    std::vector<std::string> getPerformanceIssues(int channelIndex) const;
    
    // Metrics retrieval
    SystemMetrics getSystemMetrics() const;
    ChannelPerformanceMetrics getChannelMetrics(int channelIndex) const;
    std::vector<ChannelPerformanceMetrics> getAllChannelMetrics() const;
    std::vector<SystemMetrics> getMetricsHistory() const;
    
    // Configuration
    void setPerformanceThresholds(const PerformanceThresholds& thresholds);
    PerformanceThresholds getPerformanceThresholds() const;
    void setMonitorInterval(int intervalMs);
    void setAutoOptimization(bool enabled);
    void setDetailedLogging(bool enabled);
    
    // Event handling
    void setEventListener(PerformanceEventListener* listener);
    
    // Optimization
    void scheduleOptimization(const OptimizationAction& action);
    void applyOptimization(int channelIndex, const std::string& actionType);
    std::vector<OptimizationAction> generateOptimizationRecommendations() const;
    
    // Reporting
    std::string generatePerformanceReport() const;
    std::string generateChannelReport(int channelIndex) const;
    std::string generateOptimizationReport() const;
    void exportPerformanceData(const std::string& filename) const;

private:
    // Internal monitoring
    void monitoringLoop();
    void optimizationLoop();
    void collectSystemMetrics();
    void collectChannelMetrics();
    void analyzePerformance();
    void detectPerformanceIssues();
    
    // System resource collection
    float collectCpuUsage();
    long collectMemoryUsage();
    float collectGpuUsage();
    float collectNetworkBandwidth();
    float collectDiskIO();
    
    // Performance analysis
    void updateChannelPerformanceLevel(int channelIndex);
    void updateSystemPerformanceLevel();
    void identifyBottlenecks();
    void generateOptimizationActions();
    
    // Optimization execution
    void executeOptimizationAction(const OptimizationAction& action);
    void optimizeChannelFrameRate(int channelIndex);
    void optimizeChannelDetection(int channelIndex);
    void optimizeChannelRendering(int channelIndex);
    void optimizeSystemResources();
    
    // Utility methods
    ChannelPerformanceMetrics* getChannelMetricsInternal(int channelIndex);
    const ChannelPerformanceMetrics* getChannelMetricsInternal(int channelIndex) const;
    std::string performanceLevelToString(PerformanceLevel level) const;
    std::string resourceTypeToString(ResourceType type) const;
    void addMetricsToHistory(const SystemMetrics& metrics);
    void logPerformanceData(const std::string& data);
    bool validateChannelIndex(int channelIndex) const;
    
    // Event notifications
    void notifyPerformanceLevelChanged(int channelIndex, PerformanceLevel oldLevel, PerformanceLevel newLevel);
    void notifySystemPerformanceAlert(PerformanceLevel level, const std::string& message);
    void notifyOptimizationApplied(const OptimizationAction& action);
    void notifyResourceThresholdExceeded(ResourceType resource, float value, float threshold);
    void notifyPerformanceReport(const SystemMetrics& metrics);
};

/**
 * Performance Analytics Engine
 * Advanced analytics for performance data analysis and prediction
 */
class PerformanceAnalyticsEngine {
public:
    struct PerformanceTrend {
        SystemPerformanceMonitor::ResourceType resource;
        float currentValue;
        float trendSlope;
        float prediction;
        int confidenceLevel;
        std::string trendDescription;

        PerformanceTrend(SystemPerformanceMonitor::ResourceType res) : resource(res), currentValue(0.0f),
                                           trendSlope(0.0f), prediction(0.0f),
                                           confidenceLevel(0) {}
    };

    struct PerformanceInsight {
        std::string category;
        std::string insight;
        int severity;
        std::vector<std::string> recommendations;
        
        PerformanceInsight(const std::string& cat, const std::string& ins, int sev)
            : category(cat), insight(ins), severity(sev) {}
    };

private:
    SystemPerformanceMonitor* performanceMonitor;
    std::vector<SystemPerformanceMonitor::SystemMetrics> historicalData;
    std::mutex analyticsMutex;

public:
    PerformanceAnalyticsEngine(SystemPerformanceMonitor* monitor);
    ~PerformanceAnalyticsEngine();
    
    // Analytics
    std::vector<PerformanceTrend> analyzePerformanceTrends() const;
    std::vector<PerformanceInsight> generatePerformanceInsights() const;
    float predictSystemLoad(int minutesAhead) const;
    std::vector<int> predictBottleneckChannels() const;
    
    // Reporting
    std::string generateAnalyticsReport() const;
    std::string generatePredictionReport() const;

private:
    void updateHistoricalData();
    float calculateTrendSlope(const std::vector<float>& values) const;
    int calculateConfidenceLevel(const std::vector<float>& values) const;
};

#endif // AIBOX_SYSTEM_PERFORMANCE_MONITOR_H
