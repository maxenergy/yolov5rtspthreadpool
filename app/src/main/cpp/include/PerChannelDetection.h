#ifndef AIBOX_PER_CHANNEL_DETECTION_H
#define AIBOX_PER_CHANNEL_DETECTION_H

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

#include "yolov5_thread_pool.h"
#include "user_comm.h"
#include "log4c.h"

/**
 * Per-Channel Detection Manager for independent YOLOv5 processing per channel
 * Ensures each channel has its own detection pipeline and result queue
 */
class PerChannelDetection {
public:
    enum DetectionState {
        INACTIVE = 0,
        INITIALIZING = 1,
        ACTIVE = 2,
        PAUSED = 3,
        ERROR = 4,
        DESTROYED = 5
    };

    struct DetectionConfig {
        int channelIndex;
        bool enabled;
        float confidenceThreshold;
        int maxDetections;
        int threadPoolSize;
        int maxQueueSize;
        bool enableNMS;
        float nmsThreshold;
        std::vector<int> enabledClasses;
        
        DetectionConfig(int index) : channelIndex(index), enabled(true),
                                   confidenceThreshold(0.5f), maxDetections(100),
                                   threadPoolSize(4), maxQueueSize(50),
                                   enableNMS(true), nmsThreshold(0.4f) {}
    };

    struct DetectionStats {
        int channelIndex;
        int totalFramesProcessed;
        int totalDetections;
        float averageDetectionsPerFrame;
        float averageProcessingTime;
        float peakProcessingTime;
        int queueSize;
        int droppedFrames;
        std::chrono::steady_clock::time_point lastUpdate;
        
        DetectionStats() : channelIndex(-1), totalFramesProcessed(0),
                         totalDetections(0), averageDetectionsPerFrame(0.0f),
                         averageProcessingTime(0.0f), peakProcessingTime(0.0f),
                         queueSize(0), droppedFrames(0) {
            lastUpdate = std::chrono::steady_clock::now();
        }

        DetectionStats(int index) : channelIndex(index), totalFramesProcessed(0),
                                  totalDetections(0), averageDetectionsPerFrame(0.0f),
                                  averageProcessingTime(0.0f), peakProcessingTime(0.0f),
                                  queueSize(0), droppedFrames(0) {
            lastUpdate = std::chrono::steady_clock::now();
        }
    };

    struct DetectionResult {
        int channelIndex;
        int frameId;
        std::vector<Detection> detections;
        std::chrono::steady_clock::time_point timestamp;
        float processingTime;

        DetectionResult() : channelIndex(-1), frameId(-1), processingTime(0.0f) {
            timestamp = std::chrono::steady_clock::now();
        }

        DetectionResult(int channel, int frame) : channelIndex(channel), frameId(frame),
                                                processingTime(0.0f) {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    // Event listener interface
    class DetectionEventListener {
    public:
        virtual ~DetectionEventListener() = default;
        virtual void onDetectionCompleted(int channelIndex, const DetectionResult& result) = 0;
        virtual void onDetectionError(int channelIndex, const std::string& error) = 0;
        virtual void onQueueOverflow(int channelIndex, int droppedFrames) = 0;
        virtual void onStateChanged(int channelIndex, DetectionState oldState, DetectionState newState) = 0;
    };

private:
    struct ChannelDetectionInfo {
        int channelIndex;
        DetectionState state;
        DetectionConfig config;
        DetectionStats stats;
        std::unique_ptr<Yolov5ThreadPool> threadPool;
        std::queue<std::shared_ptr<frame_data_t>> inputQueue;
        std::queue<DetectionResult> resultQueue;
        std::thread processingThread;
        mutable std::mutex inputMutex;
        mutable std::mutex resultMutex;
        std::condition_variable inputCondition;
        std::atomic<bool> shouldStop;
        std::atomic<bool> isProcessing;
        
        ChannelDetectionInfo(int index) : channelIndex(index), state(INACTIVE),
                                        config(index), stats(index),
                                        shouldStop(false), isProcessing(false) {}
        
        ~ChannelDetectionInfo() {
            shouldStop = true;
            inputCondition.notify_all();
            if (processingThread.joinable()) {
                processingThread.join();
            }
        }
    };

    std::map<int, std::unique_ptr<ChannelDetectionInfo>> channels;
    mutable std::mutex channelsMutex;
    DetectionEventListener* eventListener;

    // Global configuration
    char* modelData;
    int modelDataSize;
    std::atomic<int> activeChannelCount;
    std::atomic<bool> globalEnabled;

    // Statistics thread
    std::thread statsThread;
    std::atomic<bool> statsThreadRunning;
    std::condition_variable statsCondition;
    mutable std::mutex statsMutex;

public:
    PerChannelDetection();
    ~PerChannelDetection();
    
    // Initialization
    bool initialize(char* modelData, int modelSize);
    void cleanup();
    
    // Channel management
    bool addChannel(int channelIndex, const DetectionConfig& config = DetectionConfig(0));
    bool removeChannel(int channelIndex);
    bool isChannelActive(int channelIndex) const;
    
    // Detection control
    bool startDetection(int channelIndex);
    bool stopDetection(int channelIndex);
    bool pauseDetection(int channelIndex);
    bool resumeDetection(int channelIndex);
    
    // Frame processing
    bool submitFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData);
    bool getDetectionResult(int channelIndex, DetectionResult& result);
    bool getDetectionResultNonBlocking(int channelIndex, DetectionResult& result);
    
