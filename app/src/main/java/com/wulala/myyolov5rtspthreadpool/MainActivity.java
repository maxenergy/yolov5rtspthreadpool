package com.wulala.myyolov5rtspthreadpool;

import androidx.appcompat.app.AppCompatActivity;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

import java.util.List;

import org.json.JSONObject;

import com.wulala.myyolov5rtspthreadpool.databinding.ActivityMainBinding;

public class MainActivity extends AppCompatActivity implements SurfaceHolder.Callback {

    private static final String TAG = "MainActivity";

    static {
        System.loadLibrary("myyolov5rtspthreadpool");
    }

    private ActivityMainBinding binding;
    AssetManager assetManager;
    private long nativePlayerObj = 0;
    private SurfaceView surfaceView;
    private SurfaceHolder surfaceHolder;
    private NVRLayoutManager layoutManager;
    private MultiSurfaceManager surfaceManager;
    private ChannelManager channelManager;
    private ChannelConfigManager configManager;

    // Testing support
    private boolean testModeEnabled = false;
    private TestUtils.TestScenario currentTestScenario;
    private long testStartTime;
    private TestUtils.PerformanceTargets performanceTargets;
    private TestLauncher testLauncher;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Enable full-screen immersive mode for NVR interface
        enableFullScreenMode();

        // Use NVR layout instead of main layout
        setContentView(R.layout.activity_nvr);

        // Initialize Multi-Surface Manager
        surfaceManager = new MultiSurfaceManager();
        surfaceManager.setListener(new MultiSurfaceManager.SurfaceEventListener() {
            @Override
            public void onSurfaceCreated(int channelIndex, android.view.Surface surface) {
                Log.d(TAG, "Surface created callback for channel " + channelIndex + ", surface: " + surface);

                // Set surface in ChannelManager first
                if (channelManager != null) {
                    Log.d(TAG, "Setting surface in ChannelManager for channel " + channelIndex);
                    channelManager.setChannelSurface(channelIndex, surface);

                    // If this is a channel that should be running, try to restart it with the new surface
                    List<ChannelConfigManager.ChannelConfigExtended> enabledChannels = configManager.getEnabledChannels();
                    for (ChannelConfigManager.ChannelConfigExtended config : enabledChannels) {
                        if (config.channelIndex == channelIndex && config.enabled && !config.rtspUrl.isEmpty()) {
                            Log.d(TAG, "Surface ready for channel " + channelIndex + ", restarting if needed");
                            // Small delay to ensure surface is fully ready
                            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                                if (!channelManager.isChannelRunning(channelIndex)) {
                                    Log.d(TAG, "Restarting channel " + channelIndex + " with new surface");
                                    channelManager.startChannel(channelIndex);
                                }
                            }, 100);
                            break;
                        }
                    }
                } else {
                    Log.w(TAG, "ChannelManager not initialized yet for channel " + channelIndex);
                }

                // Also notify native code for backward compatibility
                setNativeSurfaceForChannel(channelIndex, surface);
            }

            @Override
            public void onSurfaceChanged(int channelIndex, android.view.Surface surface, int format, int width, int height) {
                Log.d(TAG, "Surface changed for channel " + channelIndex + ", size: " + width + "x" + height);
                // Handle surface changes if needed
            }

