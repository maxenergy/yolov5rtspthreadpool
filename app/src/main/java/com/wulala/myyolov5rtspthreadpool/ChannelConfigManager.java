package com.wulala.myyolov5rtspthreadpool;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.List;

/**
 * Configuration Manager for NVR Channel Settings
 * Handles persistent storage and management of channel configurations
 */
public class ChannelConfigManager {
    
    private static final String TAG = "ChannelConfigManager";
    private static final String PREFS_NAME = "nvr_channel_config";
    private static final String KEY_CHANNEL_CONFIGS = "channel_configs";
    private static final String KEY_SYSTEM_CONFIG = "system_config";
    
    public static class SystemConfig {
        public int maxChannels = 16;
        public int defaultThreadPoolSize = 20;
        public boolean autoReconnect = true;
        public int reconnectDelay = 5; // seconds
        public int maxRetries = 3;
        public boolean enablePerformanceMonitoring = true;
        public int performanceUpdateInterval = 1000; // ms
        public boolean enableDetectionByDefault = true;
        public float targetFps = 30.0f;
        
        public JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("maxChannels", maxChannels);
            json.put("defaultThreadPoolSize", defaultThreadPoolSize);
            json.put("autoReconnect", autoReconnect);
            json.put("reconnectDelay", reconnectDelay);
            json.put("maxRetries", maxRetries);
            json.put("enablePerformanceMonitoring", enablePerformanceMonitoring);
            json.put("performanceUpdateInterval", performanceUpdateInterval);
            json.put("enableDetectionByDefault", enableDetectionByDefault);
            json.put("targetFps", targetFps);
            return json;
        }
        
