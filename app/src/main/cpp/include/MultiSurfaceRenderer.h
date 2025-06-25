#ifndef AIBOX_MULTI_SURFACE_RENDERER_H
#define AIBOX_MULTI_SURFACE_RENDERER_H

#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <functional>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include "user_comm.h"
#include "log4c.h"
#include "display_queue.h"

/**
 * Multi-Surface Renderer for handling independent surface rendering
 * Provides isolated rendering for each channel with performance optimization
 */
class MultiSurfaceRenderer {
public:
    enum RenderState {
        INACTIVE = 0,
        INITIALIZING = 1,
        ACTIVE = 2,
        PAUSED = 3,
        ERROR = 4
    };

    struct SurfaceInfo {
        int channelIndex;
        ANativeWindow* surface;
        RenderState state;
        std::atomic<int> frameCount;
        std::atomic<int> renderCount;
        std::atomic<int> droppedFrames;
        std::chrono::steady_clock::time_point lastRenderTime;
        std::chrono::steady_clock::time_point creationTime;
        float targetFps;
        float currentFps;
        int width;
        int height;
        int format;
        std::string lastError;
        
        // Render queue for this surface
        std::unique_ptr<RenderFrameQueue> renderQueue;
        std::mutex surfaceMutex;
        
        SurfaceInfo(int index, ANativeWindow* surf) 
            : channelIndex(index), surface(surf), state(INACTIVE),
              frameCount(0), renderCount(0), droppedFrames(0),
              targetFps(30.0f), currentFps(0.0f), width(0), height(0), format(0) {
            lastRenderTime = std::chrono::steady_clock::now();
            creationTime = std::chrono::steady_clock::now();
            renderQueue = std::make_unique<RenderFrameQueue>();
            
            if (surface) {
                ANativeWindow_acquire(surface);
            }
        }
        
        ~SurfaceInfo() {
            if (surface) {
                ANativeWindow_release(surface);
                surface = nullptr;
            }
        }
    };

    // Callback interface for render events
    class RenderEventListener {
    public:
        virtual ~RenderEventListener() = default;
        virtual void onSurfaceReady(int channelIndex) = 0;
        virtual void onFrameRendered(int channelIndex, int width, int height) = 0;
        virtual void onRenderError(int channelIndex, const std::string& error) = 0;
        virtual void onSurfaceDestroyed(int channelIndex) = 0;
    };

private:
    std::map<int, std::unique_ptr<SurfaceInfo>> surfaces;
    std::mutex surfacesMutex;
    
    // Rendering threads
    std::vector<std::thread> renderThreads;
    std::queue<int> renderQueue;
    std::mutex renderQueueMutex;
    std::condition_variable renderQueueCv;
    std::atomic<bool> shouldStop;
    
    // Performance monitoring
    std::thread performanceMonitorThread;
    std::atomic<float> systemRenderLoad;
    std::atomic<int> activeSurfaceCount;
    
    // Event listener
    RenderEventListener* eventListener;
    
    // Configuration
    int maxSurfaces;
    int renderThreadCount;
    float maxRenderLoad;

public:
    MultiSurfaceRenderer(int maxSurfaces = 16, int threadCount = 2);
    ~MultiSurfaceRenderer();
    
    // Surface management
    bool addSurface(int channelIndex, ANativeWindow* surface);
    bool removeSurface(int channelIndex);
    bool updateSurface(int channelIndex, ANativeWindow* surface);
    
    // Rendering operations
    bool queueFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData);
    bool renderFrame(int channelIndex);
    bool isSurfaceReady(int channelIndex) const;
    
    // Surface configuration
    void setSurfaceFormat(int channelIndex, int width, int height, int format);
    void setTargetFps(int channelIndex, float fps);
    void pauseSurface(int channelIndex);
    void resumeSurface(int channelIndex);
    
    // Statistics and monitoring
    RenderState getSurfaceState(int channelIndex) const;
    float getSurfaceFps(int channelIndex) const;
    int getFrameCount(int channelIndex) const;
    int getRenderCount(int channelIndex) const;
    int getDroppedFrames(int channelIndex) const;
    std::vector<int> getActiveSurfaces() const;
    
    // Performance management
    float getSystemRenderLoad() const { return systemRenderLoad.load(); }
    int getActiveSurfaceCount() const { return activeSurfaceCount.load(); }
    void optimizeRenderPerformance();
    
    // Event handling
    void setEventListener(RenderEventListener* listener);
    
    // Cleanup
    void cleanup();

