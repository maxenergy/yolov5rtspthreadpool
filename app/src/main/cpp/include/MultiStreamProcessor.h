#ifndef AIBOX_MULTI_STREAM_PROCESSOR_H
#define AIBOX_MULTI_STREAM_PROCESSOR_H

#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <functional>
#include <chrono>

#include "RTSPStreamManager.h"
#include "log4c.h"

/**
 * Multi-Stream Processor for coordinating multiple RTSP streams
 * Handles load balancing, resource allocation, and stream prioritization
 */
class MultiStreamProcessor {
public:
    enum ProcessingPriority {
        LOW = 0,
        NORMAL = 1,
        HIGH = 2,
        CRITICAL = 3
    };

    struct StreamConfig {
        int channelIndex;
        std::string rtspUrl;
        ProcessingPriority priority;
        bool detectionEnabled;
        float targetFps;
        int maxRetries;
        bool autoReconnect;

        StreamConfig()
            : channelIndex(-1), rtspUrl(""), priority(NORMAL),
              detectionEnabled(true), targetFps(30.0f), maxRetries(5), autoReconnect(true) {}

        StreamConfig(int index, const std::string& url)
            : channelIndex(index), rtspUrl(url), priority(NORMAL),
              detectionEnabled(true), targetFps(30.0f), maxRetries(5), autoReconnect(true) {}
    };

    struct StreamStats {
        int channelIndex;
        RTSPStreamManager::StreamState state;
        float currentFps;
        int frameCount;
        int droppedFrames;
        int reconnectCount;
        std::chrono::steady_clock::time_point lastFrameTime;
        std::chrono::steady_clock::time_point startTime;
        double totalProcessingTime;
        double averageProcessingTime;
        
        StreamStats() : channelIndex(-1), state(RTSPStreamManager::DISCONNECTED),
                       currentFps(0.0f), frameCount(0), droppedFrames(0),
                       reconnectCount(0), totalProcessingTime(0.0), averageProcessingTime(0.0) {
            lastFrameTime = std::chrono::steady_clock::now();
            startTime = std::chrono::steady_clock::now();
        }

        StreamStats(int index) : channelIndex(index), state(RTSPStreamManager::DISCONNECTED),
                                currentFps(0.0f), frameCount(0), droppedFrames(0),
                                reconnectCount(0), totalProcessingTime(0.0), averageProcessingTime(0.0) {
            lastFrameTime = std::chrono::steady_clock::now();
            startTime = std::chrono::steady_clock::now();
        }
    };

    // Callback interface for processing events
    class ProcessingEventListener {
    public:
        virtual ~ProcessingEventListener() = default;
        virtual void onStreamProcessingStarted(int channelIndex) = 0;
        virtual void onStreamProcessingStopped(int channelIndex) = 0;
        virtual void onFrameProcessed(int channelIndex, void* frameData, int size) = 0;
        virtual void onProcessingError(int channelIndex, const std::string& error) = 0;
        virtual void onLoadBalancingTriggered(const std::vector<int>& affectedChannels) = 0;
    };

private:
    // Stream management
    std::map<int, std::unique_ptr<RTSPStreamManager>> streamManagers;
    std::map<int, StreamConfig> streamConfigs;
    std::map<int, StreamStats> streamStats;
    std::mutex streamsMutex;
    
    // Processing threads
    std::vector<std::thread> processingThreads;
    std::queue<int> processingQueue;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::atomic<bool> shouldStop;
    
    // Load balancing
    std::thread loadBalancerThread;
    std::mutex loadBalancerMutex;
    std::condition_variable loadBalancerCv;
    
    // Resource monitoring
    std::thread resourceMonitorThread;
    std::atomic<float> systemCpuUsage;
    std::atomic<long> systemMemoryUsage;
    std::atomic<int> activeStreamCount;
    
    // Event listener
    ProcessingEventListener* eventListener;
    
    // Configuration
    int maxConcurrentStreams;
    int processingThreadCount;
    float cpuThreshold;
    long memoryThreshold;
    int loadBalanceInterval;

public:
    MultiStreamProcessor(int maxStreams = 16, int threadCount = 4);
    ~MultiStreamProcessor();
    