        public static SystemConfig fromJson(JSONObject json) throws JSONException {
            SystemConfig config = new SystemConfig();
            config.maxChannels = json.optInt("maxChannels", 16);
            config.defaultThreadPoolSize = json.optInt("defaultThreadPoolSize", 20);
            config.autoReconnect = json.optBoolean("autoReconnect", true);
            config.reconnectDelay = json.optInt("reconnectDelay", 5);
            config.maxRetries = json.optInt("maxRetries", 3);
            config.enablePerformanceMonitoring = json.optBoolean("enablePerformanceMonitoring", true);
            config.performanceUpdateInterval = json.optInt("performanceUpdateInterval", 1000);
            config.enableDetectionByDefault = json.optBoolean("enableDetectionByDefault", true);
            config.targetFps = (float) json.optDouble("targetFps", 30.0);
            return config;
        }
    }
    
    public static class ChannelConfigExtended extends ChannelManager.ChannelConfig {
        public boolean enabled;
        public String description;
        public int streamQuality; // 0=low, 1=medium, 2=high
        public boolean audioEnabled;
        public String username;
        public String password;
        public int connectionTimeout; // seconds
        public boolean useHardwareDecoding;
        public String profileName;
        
        public ChannelConfigExtended(int channelIndex, String rtspUrl) {
            super(channelIndex, rtspUrl);
            this.enabled = false;
            this.description = "";
            this.streamQuality = 1; // medium
            this.audioEnabled = false;
            this.username = "";
            this.password = "";
            this.connectionTimeout = 10;
            this.useHardwareDecoding = true;
            this.profileName = "Default";
        }
        
        public JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("channelIndex", channelIndex);
            json.put("rtspUrl", rtspUrl);
            json.put("channelName", channelName);
            json.put("enabled", enabled);
            json.put("description", description);
            json.put("detectionEnabled", detectionEnabled);
            json.put("recordingEnabled", recordingEnabled);
            json.put("priority", priority);
            json.put("maxRetries", maxRetries);
            json.put("reconnectDelay", reconnectDelay);
            json.put("streamQuality", streamQuality);
            json.put("audioEnabled", audioEnabled);
            json.put("username", username);
            json.put("password", password);
            json.put("connectionTimeout", connectionTimeout);
            json.put("useHardwareDecoding", useHardwareDecoding);
            json.put("profileName", profileName);
            return json;
        }
        
        public static ChannelConfigExtended fromJson(JSONObject json) throws JSONException {
            int channelIndex = json.getInt("channelIndex");
            String rtspUrl = json.getString("rtspUrl");
            
            ChannelConfigExtended config = new ChannelConfigExtended(channelIndex, rtspUrl);
            config.channelName = json.optString("channelName", "Channel " + (channelIndex + 1));
            config.enabled = json.optBoolean("enabled", false);
            config.description = json.optString("description", "");
            config.detectionEnabled = json.optBoolean("detectionEnabled", true);
            config.recordingEnabled = json.optBoolean("recordingEnabled", false);
            config.priority = json.optInt("priority", 5);
            config.maxRetries = json.optInt("maxRetries", 3);
            config.reconnectDelay = json.optInt("reconnectDelay", 5);
            config.streamQuality = json.optInt("streamQuality", 1);
            config.audioEnabled = json.optBoolean("audioEnabled", false);
            config.username = json.optString("username", "");
            config.password = json.optString("password", "");
            config.connectionTimeout = json.optInt("connectionTimeout", 10);
            config.useHardwareDecoding = json.optBoolean("useHardwareDecoding", true);
            config.profileName = json.optString("profileName", "Default");
            
            return config;
        }
        
        public String getFullRtspUrl() {
            if (username.isEmpty() || password.isEmpty()) {
                return rtspUrl;
            }
            
            // Insert credentials into RTSP URL
            if (rtspUrl.startsWith("rtsp://")) {
                return "rtsp://" + username + ":" + password + "@" + rtspUrl.substring(7);
            }
            
            return rtspUrl;
        }
    }
    
    private Context context;
    private SharedPreferences prefs;
    private SystemConfig systemConfig;
    private List<ChannelConfigExtended> channelConfigs;
    
    public ChannelConfigManager(Context context) {
        this.context = context;
        this.prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        this.channelConfigs = new ArrayList<>();
        
        loadConfigurations();
    }
    
    public void loadConfigurations() {
        // Load system configuration
        String systemConfigJson = prefs.getString(KEY_SYSTEM_CONFIG, null);
        if (systemConfigJson != null) {
            try {
                JSONObject json = new JSONObject(systemConfigJson);
                systemConfig = SystemConfig.fromJson(json);
            } catch (JSONException e) {
                Log.e(TAG, "Failed to load system config", e);
                systemConfig = new SystemConfig();
            }
        } else {
            systemConfig = new SystemConfig();
        }
        
        // Load channel configurations
        String channelConfigsJson = prefs.getString(KEY_CHANNEL_CONFIGS, null);
        if (channelConfigsJson != null) {
            try {
                JSONArray jsonArray = new JSONArray(channelConfigsJson);
                channelConfigs.clear();
                
                for (int i = 0; i < jsonArray.length(); i++) {
                    JSONObject channelJson = jsonArray.getJSONObject(i);
                    ChannelConfigExtended config = ChannelConfigExtended.fromJson(channelJson);
                    channelConfigs.add(config);
                }
            } catch (JSONException e) {
                Log.e(TAG, "Failed to load channel configs", e);
                initializeDefaultChannelConfigs();
            }
        } else {
            initializeDefaultChannelConfigs();
        }
    }
    
    public void saveConfigurations() {
        SharedPreferences.Editor editor = prefs.edit();
        
        try {
            // Save system configuration
            JSONObject systemJson = systemConfig.toJson();
            editor.putString(KEY_SYSTEM_CONFIG, systemJson.toString());
            
            // Save channel configurations
            JSONArray channelArray = new JSONArray();
            for (ChannelConfigExtended config : channelConfigs) {
                channelArray.put(config.toJson());
            }
            editor.putString(KEY_CHANNEL_CONFIGS, channelArray.toString());
            
            editor.apply();
            Log.d(TAG, "Configurations saved successfully");
            
        } catch (JSONException e) {
            Log.e(TAG, "Failed to save configurations", e);
        }
    }
    
    private void initializeDefaultChannelConfigs() {
        channelConfigs.clear();
        
        // Create test configurations with 4 specified RTSP URLs for multi-channel testing
        String[] testUrls = {
            "rtsp://192.168.31.22:8554/unicast",    // Channel 0
            "rtsp://192.168.31.147:8554/unicast",   // Channel 1
            "rtsp://192.168.31.22:8554/unicast",    // Channel 2
            "rtsp://192.168.31.147:8554/unicast"    // Channel 3
        };

        for (int i = 0; i < systemConfig.maxChannels; i++) {
            String rtspUrl = (i < testUrls.length) ? testUrls[i] : "";
            ChannelConfigExtended config = new ChannelConfigExtended(i, rtspUrl);
            config.channelName = "Test Channel " + (i + 1);
            // Enable all 4 test channels to verify multi-channel layout switching
            config.enabled = (i < 4); // Enable first 4 channels for testing
            config.description = "Test channel " + (i + 1) + " - " +
                               (i % 2 == 0 ? "192.168.31.22" : "192.168.31.147");
            config.detectionEnabled = systemConfig.enableDetectionByDefault;

            channelConfigs.add(config);
        }
    }
    
    // Getters and Setters
    public SystemConfig getSystemConfig() {
        return systemConfig;
    }
    
    public void setSystemConfig(SystemConfig systemConfig) {
        this.systemConfig = systemConfig;
        saveConfigurations();
    }
    
    public List<ChannelConfigExtended> getChannelConfigs() {
        return new ArrayList<>(channelConfigs);
    }
    
    public ChannelConfigExtended getChannelConfig(int channelIndex) {
        for (ChannelConfigExtended config : channelConfigs) {
            if (config.channelIndex == channelIndex) {
                return config;
            }
        }
        return null;
    }
    
    public void setChannelConfig(ChannelConfigExtended config) {
        for (int i = 0; i < channelConfigs.size(); i++) {
            if (channelConfigs.get(i).channelIndex == config.channelIndex) {
                channelConfigs.set(i, config);
                saveConfigurations();
                return;
            }
        }
        
        // Add new config if not found
        channelConfigs.add(config);
        saveConfigurations();
    }
    
    public List<ChannelConfigExtended> getEnabledChannels() {
        List<ChannelConfigExtended> enabled = new ArrayList<>();
        for (ChannelConfigExtended config : channelConfigs) {
            if (config.enabled) {
                enabled.add(config);
            }
        }
        return enabled;
    }
    
    public void enableChannel(int channelIndex, boolean enabled) {
        ChannelConfigExtended config = getChannelConfig(channelIndex);
        if (config != null) {
            config.enabled = enabled;
            saveConfigurations();
        }
    }
    
    public void updateChannelRtspUrl(int channelIndex, String rtspUrl) {
        ChannelConfigExtended config = getChannelConfig(channelIndex);
        if (config != null) {
            config.rtspUrl = rtspUrl;
            saveConfigurations();
        }
    }
    
    public void resetToDefaults() {
        systemConfig = new SystemConfig();
        initializeDefaultChannelConfigs();
        saveConfigurations();
    }
    
    // Import/Export functionality
    public String exportConfiguration() {
        try {
            JSONObject exportJson = new JSONObject();
            exportJson.put("systemConfig", systemConfig.toJson());
            
            JSONArray channelArray = new JSONArray();
            for (ChannelConfigExtended config : channelConfigs) {
                channelArray.put(config.toJson());
            }
            exportJson.put("channelConfigs", channelArray);
            
            return exportJson.toString(2); // Pretty print with 2-space indentation
            
        } catch (JSONException e) {
            Log.e(TAG, "Failed to export configuration", e);
            return null;
        }
    }
    
    public boolean importConfiguration(String configJson) {
        try {
            JSONObject importJson = new JSONObject(configJson);
            
            // Import system config
            if (importJson.has("systemConfig")) {
                systemConfig = SystemConfig.fromJson(importJson.getJSONObject("systemConfig"));
            }
            
            // Import channel configs
            if (importJson.has("channelConfigs")) {
                JSONArray channelArray = importJson.getJSONArray("channelConfigs");
                channelConfigs.clear();
                
                for (int i = 0; i < channelArray.length(); i++) {
                    JSONObject channelJson = channelArray.getJSONObject(i);
                    ChannelConfigExtended config = ChannelConfigExtended.fromJson(channelJson);
                    channelConfigs.add(config);
                }
            }
            
            saveConfigurations();
            return true;
            
        } catch (JSONException e) {
            Log.e(TAG, "Failed to import configuration", e);
            return false;
        }
    }
}
