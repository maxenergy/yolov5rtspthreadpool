#ifndef AIBOX_CHANNEL_MANAGER_H
#define AIBOX_CHANNEL_MANAGER_H

#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <android/native_window.h>
#include "ZLPlayer.h"
#include "user_comm.h"
#include "log4c.h"

#define MAX_CHANNELS 16
#define SHARED_THREAD_POOL_SIZE 20
#define PERFORMANCE_UPDATE_INTERVAL_MS 1000

// Forward declarations
class MultiChannelZLPlayer;

// RAII wrapper for rknn_app_context_t to ensure proper cleanup
class ChannelContextRAII {
private:
    rknn_app_context_t* context;

public:
    ChannelContextRAII() : context(nullptr) {}

    ~ChannelContextRAII() {
        cleanup();
    }

    // Non-copyable
    ChannelContextRAII(const ChannelContextRAII&) = delete;
    ChannelContextRAII& operator=(const ChannelContextRAII&) = delete;

    // Movable
    ChannelContextRAII(ChannelContextRAII&& other) noexcept : context(other.context) {
        other.context = nullptr;
    }

    ChannelContextRAII& operator=(ChannelContextRAII&& other) noexcept {
        if (this != &other) {
            cleanup();
            context = other.context;
            other.context = nullptr;
        }
        return *this;
    }

    bool initialize() {
        cleanup(); // Clean up any existing context

        context = new rknn_app_context_t();
        memset(context, 0, sizeof(rknn_app_context_t));
        return true;
    }

    void cleanup() {
        if (context) {
            if (context->yolov5ThreadPool) {
                context->yolov5ThreadPool->stopAll();
                delete context->yolov5ThreadPool;
                context->yolov5ThreadPool = nullptr;
            }

            if (context->renderFrameQueue) {
                delete context->renderFrameQueue;
                context->renderFrameQueue = nullptr;
            }

            if (context->decoder) {
                delete context->decoder;
                context->decoder = nullptr;
            }

            delete context;
            context = nullptr;
        }
    }

    rknn_app_context_t* get() { return context; }
    const rknn_app_context_t* get() const { return context; }

    rknn_app_context_t* operator->() { return context; }
    const rknn_app_context_t* operator->() const { return context; }

    explicit operator bool() const { return context != nullptr; }
};

/**
 * Native Multi-Channel Manager for coordinating multiple ZLPlayer instances
 * Manages shared resources and provides thread-safe channel operations
 */
class NativeChannelManager {
public:
    enum ChannelState {
        INACTIVE = 0,
        CONNECTING = 1,
        ACTIVE = 2,
        ERROR = 3,
        RECONNECTING = 4
    };
    
    struct ChannelInfo {
        int channelIndex;
        std::unique_ptr<MultiChannelZLPlayer> player;
        ANativeWindow* surface;
        std::string rtspUrl;
        ChannelState state;
        bool detectionEnabled;
        std::atomic<int> frameCount;
        std::atomic<int> detectionCount;
        std::atomic<int> renderCount;
        std::atomic<int> droppedFrameCount;
        std::chrono::steady_clock::time_point lastFrameTime;
        std::chrono::steady_clock::time_point lastRenderTime;
        float fps;
        float renderFps;
        std::string errorMessage;
        int retryCount;

        // Frame rate control
        std::chrono::microseconds frameInterval;
        int frameSkipCounter;
        
        ChannelInfo(int index) :
            channelIndex(index),
            surface(nullptr),
            state(INACTIVE),
            detectionEnabled(true),
            frameCount(0),
            detectionCount(0),
            renderCount(0),
            droppedFrameCount(0),
            fps(0.0f),
            renderFps(0.0f),
            retryCount(0),
            frameInterval(std::chrono::microseconds(33333)), // ~30 FPS
            frameSkipCounter(0) {}
    };
    
    struct SharedResources {
        char* modelData;
        int modelSize;
        std::shared_ptr<Yolov5ThreadPool> sharedThreadPool;
        std::mutex resourceMutex;
        
        SharedResources() : modelData(nullptr), modelSize(0) {}
        
        ~SharedResources() {
            if (modelData) {
                delete[] modelData;
                modelData = nullptr;
            }
        }
    };
    
    // Performance monitoring
    struct PerformanceMetrics {
        std::atomic<int> totalFrameCount;
        std::atomic<int> totalRenderCount;
        std::atomic<int> totalDetectionCount;
        std::atomic<int> activeChannelCount;
        float systemFps;
        float targetFps;
        std::chrono::steady_clock::time_point lastUpdate;
        std::chrono::steady_clock::time_point lastFrameTime;

        // Performance thresholds
        static constexpr float TARGET_FPS = 30.0f;
        static constexpr float MIN_FPS_THRESHOLD = 25.0f;
        static constexpr int MAX_FRAME_SKIP = 2;

        PerformanceMetrics() :
            totalFrameCount(0),
            totalRenderCount(0),
            totalDetectionCount(0),
            activeChannelCount(0),
            systemFps(0.0f),
            targetFps(TARGET_FPS),
            lastUpdate(std::chrono::steady_clock::now()),
            lastFrameTime(std::chrono::steady_clock::now()) {}
    };

private:
    std::map<int, std::unique_ptr<ChannelInfo>> channels;
    SharedResources sharedResources;
    PerformanceMetrics performanceMetrics;
    