    // Configuration
    void setChannelConfig(int channelIndex, const DetectionConfig& config);
    DetectionConfig getChannelConfig(int channelIndex) const;
    void setEventListener(DetectionEventListener* listener);
    
    // Statistics and monitoring
    DetectionStats getChannelStats(int channelIndex) const;
    std::vector<DetectionStats> getAllChannelStats() const;
    std::vector<int> getActiveChannels() const;
    int getActiveChannelCount() const { return activeChannelCount.load(); }
    
    // Global control
    void enableGlobalDetection(bool enabled);
    bool isGlobalDetectionEnabled() const { return globalEnabled.load(); }
    void setGlobalConfidenceThreshold(float threshold);
    
    // Queue management
    int getChannelQueueSize(int channelIndex) const;
    void clearChannelQueue(int channelIndex);
    void clearAllQueues();

private:
    // Internal processing
    void channelProcessingLoop(int channelIndex);
    void processFrame(ChannelDetectionInfo* channelInfo, std::shared_ptr<frame_data_t> frameData);
    void updateChannelStats(ChannelDetectionInfo* channelInfo, const DetectionResult& result);
    
    // State management
    void changeChannelState(int channelIndex, DetectionState newState);
    void notifyStateChange(int channelIndex, DetectionState oldState, DetectionState newState);
    void notifyError(int channelIndex, const std::string& error);
    void notifyQueueOverflow(int channelIndex, int droppedFrames);
    
    // Statistics management
    void statisticsLoop();
    void updateGlobalStatistics();
    
    // Utility methods
    ChannelDetectionInfo* getChannelInfo(int channelIndex);
    const ChannelDetectionInfo* getChannelInfo(int channelIndex) const;
    bool validateChannelIndex(int channelIndex) const;
    void cleanupChannel(ChannelDetectionInfo* channelInfo);
};

/**
 * Detection Result Manager for managing detection results across channels
 */
class DetectionResultManager {
public:
    struct ChannelResults {
        int channelIndex;
        std::queue<PerChannelDetection::DetectionResult> results;
        mutable std::mutex resultsMutex;
        int maxResults;
        
        ChannelResults(int index, int maxSize = 100) : channelIndex(index), maxResults(maxSize) {}
    };

private:
    std::map<int, std::unique_ptr<ChannelResults>> channelResults;
    mutable std::mutex managerMutex;

public:
    DetectionResultManager();
    ~DetectionResultManager();
    
    // Channel management
    bool addChannel(int channelIndex, int maxResults = 100);
    bool removeChannel(int channelIndex);
    
    // Result management
    bool storeResult(int channelIndex, const PerChannelDetection::DetectionResult& result);
    bool getLatestResult(int channelIndex, PerChannelDetection::DetectionResult& result);
    bool getAllResults(int channelIndex, std::vector<PerChannelDetection::DetectionResult>& results);
    
    // Statistics
    int getResultCount(int channelIndex) const;
    void clearChannelResults(int channelIndex);
    void clearAllResults();
    
    // Utility
    std::vector<int> getActiveChannels() const;
};

#endif // AIBOX_PER_CHANNEL_DETECTION_H
