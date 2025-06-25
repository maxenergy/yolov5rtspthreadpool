#ifndef AIBOX_MULTI_CHANNEL_FRAME_COMPOSITOR_H
#define AIBOX_MULTI_CHANNEL_FRAME_COMPOSITOR_H

#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <queue>
#include <functional>

#include "log4c.h"
#include "user_comm.h"
#include "display_queue.h"

/**
 * Multi-Channel Frame Compositor
 * Efficiently combines multiple channel frames into optimized display buffers
 */
class MultiChannelFrameCompositor {
public:
    enum CompositionMode {
        INDIVIDUAL_SURFACES = 0,  // Each channel renders to its own surface
        UNIFIED_COMPOSITION = 1,  // All channels composed into single buffer
        HYBRID_COMPOSITION = 2    // Mix of individual and unified based on layout
    };

    enum LayoutMode {
        SINGLE = 1,
        QUAD = 4,
        NINE = 9,
        SIXTEEN = 16
    };

    struct ChannelViewport {
        int channelIndex;
        int x, y;           // Position in composite buffer
        int width, height;  // Size in composite buffer
        float scaleX, scaleY; // Scaling factors
        bool visible;
        bool needsUpdate;
        
        ChannelViewport(int index = -1) : channelIndex(index), x(0), y(0), 
                                        width(0), height(0), scaleX(1.0f), scaleY(1.0f),
                                        visible(true), needsUpdate(true) {}
    };

    struct CompositeFrame {
        std::shared_ptr<uint8_t> data;
        int width, height;
        int stride;
        int format;
        std::chrono::steady_clock::time_point timestamp;
        std::vector<int> includedChannels;
        
        CompositeFrame() : width(0), height(0), stride(0), format(0) {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    struct CompositionConfig {
        CompositionMode mode;
        LayoutMode layout;
        int outputWidth, outputHeight;
        int outputFormat;
        bool enableBlending;
        bool enableScaling;
        bool enableCropping;
        float backgroundAlpha;
        uint32_t backgroundColor;
        
        CompositionConfig() : mode(INDIVIDUAL_SURFACES), layout(QUAD),
                            outputWidth(1920), outputHeight(1080),
                            outputFormat(MPP_FMT_RGBA8888),
                            enableBlending(true), enableScaling(true),
                            enableCropping(false), backgroundAlpha(1.0f),
                            backgroundColor(0xFF000000) {}
    };

    // Performance metrics
    struct CompositionMetrics {
        std::atomic<int> framesComposed;
        std::atomic<int> framesDropped;
        std::atomic<float> averageCompositionTime;
        std::atomic<float> compositionFps;
        std::atomic<long> memoryUsage;
        std::chrono::steady_clock::time_point lastUpdate;
        
        CompositionMetrics() : framesComposed(0), framesDropped(0),
                             averageCompositionTime(0.0f), compositionFps(0.0f),
                             memoryUsage(0) {
            lastUpdate = std::chrono::steady_clock::now();
        }

        // Copy constructor for atomic members
        CompositionMetrics(const CompositionMetrics& other)
            : framesComposed(other.framesComposed.load()),
              framesDropped(other.framesDropped.load()),
              averageCompositionTime(other.averageCompositionTime.load()),
              compositionFps(other.compositionFps.load()),
              memoryUsage(other.memoryUsage.load()),
              lastUpdate(other.lastUpdate) {
        }
    };

    // Event listener interface
    class CompositionEventListener {
    public:
        virtual ~CompositionEventListener() = default;
        virtual void onCompositeFrameReady(const CompositeFrame& frame) = 0;
        virtual void onCompositionError(int errorCode, const std::string& message) = 0;
        virtual void onPerformanceUpdate(const CompositionMetrics& metrics) = 0;
    };

private:
    // Configuration
    CompositionConfig config;
    mutable std::mutex configMutex;

    // Channel management
    std::map<int, ChannelViewport> channelViewports;
    std::map<int, std::shared_ptr<frame_data_t>> latestChannelFrames;
    mutable std::mutex channelsMutex;

    // Composition thread
    std::thread compositionThread;
    std::atomic<bool> compositionRunning;
    std::condition_variable compositionCv;
    mutable std::mutex compositionMutex;

    // Frame queues
    std::queue<std::pair<int, std::shared_ptr<frame_data_t>>> inputQueue;
    std::queue<CompositeFrame> outputQueue;
    mutable std::mutex inputQueueMutex;
    mutable std::mutex outputQueueMutex;

    // Output buffer management
    std::vector<std::shared_ptr<uint8_t>> bufferPool;
    mutable std::mutex bufferPoolMutex;
    static constexpr int BUFFER_POOL_SIZE = 8;
    
    // Performance monitoring
    CompositionMetrics metrics;
    std::chrono::steady_clock::time_point lastMetricsUpdate;
    
    // Event handling
    CompositionEventListener* eventListener;
    
    // GPU acceleration support
    bool gpuAccelerationEnabled;
    void* gpuContext; // Platform-specific GPU context

public:
    MultiChannelFrameCompositor();
    ~MultiChannelFrameCompositor();
    
    // Initialization
    bool initialize(const CompositionConfig& config);
    void cleanup();
    
    // Configuration
    void setCompositionConfig(const CompositionConfig& config);
    CompositionConfig getCompositionConfig() const;
    void setLayoutMode(LayoutMode layout);
    void setCompositionMode(CompositionMode mode);
    
    // Channel management
    bool addChannel(int channelIndex, const ChannelViewport& viewport);
    bool removeChannel(int channelIndex);
    bool updateChannelViewport(int channelIndex, const ChannelViewport& viewport);
    void setChannelVisible(int channelIndex, bool visible);
    
    // Frame processing
    bool submitChannelFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData);
    CompositeFrame getCompositeFrame();
    bool hasCompositeFrame() const;
    
