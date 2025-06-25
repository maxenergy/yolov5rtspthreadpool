package com.wulala.myyolov5rtspthreadpool;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;

/**
 * Test utilities for multi-channel NVR system validation
 */
public class TestUtils {
    private static final String TAG = "TestUtils";
    
    public static class TestChannel {
        public int channelId;
        public String name;
        public String rtspUrl;
        public boolean detectionEnabled;
        public String priority;
        
        public TestChannel(int channelId, String name, String rtspUrl, boolean detectionEnabled, String priority) {
            this.channelId = channelId;
            this.name = name;
            this.rtspUrl = rtspUrl;
            this.detectionEnabled = detectionEnabled;
            this.priority = priority;
        }
    }
    
    public static class TestScenario {
        public String name;
        public String description;
        public List<Integer> activeChannels;
        public String layout;
        public int durationSeconds;
        public List<String> layoutSequence;
        public int switchIntervalSeconds;
        
        public TestScenario(String name, String description) {
            this.name = name;
            this.description = description;
            this.activeChannels = new ArrayList<>();
            this.layoutSequence = new ArrayList<>();
        }
    }
    
    public static class PerformanceTargets {
        public float targetFps;
        public float minFpsThreshold;
        public int maxChannels;
        public int maxMemoryUsageMb;
        public int maxCpuUsagePercent;
    }
    
    /**
     * Load test configuration from assets
     */
    public static JSONObject loadTestConfig(Context context) {
        try {
            AssetManager assetManager = context.getAssets();
            InputStream inputStream = assetManager.open("test_config.json");
            BufferedReader reader = new BufferedReader(new InputStreamReader(inputStream));
            StringBuilder stringBuilder = new StringBuilder();
            String line;
            
            while ((line = reader.readLine()) != null) {
                stringBuilder.append(line);
            }
            
            reader.close();
            inputStream.close();
            
            return new JSONObject(stringBuilder.toString());
        } catch (IOException | JSONException e) {
            Log.e(TAG, "Failed to load test configuration", e);
            return null;
        }
    }
    
    /**
     * Parse test channels from configuration
     */
    public static List<TestChannel> parseTestChannels(JSONObject config) {
        List<TestChannel> channels = new ArrayList<>();
        
        try {
            JSONArray channelsArray = config.getJSONArray("test_channels");
            for (int i = 0; i < channelsArray.length(); i++) {
                JSONObject channelObj = channelsArray.getJSONObject(i);
                
                TestChannel channel = new TestChannel(
                    channelObj.getInt("channel_id"),
                    channelObj.getString("name"),
                    channelObj.getString("rtsp_url"),
                    channelObj.getBoolean("detection_enabled"),
                    channelObj.getString("priority")
                );
                
                channels.add(channel);
            }
        } catch (JSONException e) {
            Log.e(TAG, "Failed to parse test channels", e);
        }
        
        return channels;
    }
    
    /**
     * Parse test scenarios from configuration
     */
    public static List<TestScenario> parseTestScenarios(JSONObject config) {
        List<TestScenario> scenarios = new ArrayList<>();
        
        try {
            JSONArray scenariosArray = config.getJSONArray("test_scenarios");
            for (int i = 0; i < scenariosArray.length(); i++) {
                JSONObject scenarioObj = scenariosArray.getJSONObject(i);
                
                TestScenario scenario = new TestScenario(
                    scenarioObj.getString("name"),
                    scenarioObj.getString("description")
                );
                
                // Parse active channels
                JSONArray activeChannelsArray = scenarioObj.getJSONArray("active_channels");
                for (int j = 0; j < activeChannelsArray.length(); j++) {
                    scenario.activeChannels.add(activeChannelsArray.getInt(j));
                }
                
                // Parse layout
                if (scenarioObj.has("layout")) {
                    scenario.layout = scenarioObj.getString("layout");
                }
                
                // Parse duration
                if (scenarioObj.has("duration_seconds")) {
                    scenario.durationSeconds = scenarioObj.getInt("duration_seconds");
                }
                
                // Parse layout sequence for switching tests
                if (scenarioObj.has("layout_sequence")) {
                    JSONArray layoutSequenceArray = scenarioObj.getJSONArray("layout_sequence");
                    for (int j = 0; j < layoutSequenceArray.length(); j++) {
                        scenario.layoutSequence.add(layoutSequenceArray.getString(j));
                    }
                }
                
                // Parse switch interval
                if (scenarioObj.has("switch_interval_seconds")) {
                    scenario.switchIntervalSeconds = scenarioObj.getInt("switch_interval_seconds");
                }
                
                scenarios.add(scenario);
            }
        } catch (JSONException e) {
            Log.e(TAG, "Failed to parse test scenarios", e);
        }
        
        return scenarios;
    }
    
