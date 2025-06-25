#ifndef AIBOX_MULTI_STREAM_INTEGRATION_H
#define AIBOX_MULTI_STREAM_INTEGRATION_H

#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

#include "RTSPStreamManager.h"
#include "MultiStreamProcessor.h"
#include "DecoderManager.h"
#include "MultiSurfaceRenderer.h"
#include "StreamHealthMonitor.h"
#include "ResourceManager.h"
#include "log4c.h"

/**
 * Multi-Stream Integration Manager
 * Coordinates all multi-stream processing components for comprehensive stream management
 */
class MultiStreamIntegration : 
    public RTSPStreamManager::StreamEventListener,
    public DecoderManager::DecoderEventListener,
    public MultiSurfaceRenderer::RenderEventListener,
    public StreamHealthMonitor::HealthEventListener,
    public ResourceManager::ResourceEventListener,
    public MultiStreamProcessor::ProcessingEventListener {

public:
    struct StreamConfiguration {
        int channelIndex;
        std::string rtspUrl;
        std::string channelName;
        bool detectionEnabled;
        bool renderingEnabled;
        int priority;
        float targetFps;
        
        StreamConfiguration(int index, const std::string& url, const std::string& name = "")
            : channelIndex(index), rtspUrl(url), channelName(name),
              detectionEnabled(true), renderingEnabled(true), priority(1), targetFps(30.0f) {}
    };

    struct SystemStatus {
        int totalChannels;
        int activeChannels;
        int healthyChannels;
        int warningChannels;
        int criticalChannels;
        int failedChannels;
        float systemFps;
        float cpuUsage;
        long memoryUsage;
        std::map<int, StreamHealthMonitor::HealthStatus> channelHealth;
        
        SystemStatus() : totalChannels(0), activeChannels(0), healthyChannels(0),
                        warningChannels(0), criticalChannels(0), failedChannels(0),
                        systemFps(0.0f), cpuUsage(0.0f), memoryUsage(0) {}
    };

private:
    // Core components
    std::unique_ptr<RTSPStreamManager> rtspManager;
    std::unique_ptr<MultiStreamProcessor> streamProcessor;
    std::unique_ptr<DecoderManager> decoderManager;
    std::unique_ptr<MultiSurfaceRenderer> surfaceRenderer;
    std::unique_ptr<StreamHealthMonitor> healthMonitor;
    std::unique_ptr<ResourceManager> resourceManager;
    
    // Stream configurations
    std::map<int, StreamConfiguration> streamConfigs;
    std::mutex configMutex;
    
    // System state
    std::atomic<bool> systemActive;
    std::atomic<int> maxChannels;
    
    // Event callbacks
    std::function<void(int, const std::string&)> errorCallback;
    std::function<void(int, StreamHealthMonitor::HealthStatus)> healthCallback;
    std::function<void(SystemStatus)> statusCallback;

public:
    MultiStreamIntegration(int maxChannels = 16);
    ~MultiStreamIntegration();
    
    // System lifecycle
    bool initialize();
    bool start();
    bool stop();
    void cleanup();
    
    // Stream management
    bool addStream(const StreamConfiguration& config);
    bool removeStream(int channelIndex);
    bool updateStreamConfig(int channelIndex, const StreamConfiguration& config);
    
    // Stream control
    bool startStream(int channelIndex);
    bool stopStream(int channelIndex);
    bool startAllStreams();
    bool stopAllStreams();
    
    // Surface management
    bool setSurface(int channelIndex, ANativeWindow* surface);
    bool removeSurface(int channelIndex);
    
    // Configuration
    void setStreamPriority(int channelIndex, int priority);
    void setDetectionEnabled(int channelIndex, bool enabled);
    void setRenderingEnabled(int channelIndex, bool enabled);
    void setTargetFps(int channelIndex, float fps);
    
    // Status and monitoring
    SystemStatus getSystemStatus() const;
    StreamHealthMonitor::HealthData getStreamHealth(int channelIndex) const;
    std::vector<int> getActiveStreams() const;
    std::string generateSystemReport() const;
    
    // Event callbacks
    void setErrorCallback(std::function<void(int, const std::string&)> callback);
    void setHealthCallback(std::function<void(int, StreamHealthMonitor::HealthStatus)> callback);
    void setStatusCallback(std::function<void(SystemStatus)> callback);
    
    // Performance optimization
    void optimizeSystem();
    void rebalanceResources();
    void triggerRecovery(int channelIndex);

private:
    // Component initialization
    bool initializeComponents();
    void setupEventListeners();
    
    // Stream lifecycle management
    bool setupStream(int channelIndex);
    void teardownStream(int channelIndex);
    
    // Event handling coordination
    void handleStreamEvent(int channelIndex, const std::string& event, const std::string& details);
    void updateSystemStatus();
    
    // RTSPStreamManager::StreamEventListener implementation
    void onStreamConnected(int channelIndex) override;
    void onStreamDisconnected(int channelIndex) override;
    void onStreamError(int channelIndex, const std::string& error) override;
    void onFrameReceived(int channelIndex, void* frameData, int size) override;
    void onStreamStateChanged(int channelIndex, RTSPStreamManager::StreamState oldState, 
                             RTSPStreamManager::StreamState newState) override;
    
    // DecoderManager::DecoderEventListener implementation
    void onDecoderReady(int channelIndex) override;
    void onFrameDecoded(int channelIndex, void* frameData, int width, int height) override;
    void onDecoderError(int channelIndex, const std::string& error) override;
    void onDecoderDestroyed(int channelIndex) override;
    
    // MultiSurfaceRenderer::RenderEventListener implementation
    void onSurfaceReady(int channelIndex) override;
    void onFrameRendered(int channelIndex, int width, int height) override;
    void onRenderError(int channelIndex, const std::string& error) override;
    void onSurfaceDestroyed(int channelIndex) override;
    
    // StreamHealthMonitor::HealthEventListener implementation
    void onHealthStatusChanged(int channelIndex, StreamHealthMonitor::HealthStatus oldStatus, 
                              StreamHealthMonitor::HealthStatus newStatus) override;
    void onHealthAlert(int channelIndex, StreamHealthMonitor::HealthMetric metric, 
                      const std::string& message) override;
    void onHealthRecovered(int channelIndex, StreamHealthMonitor::HealthMetric metric) override;
    void onStreamFailure(int channelIndex, const std::string& reason) override;
    void onRecoveryAction(int channelIndex, const std::string& action) override;
    
    // ResourceManager::ResourceEventListener implementation
    void onResourceAllocated(int channelIndex, ResourceManager::ResourceType type, long amount) override;
    void onResourceDeallocated(int channelIndex, ResourceManager::ResourceType type, long amount) override;
    void onResourceExhausted(ResourceManager::ResourceType type, long requested, long available) override;
    void onResourceRebalanced(const std::vector<int>& affectedChannels) override;
    
    // MultiStreamProcessor::ProcessingEventListener implementation
    void onStreamProcessingStarted(int channelIndex) override;
    void onStreamProcessingStopped(int channelIndex) override;
    void onFrameProcessed(int channelIndex, void* frameData, int size) override;
    void onProcessingError(int channelIndex, const std::string& error) override;
    void onLoadBalancingTriggered(const std::vector<int>& affectedChannels) override;
    
    // Utility methods
    StreamConfiguration* getStreamConfig(int channelIndex);
    const StreamConfiguration* getStreamConfig(int channelIndex) const;
    void notifyError(int channelIndex, const std::string& error);
    void notifyHealthChange(int channelIndex, StreamHealthMonitor::HealthStatus status);
    void notifyStatusUpdate();
    
    // Thread safety helpers
    std::unique_lock<std::mutex> lockConfigs() { return std::unique_lock<std::mutex>(configMutex); }
};