    // Composition control
    void startComposition();
    void stopComposition();
    void pauseComposition();
    void resumeComposition();
    
    // Performance optimization
    void enableGpuAcceleration(bool enabled);
    void setBufferPoolSize(int size);
    void optimizeForLayout(LayoutMode layout);
    
    // Metrics and monitoring
    CompositionMetrics getMetrics() const;
    void resetMetrics();
    std::string generatePerformanceReport() const;
    
    // Event handling
    void setEventListener(CompositionEventListener* listener);

private:
    // Core composition methods
    void compositionLoop();
    bool composeFrame();
    bool composeIndividualSurfaces();
    bool composeUnifiedFrame();
    bool composeHybridFrame();
    
    // Frame processing
    bool processChannelFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData);
    bool scaleFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport);
    bool blendFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport, float alpha);
    bool cropFrame(const frame_data_t* src, uint8_t* dst, int cropX, int cropY, int cropW, int cropH);
    
    // Buffer management
    std::shared_ptr<uint8_t> acquireBuffer();
    void releaseBuffer(std::shared_ptr<uint8_t> buffer);
    void initializeBufferPool();
    void cleanupBufferPool();
    
    // Layout calculation
    void calculateViewportsForLayout(LayoutMode layout);
    ChannelViewport calculateChannelViewport(int channelIndex, LayoutMode layout);
    bool validateViewport(const ChannelViewport& viewport) const;
    
    // Performance optimization
    void updateMetrics();
    void optimizeCompositionPerformance();
    bool shouldSkipFrame(int channelIndex) const;
    void adaptiveQualityControl();
    
    // GPU acceleration (platform-specific)
    bool initializeGpuAcceleration();
    void cleanupGpuAcceleration();
    bool gpuScaleFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport);
    bool gpuBlendFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport, float alpha);
    
    // Utility methods
    int calculateBufferSize(int width, int height, int format) const;
    void clearBuffer(uint8_t* buffer, int size, uint32_t color);
    bool copyFrameData(const frame_data_t* src, uint8_t* dst, int dstStride);
    
    // Error handling
    void handleCompositionError(int errorCode, const std::string& message);
    void notifyPerformanceUpdate();
    void notifyCompositeFrameReady(const CompositeFrame& frame);
};

/**
 * Frame Composition Utilities
 * Helper functions for frame composition operations
 */
class FrameCompositionUtils {
public:
    // Color space conversion
    static bool convertYUVtoRGBA(const uint8_t* yuv, uint8_t* rgba, int width, int height);
    static bool convertRGBAtoYUV(const uint8_t* rgba, uint8_t* yuv, int width, int height);
    
    // Scaling algorithms
    static bool bilinearScale(const uint8_t* src, uint8_t* dst, 
                             int srcW, int srcH, int dstW, int dstH, int channels);
    static bool bicubicScale(const uint8_t* src, uint8_t* dst,
                            int srcW, int srcH, int dstW, int dstH, int channels);
    
    // Blending operations
    static bool alphaBlend(const uint8_t* src, uint8_t* dst, int width, int height, float alpha);
    static bool additiveBlend(const uint8_t* src, uint8_t* dst, int width, int height);
    static bool multiplyBlend(const uint8_t* src, uint8_t* dst, int width, int height);
    
    // Image processing
    static bool applyGaussianBlur(uint8_t* data, int width, int height, int channels, float sigma);
    static bool adjustBrightness(uint8_t* data, int width, int height, int channels, float brightness);
    static bool adjustContrast(uint8_t* data, int width, int height, int channels, float contrast);
    
    // Performance utilities
    static void prefetchMemory(const void* addr, size_t len);
    static bool isMemoryAligned(const void* ptr, size_t alignment);
    static void* alignedAlloc(size_t size, size_t alignment);
    static void alignedFree(void* ptr);
};

/**
 * Composition Performance Analyzer
 * Analyzes and optimizes composition performance
 */
class CompositionPerformanceAnalyzer {
public:
    struct PerformanceProfile {
        float averageFrameTime;
        float peakFrameTime;
        float memoryBandwidth;
        float cpuUtilization;
        float gpuUtilization;
        int bottleneckType; // 0=CPU, 1=GPU, 2=Memory, 3=I/O
        
        PerformanceProfile() : averageFrameTime(0), peakFrameTime(0),
                             memoryBandwidth(0), cpuUtilization(0),
                             gpuUtilization(0), bottleneckType(0) {}
    };

private:
    std::vector<float> frameTimes;
    mutable std::mutex profileMutex;
    
public:
    void recordFrameTime(float frameTime);
    PerformanceProfile analyzePerformance() const;
    std::vector<std::string> generateOptimizationRecommendations(const PerformanceProfile& profile) const;
    void resetProfile();
};

#endif // AIBOX_MULTI_CHANNEL_FRAME_COMPOSITOR_H
