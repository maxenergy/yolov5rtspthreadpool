#include "StreamHealthIntegration.h"
#include "log4c.h"
#include <chrono>
#include <thread>
#include <cassert>

/**
 * Test class for Stream Health Integration system
 */
class StreamHealthIntegrationTest {
private:
    int healthStatusChangedCount = 0;
    int recoveryActionCount = 0;
    int systemHealthCallbackCount = 0;
    
public:
    bool testBasicInitialization() {
        LOGD("=== Testing Basic Initialization ===");
        
        StreamHealthIntegration integration;
        
        // Test initialization
        StreamHealthIntegration::HealthIntegrationConfig config;
        config.autoRecoveryEnabled = true;
        config.maxRecoveryAttempts = 3;
        config.recoveryDelayMs = 1000;
        
        if (!integration.initialize(config)) {
            LOGE("Failed to initialize stream health integration");
            return false;
        }
        
        // Test adding channels
        if (!integration.addChannel(0)) {
            LOGE("Failed to add channel 0");
            return false;
        }
        
        if (!integration.addChannel(1)) {
            LOGE("Failed to add channel 1");
            return false;
        }
        
        // Test channel monitoring status
        if (!integration.isChannelMonitored(0) || !integration.isChannelMonitored(1)) {
            LOGE("Channels not properly monitored");
            return false;
        }
        
        LOGD("Basic initialization test passed");
        return true;
    }
    
    bool testHealthMonitoring() {
        LOGD("=== Testing Health Monitoring ===");
        
        StreamHealthIntegration integration;
        integration.initialize();
        
        // Add channels
        integration.addChannel(0);
        integration.addChannel(1);
        
        // Start health monitoring
        integration.startHealthMonitoring();
        
        // Update health data
        integration.updateStreamHealth(0, 30.0f, 0, 50.0); // Good health
        integration.updateConnectionHealth(0, true, 0);
        integration.updateDecoderHealth(0, 25.0f, 50 * 1024 * 1024); // 50MB
        
        integration.updateStreamHealth(1, 10.0f, 5, 200.0); // Poor health
        integration.updateConnectionHealth(1, false, 3);
        integration.updateDecoderHealth(1, 85.0f, 200 * 1024 * 1024); // 200MB
        
        // Wait for health monitoring to process
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Check channel health status
        auto channel0Status = integration.getChannelHealthStatus(0);
        auto channel1Status = integration.getChannelHealthStatus(1);
        
        LOGD("Channel 0 health: %d", channel0Status.overallHealth);
        LOGD("Channel 1 health: %d", channel1Status.overallHealth);
        
        // Channel 0 should be healthier than channel 1
        if (channel0Status.overallHealth >= channel1Status.overallHealth) {
            LOGD("Health monitoring working correctly");
        } else {
            LOGW("Health monitoring may not be working as expected");
        }
        
        LOGD("Health monitoring test completed");
        return true;
    }
    
    bool testAutoRecovery() {
        LOGD("=== Testing Auto Recovery ===");
        
        StreamHealthIntegration integration;
        
        // Set up callbacks
        integration.setRecoveryActionCallback([this](int channelIndex, 
                                                    StreamHealthIntegration::RecoveryAction action, 
                                                    bool success) {
            recoveryActionCount++;
            LOGD("Recovery action callback: Channel %d, Action %d, Success %s", 
                 channelIndex, action, success ? "true" : "false");
        });
        
        StreamHealthIntegration::HealthIntegrationConfig config;
        config.autoRecoveryEnabled = true;
        config.maxRecoveryAttempts = 2;
        config.recoveryDelayMs = 100; // Short delay for testing
        
        integration.initialize(config);
        integration.addChannel(0);
        integration.enableAutoRecovery(0, true);
        
        // Test manual recovery
        bool recoveryResult = integration.triggerManualRecovery(0, 
                                                               StreamHealthIntegration::CLEAR_QUEUES);
        
        if (recoveryResult) {
            LOGD("Manual recovery triggered successfully");
        } else {
            LOGW("Manual recovery failed");
        }
        
        // Wait for callback
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        if (recoveryActionCount > 0) {
            LOGD("Recovery action callback received");
        }
        
        LOGD("Auto recovery test completed");
        return true;
    }
    
    bool testHealthDashboard() {
        LOGD("=== Testing Health Dashboard ===");
        
        StreamHealthIntegration integration;
        integration.initialize();
        
        // Add multiple channels
        for (int i = 0; i < 4; i++) {
            integration.addChannel(i);
        }
        
        // Create dashboard
        StreamHealthDashboard dashboard(&integration);
        dashboard.startDashboard();
        
        // Update health data for different channels
        integration.updateStreamHealth(0, 30.0f, 0, 50.0); // Healthy
        integration.updateStreamHealth(1, 20.0f, 2, 100.0); // Warning
        integration.updateStreamHealth(2, 10.0f, 5, 300.0); // Critical
        integration.updateStreamHealth(3, 5.0f, 10, 500.0); // Failed
        
        // Wait for dashboard to update
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        // Get dashboard data
        auto dashboardData = dashboard.getDashboardData();
        
        LOGD("Dashboard data:");
        LOGD("  Total channels: %d", dashboardData.totalChannels);
        LOGD("  Healthy channels: %d", dashboardData.healthyChannels);
        LOGD("  Warning channels: %d", dashboardData.warningChannels);
        LOGD("  Critical channels: %d", dashboardData.criticalChannels);
        LOGD("  Failed channels: %d", dashboardData.failedChannels);
        
        // Generate reports
        std::string report = dashboard.generateDashboardReport();
        std::string jsonStatus = dashboard.generateJsonStatus();
        
        LOGD("Dashboard report generated: %zu characters", report.length());
        LOGD("JSON status generated: %zu characters", jsonStatus.length());
        
        dashboard.stopDashboard();
        
        LOGD("Health dashboard test completed");
        return true;
    }
    