    // Stream configuration
    bool addStream(const StreamConfig& config);
    bool removeStream(int channelIndex);
    bool updateStreamConfig(int channelIndex, const StreamConfig& config);
    
    // Stream control
    bool startStream(int channelIndex);
    bool stopStream(int channelIndex);
    bool startAllStreams();
    bool stopAllStreams();
    
    // Priority management
    void setStreamPriority(int channelIndex, ProcessingPriority priority);
    ProcessingPriority getStreamPriority(int channelIndex) const;
    
    // Resource management
    void setResourceLimits(float cpuThreshold, long memoryThreshold);
    void setMaxConcurrentStreams(int maxStreams);
    
    // Statistics and monitoring
    StreamStats getStreamStats(int channelIndex) const;
    std::vector<StreamStats> getAllStreamStats() const;
    float getSystemCpuUsage() const { return systemCpuUsage.load(); }
    long getSystemMemoryUsage() const { return systemMemoryUsage.load(); }
    int getActiveStreamCount() const { return activeStreamCount.load(); }
    
    // Event handling
    void setEventListener(ProcessingEventListener* listener);
    
    // Load balancing
    void enableLoadBalancing(bool enabled);
    void triggerLoadBalancing();
    
    // Cleanup
    void cleanup();

private:
    // Internal processing
    void processingThreadLoop(int threadId);
    void processStream(int channelIndex);
    void updateStreamStats(int channelIndex, bool frameProcessed, double processingTime);
    
    // Load balancing
    void loadBalancerLoop();
    void performLoadBalancing();
    std::vector<int> identifyOverloadedStreams();
    void redistributeLoad(const std::vector<int>& overloadedStreams);
    
    // Resource monitoring
    void resourceMonitorLoop();
    void updateSystemResources();
    bool isSystemOverloaded() const;
    
    // Priority management
    void sortStreamsByPriority(std::vector<int>& channels);
    bool shouldProcessStream(int channelIndex) const;
    
    // Utility methods
    StreamConfig* getStreamConfig(int channelIndex);
    const StreamConfig* getStreamConfig(int channelIndex) const;
    StreamStats* getStreamStatsInternal(int channelIndex);
    RTSPStreamManager* getStreamManager(int channelIndex);
    
    // Thread safety helpers
    std::unique_lock<std::mutex> lockStreams() { return std::unique_lock<std::mutex>(streamsMutex); }
    std::unique_lock<std::mutex> lockQueue() { return std::unique_lock<std::mutex>(queueMutex); }
};

/**
 * Stream Processing Worker - handles individual stream processing tasks
 */
class StreamProcessingWorker {
private:
    int workerId;
    std::thread workerThread;
    std::atomic<bool> isActive;
    std::queue<std::function<void()>> taskQueue;
    std::mutex taskMutex;
    std::condition_variable taskCv;
    
public:
    StreamProcessingWorker(int id);
    ~StreamProcessingWorker();
    
    void start();
    void stop();
    void addTask(std::function<void()> task);
    bool isWorkerActive() const { return isActive.load(); }
    int getWorkerId() const { return workerId; }

private:
    void workerLoop();
};

/**
 * Stream Load Balancer - manages resource allocation across streams
 */
class StreamLoadBalancer {
public:
    struct LoadMetrics {
        float cpuUsage;
        long memoryUsage;
        int activeStreams;
        float averageFps;
        int totalDroppedFrames;
        
        LoadMetrics() : cpuUsage(0.0f), memoryUsage(0), activeStreams(0), 
                       averageFps(0.0f), totalDroppedFrames(0) {}
    };

private:
    LoadMetrics currentMetrics;
    std::vector<int> priorityQueue;
    std::mutex balancerMutex;
    
public:
    void updateMetrics(const LoadMetrics& metrics);
    std::vector<int> getOptimalStreamDistribution(const std::vector<int>& channels, 
                                                  const std::map<int, MultiStreamProcessor::ProcessingPriority>& priorities);
    bool shouldThrottleStream(int channelIndex, const LoadMetrics& metrics) const;
    void rebalanceStreams(std::vector<int>& channels);
};

#endif // AIBOX_MULTI_STREAM_PROCESSOR_H