private:
    // Internal rendering
    void renderThreadLoop(int threadId);
    void processSurfaceRender(int channelIndex);
    bool renderFrameToSurface(SurfaceInfo* surfaceInfo, frame_data_t* frameData);
    
    // Performance monitoring
    void performanceMonitorLoop();
    void updateSurfaceStats(SurfaceInfo* surfaceInfo);
    void updateSystemLoad();
    
    // Surface management
    SurfaceInfo* getSurfaceInfo(int channelIndex);
    const SurfaceInfo* getSurfaceInfo(int channelIndex) const;
    void updateSurfaceState(int channelIndex, RenderState newState);
    
    // Error handling
    void handleRenderError(int channelIndex, const std::string& error);
    
    // Frame rate control
    bool shouldRenderFrame(SurfaceInfo* surfaceInfo) const;
    void adaptiveFrameSkipping(SurfaceInfo* surfaceInfo);
    
    // Thread safety helpers
    std::unique_lock<std::mutex> lockSurfaces() { return std::unique_lock<std::mutex>(surfacesMutex); }
    std::unique_lock<std::mutex> lockRenderQueue() { return std::unique_lock<std::mutex>(renderQueueMutex); }
};

/**
 * Surface Render Worker - handles individual surface rendering tasks
 */
class SurfaceRenderWorker {
private:
    int workerId;
    std::thread workerThread;
    std::atomic<bool> isActive;
    std::queue<std::function<void()>> taskQueue;
    std::mutex taskMutex;
    std::condition_variable taskCv;
    
public:
    SurfaceRenderWorker(int id);
    ~SurfaceRenderWorker();
    
    void start();
    void stop();
    void addRenderTask(std::function<void()> task);
    bool isWorkerActive() const { return isActive.load(); }
    int getWorkerId() const { return workerId; }

private:
    void workerLoop();
};

/**
 * Render Load Balancer - manages rendering load across surfaces
 */
class RenderLoadBalancer {
public:
    struct LoadMetrics {
        float totalRenderLoad;
        int activeSurfaces;
        float averageFps;
        int totalDroppedFrames;
        
        LoadMetrics() : totalRenderLoad(0.0f), activeSurfaces(0), 
                       averageFps(0.0f), totalDroppedFrames(0) {}
    };

private:
    LoadMetrics currentMetrics;
    std::mutex balancerMutex;
    
public:
    void updateMetrics(const LoadMetrics& metrics);
    std::vector<int> getOptimalRenderOrder(const std::vector<int>& surfaces);
    bool shouldThrottleRender(int channelIndex, const LoadMetrics& metrics) const;
    void rebalanceRenderLoad(std::vector<int>& surfaces);
    float calculateOptimalFps(int channelIndex, const LoadMetrics& metrics) const;
};

/**
 * Enhanced Multi-Surface Manager with advanced rendering features
 */
class EnhancedMultiSurfaceManager {
private:
    std::unique_ptr<MultiSurfaceRenderer> renderer;
    std::map<int, std::function<void(int, int, int)>> renderCallbacks;
    std::mutex callbacksMutex;
    
    // Layout management
    enum LayoutMode {
        SINGLE = 1,
        QUAD = 4,
        NINE = 9,
        SIXTEEN = 16
    };
    
    LayoutMode currentLayout;
    std::vector<int> visibleChannels;
    
public:
    EnhancedMultiSurfaceManager(int maxSurfaces = 16);
    ~EnhancedMultiSurfaceManager();
    
    // Surface management
    bool addChannelSurface(int channelIndex, ANativeWindow* surface);
    bool removeChannelSurface(int channelIndex);
    
    // Rendering operations
    bool renderChannelFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData);
    bool isChannelSurfaceReady(int channelIndex) const;
    
    // Layout management
    void setLayout(LayoutMode layout);
    void setVisibleChannels(const std::vector<int>& channels);
    LayoutMode getCurrentLayout() const { return currentLayout; }
    
    // Callback management
    void setRenderCallback(int channelIndex, std::function<void(int, int, int)> callback);
    void removeRenderCallback(int channelIndex);
    
    // Performance optimization
    void optimizeRenderingPerformance();
    void enableAdaptiveRendering(bool enabled);
    
    // Statistics
    int getActiveSurfaceCount() const;
    std::vector<int> getActiveSurfaces() const;
    
    // Cleanup
    void cleanup();

private:
    void handleFrameRendered(int channelIndex, int width, int height);
    void updateLayoutConfiguration();
};

#endif // AIBOX_MULTI_SURFACE_RENDERER_H
