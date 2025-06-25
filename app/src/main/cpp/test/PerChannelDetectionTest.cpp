#include "PerChannelDetection.h"
#include "MultiStreamDetectionIntegration.h"
#include "log4c.h"
#include <chrono>
#include <thread>
#include <cassert>

/**
 * Test class for Per-Channel Detection system
 */
class PerChannelDetectionTest : public PerChannelDetection::DetectionEventListener {
private:
    int detectionCompletedCount = 0;
    int errorCount = 0;
    int queueOverflowCount = 0;
    int stateChangeCount = 0;
    
public:
    // DetectionEventListener implementation
    void onDetectionCompleted(int channelIndex, const PerChannelDetection::DetectionResult& result) override {
        detectionCompletedCount++;
        LOGD("Test: Detection completed for channel %d, frame %d, detections: %zu", 
             channelIndex, result.frameId, result.detections.size());
    }
    
    void onDetectionError(int channelIndex, const std::string& error) override {
        errorCount++;
        LOGE("Test: Detection error on channel %d: %s", channelIndex, error.c_str());
    }
    
    void onQueueOverflow(int channelIndex, int droppedFrames) override {
        queueOverflowCount++;
        LOGW("Test: Queue overflow on channel %d: %d frames dropped", channelIndex, droppedFrames);
    }
    
    void onStateChanged(int channelIndex, PerChannelDetection::DetectionState oldState, 
                       PerChannelDetection::DetectionState newState) override {
        stateChangeCount++;
        LOGD("Test: Channel %d state changed: %d -> %d", channelIndex, oldState, newState);
    }
    
    // Test methods
    bool testBasicFunctionality() {
        LOGD("=== Testing Basic Functionality ===");
        
        // Create dummy model data
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        PerChannelDetection detection;
        
        // Test initialization
        if (!detection.initialize(dummyModel, sizeof(dummyModel))) {
            LOGE("Failed to initialize per-channel detection");
            return false;
        }
        
        detection.setEventListener(this);
        
        // Test adding channels
        PerChannelDetection::DetectionConfig config(0);
        config.enabled = true;
        config.confidenceThreshold = 0.5f;
        config.maxDetections = 50;
        
        if (!detection.addChannel(0, config)) {
            LOGE("Failed to add channel 0");
            return false;
        }
        
        if (!detection.addChannel(1, config)) {
            LOGE("Failed to add channel 1");
            return false;
        }
        
        // Test starting detection
        if (!detection.startDetection(0)) {
            LOGE("Failed to start detection for channel 0");
            return false;
        }
        
        if (!detection.startDetection(1)) {
            LOGE("Failed to start detection for channel 1");
            return false;
        }
        
        // Test channel status
        if (!detection.isChannelActive(0) || !detection.isChannelActive(1)) {
            LOGE("Channels not active after starting detection");
            return false;
        }
        
        LOGD("Basic functionality test passed");
        return true;
    }
    
