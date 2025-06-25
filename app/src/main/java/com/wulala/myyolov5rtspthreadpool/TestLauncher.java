package com.wulala.myyolov5rtspthreadpool;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.util.Log;

/**
 * Test launcher for automated testing via ADB commands
 * 
 * Usage examples:
 * adb shell am broadcast -a com.wulala.myyolov5rtspthreadpool.TEST_ACTION --es test_name "Single Channel Test"
 * adb shell am broadcast -a com.wulala.myyolov5rtspthreadpool.TEST_ACTION --es test_name "Quad Channel Test"
 * adb shell am broadcast -a com.wulala.myyolov5rtspthreadpool.TEST_ACTION --es test_name "Performance Stress Test"
 */
public class TestLauncher extends BroadcastReceiver {
    private static final String TAG = "TestLauncher";
    private static final String TEST_ACTION = "com.wulala.myyolov5rtspthreadpool.TEST_ACTION";
    private static final String EXTRA_TEST_NAME = "test_name";
    
    private MainActivity mainActivity;
    
    public TestLauncher(MainActivity activity) {
        this.mainActivity = activity;
    }
    
    public void register() {
        IntentFilter filter = new IntentFilter(TEST_ACTION);
        mainActivity.registerReceiver(this, filter);
        Log.i(TAG, "Test launcher registered for broadcast commands");
    }
    
    public void unregister() {
        try {
            mainActivity.unregisterReceiver(this);
            Log.i(TAG, "Test launcher unregistered");
        } catch (IllegalArgumentException e) {
            // Already unregistered
        }
    }
    
    @Override
    public void onReceive(Context context, Intent intent) {
        if (TEST_ACTION.equals(intent.getAction())) {
            String testName = intent.getStringExtra(EXTRA_TEST_NAME);
            
            if (testName != null && !testName.isEmpty()) {
                Log.i(TAG, "Received test command: " + testName);
                
                // Run test on UI thread
                mainActivity.runOnUiThread(() -> {
                    handleTestCommand(testName);
                });
            } else {
                Log.w(TAG, "Test command received but no test name specified");
                showAvailableTests();
            }
        }
    }
    
    private void handleTestCommand(String testName) {
        switch (testName.toLowerCase()) {
            case "single":
            case "single channel test":
                mainActivity.runTestScenario("Single Channel Test");
                break;
                
            case "quad":
            case "quad channel test":
                mainActivity.runTestScenario("Quad Channel Test");
                break;
                
            case "nine":
            case "nine channel test":
                mainActivity.runTestScenario("Nine Channel Test");
                break;
                
            case "full":
            case "full channel test":
                mainActivity.runTestScenario("Full Channel Test");
                break;
                
            case "layout":
            case "layout switching test":
                mainActivity.runTestScenario("Layout Switching Test");
                break;
                
            case "stress":
            case "performance stress test":
                mainActivity.runTestScenario("Performance Stress Test");
                break;
                
            case "quick":
                mainActivity.runQuickTest();
                break;
                
            case "enable_test_mode":
                mainActivity.enableTestMode();
                Log.i(TAG, "Test mode enabled");
                break;
                
            default:
                Log.w(TAG, "Unknown test command: " + testName);
                showAvailableTests();
                break;
        }
    }
    
    private void showAvailableTests() {
        Log.i(TAG, "Available test commands:");
        Log.i(TAG, "  - single / 'Single Channel Test'");
        Log.i(TAG, "  - quad / 'Quad Channel Test'");
        Log.i(TAG, "  - nine / 'Nine Channel Test'");
        Log.i(TAG, "  - full / 'Full Channel Test'");
        Log.i(TAG, "  - layout / 'Layout Switching Test'");
        Log.i(TAG, "  - stress / 'Performance Stress Test'");
        Log.i(TAG, "  - quick (runs quad test)");
        Log.i(TAG, "  - enable_test_mode");
        Log.i(TAG, "");
        Log.i(TAG, "Example usage:");
        Log.i(TAG, "adb shell am broadcast -a " + TEST_ACTION + " --es " + EXTRA_TEST_NAME + " \"quad\"");
    }
    
    /**
     * Run all test scenarios in sequence
     */
    public void runAllTests() {
        Log.i(TAG, "Starting comprehensive test suite");
        
        String[] testSequence = {
            "Single Channel Test",
            "Quad Channel Test", 
            "Nine Channel Test",
            "Layout Switching Test",
            "Performance Stress Test"
        };
        
        runTestSequence(testSequence, 0);
    }
    
    private void runTestSequence(String[] tests, int currentIndex) {
        if (currentIndex >= tests.length) {
            Log.i(TAG, "All tests completed");
            return;
        }
        
        String currentTest = tests[currentIndex];
        Log.i(TAG, "Running test " + (currentIndex + 1) + "/" + tests.length + ": " + currentTest);
        
        mainActivity.runTestScenario(currentTest);
        
        // Schedule next test (assuming each test takes about 2 minutes)
        mainActivity.findViewById(android.R.id.content).postDelayed(() -> {
            runTestSequence(tests, currentIndex + 1);
        }, 120000); // 2 minutes
    }
    
    /**
     * Generate test report
     */
    public void generateTestReport() {
        Log.i(TAG, "=== Multi-Channel NVR System Test Report ===");
        Log.i(TAG, "System Information:");
        Log.i(TAG, "  - Android Version: " + android.os.Build.VERSION.RELEASE);
        Log.i(TAG, "  - Device Model: " + android.os.Build.MODEL);
        Log.i(TAG, "  - Available Memory: " + getAvailableMemory() + " MB");
        Log.i(TAG, "  - CPU Cores: " + Runtime.getRuntime().availableProcessors());
        Log.i(TAG, "");
        Log.i(TAG, "Test Configuration:");
        Log.i(TAG, "  - Max Channels: 16");
        Log.i(TAG, "  - Target FPS: 30");
        Log.i(TAG, "  - Detection Enabled: Yes");
        Log.i(TAG, "  - Layouts Tested: 1x1, 2x2, 3x3, 4x4");
        Log.i(TAG, "============================================");
    }
    
    private long getAvailableMemory() {
        Runtime runtime = Runtime.getRuntime();
        return runtime.maxMemory() / (1024 * 1024); // MB
    }
}