/**
 * Multi-Stream Factory for creating pre-configured stream processing systems
 */
class MultiStreamFactory {
public:
    enum SystemProfile {
        BASIC_NVR = 0,      // Basic NVR with 4 channels
        STANDARD_NVR = 1,   // Standard NVR with 9 channels
        PROFESSIONAL_NVR = 2, // Professional NVR with 16 channels
        HIGH_PERFORMANCE = 3  // High-performance system with optimization
    };

    static std::unique_ptr<MultiStreamIntegration> createSystem(SystemProfile profile);
    static MultiStreamIntegration::StreamConfiguration createStreamConfig(int channelIndex, const std::string& rtspUrl,
                                                 const std::string& name = "");
    static std::vector<MultiStreamIntegration::StreamConfiguration> createTestConfigurations(int channelCount);

private:
    static void configureBasicNVR(MultiStreamIntegration* system);
    static void configureStandardNVR(MultiStreamIntegration* system);
    static void configureProfessionalNVR(MultiStreamIntegration* system);
    static void configureHighPerformance(MultiStreamIntegration* system);
};

/**
 * Multi-Stream Diagnostics for system analysis and troubleshooting
 */
class MultiStreamDiagnostics {
public:
    struct DiagnosticReport {
        std::string timestamp;
        MultiStreamIntegration::SystemStatus systemStatus;
        std::map<int, StreamHealthMonitor::HealthData> channelHealth;
        std::map<ResourceManager::ResourceType, float> resourceUtilization;
        std::vector<std::string> recommendations;
        std::vector<std::string> warnings;
        std::vector<std::string> errors;
    };

    static DiagnosticReport generateReport(const MultiStreamIntegration* system);
    static std::vector<std::string> analyzePerformance(const MultiStreamIntegration* system);
    static std::vector<std::string> detectBottlenecks(const MultiStreamIntegration* system);
    static std::string formatReport(const DiagnosticReport& report);

private:
    static void analyzeSystemHealth(const MultiStreamIntegration::SystemStatus& status, 
                                   DiagnosticReport& report);
    static void analyzeResourceUsage(const std::map<ResourceManager::ResourceType, float>& utilization,
                                    DiagnosticReport& report);
    static void generateRecommendations(DiagnosticReport& report);
};

#endif // AIBOX_MULTI_STREAM_INTEGRATION_H