    // Thread safety
    std::mutex channelsMutex;
    std::mutex performanceMutex;
    
    // Performance monitoring thread
    std::thread performanceThread;
    std::atomic<bool> shouldStop;
    std::condition_variable performanceCv;
    
    // JNI callback references
    JavaVM* jvm;
    jobject javaChannelManager;
    jmethodID onFrameReceivedMethod;
    jmethodID onDetectionReceivedMethod;
    jmethodID onChannelStateChangedMethod;
    jmethodID onChannelErrorMethod;

public:
    NativeChannelManager();
    ~NativeChannelManager();
    
    // Initialization
    bool initialize(char* modelData, int modelSize);
    void setJavaCallbacks(JNIEnv* env, jobject javaObject);
    
    // Channel management
    bool createChannel(int channelIndex);
    bool destroyChannel(int channelIndex);
    bool startChannel(int channelIndex, const char* rtspUrl);
    bool stopChannel(int channelIndex);
    
    // Channel configuration
    bool setChannelSurface(int channelIndex, ANativeWindow* surface);
    bool setChannelRTSPUrl(int channelIndex, const char* rtspUrl);
    bool setChannelDetectionEnabled(int channelIndex, bool enabled);
    
    // Channel state
    ChannelState getChannelState(int channelIndex);
    float getChannelFps(int channelIndex);
    int getChannelFrameCount(int channelIndex);
    int getChannelDetectionCount(int channelIndex);
    std::string getChannelError(int channelIndex);
    
    // System status
    int getActiveChannelCount();
    float getSystemFps();
    
    // Callbacks from ZLPlayer instances
    void onChannelFrameReceived(int channelIndex);
    void onChannelDetectionReceived(int channelIndex, int detectionCount);
    void onChannelFrameRendered(int channelIndex);
    void onChannelError(int channelIndex, const std::string& errorMessage);
    void onChannelStateChanged(int channelIndex, ChannelState newState);
    
    // Resource management
    void cleanup();

private:
    // Internal methods
    void updateChannelState(int channelIndex, ChannelState newState);
    void performanceMonitorLoop();
    void updatePerformanceMetrics();
    void applyGlobalPerformanceOptimizations();
    void optimizeChannelPerformance(int channelIndex);
    
    // JNI callback helpers
    void notifyJavaFrameReceived(int channelIndex);
    void notifyJavaDetectionReceived(int channelIndex, int detectionCount);
    void notifyJavaChannelStateChanged(int channelIndex, ChannelState newState);
    void notifyJavaChannelError(int channelIndex, const std::string& errorMessage);
    
    // Shared resource management
    bool initializeSharedResources(char* modelData, int modelSize);
    void cleanupSharedResources();
    
    // Channel validation
    bool isValidChannelIndex(int channelIndex);
    ChannelInfo* getChannelInfo(int channelIndex);
};

// Global instance for JNI access
extern std::unique_ptr<NativeChannelManager> g_channelManager;

// Custom ZLPlayer wrapper for multi-channel support
class MultiChannelZLPlayer : public ZLPlayer {
protected:
    int channelIndex;
    NativeChannelManager* channelManager;
    ChannelContextRAII channelContext;   // RAII-managed context for this channel
    ANativeWindow* channelSurface;       // Independent surface for this channel
    mutable std::mutex channelMutex;    // Mutex for thread safety (use std::mutex for consistency)
    std::atomic<bool> detectionEnabled; // Use atomic for thread-safe access
    std::string channelRtspUrl;
    std::unique_ptr<char[]> modelData;   // Use smart pointer for automatic memory management
    int modelDataSize;                   // Model data size

    // Frame rate control
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::steady_clock::time_point lastRenderTime;
    std::atomic<int> frameSkipCounter;
    std::atomic<float> currentFps;
    
public:
    MultiChannelZLPlayer(int channelIndex, char* modelFileData, int modelDataLen, 
                        NativeChannelManager* manager);
    ~MultiChannelZLPlayer();
    
    // Channel-specific callback methods
    void onFrameProcessed();
    void onDetectionCompleted(int detectionCount);
    void onError(const std::string& errorMessage);
    
    // Channel-specific configuration
    void setChannelRTSPUrl(const char* url);
    void setChannelSurface(ANativeWindow* surface);
    void setDetectionEnabled(bool enabled);

    // Override parent methods for channel-specific behavior
    void display();
    void get_detect_result();

    // Channel-specific frame callback
    void onChannelFrameCallback(void* userdata, int width_stride, int height_stride,
                               int width, int height, int format, int fd, void* data);

    // Channel lifecycle management
    bool initializeChannel();
    void cleanupChannel();
    bool startRTSPStream();
    void stopRTSPStream();

    // Channel state queries
    bool isChannelActive() const;
    int getChannelIndex() const { return channelIndex; }
    const std::string& getRTSPUrl() const { return channelRtspUrl; }

    // Channel-specific rendering
    void renderToChannelSurface(frame_data_t* frameData);

    // Frame rate control methods
    bool shouldProcessFrame();
    bool shouldRenderFrame();
    void updateFrameRateStats();
    void adaptiveFrameSkipping();
};

// Thread-safe frame callback wrapper
extern "C" {
    void multi_channel_frame_callback(void* userdata, int width_stride, int height_stride, 
                                    int width, int height, int format, int fd, void* data);
}

#endif // AIBOX_CHANNEL_MANAGER_H