    /**
     * Parse performance targets from configuration
     */
    public static PerformanceTargets parsePerformanceTargets(JSONObject config) {
        PerformanceTargets targets = new PerformanceTargets();
        
        try {
            JSONObject targetsObj = config.getJSONObject("performance_targets");
            targets.targetFps = (float) targetsObj.getDouble("target_fps");
            targets.minFpsThreshold = (float) targetsObj.getDouble("min_fps_threshold");
            targets.maxChannels = targetsObj.getInt("max_channels");
            targets.maxMemoryUsageMb = targetsObj.getInt("max_memory_usage_mb");
            targets.maxCpuUsagePercent = targetsObj.getInt("max_cpu_usage_percent");
        } catch (JSONException e) {
            Log.e(TAG, "Failed to parse performance targets", e);
            // Set default values
            targets.targetFps = 30.0f;
            targets.minFpsThreshold = 25.0f;
            targets.maxChannels = 16;
            targets.maxMemoryUsageMb = 512;
            targets.maxCpuUsagePercent = 80;
        }
        
        return targets;
    }
    
    /**
     * Validate system performance against targets
     */
    public static boolean validatePerformance(float currentFps, long memoryUsageMb, 
                                            float cpuUsagePercent, PerformanceTargets targets) {
        boolean fpsValid = currentFps >= targets.minFpsThreshold;
        boolean memoryValid = memoryUsageMb <= targets.maxMemoryUsageMb;
        boolean cpuValid = cpuUsagePercent <= targets.maxCpuUsagePercent;
        
        Log.d(TAG, String.format("Performance Validation: FPS=%.2f (target>=%.2f) %s, " +
                "Memory=%dMB (target<=%dMB) %s, CPU=%.1f%% (target<=%.1f%%) %s",
                currentFps, targets.minFpsThreshold, fpsValid ? "✓" : "✗",
                memoryUsageMb, targets.maxMemoryUsageMb, memoryValid ? "✓" : "✗",
                cpuUsagePercent, targets.maxCpuUsagePercent, cpuValid ? "✓" : "✗"));
        
        return fpsValid && memoryValid && cpuValid;
    }
    
    /**
     * Log test scenario start
     */
    public static void logTestStart(TestScenario scenario) {
        Log.i(TAG, "=== Starting Test Scenario ===");
        Log.i(TAG, "Name: " + scenario.name);
        Log.i(TAG, "Description: " + scenario.description);
        Log.i(TAG, "Active Channels: " + scenario.activeChannels.toString());
        Log.i(TAG, "Layout: " + scenario.layout);
        Log.i(TAG, "Duration: " + scenario.durationSeconds + " seconds");
        Log.i(TAG, "==============================");
    }
    
    /**
     * Log test scenario completion
     */
    public static void logTestComplete(TestScenario scenario, boolean success) {
        Log.i(TAG, "=== Test Scenario Complete ===");
        Log.i(TAG, "Name: " + scenario.name);
        Log.i(TAG, "Result: " + (success ? "PASSED" : "FAILED"));
        Log.i(TAG, "===============================");
    }
}
