#ifndef AIBOX_MULTI_STREAM_DETECTION_INTEGRATION_H
#define AIBOX_MULTI_STREAM_DETECTION_INTEGRATION_H

#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

#include "PerChannelDetection.h"
#include "MultiStreamIntegration.h"
#include "RTSPStreamManager.h"
#include "MultiSurfaceRenderer.h"
#include "log4c.h"

/**
 * Multi-Stream Detection Integration
 * Integrates Per-Channel Detection with the Multi-Stream Processing system
 */
class MultiStreamDetectionIntegration : public PerChannelDetection::DetectionEventListener {
public:
    struct DetectionChannelConfig {
        int channelIndex;
        bool detectionEnabled;
        bool visualizationEnabled;
        float confidenceThreshold;
        int maxDetections;
        std::vector<int> enabledClasses;
        bool enableNMS;
        float nmsThreshold;
        
        DetectionChannelConfig() : channelIndex(-1), detectionEnabled(true),
                                  visualizationEnabled(true), confidenceThreshold(0.5f),
                                  maxDetections(100), enableNMS(true), nmsThreshold(0.4f) {}

        DetectionChannelConfig(int index) : channelIndex(index), detectionEnabled(true),
                                          visualizationEnabled(true), confidenceThreshold(0.5f),
                                          maxDetections(100), enableNMS(true), nmsThreshold(0.4f) {}
    };

    struct DetectionSystemStats {
        int totalChannels;
        int activeDetectionChannels;
        int totalFramesProcessed;
        int totalDetections;
        float averageDetectionsPerFrame;
        float systemDetectionFps;
        std::map<int, PerChannelDetection::DetectionStats> channelStats;
        
        DetectionSystemStats() : totalChannels(0), activeDetectionChannels(0),
                               totalFramesProcessed(0), totalDetections(0),
                               averageDetectionsPerFrame(0.0f), systemDetectionFps(0.0f) {}
    };

    // Event callbacks
    using DetectionCallback = std::function<void(int channelIndex, const std::vector<Detection>&)>;
    using ErrorCallback = std::function<void(int channelIndex, const std::string&)>;
    using StatsCallback = std::function<void(const DetectionSystemStats&)>;

private:
    std::unique_ptr<PerChannelDetection> perChannelDetection;
    std::unique_ptr<DetectionResultManager> resultManager;
    
    // Channel configurations
    std::map<int, DetectionChannelConfig> channelConfigs;
    mutable std::mutex configMutex;

    // Integration with multi-stream system
    MultiStreamIntegration* multiStreamSystem;

    // Statistics and monitoring
    DetectionSystemStats systemStats;
    mutable std::mutex statsMutex;
    std::thread statsUpdateThread;
    std::atomic<bool> statsThreadRunning;

    // Event callbacks
    DetectionCallback detectionCallback;
    ErrorCallback errorCallback;
    StatsCallback statsCallback;

    // Frame processing
    std::map<int, std::atomic<int>> channelFrameCounters;
    mutable std::mutex frameCounterMutex;

public:
    MultiStreamDetectionIntegration();
    ~MultiStreamDetectionIntegration();
    
    // Initialization
    bool initialize(char* modelData, int modelSize, MultiStreamIntegration* multiStreamSystem = nullptr);
    void cleanup();
    
    // Channel management
    bool addDetectionChannel(int channelIndex, const DetectionChannelConfig& config = DetectionChannelConfig(0));
    bool removeDetectionChannel(int channelIndex);
    bool isDetectionChannelActive(int channelIndex) const;
    
    // Detection control
    bool startChannelDetection(int channelIndex);
    bool stopChannelDetection(int channelIndex);
    bool pauseChannelDetection(int channelIndex);
    bool resumeChannelDetection(int channelIndex);
    
    // Global detection control
    void enableGlobalDetection(bool enabled);
    bool isGlobalDetectionEnabled() const;
    void startAllDetection();
    void stopAllDetection();
    
    // Configuration
    void setChannelConfig(int channelIndex, const DetectionChannelConfig& config);
    DetectionChannelConfig getChannelConfig(int channelIndex) const;
    void setGlobalConfidenceThreshold(float threshold);
    void setGlobalMaxDetections(int maxDetections);
    
