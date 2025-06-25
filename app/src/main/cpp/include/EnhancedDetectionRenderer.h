#ifndef ENHANCED_DETECTION_RENDERER_H
#define ENHANCED_DETECTION_RENDERER_H

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
#include "yolo_datatype.h"
#include "cv_draw.h"

/**
 * Enhanced Detection Renderer for Multi-Channel NVR Systems
 * Optimizes detection visualization for small viewports and multi-channel environments
 */
class EnhancedDetectionRenderer {
public:
    enum RenderingMode {
        FULL_DETAIL = 0,        // Show all detection details
        ADAPTIVE = 1,           // Adapt based on viewport size and system load
        MINIMAL = 2,            // Show only essential information
        PERFORMANCE_FIRST = 3   // Prioritize performance over visual quality
    };

    struct ChannelRenderState {
        bool isActive;
        bool isVisible;
        int viewportWidth;
        int viewportHeight;
        float lastRenderTime;
        int detectionCount;
        RenderingMode mode;
        ViewportRenderConfig config;
        std::chrono::steady_clock::time_point lastUpdate;
        
        ChannelRenderState() : isActive(false), isVisible(true), 
                              viewportWidth(0), viewportHeight(0),
                              lastRenderTime(0.0f), detectionCount(0),
                              mode(ADAPTIVE), lastUpdate(std::chrono::steady_clock::now()) {}
    };

    struct SystemRenderMetrics {
        float totalRenderLoad;
        float averageRenderTime;
        int activeChannels;
        int totalDetections;
        std::chrono::steady_clock::time_point lastUpdate;
        
        SystemRenderMetrics() : totalRenderLoad(0.0f), averageRenderTime(0.0f),
                               activeChannels(0), totalDetections(0),
                               lastUpdate(std::chrono::steady_clock::now()) {}
    };

private:
    std::unordered_map<int, std::unique_ptr<ChannelRenderState>> channelStates;
    SystemRenderMetrics systemMetrics;
    mutable std::mutex statesMutex;
    mutable std::mutex metricsMutex;
    
    // Configuration
    std::atomic<bool> adaptiveRenderingEnabled{true};
    std::atomic<bool> performanceOptimizationEnabled{true};
    std::atomic<float> systemLoadThreshold{0.8f};
    std::atomic<int> maxDetectionsPerChannel{50};
    
    // Performance tracking
    std::atomic<float> currentSystemLoad{0.0f};
    std::chrono::steady_clock::time_point lastOptimization;

public:
    EnhancedDetectionRenderer();
    ~EnhancedDetectionRenderer();

    // Channel management
    bool addChannel(int channelIndex, int width, int height);
    bool removeChannel(int channelIndex);
    bool updateChannelViewport(int channelIndex, int width, int height);
    bool setChannelActive(int channelIndex, bool active);
    bool setChannelVisible(int channelIndex, bool visible);
    bool setChannelRenderingMode(int channelIndex, RenderingMode mode);

    // Main rendering function
    bool renderDetections(int channelIndex, uint8_t* frameData, int width, int height, int stride,
                         const std::vector<Detection>& detections);

    // System optimization
    void updateSystemLoad(float load);
    void optimizeRenderingPerformance();
    void setAdaptiveRenderingEnabled(bool enabled);
    void setPerformanceOptimizationEnabled(bool enabled);
    void setSystemLoadThreshold(float threshold);
    void setMaxDetectionsPerChannel(int maxDetections);

    // Metrics and monitoring
    SystemRenderMetrics getSystemMetrics() const;
    ChannelRenderState getChannelState(int channelIndex) const;
    std::vector<int> getActiveChannels() const;
    std::vector<int> getOverloadedChannels() const;

    // Configuration
    void setGlobalRenderingMode(RenderingMode mode);
    void resetChannelConfigurations();

private:
    // Internal helper methods
    ChannelRenderState* getChannelStateInternal(int channelIndex) const;
    void updateChannelMetrics(int channelIndex, float renderTime, int detectionCount);
    void updateSystemMetrics();
    bool shouldOptimizeChannel(int channelIndex) const;
    RenderingMode determineOptimalRenderingMode(int channelIndex) const;
    ViewportRenderConfig createOptimizedConfig(int channelIndex, int width, int height) const;
    std::vector<Detection> filterDetectionsForChannel(int channelIndex, 
                                                     const std::vector<Detection>& detections) const;
    void applyPerformanceOptimizations();
    bool validateChannelIndex(int channelIndex) const;
};

/**
 * Detection Rendering Performance Monitor
 * Monitors and analyzes detection rendering performance across channels
 */
class DetectionRenderingMonitor {
public:
    struct RenderingMetrics {
        float averageRenderTime;
        float peakRenderTime;
        int totalFramesRendered;
        int totalDetectionsRendered;
        float detectionDensity; // detections per frame
        std::chrono::steady_clock::time_point lastUpdate;
        
        RenderingMetrics() : averageRenderTime(0.0f), peakRenderTime(0.0f),
                           totalFramesRendered(0), totalDetectionsRendered(0),
                           detectionDensity(0.0f), lastUpdate(std::chrono::steady_clock::now()) {}
    };

private:
    std::unordered_map<int, RenderingMetrics> channelMetrics;
    mutable std::mutex metricsMutex;
    std::atomic<bool> monitoringEnabled{true};

public:
    DetectionRenderingMonitor();
    ~DetectionRenderingMonitor();

    // Monitoring functions
    void recordRenderingEvent(int channelIndex, float renderTime, int detectionCount);
    void startMonitoring();
    void stopMonitoring();
    void resetMetrics();

    // Analysis functions
    RenderingMetrics getChannelMetrics(int channelIndex) const;
    std::vector<int> identifySlowChannels(float thresholdMs = 16.67f) const; // 60 FPS threshold
    std::vector<int> identifyHighDensityChannels(float thresholdDensity = 10.0f) const;
    float calculateSystemRenderingLoad() const;

    // Optimization recommendations
    std::vector<std::string> generateOptimizationRecommendations() const;
    bool shouldReduceRenderingQuality(int channelIndex) const;
    bool shouldSkipFrameRendering(int channelIndex) const;
};

#endif // ENHANCED_DETECTION_RENDERER_H