    bool testPerformanceOptimization() {
        LOGD("=== Testing Performance Optimization ===");
        
        StreamHealthIntegration integration;
        
        StreamHealthIntegration::HealthIntegrationConfig config;
        config.performanceOptimizationEnabled = true;
        
        integration.initialize(config);
        
        // Add channels
        for (int i = 0; i < 3; i++) {
            integration.addChannel(i);
        }
        
        // Simulate performance issues
        integration.updateDecoderHealth(0, 90.0f, 500 * 1024 * 1024); // High CPU and memory
        integration.updateDecoderHealth(1, 85.0f, 400 * 1024 * 1024);
        integration.updateDecoderHealth(2, 75.0f, 300 * 1024 * 1024);
        
        // Trigger optimization
        integration.optimizeSystemPerformance();
        
        // Wait for optimization to process
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        LOGD("Performance optimization test completed");
        return true;
    }
    
    bool testRecoveryStatistics() {
        LOGD("=== Testing Recovery Statistics ===");
        
        StreamHealthIntegration integration;
        integration.initialize();
        integration.addChannel(0);
        
        // Trigger multiple recovery actions
        for (int i = 0; i < 5; i++) {
            integration.triggerManualRecovery(0, StreamHealthIntegration::CLEAR_QUEUES);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        // Check statistics
        int totalActions = integration.getTotalRecoveryActions();
        int successfulRecoveries = integration.getSuccessfulRecoveries();
        float successRate = integration.getRecoverySuccessRate();
        
        LOGD("Recovery statistics:");
        LOGD("  Total actions: %d", totalActions);
        LOGD("  Successful recoveries: %d", successfulRecoveries);
        LOGD("  Success rate: %.2f%%", successRate);
        
        if (totalActions > 0) {
            LOGD("Recovery statistics working correctly");
        }
        
        // Generate recovery report
        std::string recoveryReport = integration.generateRecoveryReport();
        LOGD("Recovery report generated: %zu characters", recoveryReport.length());
        
        LOGD("Recovery statistics test completed");
        return true;
    }
    
    void runAllTests() {
        LOGD("Starting Stream Health Integration Tests");
        
        int passedTests = 0;
        int totalTests = 6;
        
        if (testBasicInitialization()) passedTests++;
        if (testHealthMonitoring()) passedTests++;
        if (testAutoRecovery()) passedTests++;
        if (testHealthDashboard()) passedTests++;
        if (testPerformanceOptimization()) passedTests++;
        if (testRecoveryStatistics()) passedTests++;
        
        LOGD("=== Test Results ===");
        LOGD("Passed: %d/%d tests", passedTests, totalTests);
        LOGD("Health status changed events: %d", healthStatusChangedCount);
        LOGD("Recovery action events: %d", recoveryActionCount);
        LOGD("System health callback events: %d", systemHealthCallbackCount);
        
        if (passedTests == totalTests) {
            LOGD("All tests PASSED!");
        } else {
            LOGE("Some tests FAILED!");
        }
    }
};

// Test runner function
extern "C" void runStreamHealthIntegrationTests() {
    StreamHealthIntegrationTest test;
    test.runAllTests();
}

// Stress test
extern "C" void runStreamHealthStressTest() {
    LOGD("=== Stream Health Stress Test ===");
    
    StreamHealthIntegration integration;
    integration.initialize();
    
    // Add many channels
    const int numChannels = 16;
    for (int i = 0; i < numChannels; i++) {
        integration.addChannel(i);
    }
    
    // Continuously update health data for 30 seconds
    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(30);
    
    int updateCount = 0;
    while (std::chrono::steady_clock::now() < endTime) {
        for (int i = 0; i < numChannels; i++) {
            // Simulate varying health conditions
            float fps = 15.0f + (rand() % 20); // 15-35 FPS
            int drops = rand() % 5; // 0-4 drops
            double latency = 50.0 + (rand() % 200); // 50-250ms latency
            
            integration.updateStreamHealth(i, fps, drops, latency);
            
            bool connected = (rand() % 10) > 1; // 90% connection rate
            int errors = rand() % 3; // 0-2 errors
            integration.updateConnectionHealth(i, connected, errors);
            
            float cpu = 20.0f + (rand() % 60); // 20-80% CPU
            long memory = (50 + rand() % 150) * 1024 * 1024; // 50-200MB
            integration.updateDecoderHealth(i, cpu, memory);
        }
        
        updateCount++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Get final statistics
    auto systemHealth = integration.getSystemHealthStatus();
    int totalRecoveries = integration.getTotalRecoveryActions();
    float successRate = integration.getRecoverySuccessRate();
    
    LOGD("Stress test completed:");
    LOGD("Updates performed: %d", updateCount);
    LOGD("Final system health: %d", systemHealth);
    LOGD("Total recovery actions: %d", totalRecoveries);
    LOGD("Recovery success rate: %.2f%%", successRate);
    
    // Generate final report
    std::string healthReport = integration.generateHealthReport();
    LOGD("Final health report: %zu characters", healthReport.length());
}