    bool testFrameProcessing() {
        LOGD("=== Testing Frame Processing ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        PerChannelDetection detection;
        detection.initialize(dummyModel, sizeof(dummyModel));
        detection.setEventListener(this);
        
        // Add and start channel
        PerChannelDetection::DetectionConfig config(0);
        detection.addChannel(0, config);
        detection.startDetection(0);
        
        // Create dummy frame data
        auto frameData = std::make_shared<frame_data_t>();
        frameData->frameId = 1;
        frameData->screenW = 640;
        frameData->screenH = 480;
        frameData->dataSize = 640 * 480 * 4; // RGBA
        frameData->data = std::shared_ptr<void>(new uint8_t[frameData->dataSize], 
                                              [](void* p) { delete[] static_cast<uint8_t*>(p); });
        
        // Submit frame
        if (!detection.submitFrame(0, frameData)) {
            LOGE("Failed to submit frame");
            return false;
        }
        
        // Wait a bit for processing
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Try to get result (non-blocking)
        PerChannelDetection::DetectionResult result;
        bool hasResult = detection.getDetectionResultNonBlocking(0, result);
        
        LOGD("Frame processing test completed, has result: %s", hasResult ? "yes" : "no");
        return true;
    }
    
    bool testMultiChannelProcessing() {
        LOGD("=== Testing Multi-Channel Processing ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        PerChannelDetection detection;
        detection.initialize(dummyModel, sizeof(dummyModel));
        detection.setEventListener(this);
        
        // Add multiple channels
        const int numChannels = 4;
        for (int i = 0; i < numChannels; i++) {
            PerChannelDetection::DetectionConfig config(i);
            config.confidenceThreshold = 0.3f + (i * 0.1f); // Different thresholds
            
            if (!detection.addChannel(i, config)) {
                LOGE("Failed to add channel %d", i);
                return false;
            }
            
            if (!detection.startDetection(i)) {
                LOGE("Failed to start detection for channel %d", i);
                return false;
            }
        }
        
        // Submit frames to all channels
        for (int i = 0; i < numChannels; i++) {
            auto frameData = std::make_shared<frame_data_t>();
            frameData->frameId = i + 1;
            frameData->screenW = 640;
            frameData->screenH = 480;
            frameData->dataSize = 640 * 480 * 4;
            frameData->data = std::shared_ptr<void>(new uint8_t[frameData->dataSize], 
                                                  [](void* p) { delete[] static_cast<uint8_t*>(p); });
            
            detection.submitFrame(i, frameData);
        }
        
        // Wait for processing
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Check statistics
        auto allStats = detection.getAllChannelStats();
        LOGD("Multi-channel processing: %zu channels processed", allStats.size());
        
        for (const auto& stats : allStats) {
            LOGD("Channel %d: %d frames processed, %d detections", 
                 stats.channelIndex, stats.totalFramesProcessed, stats.totalDetections);
        }
        
        return true;
    }
    
    bool testDetectionIntegration() {
        LOGD("=== Testing Detection Integration ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        MultiStreamDetectionIntegration integration;
        
        // Initialize integration
        if (!integration.initialize(dummyModel, sizeof(dummyModel))) {
            LOGE("Failed to initialize detection integration");
            return false;
        }
        
        // Add detection channels
        MultiStreamDetectionIntegration::DetectionChannelConfig config(0);
        config.detectionEnabled = true;
        config.visualizationEnabled = true;
        config.confidenceThreshold = 0.5f;
        
        if (!integration.addDetectionChannel(0, config)) {
            LOGE("Failed to add detection channel 0");
            return false;
        }
        
        if (!integration.addDetectionChannel(1, config)) {
            LOGE("Failed to add detection channel 1");
            return false;
        }
        
        // Start detection
        integration.startChannelDetection(0);
        integration.startChannelDetection(1);
        
        // Test global settings
        integration.enableGlobalDetection(true);
        integration.setGlobalConfidenceThreshold(0.6f);
        
        // Get system stats
        auto systemStats = integration.getSystemStats();
        LOGD("System stats: %d total channels, %d active detection channels", 
             systemStats.totalChannels, systemStats.activeDetectionChannels);
        
        LOGD("Detection integration test passed");
        return true;
    }
    
    void runAllTests() {
        LOGD("Starting Per-Channel Detection Tests");
        
        int passedTests = 0;
        int totalTests = 4;
        
        if (testBasicFunctionality()) passedTests++;
        if (testFrameProcessing()) passedTests++;
        if (testMultiChannelProcessing()) passedTests++;
        if (testDetectionIntegration()) passedTests++;
        
        LOGD("=== Test Results ===");
        LOGD("Passed: %d/%d tests", passedTests, totalTests);
        LOGD("Detection completed events: %d", detectionCompletedCount);
        LOGD("Error events: %d", errorCount);
        LOGD("Queue overflow events: %d", queueOverflowCount);
        LOGD("State change events: %d", stateChangeCount);
        
        if (passedTests == totalTests) {
            LOGD("All tests PASSED!");
        } else {
            LOGE("Some tests FAILED!");
        }
    }
};

// Test runner function
extern "C" void runPerChannelDetectionTests() {
    PerChannelDetectionTest test;
    test.runAllTests();
}

// Performance test
extern "C" void runPerChannelDetectionPerformanceTest() {
    LOGD("=== Performance Test ===");
    
    char dummyModel[1024];
    memset(dummyModel, 0, sizeof(dummyModel));
    
    PerChannelDetection detection;
    detection.initialize(dummyModel, sizeof(dummyModel));
    
    // Add 8 channels
    const int numChannels = 8;
    for (int i = 0; i < numChannels; i++) {
        PerChannelDetection::DetectionConfig config(i);
        detection.addChannel(i, config);
        detection.startDetection(i);
    }
    
    // Submit frames continuously for 10 seconds
    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(10);
    
    int frameCount = 0;
    while (std::chrono::steady_clock::now() < endTime) {
        for (int i = 0; i < numChannels; i++) {
            auto frameData = std::make_shared<frame_data_t>();
            frameData->frameId = frameCount++;
            frameData->screenW = 640;
            frameData->screenH = 480;
            frameData->dataSize = 640 * 480 * 4;
            frameData->data = std::shared_ptr<void>(new uint8_t[frameData->dataSize], 
                                                  [](void* p) { delete[] static_cast<uint8_t*>(p); });
            
            detection.submitFrame(i, frameData);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS
    }
    
    // Get final statistics
    auto allStats = detection.getAllChannelStats();
    int totalFramesProcessed = 0;
    int totalDetections = 0;
    
    for (const auto& stats : allStats) {
        totalFramesProcessed += stats.totalFramesProcessed;
        totalDetections += stats.totalDetections;
        LOGD("Channel %d: %d frames, %.2f avg processing time", 
             stats.channelIndex, stats.totalFramesProcessed, stats.averageProcessingTime);
    }
    
    LOGD("Performance test completed:");
    LOGD("Total frames submitted: %d", frameCount);
    LOGD("Total frames processed: %d", totalFramesProcessed);
    LOGD("Total detections: %d", totalDetections);
    LOGD("Processing rate: %.2f%%", (float)totalFramesProcessed / frameCount * 100.0f);
}