            @Override
            public void onSurfaceDestroyed(int channelIndex, android.view.Surface surface) {
                Log.d(TAG, "Surface destroyed for channel " + channelIndex);

                // Clear surface in ChannelManager
                if (channelManager != null) {
                    channelManager.setChannelSurface(channelIndex, null);
                }

                // Notify native code about surface destruction
                setNativeSurfaceForChannel(channelIndex, null);
            }
        });

        // Initialize NVR Layout Manager
        layoutManager = new NVRLayoutManager(this, findViewById(R.id.video_container));

        // Initialize SurfaceView
        surfaceView = findViewById(R.id.surface_view);
        surfaceHolder = surfaceView.getHolder();
        surfaceHolder.addCallback(this);

        // Add main surface to surface manager
        surfaceManager.addSurface(0, surfaceView);

        // Initialize AssetManager first (required for model loading)
        assetManager = getAssets();
        setNativeAssetManager(assetManager);

        // Initialize Configuration Manager
        configManager = new ChannelConfigManager(this);

        // Initialize Channel Manager (this will load model data)
        initializeChannelManager();

        // Restore layout state from preferences
        restoreLayoutState();

        // Start surface health monitoring
        startSurfaceHealthMonitoring();

        // Initialize all channel surfaces after ChannelManager is ready
        // Use a small delay to ensure all components are properly initialized
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            Log.d(TAG, "Initializing channel surfaces after ChannelManager setup...");
            initializeChannelSurfaces();
        }, 500); // 500ms delay

        // Setup layout change listener
        layoutManager.setLayoutChangeListener(new NVRLayoutManager.LayoutChangeListener() {
            @Override
            public void onLayoutChanged(NVRLayoutManager.LayoutMode newMode, NVRLayoutManager.LayoutMode oldMode) {
                Log.d(TAG, "Layout changed from " + oldMode + " to " + newMode);
                updateNativeLayoutMode(newMode.channels);

                // Save layout state when user changes layout
                saveLayoutState(newMode);

                // Re-initialize surfaces after layout change with proper delay
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                    Log.d(TAG, "Re-initializing surfaces after layout change...");
                    initializeChannelSurfacesWithRetry(0);
                }, 300); // Give time for layout to settle
            }

            @Override
            public void onChannelSelected(int channelIndex) {
                // Handle channel selection
                setActiveChannel(channelIndex);
            }

            @Override
            public void onChannelDoubleClicked(int channelIndex) {
                // Switch to single channel view
                layoutManager.switchLayout(NVRLayoutManager.LayoutMode.SINGLE);
            }
        });

        // Temporarily disable single-channel player to focus on multi-channel system
        Log.d(TAG, "Skipping single-channel player initialization, using multi-channel system only");
        nativePlayerObj = 0;

        // Initialize test launcher for automated testing
        testLauncher = new TestLauncher(this);
        testLauncher.register();
    }

    private void enableFullScreenMode() {
        // Hide system UI for clean NVR interface
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN);

        // Keep screen on for NVR monitoring
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            // Re-enable full screen when window regains focus
            enableFullScreenMode();
        }
    }

    // SurfaceHolder.Callback methods
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // Surface is created, set it to native code
        setNativeSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Surface dimensions changed
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Surface is destroyed, clear native surface
        setNativeSurface(null);
    }

    // Initialize Channel Manager with model data and event listeners
    private void initializeChannelManager() {
        channelManager = new ChannelManager(this);

        // Set up event listener for channel state changes
        channelManager.setEventListener(new ChannelManager.ChannelEventListener() {
            @Override
            public void onChannelStateChanged(int channelIndex, ChannelManager.ChannelState oldState, ChannelManager.ChannelState newState) {
                runOnUiThread(() -> {
                    // Update UI based on channel state changes
                    updateChannelStatusUI(channelIndex, newState);
                });
            }

            @Override
            public void onChannelError(int channelIndex, String errorMessage) {
                runOnUiThread(() -> {
                    // Handle channel errors
                    handleChannelError(channelIndex, errorMessage);
                });
            }

            @Override
            public void onChannelFrameReceived(int channelIndex, long frameTime) {
                // Update frame statistics (called frequently, avoid heavy UI operations)
                updateFrameStatistics(channelIndex, frameTime);
            }

            @Override
            public void onChannelDetection(int channelIndex, int detectionCount) {
                // Update detection statistics
                updateDetectionStatistics(channelIndex, detectionCount);
            }

            @Override
            public void onSystemPerformanceUpdate(float overallFps, int activeChannels) {
                runOnUiThread(() -> {
                    // Update system performance display
                    layoutManager.updateStatus("NVR System - " + activeChannels + " channels", (int)overallFps);
                });
            }
        });

        // Initialize with model data
        try {
            Log.d(TAG, "Loading model data from assets...");
            byte[] modelData = loadModelDataFromAssets();
            Log.d(TAG, "Model data loaded, size: " + modelData.length + " bytes");

            Log.d(TAG, "Setting shared model data to ChannelManager...");
            channelManager.setSharedModelData(modelData, modelData.length);
            Log.d(TAG, "ChannelManager initialized successfully");

            // Set surface recovery listener
            channelManager.setSurfaceRecoveryListener(new ChannelManager.SurfaceRecoveryListener() {
                @Override
                public void onSurfaceRecoveryNeeded(int channelIndex) {
                    Log.w(TAG, "Surface recovery needed for channel " + channelIndex);
                    runOnUiThread(() -> {
                        recoverChannelSurface(channelIndex);
                    });
                }
            });

            // Configure channels from saved configuration
            Log.d(TAG, "Configuring channels from config...");
            configureChannelsFromConfig();

            // Auto-start enabled channels after a short delay
            Log.d(TAG, "Scheduling channel startup in 2 seconds...");
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                Log.d(TAG, "Starting configured channels...");
                startConfiguredChannels();
            }, 2000); // 2 second delay to ensure surfaces are ready

        } catch (Exception e) {
            Log.e(TAG, "Failed to initialize ChannelManager", e);
            e.printStackTrace();
        }
    }

    // Initialize all channel surfaces for multi-channel support
    private void initializeChannelSurfaces() {
        List<SurfaceView> channelSurfaces = layoutManager.getChannelSurfaces();

        Log.d(TAG, "Initializing channel surfaces, total count: " + channelSurfaces.size());

        // Initialize all channel surfaces including channel 0
        for (int i = 0; i < channelSurfaces.size(); i++) {
            SurfaceView surfaceView = channelSurfaces.get(i);
            if (surfaceView != null) {
                Log.d(TAG, "Processing channel " + i + " SurfaceView: " + surfaceView);

                // Add surface to surface manager (skip if already added for channel 0)
                if (i != 0) {
                    surfaceManager.addSurface(i, surfaceView);
                }

                // Get Surface from SurfaceHolder
                SurfaceHolder holder = surfaceView.getHolder();
                Surface surface = holder.getSurface();

                Log.d(TAG, "Channel " + i + " - SurfaceHolder: " + holder + ", Surface: " + surface);
                Log.d(TAG, "Channel " + i + " - Surface valid: " + (surface != null && surface.isValid()));

                // Only set surface if it's valid, otherwise rely on SurfaceHolder callback
                if (surface != null && surface.isValid()) {
                    Log.d(TAG, "Setting valid surface for channel " + i);
                    channelManager.setChannelSurface(i, surface);
                } else {
                    Log.w(TAG, "Surface for channel " + i + " is not ready yet, will be set via callback");
                    // Surface will be set when SurfaceHolder.Callback.surfaceCreated is called
                }
            } else {
                Log.w(TAG, "SurfaceView for channel " + i + " is null");
            }
        }
    }

    private void initializeChannelSurfacesWithRetry(int retryCount) {
        final int MAX_RETRIES = 5;
        final int RETRY_DELAY_MS = 200;

        Log.d(TAG, "Initializing channel surfaces with retry, attempt: " + (retryCount + 1));

        List<SurfaceView> channelSurfaces = layoutManager.getChannelSurfaces();
        int validSurfaces = 0;
        int totalSurfaces = channelSurfaces.size();

        for (int i = 0; i < totalSurfaces; i++) {
            SurfaceView surfaceView = channelSurfaces.get(i);

            if (surfaceView != null) {
                // Add to surface manager if not already added
                if (i != 0) { // Skip channel 0 as it's already added
                    surfaceManager.addSurface(i, surfaceView);
                }

                SurfaceHolder holder = surfaceView.getHolder();
                Surface surface = holder.getSurface();

                Log.d(TAG, "Channel " + i + " - Surface valid: " + (surface != null && surface.isValid()));

                if (surface != null && surface.isValid()) {
                    Log.d(TAG, "Setting valid surface for channel " + i);
                    channelManager.setChannelSurface(i, surface);
                    validSurfaces++;
                } else {
                    Log.w(TAG, "Surface for channel " + i + " is not ready yet");
                }
            }
        }

        Log.d(TAG, "Valid surfaces: " + validSurfaces + "/" + totalSurfaces);

        // If we don't have enough valid surfaces and haven't exceeded max retries, try again
        if (validSurfaces < Math.min(4, totalSurfaces) && retryCount < MAX_RETRIES) {
            Log.d(TAG, "Not enough valid surfaces, retrying in " + RETRY_DELAY_MS + "ms...");
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                initializeChannelSurfacesWithRetry(retryCount + 1);
            }, RETRY_DELAY_MS);
        } else {
            Log.d(TAG, "Surface initialization complete. Valid surfaces: " + validSurfaces);
        }
    }

    // Helper methods for layout management
    private void updateNativeLayoutMode(int channelCount) {
        // TODO: Implement native method to update channel count
        // This will be called when layout changes to inform native code
        setMultiChannelMode(channelCount);
    }

    private void setActiveChannel(int channelIndex) {
        // TODO: Implement channel selection logic
        // This will be used to focus on specific channel
        setNativeActiveChannel(channelIndex);
    }

    private void setNativeSurfaceForChannel(int channelIndex, android.view.Surface surface) {
        if (channelIndex == 0) {
            // Main surface (backward compatibility)
            setNativeSurface(surface);
        } else {
            // Additional channel surfaces
            // TODO: Implement native method for multi-channel surfaces
            setNativeChannelSurface(channelIndex, surface);
        }
    }

    // jni native methods
    private native long prepareNative();
    private native void setNativeAssetManager(AssetManager assetManager);
    private native void setNativeSurface(Object surface);

    // Native methods for multi-channel support (to be implemented in C++)
    private native void setMultiChannelMode(int channelCount);
    private native void setChannelRTSPUrl(int channelIndex, String url);
    private native void setNativeActiveChannel(int channelIndex);
    private native void setNativeChannelSurface(int channelIndex, Object surface);

    // Channel management helper methods
    private void updateChannelStatusUI(int channelIndex, ChannelManager.ChannelState newState) {
        // Update channel label or status indicator based on state
        String statusText = "CH" + (channelIndex + 1) + ": ";
        switch (newState) {
            case INACTIVE:
                statusText += "Inactive";
                break;
            case CONNECTING:
                statusText += "Connecting...";
                break;
            case ACTIVE:
                statusText += "Active";
                break;
            case ERROR:
                statusText += "Error";
                break;
            case RECONNECTING:
                statusText += "Reconnecting...";
                break;
        }

        // Update channel label if available
        layoutManager.setChannelLabel(channelIndex, statusText);
    }

    private void handleChannelError(int channelIndex, String errorMessage) {
        // Log error and potentially show user notification
        android.util.Log.e("ChannelManager", "Channel " + channelIndex + " error: " + errorMessage);

        // Update channel status
        layoutManager.setChannelActive(channelIndex, false);
    }

    private void updateFrameStatistics(int channelIndex, long frameTime) {
        // Update frame statistics (avoid heavy operations as this is called frequently)
        // This could be used to calculate FPS or detect frame drops
    }

    private void updateDetectionStatistics(int channelIndex, int detectionCount) {
        // Update detection statistics for the channel
        // This could be used to show detection counts in the UI
    }

    private void configureChannelsFromConfig() {
        // Load and configure channels from saved configuration
        List<ChannelConfigManager.ChannelConfigExtended> enabledChannels = configManager.getEnabledChannels();
        Log.d(TAG, "Found " + enabledChannels.size() + " enabled channels in config");

        if (enabledChannels.isEmpty()) {
            // No saved configuration, use demo configuration
            Log.d(TAG, "No saved configuration found, using demo channels");
            //configureDemoChannels();
            return;
        }

        for (ChannelConfigManager.ChannelConfigExtended config : enabledChannels) {
            // Convert extended config to basic channel config
            ChannelManager.ChannelConfig basicConfig = new ChannelManager.ChannelConfig(
                config.channelIndex,
                config.getFullRtspUrl()
            );
            basicConfig.channelName = config.channelName;
            basicConfig.detectionEnabled = config.detectionEnabled;
            basicConfig.recordingEnabled = config.recordingEnabled;
            basicConfig.priority = config.priority;
            basicConfig.maxRetries = config.maxRetries;
            basicConfig.reconnectDelay = config.reconnectDelay;

            // Configure the channel
            channelManager.configureChannel(basicConfig);

            // Update UI
            layoutManager.setChannelLabel(config.channelIndex, config.channelName);
            layoutManager.setChannelActive(config.channelIndex, true);
        }
    }

    private void configureDemoChannels() {
        // Configure demo channels with 4 test RTSP URLs
        String[] demoUrls = {
            "rtsp://admin:sharpi1688@192.168.1.235:554/live?profile=Profile_0000",
            "rtsp://192.168.1.137:554/live/2",
            "rtsp://admin:sharpi1688@192.168.1.2:554/1/1",
            "rtsp://admin:sharpi1688@192.168.1.127:554/"
        };

        for (int i = 0; i < Math.min(demoUrls.length, 4); i++) {
            Log.d(TAG, "Configuring demo channel " + i + " with URL: " + demoUrls[i]);
            ChannelManager.ChannelConfig config = new ChannelManager.ChannelConfig(i, demoUrls[i]);
            config.channelName = "Demo Channel " + (i + 1);
            config.detectionEnabled = true;

            channelManager.configureChannel(config);

            // Update UI
            layoutManager.setChannelLabel(config.channelIndex, config.channelName);
            layoutManager.setChannelActive(config.channelIndex, true);
        }
    }

    private void startConfiguredChannels() {
        // Start all enabled channels
        Log.d(TAG, "Getting enabled channels from config manager...");

        List<ChannelConfigManager.ChannelConfigExtended> enabledChannels = configManager.getEnabledChannels();
        Log.d(TAG, "Found " + enabledChannels.size() + " enabled channels");

        // Check if user has explicitly configured multiple channels with valid URLs
        int channelsWithValidUrls = 0;
        for (ChannelConfigManager.ChannelConfigExtended config : enabledChannels) {
            if (config.enabled && !config.rtspUrl.isEmpty() && !isDefaultUrl(config.rtspUrl)) {
                channelsWithValidUrls++;
            }
        }

        // Only switch to multi-channel layout if user has explicitly configured multiple channels
        // with valid (non-default) RTSP URLs, indicating intentional multi-channel setup
        if (channelsWithValidUrls > 1) {
            Log.d(TAG, "Multiple channels with valid URLs detected (" + channelsWithValidUrls + "), switching to 2x2 layout mode");
            layoutManager.switchLayout(NVRLayoutManager.LayoutMode.QUAD);

            // Surface initialization will be handled in onLayoutChanged callback
            // Start channels after a short delay to allow layout to settle
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                Log.d(TAG, "Starting channels after layout switch...");
                for (ChannelConfigManager.ChannelConfigExtended config : enabledChannels) {
                    Log.d(TAG, "Checking channel " + config.channelIndex + ", enabled: " + config.enabled + ", URL: " + config.rtspUrl);
                    if (config.enabled && !config.rtspUrl.isEmpty()) {
                        Log.d(TAG, "Starting channel " + config.channelIndex);
                        channelManager.startChannel(config.channelIndex);
                    }
                }
            }, 1500); // Wait for layout change and surface initialization
        } else {
            // Single channel mode - either one channel or multiple channels with default/empty URLs
            Log.d(TAG, "Single channel mode, keeping current layout (enabled channels: " + enabledChannels.size() + ", valid URLs: " + channelsWithValidUrls + ")");
            channelManager.debugSurfaceStates();

            for (ChannelConfigManager.ChannelConfigExtended config : enabledChannels) {
                Log.d(TAG, "Checking channel " + config.channelIndex + ", enabled: " + config.enabled + ", URL: " + config.rtspUrl);
                if (config.enabled && !config.rtspUrl.isEmpty()) {
                    Log.d(TAG, "Starting channel " + config.channelIndex);
                    channelManager.startChannel(config.channelIndex);
                }
            }
        }
    }

    /**
     * Check if the given URL is a default/demo URL that doesn't represent user configuration
     * Note: Test URLs (192.168.31.22:8554 and 192.168.31.147:8554) are considered valid user configuration
     */
    private boolean isDefaultUrl(String url) {
        if (url == null || url.isEmpty()) {
            return true;
        }

        // Check for old default/demo URLs that indicate default configuration
        // The new test URLs (192.168.31.22:8554 and 192.168.31.147:8554) are NOT considered default
        String[] oldDefaultUrls = {
            "rtsp://admin:sharpi1688@192.168.1.235:554/live?profile=Profile_0000",
            "rtsp://192.168.1.137:554/live/2",
            "rtsp://admin:sharpi1688@192.168.1.2:554/1/1",
            "rtsp://admin:sharpi1688@192.168.1.127:554/"
        };

        for (String defaultUrl : oldDefaultUrls) {
            if (url.equals(defaultUrl)) {
                return true;
            }
        }

        // Test URLs for multi-channel testing are considered valid user configuration
        // so they will trigger automatic layout switching
        return false;
    }

    private void stopAllChannels() {
        // Stop all active channels
        for (int i = 0; i < 16; i++) {
            channelManager.stopChannel(i);
        }
    }

    private byte[] loadModelDataFromAssets() throws Exception {
        // Load YOLOv5 model data from assets
        // Try quantized model first, fallback to regular model
        String[] modelFiles = {"yolov5s_quant.rknn", "yolov5s.rknn"};

        for (String modelFile : modelFiles) {
            try {
                Log.d(TAG, "Attempting to load model: " + modelFile);
                java.io.InputStream inputStream = assetManager.open(modelFile);
                byte[] buffer = new byte[inputStream.available()];
                int bytesRead = inputStream.read(buffer);
                inputStream.close();

                if (bytesRead > 0) {
                    Log.d(TAG, "Successfully loaded model: " + modelFile + " (" + bytesRead + " bytes)");
                    return buffer;
                }
            } catch (Exception e) {
                Log.w(TAG, "Failed to load model " + modelFile + ": " + e.getMessage());
            }
        }

        throw new Exception("Failed to load any YOLOv5 model file");
    }

    // Demo method to start multiple channels with test RTSP URLs
    private void startDemoChannels() {
        // Configure and start demo channels with 4 test RTSP URLs
        String[] demoUrls = {
            "rtsp://admin:sharpi1688@192.168.1.235:554/live?profile=Profile_0000",
            "rtsp://192.168.1.137:554/live/2",
            "rtsp://admin:sharpi1688@192.168.1.2:554/1/1",
            "rtsp://admin:sharpi1688@192.168.1.127:554/"
        };

        for (int i = 0; i < Math.min(demoUrls.length, 4); i++) {
            ChannelManager.ChannelConfig config = new ChannelManager.ChannelConfig(i, demoUrls[i]);
            config.channelName = "Demo Channel " + (i + 1);
            config.detectionEnabled = true;

            channelManager.configureChannel(config);
            channelManager.startChannel(i);
        }
    }

    // Testing methods
    public void enableTestMode() {
        testModeEnabled = true;

        // Load test configuration
        JSONObject testConfig = TestUtils.loadTestConfig(this);
        if (testConfig != null) {
            performanceTargets = TestUtils.parsePerformanceTargets(testConfig);
            android.util.Log.i("MainActivity", "Test mode enabled with performance targets");
        }
    }

    public void runTestScenario(String scenarioName) {
        if (!testModeEnabled) {
            enableTestMode();
        }

        JSONObject testConfig = TestUtils.loadTestConfig(this);
        if (testConfig == null) {
            android.util.Log.e("MainActivity", "Failed to load test configuration");
            return;
        }

        List<TestUtils.TestScenario> scenarios = TestUtils.parseTestScenarios(testConfig);
        TestUtils.TestScenario targetScenario = null;

        for (TestUtils.TestScenario scenario : scenarios) {
            if (scenario.name.equals(scenarioName)) {
                targetScenario = scenario;
                break;
            }
        }

        if (targetScenario == null) {
            android.util.Log.e("MainActivity", "Test scenario not found: " + scenarioName);
            return;
        }

        runTestScenario(targetScenario);
    }

    private void runTestScenario(TestUtils.TestScenario scenario) {
        currentTestScenario = scenario;
        testStartTime = System.currentTimeMillis();

        TestUtils.logTestStart(scenario);

        // Stop all current channels
        stopAllChannels();

        // Wait a moment for cleanup
        surfaceView.postDelayed(() -> {
            // Configure test channels
            configureTestChannels(scenario);

            // Set layout
            setTestLayout(scenario.layout);

            // Start test channels
            startTestChannels(scenario);

            // Schedule test completion check
            if (scenario.durationSeconds > 0) {
                surfaceView.postDelayed(() -> {
                    completeTestScenario(scenario);
                }, scenario.durationSeconds * 1000);
            }
        }, 1000);
    }

    private void configureTestChannels(TestUtils.TestScenario scenario) {
        JSONObject testConfig = TestUtils.loadTestConfig(this);
        if (testConfig == null) return;

        List<TestUtils.TestChannel> testChannels = TestUtils.parseTestChannels(testConfig);

        for (int channelIndex : scenario.activeChannels) {
            if (channelIndex < testChannels.size()) {
                TestUtils.TestChannel testChannel = testChannels.get(channelIndex);

                ChannelManager.ChannelConfig config = new ChannelManager.ChannelConfig(
                    testChannel.channelId, testChannel.rtspUrl);
                config.channelName = testChannel.name;
                config.detectionEnabled = testChannel.detectionEnabled;

                channelManager.configureChannel(config);
                layoutManager.setChannelLabel(channelIndex, testChannel.name);
            }
        }
    }

    private void setTestLayout(String layout) {
        NVRLayoutManager.LayoutMode layoutMode;
        switch (layout) {
            case "1x1":
                layoutMode = NVRLayoutManager.LayoutMode.SINGLE;
                break;
            case "2x2":
                layoutMode = NVRLayoutManager.LayoutMode.QUAD;
                break;
            case "3x3":
                layoutMode = NVRLayoutManager.LayoutMode.NINE;
                break;
            case "4x4":
                layoutMode = NVRLayoutManager.LayoutMode.SIXTEEN;
                break;
            default:
                layoutMode = NVRLayoutManager.LayoutMode.QUAD;
                break;
        }

        layoutManager.switchLayout(layoutMode);
    }

    private void startTestChannels(TestUtils.TestScenario scenario) {
        for (int channelIndex : scenario.activeChannels) {
            channelManager.startChannel(channelIndex);
        }
    }

    private void completeTestScenario(TestUtils.TestScenario scenario) {
        long testDuration = System.currentTimeMillis() - testStartTime;

        // Validate performance
        float currentFps = channelManager.getSystemFps();
        long memoryUsage = getMemoryUsage();
        float cpuUsage = getCpuUsage();

        boolean success = TestUtils.validatePerformance(currentFps, memoryUsage, cpuUsage, performanceTargets);

        TestUtils.logTestComplete(scenario, success);

        android.util.Log.i("MainActivity", String.format("Test completed in %d ms", testDuration));

        currentTestScenario = null;
    }

    private long getMemoryUsage() {
        Runtime runtime = Runtime.getRuntime();
        return (runtime.totalMemory() - runtime.freeMemory()) / (1024 * 1024); // MB
    }

    private float getCpuUsage() {
        // Simplified CPU usage estimation
        // In a real implementation, you might use more sophisticated methods
        return 50.0f; // Placeholder
    }

    // Quick test methods for debugging
    public void runQuickTest() {
        runTestScenario("Quad Channel Test");
    }

    public void runStressTest() {
        runTestScenario("Performance Stress Test");
    }

    // Surface health monitoring and recovery
    private android.os.Handler surfaceHealthHandler;
    private Runnable surfaceHealthChecker;

    private void startSurfaceHealthMonitoring() {
        surfaceHealthHandler = new android.os.Handler(android.os.Looper.getMainLooper());
        surfaceHealthChecker = new Runnable() {
            @Override
            public void run() {
                if (channelManager != null) {
                    try {
                        channelManager.checkSurfaceHealth();
                    } catch (Exception e) {
                        Log.e(TAG, "Error during surface health check", e);
                    }
                }

                // Schedule next check in 5 seconds
                surfaceHealthHandler.postDelayed(this, 5000);
            }
        };

        // Start monitoring after 10 seconds to allow initialization
        surfaceHealthHandler.postDelayed(surfaceHealthChecker, 10000);
        Log.d(TAG, "Surface health monitoring started");
    }

    private void recoverChannelSurface(int channelIndex) {
        Log.d(TAG, "Attempting to recover surface for channel " + channelIndex);

        try {
            // Get the surface view for this channel
            SurfaceView surfaceView = layoutManager.getChannelSurface(channelIndex);

            if (surfaceView != null) {
                SurfaceHolder holder = surfaceView.getHolder();
                Surface surface = holder.getSurface();

                if (surface != null && surface.isValid()) {
                    Log.d(TAG, "Re-setting surface for channel " + channelIndex);
                    channelManager.setChannelSurface(channelIndex, surface);
                } else {
                    Log.w(TAG, "Surface for channel " + channelIndex + " is not valid, triggering surface re-initialization");
                    // Force surface recreation by briefly hiding and showing the view
                    surfaceView.setVisibility(View.GONE);
                    surfaceView.post(() -> {
                        surfaceView.setVisibility(View.VISIBLE);
                    });
                }
            } else {
                Log.e(TAG, "No surface view found for channel " + channelIndex);
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to recover surface for channel " + channelIndex, e);
        }
    }

    @Override
    protected void onDestroy() {
        // Stop surface health monitoring
        if (surfaceHealthHandler != null && surfaceHealthChecker != null) {
            surfaceHealthHandler.removeCallbacks(surfaceHealthChecker);
        }
        super.onDestroy();

        // Cleanup test launcher
        if (testLauncher != null) {
            testLauncher.unregister();
        }

        // Cleanup channel manager
        if (channelManager != null) {
            channelManager.cleanup();
        }

        // Cleanup surface manager
        if (surfaceManager != null) {
            surfaceManager.cleanup();
        }
    }

    /**
     * Save current layout state to preferences
     */
    private void saveLayoutState(NVRLayoutManager.LayoutMode layoutMode) {
        android.content.SharedPreferences prefs = getSharedPreferences("nvr_layout_prefs", MODE_PRIVATE);
        android.content.SharedPreferences.Editor editor = prefs.edit();
        editor.putString("layout_mode", layoutMode.name());
        editor.putLong("layout_save_time", System.currentTimeMillis());
        editor.apply();
        Log.d(TAG, "Layout state saved: " + layoutMode.name());
    }

    /**
     * Restore layout state from preferences
     */
    private void restoreLayoutState() {
        android.content.SharedPreferences prefs = getSharedPreferences("nvr_layout_prefs", MODE_PRIVATE);
        String savedLayoutMode = prefs.getString("layout_mode", null);
        long saveTime = prefs.getLong("layout_save_time", 0);

        // Only restore if saved within last 24 hours to avoid stale state
        long currentTime = System.currentTimeMillis();
        boolean isRecentSave = (currentTime - saveTime) < (24 * 60 * 60 * 1000); // 24 hours

        if (savedLayoutMode != null && isRecentSave) {
            try {
                NVRLayoutManager.LayoutMode layoutMode = NVRLayoutManager.LayoutMode.valueOf(savedLayoutMode);
                Log.d(TAG, "Restoring layout state: " + layoutMode.name());

                // Restore layout after a short delay to ensure UI is ready
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                    layoutManager.switchLayout(layoutMode);
                }, 500);

            } catch (IllegalArgumentException e) {
                Log.w(TAG, "Invalid saved layout mode: " + savedLayoutMode, e);
            }
        } else {
            Log.d(TAG, "No recent layout state to restore (saved: " + savedLayoutMode + ", recent: " + isRecentSave + ")");
        }
    }

}