    // Frame processing integration
    bool processFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData);
    bool getChannelDetections(int channelIndex, std::vector<Detection>& detections);
    bool getChannelDetectionsNonBlocking(int channelIndex, std::vector<Detection>& detections);
    
    // Statistics and monitoring
    DetectionSystemStats getSystemStats() const;
    PerChannelDetection::DetectionStats getChannelStats(int channelIndex) const;
    std::vector<int> getActiveDetectionChannels() const;
    
    // Event callbacks
    void setDetectionCallback(DetectionCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void setStatsCallback(StatsCallback callback);
    
    // Multi-stream integration
    void setMultiStreamSystem(MultiStreamIntegration* system);
    bool integrateWithMultiStream();
    void disconnectFromMultiStream();
    
    // Visualization integration
    bool enableVisualization(int channelIndex, bool enabled);
    bool isVisualizationEnabled(int channelIndex) const;
    void setVisualizationStyle(int channelIndex, const std::string& style);
    
    // Performance optimization
    void optimizeForPerformance();
    void setDetectionFrameSkip(int channelIndex, int skipFrames);
    void enableAdaptiveDetection(bool enabled);

protected:
    // PerChannelDetection::DetectionEventListener implementation
    void onDetectionCompleted(int channelIndex, const PerChannelDetection::DetectionResult& result) override;
    void onDetectionError(int channelIndex, const std::string& error) override;
    void onQueueOverflow(int channelIndex, int droppedFrames) override;
    void onStateChanged(int channelIndex, PerChannelDetection::DetectionState oldState, 
                       PerChannelDetection::DetectionState newState) override;

private:
    // Internal processing
    void updateSystemStatistics();
    void statisticsUpdateLoop();
    void processDetectionResult(int channelIndex, const PerChannelDetection::DetectionResult& result);
    
    // Configuration management
    DetectionChannelConfig* getChannelConfigInternal(int channelIndex);
    void applyChannelConfig(int channelIndex, const DetectionChannelConfig& config);
    
    // Multi-stream integration helpers
    void setupMultiStreamCallbacks();
    void onMultiStreamFrameReceived(int channelIndex, std::shared_ptr<frame_data_t> frameData);
    void onMultiStreamChannelStateChanged(int channelIndex, const std::string& state);
    
    // Utility methods
    bool validateChannelIndex(int channelIndex) const;
    void notifyDetectionCallback(int channelIndex, const std::vector<Detection>& detections);
    void notifyErrorCallback(int channelIndex, const std::string& error);
    void notifyStatsCallback();
};

/**
 * Detection Visualization Manager
 * Handles visualization of detection results on video frames
 */
class DetectionVisualizationManager {
public:
    enum VisualizationStyle {
        SIMPLE_BOXES = 0,
        DETAILED_BOXES = 1,
        CONFIDENCE_BASED = 2,
        CLASS_COLORED = 3,
        MINIMAL = 4
    };

    struct VisualizationConfig {
        VisualizationStyle style;
        bool showConfidence;
        bool showClassNames;
        bool showBoundingBoxes;
        float boxThickness;
        float textScale;
        std::map<int, std::string> classColors;
        
        VisualizationConfig() : style(DETAILED_BOXES), showConfidence(true),
                              showClassNames(true), showBoundingBoxes(true),
                              boxThickness(2.0f), textScale(0.5f) {}
    };

private:
    std::map<int, VisualizationConfig> channelConfigs;
    mutable std::mutex configMutex;

public:
    DetectionVisualizationManager();
    ~DetectionVisualizationManager();
    
    // Configuration
    void setChannelVisualizationConfig(int channelIndex, const VisualizationConfig& config);
    VisualizationConfig getChannelVisualizationConfig(int channelIndex) const;
    
    // Visualization
    bool visualizeDetections(int channelIndex, std::shared_ptr<frame_data_t> frameData,
                           const std::vector<Detection>& detections);
    bool drawDetectionsOnFrame(uint8_t* frameData, int width, int height, int stride,
                             const std::vector<Detection>& detections,
                             const VisualizationConfig& config);
    
    // Style management
    void setGlobalVisualizationStyle(VisualizationStyle style);
    void enableGlobalConfidenceDisplay(bool enabled);
    void enableGlobalClassNameDisplay(bool enabled);
    
    // Color management
    void setClassColor(int classId, const std::string& color);
    std::string getClassColor(int classId) const;
    void loadDefaultClassColors();

private:
    // Internal visualization methods
    void drawBoundingBox(uint8_t* frameData, int width, int height, int stride,
                        const Detection& detection, const VisualizationConfig& config);
    void drawConfidenceText(uint8_t* frameData, int width, int height, int stride,
                          const Detection& detection, const VisualizationConfig& config);
    void drawClassName(uint8_t* frameData, int width, int height, int stride,
                      const Detection& detection, const VisualizationConfig& config);
    
    // Utility methods
    uint32_t parseColor(const std::string& colorStr) const;
    std::string getDefaultClassColor(int classId) const;
};

/**
 * Detection Performance Monitor
 * Monitors and optimizes detection performance across channels
 */
class DetectionPerformanceMonitor {
public:
    struct PerformanceMetrics {
        float averageDetectionTime;
        float peakDetectionTime;
        float detectionFps;
        int queueUtilization;
        float cpuUsage;
        long memoryUsage;
        
        PerformanceMetrics() : averageDetectionTime(0.0f), peakDetectionTime(0.0f),
                             detectionFps(0.0f), queueUtilization(0), cpuUsage(0.0f),
                             memoryUsage(0) {}
    };

private:
    std::map<int, PerformanceMetrics> channelMetrics;
    mutable std::mutex metricsMutex;
    std::thread monitorThread;
    std::atomic<bool> monitorRunning;

public:
    DetectionPerformanceMonitor();
    ~DetectionPerformanceMonitor();
    
    // Monitoring control
    void startMonitoring();
    void stopMonitoring();
    
    // Metrics collection
    void updateChannelMetrics(int channelIndex, const PerformanceMetrics& metrics);
    PerformanceMetrics getChannelMetrics(int channelIndex) const;
    std::map<int, PerformanceMetrics> getAllChannelMetrics() const;
    
    // Performance optimization
    std::vector<int> identifyBottleneckChannels() const;
    std::vector<std::string> generateOptimizationRecommendations() const;
    bool shouldThrottleChannel(int channelIndex) const;

private:
    void monitoringLoop();
    void collectSystemMetrics();
    void analyzePerformance();
};

#endif // AIBOX_MULTI_STREAM_DETECTION_INTEGRATION_H
