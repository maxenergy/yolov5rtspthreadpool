package com.wulala.myyolov5rtspthreadpool;

import android.app.Activity;
import android.content.Context;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.GridLayout;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Button;
import android.animation.ObjectAnimator;
import android.animation.AnimatorSet;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.Gravity;
import android.graphics.drawable.GradientDrawable;
import android.graphics.Rect;
import android.util.TypedValue;
import android.content.SharedPreferences;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.HashMap;

/**
 * NVR Layout Manager for handling multi-channel video display layouts
 * Supports 1x1, 2x2, 3x3, and 4x4 grid layouts with smooth transitions
 */
public class NVRLayoutManager {
    private static final String TAG = "NVRLayoutManager";
    
    public enum LayoutMode {
        SINGLE(1, 1, 1),
        QUAD(2, 2, 4),
        NINE(3, 3, 9),
        SIXTEEN(4, 4, 16);
        
        public final int rows;
        public final int cols;
        public final int channels;
        
        LayoutMode(int rows, int cols, int channels) {
            this.rows = rows;
            this.cols = cols;
            this.channels = channels;
        }
    }
    
    public interface LayoutChangeListener {
        void onLayoutChanged(LayoutMode newMode, LayoutMode oldMode);
        void onChannelSelected(int channelIndex);
        void onChannelDoubleClicked(int channelIndex);
    }
    
    private Context context;
    private ViewGroup rootContainer;
    private SurfaceView singleSurfaceView;
    private ViewGroup gridScrollContainer;
    private GridLayout gridContainer;
    private ViewGroup layoutControls;
    private ViewGroup statusOverlay;
    
    private LayoutMode currentMode = LayoutMode.SINGLE;
    private int selectedChannel = 0;
    private boolean controlsVisible = false;
    private boolean statusVisible = false;
    
    private List<SurfaceView> channelSurfaces;
    private List<TextView> channelLabels;
    private List<Button> layoutButtons;
    private List<GestureDetector> channelGestureDetectors;
    private LayoutChangeListener listener;
    private GridLayoutCalculator layoutCalculator;

    // Animation duration for layout transitions
    private static final int ANIMATION_DURATION = 300;

    // Double-click detection
    private static final int DOUBLE_CLICK_TIME_DELTA = 300; // milliseconds
    private long lastClickTime = 0;
    
    public NVRLayoutManager(Context context, ViewGroup rootContainer) {
        this.context = context;
        this.rootContainer = rootContainer;

        // Initialize layout calculator with container dimensions
        rootContainer.post(() -> {
            layoutCalculator = new GridLayoutCalculator(
                rootContainer.getWidth(),
                rootContainer.getHeight()
            );
            layoutCalculator.setChannelSpacing(4); // 4px spacing between channels
            layoutCalculator.setTargetAspectRatio(16.0f / 9.0f); // 16:9 aspect ratio
        });

        initializeViews();
        setupLayoutButtons();
        setupTouchHandlers();
    }
    
    private void initializeViews() {
        // Find main views
        singleSurfaceView = rootContainer.findViewById(R.id.surface_view);
        gridScrollContainer = rootContainer.findViewById(R.id.grid_scroll_container);
        gridContainer = rootContainer.findViewById(R.id.grid_container);

        // layout_controls is in the root activity layout, not in video_container
        layoutControls = ((Activity) context).findViewById(R.id.layout_controls);
        statusOverlay = rootContainer.findViewById(R.id.status_overlay);
        
        // Initialize channel surfaces list
        channelSurfaces = new ArrayList<>();
        channelLabels = new ArrayList<>();
        channelGestureDetectors = new ArrayList<>();
        
        // Add single surface view as channel 0
        channelSurfaces.add(singleSurfaceView);
        
        // Find grid channel surfaces
        for (int i = 1; i <= 16; i++) {
            int surfaceId = context.getResources().getIdentifier(
                "surface_view_" + i, "id", context.getPackageName());
            int labelId = context.getResources().getIdentifier(
                "channel_label_" + i, "id", context.getPackageName());

            if (surfaceId != 0) {
                SurfaceView surface = rootContainer.findViewById(surfaceId);
                Log.d(TAG, "Looking for surface_view_" + i + ": " + (surface != null ? "FOUND" : "NOT FOUND"));
                if (surface != null) {
                    channelSurfaces.add(surface);
                }
            } else {
                Log.d(TAG, "Resource ID for surface_view_" + i + " not found");
            }

            if (labelId != 0) {
                TextView label = rootContainer.findViewById(labelId);
                if (label != null) {
                    channelLabels.add(label);
                }
            }
        }

        Log.d(TAG, "Total channel surfaces found: " + (channelSurfaces.size() - 1)); // -1 for single surface
    }
    
    private void setupLayoutButtons() {
        layoutButtons = new ArrayList<>();

        // Find buttons in the layout_controls container, not rootContainer
        Button btn1x1 = layoutControls.findViewById(R.id.btn_layout_1x1);
        Button btn2x2 = layoutControls.findViewById(R.id.btn_layout_2x2);
        Button btn3x3 = layoutControls.findViewById(R.id.btn_layout_3x3);
        Button btn4x4 = layoutControls.findViewById(R.id.btn_layout_4x4);

        // Check if buttons are found before adding them
        if (btn1x1 != null) layoutButtons.add(btn1x1);
        if (btn2x2 != null) layoutButtons.add(btn2x2);
        if (btn3x3 != null) layoutButtons.add(btn3x3);
        if (btn4x4 != null) layoutButtons.add(btn4x4);

        // Set click listeners only if buttons are found
        if (btn1x1 != null) btn1x1.setOnClickListener(v -> switchLayout(LayoutMode.SINGLE));
        if (btn2x2 != null) btn2x2.setOnClickListener(v -> switchLayout(LayoutMode.QUAD));
        if (btn3x3 != null) btn3x3.setOnClickListener(v -> switchLayout(LayoutMode.NINE));
        if (btn4x4 != null) btn4x4.setOnClickListener(v -> switchLayout(LayoutMode.SIXTEEN));

        // Update button states
        updateButtonStates();
    }
    
    private void setupTouchHandlers() {
        // Main container tap to show/hide controls
        GestureDetector mainGestureDetector = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onSingleTapUp(MotionEvent e) {
                toggleControls();
                return true;
            }
        });

        rootContainer.setOnTouchListener((v, event) -> mainGestureDetector.onTouchEvent(event));

        // Setup channel selection for grid surfaces
        setupChannelTouchHandlers();
    }

    private void setupChannelTouchHandlers() {
        for (int i = 1; i < channelSurfaces.size() && i <= 16; i++) {
            final int channelIndex = i;
            SurfaceView surface = channelSurfaces.get(i);
            if (surface != null) {
                setupSurfaceGestureDetector(surface, channelIndex);
            }
        }
    }

    private void setupSurfaceGestureDetector(SurfaceView surface, int channelIndex) {
        GestureDetector gestureDetector = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onSingleTapUp(MotionEvent e) {
                selectChannel(channelIndex);
                return true;
            }

            @Override
            public boolean onDoubleTap(MotionEvent e) {
                handleChannelDoubleClick(channelIndex);
                return true;
            }

            @Override
            public void onLongPress(MotionEvent e) {
                // Long press could be used for channel context menu
                handleChannelLongPress(channelIndex);
            }
        });

        surface.setOnTouchListener((v, event) -> {
            gestureDetector.onTouchEvent(event);
            return true;
        });

        // Store gesture detector for cleanup
        while (channelGestureDetectors.size() <= channelIndex) {
            channelGestureDetectors.add(null);
        }
        channelGestureDetectors.set(channelIndex, gestureDetector);
    }
    
    public void switchLayout(LayoutMode newMode) {
        if (newMode == currentMode) return;

        LayoutMode oldMode = currentMode;
        currentMode = newMode;

        Log.d(TAG, "Switching layout from " + oldMode + " to " + newMode);

        // Show layout change feedback to user
        showLayoutChangeToast(newMode);

        // Calculate transition parameters
        float scaleFactor = 1.0f;
        if (layoutCalculator != null) {
            GridLayoutCalculator.GridMode oldGridMode = convertToGridMode(oldMode);
            GridLayoutCalculator.GridMode newGridMode = convertToGridMode(newMode);
            scaleFactor = layoutCalculator.calculateScaleFactor(oldGridMode, newGridMode);
        }

        // Animate layout transition with calculated scale factor
        animateLayoutTransition(oldMode, newMode, scaleFactor);

        // Update grid configuration
        updateGridLayout(newMode);

        // Update button states
        updateButtonStates();

        // Notify listener
        if (listener != null) {
            listener.onLayoutChanged(newMode, oldMode);
        }
    }

    /**
     * Show user-friendly feedback when layout changes
     */
    private void showLayoutChangeToast(LayoutMode mode) {
        String message = "切换到 " + getLayoutModeDisplayText(mode).replace("\n", " ");
        android.widget.Toast.makeText(context, message, android.widget.Toast.LENGTH_SHORT).show();
    }

    public void switchLayoutSmooth(LayoutMode newMode) {
        // Enhanced smooth transition with pre-calculated positions
        if (newMode == currentMode || layoutCalculator == null) return;

        LayoutMode oldMode = currentMode;

        // Pre-calculate both layouts
        GridLayoutCalculator.GridMode oldGridMode = convertToGridMode(oldMode);
        GridLayoutCalculator.GridMode newGridMode = convertToGridMode(newMode);

        List<GridLayoutCalculator.ViewportInfo> oldViewports = layoutCalculator.calculateLayout(oldGridMode);
        List<GridLayoutCalculator.ViewportInfo> newViewports = layoutCalculator.calculateLayout(newGridMode);

        // Animate each channel individually
        animateChannelTransitions(oldViewports, newViewports);

        // Update current mode
        currentMode = newMode;

        // Update UI after animation
        rootContainer.postDelayed(() -> {
            updateGridLayout(newMode);
            updateButtonStates();

            if (listener != null) {
                listener.onLayoutChanged(newMode, oldMode);
            }
        }, ANIMATION_DURATION);
    }
    
    private void animateLayoutTransition(LayoutMode from, LayoutMode to, float scaleFactor) {
        AnimatorSet animatorSet = new AnimatorSet();
        List<ObjectAnimator> animators = new ArrayList<>();

        if (to == LayoutMode.SINGLE) {
            // Transition to single view
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "alpha", 1f, 0f));
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "scaleX", 1f, scaleFactor));
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "scaleY", 1f, scaleFactor));
            animators.add(ObjectAnimator.ofFloat(singleSurfaceView, "alpha", 0f, 1f));
            animators.add(ObjectAnimator.ofFloat(singleSurfaceView, "scaleX", scaleFactor, 1f));
            animators.add(ObjectAnimator.ofFloat(singleSurfaceView, "scaleY", scaleFactor, 1f));
        } else if (from == LayoutMode.SINGLE) {
            // Transition from single view
            animators.add(ObjectAnimator.ofFloat(singleSurfaceView, "alpha", 1f, 0f));
            animators.add(ObjectAnimator.ofFloat(singleSurfaceView, "scaleX", 1f, scaleFactor));
            animators.add(ObjectAnimator.ofFloat(singleSurfaceView, "scaleY", 1f, scaleFactor));
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "alpha", 0f, 1f));
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "scaleX", scaleFactor, 1f));
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "scaleY", scaleFactor, 1f));
        } else {
            // Grid to grid transition
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "scaleX", 1f, scaleFactor, 1f));
            animators.add(ObjectAnimator.ofFloat(gridScrollContainer, "scaleY", 1f, scaleFactor, 1f));
        }

        animatorSet.playTogether(animators.toArray(new ObjectAnimator[0]));
        animatorSet.setDuration(ANIMATION_DURATION);
        animatorSet.start();

        // Update visibility after animation
        animatorSet.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(android.animation.Animator animation) {
                updateViewVisibility(to);
                // Reset scale values
                gridScrollContainer.setScaleX(1f);
                gridScrollContainer.setScaleY(1f);
                singleSurfaceView.setScaleX(1f);
                singleSurfaceView.setScaleY(1f);
            }
        });
    }

    private void animateChannelTransitions(List<GridLayoutCalculator.ViewportInfo> oldViewports,
                                         List<GridLayoutCalculator.ViewportInfo> newViewports) {
        // Animate individual channel transitions for smooth layout changes
        AnimatorSet masterAnimatorSet = new AnimatorSet();
        List<ObjectAnimator> allAnimators = new ArrayList<>();

        for (int i = 0; i < Math.max(oldViewports.size(), newViewports.size()); i++) {
            View channelContainer = getChannelContainer(i + 1);
            if (channelContainer == null) continue;

            GridLayoutCalculator.ViewportInfo oldViewport = (i < oldViewports.size()) ? oldViewports.get(i) : null;
            GridLayoutCalculator.ViewportInfo newViewport = (i < newViewports.size()) ? newViewports.get(i) : null;

            if (oldViewport != null && newViewport != null) {
                // Channel exists in both layouts - animate position and size
                float scaleX = (float) newViewport.getWidth() / oldViewport.getWidth();
                float scaleY = (float) newViewport.getHeight() / oldViewport.getHeight();

                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "scaleX", 1f, scaleX, 1f));
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "scaleY", 1f, scaleY, 1f));
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "translationX",
                    0f, newViewport.getCenterX() - oldViewport.getCenterX(), 0f));
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "translationY",
                    0f, newViewport.getCenterY() - oldViewport.getCenterY(), 0f));

            } else if (oldViewport != null) {
                // Channel disappearing - fade out
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "alpha", 1f, 0f));
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "scaleX", 1f, 0.8f));
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "scaleY", 1f, 0.8f));

            } else if (newViewport != null) {
                // Channel appearing - fade in
                channelContainer.setAlpha(0f);
                channelContainer.setScaleX(0.8f);
                channelContainer.setScaleY(0.8f);

                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "alpha", 0f, 1f));
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "scaleX", 0.8f, 1f));
                allAnimators.add(ObjectAnimator.ofFloat(channelContainer, "scaleY", 0.8f, 1f));
            }
        }

        masterAnimatorSet.playTogether(allAnimators.toArray(new ObjectAnimator[0]));
        masterAnimatorSet.setDuration(ANIMATION_DURATION);
        masterAnimatorSet.start();

        // Reset all transformations after animation
        masterAnimatorSet.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(android.animation.Animator animation) {
                for (int i = 0; i < channelSurfaces.size(); i++) {
                    View channelContainer = getChannelContainer(i);
                    if (channelContainer != null) {
                        channelContainer.setAlpha(1f);
                        channelContainer.setScaleX(1f);
                        channelContainer.setScaleY(1f);
                        channelContainer.setTranslationX(0f);
                        channelContainer.setTranslationY(0f);
                    }
                }
            }
        });
    }
    
    private void updateViewVisibility(LayoutMode mode) {
        if (mode == LayoutMode.SINGLE) {
            singleSurfaceView.setVisibility(View.VISIBLE);
            gridScrollContainer.setVisibility(View.GONE);
        } else {
            singleSurfaceView.setVisibility(View.GONE);
            gridScrollContainer.setVisibility(View.VISIBLE);
        }
    }
    
    private void updateGridLayout(LayoutMode mode) {
        if (gridContainer == null || layoutCalculator == null) return;

        // Update container size in case it changed
        layoutCalculator.setContainerSize(rootContainer.getWidth(), rootContainer.getHeight());

        // Convert LayoutMode to GridMode
        GridLayoutCalculator.GridMode gridMode = convertToGridMode(mode);

        // Calculate optimal layout
        List<GridLayoutCalculator.ViewportInfo> viewports = layoutCalculator.calculateLayout(gridMode);

        // Update grid container configuration
        gridContainer.setColumnCount(mode.cols);
        gridContainer.setRowCount(mode.rows);

        // Apply calculated layout to channel containers
        applyViewportLayout(viewports);

        // Show/hide channels based on layout
        for (int i = 0; i < channelSurfaces.size() - 1; i++) { // -1 to exclude single surface
            View channelContainer = getChannelContainer(i + 1);
            if (channelContainer != null) {
                boolean shouldShow = i < mode.channels;
                channelContainer.setVisibility(shouldShow ? View.VISIBLE : View.GONE);

                // Update layout parameters if visible
                if (shouldShow && i < viewports.size()) {
                    updateChannelContainerLayout(channelContainer, viewports.get(i));
                }
            }
        }

        // Force grid container to recalculate layout
        gridContainer.requestLayout();
        gridScrollContainer.requestLayout();
    }

    private GridLayoutCalculator.GridMode convertToGridMode(LayoutMode mode) {
        switch (mode) {
            case SINGLE: return GridLayoutCalculator.GridMode.SINGLE;
            case QUAD: return GridLayoutCalculator.GridMode.QUAD;
            case NINE: return GridLayoutCalculator.GridMode.NINE;
            case SIXTEEN: return GridLayoutCalculator.GridMode.SIXTEEN;
            default: return GridLayoutCalculator.GridMode.SINGLE;
        }
    }

    private void applyViewportLayout(List<GridLayoutCalculator.ViewportInfo> viewports) {
        // Apply calculated viewport positions and sizes to the actual views
        for (GridLayoutCalculator.ViewportInfo viewport : viewports) {
            View channelContainer = getChannelContainer(viewport.channelIndex + 1); // +1 because index 0 is single view
            if (channelContainer != null) {
                updateChannelContainerLayout(channelContainer, viewport);
            }
        }
    }

    private void updateChannelContainerLayout(View channelContainer, GridLayoutCalculator.ViewportInfo viewport) {
        // Update the layout parameters of the channel container based on viewport info
        ViewGroup.LayoutParams layoutParams = channelContainer.getLayoutParams();

        if (layoutParams instanceof GridLayout.LayoutParams) {
            GridLayout.LayoutParams gridParams = (GridLayout.LayoutParams) layoutParams;

            // Set grid position
            gridParams.rowSpec = GridLayout.spec(viewport.row, 1, 1.0f);
            gridParams.columnSpec = GridLayout.spec(viewport.col, 1, 1.0f);

            // Set margins for spacing
            int spacing = 2; // 2px spacing
            gridParams.setMargins(spacing, spacing, spacing, spacing);

            channelContainer.setLayoutParams(gridParams);

            // Force layout recalculation
            channelContainer.requestLayout();
        }
    }
    
    private View getChannelContainer(int channelIndex) {
        // Find the FrameLayout container for the channel
        if (channelIndex > 0 && channelIndex < channelSurfaces.size()) {
            SurfaceView surface = channelSurfaces.get(channelIndex);
            return (View) surface.getParent();
        }
        return null;
    }
    
    private void updateButtonStates() {
        for (int i = 0; i < layoutButtons.size(); i++) {
            Button button = layoutButtons.get(i);
            if (button != null) {
                LayoutMode mode = LayoutMode.values()[i];
                boolean isSelected = (mode == currentMode);
                button.setSelected(isSelected);

                // Enhanced visual feedback
                if (isSelected) {
                    button.setAlpha(1.0f);
                    button.setScaleX(1.1f);
                    button.setScaleY(1.1f);
                } else {
                    button.setAlpha(0.7f);
                    button.setScaleX(1.0f);
                    button.setScaleY(1.0f);
                }

                // Add layout mode text for better user understanding
                String buttonText = getLayoutModeDisplayText(mode);
                button.setText(buttonText);
            }
        }
    }

    /**
     * Get user-friendly display text for layout modes
     */
    private String getLayoutModeDisplayText(LayoutMode mode) {
        switch (mode) {
            case SINGLE:
                return "1×1\n单通道";
            case QUAD:
                return "2×2\n四通道";
            case NINE:
                return "3×3\n九通道";
            case SIXTEEN:
                return "4×4\n十六通道";
            default:
                return mode.name();
        }
    }
    
    private void selectChannel(int channelIndex) {
        selectedChannel = channelIndex;

        // Add visual feedback animation
        animateChannelSelection(channelIndex);

        // Update visual selection indicators
        updateChannelSelection();

        // Notify listener
        if (listener != null) {
            listener.onChannelSelected(channelIndex);
        }
    }

    private void handleChannelDoubleClick(int channelIndex) {
        if (currentMode == LayoutMode.SINGLE) {
            // Already in single mode, do nothing or switch back to previous layout
            return;
        }

        // Switch to single channel view
        selectedChannel = channelIndex;
        switchLayout(LayoutMode.SINGLE);

        // Notify listener
        if (listener != null) {
            listener.onChannelDoubleClicked(channelIndex);
        }
    }

    private void handleChannelLongPress(int channelIndex) {
        // Long press could show channel context menu
        // For now, just select the channel and show controls
        selectChannel(channelIndex);

        if (!controlsVisible) {
            toggleControls();
        }

        // Could add channel-specific options here:
        // - Mute/unmute audio
        // - Change stream quality
        // - Show channel info
        // - Record channel
    }
    
    private void updateChannelSelection() {
        // Update channel label backgrounds and borders to show selection
        for (int i = 0; i < channelLabels.size(); i++) {
            TextView label = channelLabels.get(i);
            View channelContainer = getChannelContainer(i + 1);

            if (label != null) {
                if (i == selectedChannel - 1) { // -1 because labels start from index 0
                    // Selected channel styling
                    label.setBackgroundColor(0xFFFF4444); // Red background for selected
                    label.setTextColor(0xFFFFFFFF); // White text

                    // Add border to container if available
                    if (channelContainer != null) {
                        channelContainer.setBackgroundColor(0xFFFF4444); // Red border
                        // Add padding to create border effect
                        channelContainer.setPadding(3, 3, 3, 3);
                    }
                } else {
                    // Unselected channel styling
                    label.setBackgroundColor(0x80000000); // Semi-transparent black
                    label.setTextColor(0xFFFFFFFF); // White text

                    // Reset container styling
                    if (channelContainer != null) {
                        channelContainer.setBackgroundColor(0xFF333333); // Dark gray
                        channelContainer.setPadding(1, 1, 1, 1);
                    }
                }
            }
        }
    }

    // Add visual feedback for channel interactions
    private void animateChannelSelection(int channelIndex) {
        View channelContainer = getChannelContainer(channelIndex);
        if (channelContainer != null) {
            // Brief scale animation to show selection
            ObjectAnimator scaleX = ObjectAnimator.ofFloat(channelContainer, "scaleX", 1.0f, 1.05f, 1.0f);
            ObjectAnimator scaleY = ObjectAnimator.ofFloat(channelContainer, "scaleY", 1.0f, 1.05f, 1.0f);

            AnimatorSet animatorSet = new AnimatorSet();
            animatorSet.playTogether(scaleX, scaleY);
            animatorSet.setDuration(200);
            animatorSet.start();
        }
    }
    
    private void toggleControls() {
        controlsVisible = !controlsVisible;
        
        if (layoutControls != null) {
            layoutControls.setVisibility(controlsVisible ? View.VISIBLE : View.GONE);
        }
        
        // Auto-hide controls after 3 seconds
        if (controlsVisible) {
            rootContainer.postDelayed(() -> {
                if (controlsVisible) {
                    toggleControls();
                }
            }, 3000);
        }
    }
    
    public void toggleStatus() {
        statusVisible = !statusVisible;
        
        if (statusOverlay != null) {
            statusOverlay.setVisibility(statusVisible ? View.VISIBLE : View.GONE);
        }
    }
    
    public void updateStatus(String status, int fps) {
        TextView statusText = rootContainer.findViewById(R.id.status_text);
        TextView fpsText = rootContainer.findViewById(R.id.fps_text);
        
        if (statusText != null) {
            statusText.setText(status);
        }
        
        if (fpsText != null) {
            fpsText.setText("FPS: " + fps);
        }
    }
    
    // Getters
    public LayoutMode getCurrentMode() { return currentMode; }
    public int getSelectedChannel() { return selectedChannel; }
    public List<SurfaceView> getChannelSurfaces() { return channelSurfaces; }
    public SurfaceView getChannelSurface(int index) {
        return (index >= 0 && index < channelSurfaces.size()) ? channelSurfaces.get(index) : null;
    }
    
    // Setters
    public void setLayoutChangeListener(LayoutChangeListener listener) {
        this.listener = listener;
    }

    // Additional utility methods for channel management
    public void selectNextChannel() {
        int nextChannel = selectedChannel + 1;
        if (nextChannel >= currentMode.channels) {
            nextChannel = 0; // Wrap around to first channel
        }
        selectChannel(nextChannel);
    }

    public void selectPreviousChannel() {
        int prevChannel = selectedChannel - 1;
        if (prevChannel < 0) {
            prevChannel = currentMode.channels - 1; // Wrap around to last channel
        }
        selectChannel(prevChannel);
    }

    public void selectChannelByNumber(int channelNumber) {
        if (channelNumber >= 0 && channelNumber < currentMode.channels) {
            selectChannel(channelNumber);
        }
    }

    // Layout switching with keyboard shortcuts
    public void switchToNextLayout() {
        LayoutMode[] modes = LayoutMode.values();
        int currentIndex = currentMode.ordinal();
        int nextIndex = (currentIndex + 1) % modes.length;
        switchLayout(modes[nextIndex]);
    }

    public void switchToPreviousLayout() {
        LayoutMode[] modes = LayoutMode.values();
        int currentIndex = currentMode.ordinal();
        int prevIndex = (currentIndex - 1 + modes.length) % modes.length;
        switchLayout(modes[prevIndex]);
    }

    // Channel state management
    public void setChannelActive(int channelIndex, boolean active) {
        View channelContainer = getChannelContainer(channelIndex);
        if (channelContainer != null) {
            channelContainer.setVisibility(active ? View.VISIBLE : View.GONE);
        }

        // Update channel label
        if (channelIndex > 0 && channelIndex <= channelLabels.size()) {
            TextView label = channelLabels.get(channelIndex - 1);
            if (label != null) {
                label.setAlpha(active ? 1.0f : 0.5f);
            }
        }
    }

    public void setChannelLabel(int channelIndex, String label) {
        if (channelIndex > 0 && channelIndex <= channelLabels.size()) {
            TextView labelView = channelLabels.get(channelIndex - 1);
            if (labelView != null) {
                labelView.setText(label);
            }
        }
    }

    // Enhanced channel status and visual indicators
    public enum ChannelStatus {
        INACTIVE(0x80666666, "INACTIVE"),
        CONNECTING(0x80FFAA00, "CONNECTING"),
        ACTIVE(0x8000AA00, "ACTIVE"),
        ERROR(0x80FF0000, "ERROR"),
        RECORDING(0x80FF0000, "REC"),
        BUFFERING(0x80FFFF00, "BUFFERING");

        public final int color;
        public final String text;

        ChannelStatus(int color, String text) {
            this.color = color;
            this.text = text;
        }
    }

    private Map<Integer, ChannelStatus> channelStatuses = new HashMap<>();
    private Map<Integer, String> channelCustomLabels = new HashMap<>();
    private Map<Integer, Boolean> channelRecordingStatus = new HashMap<>();
    private Map<Integer, Integer> channelFpsCounters = new HashMap<>();

    public void setChannelStatus(int channelIndex, ChannelStatus status) {
        channelStatuses.put(channelIndex, status);
        updateChannelVisualStatus(channelIndex);
    }

    public void setChannelCustomLabel(int channelIndex, String customLabel) {
        channelCustomLabels.put(channelIndex, customLabel);
        updateChannelLabel(channelIndex);
    }

    public void setChannelRecording(int channelIndex, boolean recording) {
        channelRecordingStatus.put(channelIndex, recording);
        updateChannelVisualStatus(channelIndex);
    }

    public void updateChannelFps(int channelIndex, int fps) {
        channelFpsCounters.put(channelIndex, fps);
        updateChannelLabel(channelIndex);
    }

    private void updateChannelVisualStatus(int channelIndex) {
        View channelContainer = getChannelContainer(channelIndex);
        if (channelContainer == null) return;

        ChannelStatus status = channelStatuses.getOrDefault(channelIndex, ChannelStatus.INACTIVE);
        boolean isRecording = channelRecordingStatus.getOrDefault(channelIndex, false);

        // Update container border color based on status
        int borderColor = status.color;
        if (isRecording) {
            borderColor = 0xFFFF0000; // Bright red for recording
        }

        // Create border drawable
        GradientDrawable border = new GradientDrawable();
        border.setColor(0x00000000); // Transparent fill
        border.setStroke(3, borderColor);
        border.setCornerRadius(4);

        channelContainer.setBackground(border);

        // Add status indicator
        addStatusIndicator(channelContainer, status, isRecording);
    }

    private void addStatusIndicator(View channelContainer, ChannelStatus status, boolean isRecording) {
        if (!(channelContainer instanceof FrameLayout)) return;

        FrameLayout frameLayout = (FrameLayout) channelContainer;

        // Remove existing status indicator
        View existingIndicator = frameLayout.findViewWithTag("status_indicator");
        if (existingIndicator != null) {
            frameLayout.removeView(existingIndicator);
        }

        // Create new status indicator
        TextView statusIndicator = new TextView(context);
        statusIndicator.setTag("status_indicator");
        statusIndicator.setText(isRecording ? "●REC" : status.text);
        statusIndicator.setTextColor(0xFFFFFFFF);
        statusIndicator.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        statusIndicator.setBackgroundColor(status.color);
        statusIndicator.setPadding(4, 2, 4, 2);

        // Position in top-right corner
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        );
        params.gravity = Gravity.TOP | Gravity.END;
        params.setMargins(0, 4, 4, 0);

        frameLayout.addView(statusIndicator, params);

        // Add blinking animation for recording
        if (isRecording) {
            ObjectAnimator blinkAnimator = ObjectAnimator.ofFloat(statusIndicator, "alpha", 1f, 0.3f);
            blinkAnimator.setDuration(500);
            blinkAnimator.setRepeatCount(ObjectAnimator.INFINITE);
            blinkAnimator.setRepeatMode(ObjectAnimator.REVERSE);
            blinkAnimator.start();
        }
    }

    private void updateChannelLabel(int channelIndex) {
        if (channelIndex <= 0 || channelIndex > channelLabels.size()) return;

        TextView labelView = channelLabels.get(channelIndex - 1);
        if (labelView == null) return;

        // Build label text
        StringBuilder labelText = new StringBuilder();

        // Custom label or default channel name
        String customLabel = channelCustomLabels.get(channelIndex);
        if (customLabel != null && !customLabel.isEmpty()) {
            labelText.append(customLabel);
        } else {
            labelText.append("CH").append(channelIndex);
        }

        // Add FPS if available
        Integer fps = channelFpsCounters.get(channelIndex);
        if (fps != null && fps > 0) {
            labelText.append(" (").append(fps).append("fps)");
        }

        labelView.setText(labelText.toString());

        // Update label styling based on status
        ChannelStatus status = channelStatuses.getOrDefault(channelIndex, ChannelStatus.INACTIVE);
        labelView.setBackgroundColor(status.color);

        // Add subtle animation for active channels
        if (status == ChannelStatus.ACTIVE) {
            ObjectAnimator pulseAnimator = ObjectAnimator.ofFloat(labelView, "alpha", 0.8f, 1f);
            pulseAnimator.setDuration(1000);
            pulseAnimator.setRepeatCount(ObjectAnimator.INFINITE);
            pulseAnimator.setRepeatMode(ObjectAnimator.REVERSE);
            pulseAnimator.start();
        }
    }

    // Enhanced channel border styling
    public void setChannelBorderStyle(int channelIndex, int borderWidth, int borderColor, int cornerRadius) {
        View channelContainer = getChannelContainer(channelIndex);
        if (channelContainer == null) return;

        GradientDrawable border = new GradientDrawable();
        border.setColor(0x00000000); // Transparent fill
        border.setStroke(borderWidth, borderColor);
        border.setCornerRadius(cornerRadius);

        channelContainer.setBackground(border);
    }

    public void setChannelHighlight(int channelIndex, boolean highlighted) {
        View channelContainer = getChannelContainer(channelIndex);
        if (channelContainer == null) return;

        if (highlighted) {
            // Add highlight effect
            GradientDrawable highlight = new GradientDrawable();
            highlight.setColor(0x20FFFFFF); // Semi-transparent white overlay
            highlight.setStroke(4, 0xFFFFFFFF); // White border
            highlight.setCornerRadius(8);

            channelContainer.setForeground(highlight);

            // Add scale animation
            ObjectAnimator scaleX = ObjectAnimator.ofFloat(channelContainer, "scaleX", 1f, 1.05f);
            ObjectAnimator scaleY = ObjectAnimator.ofFloat(channelContainer, "scaleY", 1f, 1.05f);
            AnimatorSet scaleSet = new AnimatorSet();
            scaleSet.playTogether(scaleX, scaleY);
            scaleSet.setDuration(200);
            scaleSet.start();
        } else {
            // Remove highlight
            channelContainer.setForeground(null);

            // Reset scale
            ObjectAnimator scaleX = ObjectAnimator.ofFloat(channelContainer, "scaleX", channelContainer.getScaleX(), 1f);
            ObjectAnimator scaleY = ObjectAnimator.ofFloat(channelContainer, "scaleY", channelContainer.getScaleY(), 1f);
            AnimatorSet scaleSet = new AnimatorSet();
            scaleSet.playTogether(scaleX, scaleY);
            scaleSet.setDuration(200);
            scaleSet.start();
        }
    }

    // Channel overlay information
    public void showChannelInfo(int channelIndex, String info, int durationMs) {
        View channelContainer = getChannelContainer(channelIndex);
        if (!(channelContainer instanceof FrameLayout)) return;

        FrameLayout frameLayout = (FrameLayout) channelContainer;

        // Create info overlay
        TextView infoOverlay = new TextView(context);
        infoOverlay.setText(info);
        infoOverlay.setTextColor(0xFFFFFFFF);
        infoOverlay.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        infoOverlay.setBackgroundColor(0xCC000000);
        infoOverlay.setPadding(8, 4, 8, 4);
        infoOverlay.setGravity(Gravity.CENTER);

        // Position in center
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        );
        params.gravity = Gravity.CENTER;

        frameLayout.addView(infoOverlay, params);

        // Fade in animation
        infoOverlay.setAlpha(0f);
        ObjectAnimator fadeIn = ObjectAnimator.ofFloat(infoOverlay, "alpha", 0f, 1f);
        fadeIn.setDuration(300);
        fadeIn.start();

        // Auto-remove after duration
        frameLayout.postDelayed(() -> {
            ObjectAnimator fadeOut = ObjectAnimator.ofFloat(infoOverlay, "alpha", 1f, 0f);
            fadeOut.setDuration(300);
            fadeOut.addListener(new AnimatorListenerAdapter() {
                @Override
                public void onAnimationEnd(Animator animation) {
                    frameLayout.removeView(infoOverlay);
                }
            });
            fadeOut.start();
        }, durationMs);
    }

    // Channel grid lines and separators
    public void setGridLinesVisible(boolean visible) {
        if (gridContainer == null) return;

        if (visible) {
            // Add grid line drawable
            GradientDrawable gridLines = new GradientDrawable();
            gridLines.setColor(0x00000000); // Transparent
            gridLines.setStroke(1, 0xFF444444); // Dark gray lines
            gridContainer.setBackground(gridLines);
        } else {
            gridContainer.setBackground(null);
        }
    }

    public void setChannelSpacing(int spacingPx) {
        if (layoutCalculator != null) {
            layoutCalculator.setChannelSpacing(spacingPx);
            // Refresh current layout
            updateGridLayout(currentMode);
        }
    }

    // Channel performance indicators
    public void updateChannelPerformance(int channelIndex, float fps, int droppedFrames, float latency) {
        // Update FPS counter
        updateChannelFps(channelIndex, (int) fps);

        // Update status based on performance
        ChannelStatus newStatus;
        if (fps < 10) {
            newStatus = ChannelStatus.ERROR;
        } else if (fps < 20 || droppedFrames > 10) {
            newStatus = ChannelStatus.BUFFERING;
        } else {
            newStatus = ChannelStatus.ACTIVE;
        }

        setChannelStatus(channelIndex, newStatus);

        // Show performance info on long press or poor performance
        if (fps < 15 || droppedFrames > 5) {
            String perfInfo = String.format("FPS: %.1f\nDropped: %d\nLatency: %.0fms", fps, droppedFrames, latency);
            showChannelInfo(channelIndex, perfInfo, 2000);
        }
    }

    // Layout optimization methods
    public void optimizeLayoutForPerformance(int activeChannels) {
        if (layoutCalculator == null) return;

        // Automatically choose optimal layout based on active channels
        List<GridLayoutCalculator.ViewportInfo> optimizedViewports =
            layoutCalculator.calculateOptimizedLayout(activeChannels);

        // Apply viewport culling - hide channels that are too small to be useful
        for (GridLayoutCalculator.ViewportInfo viewport : optimizedViewports) {
            View channelContainer = getChannelContainer(viewport.channelIndex + 1);
            if (channelContainer != null) {
                // Hide channels that are smaller than minimum useful size
                boolean shouldShow = viewport.getWidth() > 100 && viewport.getHeight() > 60;
                channelContainer.setVisibility(shouldShow ? View.VISIBLE : View.GONE);
            }
        }
    }

    public void enableViewportCulling(boolean enabled) {
        // Enable/disable viewport culling for performance optimization
        if (!enabled) {
            // Show all channels in current layout
            for (int i = 1; i <= currentMode.channels; i++) {
                View channelContainer = getChannelContainer(i);
                if (channelContainer != null) {
                    channelContainer.setVisibility(View.VISIBLE);
                }
            }
        } else {
            // Apply culling based on current layout
            optimizeLayoutForPerformance(getActiveChannelCount());
        }
    }

    private int getActiveChannelCount() {
        int count = 0;
        for (int i = 1; i < channelSurfaces.size(); i++) {
            View channelContainer = getChannelContainer(i);
            if (channelContainer != null && channelContainer.getVisibility() == View.VISIBLE) {
                count++;
            }
        }
        return count;
    }

    public void updateLayoutForOrientation(int newWidth, int newHeight) {
        // Update layout when device orientation changes
        if (layoutCalculator != null) {
            layoutCalculator.setContainerSize(newWidth, newHeight);
            updateGridLayout(currentMode);
        }
    }

    public String getLayoutDebugInfo() {
        if (layoutCalculator == null) {
            return "Layout calculator not initialized";
        }

        GridLayoutCalculator.GridMode gridMode = convertToGridMode(currentMode);
        List<GridLayoutCalculator.ViewportInfo> viewports = layoutCalculator.calculateLayout(gridMode);

        return layoutCalculator.getLayoutDebugInfo(viewports);
    }

    // Enhanced layout switching with custom animations
    public void switchLayoutWithAnimation(LayoutMode newMode, LayoutTransitionType transitionType) {
        if (newMode == currentMode) return;

        LayoutMode oldMode = currentMode;
        currentMode = newMode;

        switch (transitionType) {
            case FADE:
                animateLayoutFade(oldMode, newMode);
                break;
            case SLIDE:
                animateLayoutSlide(oldMode, newMode);
                break;
            case ZOOM:
                animateLayoutZoom(oldMode, newMode);
                break;
            case MORPH:
                animateLayoutMorph(oldMode, newMode);
                break;
            default:
                switchLayout(newMode);
                break;
        }
    }

    public enum LayoutTransitionType {
        FADE, SLIDE, ZOOM, MORPH, INSTANT
    }

    private void animateLayoutFade(LayoutMode from, LayoutMode to) {
        AnimatorSet animatorSet = new AnimatorSet();
        List<ObjectAnimator> animators = new ArrayList<>();

        // Fade out current layout
        View currentView = (from == LayoutMode.SINGLE) ? singleSurfaceView : gridScrollContainer;
        View targetView = (to == LayoutMode.SINGLE) ? singleSurfaceView : gridScrollContainer;

        ObjectAnimator fadeOut = ObjectAnimator.ofFloat(currentView, "alpha", 1f, 0f);
        fadeOut.setDuration(ANIMATION_DURATION / 2);

        ObjectAnimator fadeIn = ObjectAnimator.ofFloat(targetView, "alpha", 0f, 1f);
        fadeIn.setDuration(ANIMATION_DURATION / 2);
        fadeIn.setStartDelay(ANIMATION_DURATION / 2);

        animators.add(fadeOut);
        animators.add(fadeIn);

        animatorSet.playTogether(animators.toArray(new ObjectAnimator[0]));
        animatorSet.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationStart(Animator animation) {
                targetView.setVisibility(View.VISIBLE);
                targetView.setAlpha(0f);
            }

            @Override
            public void onAnimationEnd(Animator animation) {
                updateViewVisibility(to);
                updateGridLayout(to);
                updateButtonStates();

                if (listener != null) {
                    listener.onLayoutChanged(to, from);
                }
            }
        });

        animatorSet.start();
    }

    private void animateLayoutSlide(LayoutMode from, LayoutMode to) {
        AnimatorSet animatorSet = new AnimatorSet();
        List<ObjectAnimator> animators = new ArrayList<>();

        View currentView = (from == LayoutMode.SINGLE) ? singleSurfaceView : gridScrollContainer;
        View targetView = (to == LayoutMode.SINGLE) ? singleSurfaceView : gridScrollContainer;

        // Determine slide direction based on layout complexity
        float slideDirection = (to.channels > from.channels) ? -1f : 1f;
        float slideDistance = rootContainer.getWidth() * slideDirection;

        // Slide out current view
        ObjectAnimator slideOut = ObjectAnimator.ofFloat(currentView, "translationX", 0f, slideDistance);
        slideOut.setDuration(ANIMATION_DURATION);

        // Slide in target view
        targetView.setTranslationX(-slideDistance);
        ObjectAnimator slideIn = ObjectAnimator.ofFloat(targetView, "translationX", -slideDistance, 0f);
        slideIn.setDuration(ANIMATION_DURATION);

        animators.add(slideOut);
        animators.add(slideIn);

        animatorSet.playTogether(animators.toArray(new ObjectAnimator[0]));
        animatorSet.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationStart(Animator animation) {
                targetView.setVisibility(View.VISIBLE);
            }

            @Override
            public void onAnimationEnd(Animator animation) {
                currentView.setTranslationX(0f);
                targetView.setTranslationX(0f);
                updateViewVisibility(to);
                updateGridLayout(to);
                updateButtonStates();

                if (listener != null) {
                    listener.onLayoutChanged(to, from);
                }
            }
        });

        animatorSet.start();
    }

    private void animateLayoutZoom(LayoutMode from, LayoutMode to) {
        AnimatorSet animatorSet = new AnimatorSet();
        List<ObjectAnimator> animators = new ArrayList<>();

        View currentView = (from == LayoutMode.SINGLE) ? singleSurfaceView : gridScrollContainer;
        View targetView = (to == LayoutMode.SINGLE) ? singleSurfaceView : gridScrollContainer;

        // Calculate zoom factors
        float zoomOut = (to.channels > from.channels) ? 0.8f : 1.2f;
        float zoomIn = 1f / zoomOut;

        // Zoom out current view while fading
        ObjectAnimator scaleXOut = ObjectAnimator.ofFloat(currentView, "scaleX", 1f, zoomOut);
        ObjectAnimator scaleYOut = ObjectAnimator.ofFloat(currentView, "scaleY", 1f, zoomOut);
        ObjectAnimator alphaOut = ObjectAnimator.ofFloat(currentView, "alpha", 1f, 0f);

        // Zoom in target view while appearing
        targetView.setScaleX(zoomIn);
        targetView.setScaleY(zoomIn);
        targetView.setAlpha(0f);

        ObjectAnimator scaleXIn = ObjectAnimator.ofFloat(targetView, "scaleX", zoomIn, 1f);
        ObjectAnimator scaleYIn = ObjectAnimator.ofFloat(targetView, "scaleY", zoomIn, 1f);
        ObjectAnimator alphaIn = ObjectAnimator.ofFloat(targetView, "alpha", 0f, 1f);

        animators.add(scaleXOut);
        animators.add(scaleYOut);
        animators.add(alphaOut);
        animators.add(scaleXIn);
        animators.add(scaleYIn);
        animators.add(alphaIn);

        animatorSet.playTogether(animators.toArray(new ObjectAnimator[0]));
        animatorSet.setDuration(ANIMATION_DURATION);
        animatorSet.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationStart(Animator animation) {
                targetView.setVisibility(View.VISIBLE);
            }

            @Override
            public void onAnimationEnd(Animator animation) {
                currentView.setScaleX(1f);
                currentView.setScaleY(1f);
                currentView.setAlpha(1f);
                targetView.setScaleX(1f);
                targetView.setScaleY(1f);
                targetView.setAlpha(1f);

                updateViewVisibility(to);
                updateGridLayout(to);
                updateButtonStates();

                if (listener != null) {
                    listener.onLayoutChanged(to, from);
                }
            }
        });

        animatorSet.start();
    }

    private void animateLayoutMorph(LayoutMode from, LayoutMode to) {
        if (layoutCalculator == null) {
            switchLayout(to);
            return;
        }

        // Calculate source and target layouts
        GridLayoutCalculator.GridMode fromGridMode = convertToGridMode(from);
        GridLayoutCalculator.GridMode toGridMode = convertToGridMode(to);

        List<GridLayoutCalculator.ViewportInfo> fromViewports = layoutCalculator.calculateLayout(fromGridMode);
        List<GridLayoutCalculator.ViewportInfo> toViewports = layoutCalculator.calculateLayout(toGridMode);

        // Create morphing animation
        ValueAnimator morphAnimator = ValueAnimator.ofFloat(0f, 1f);
        morphAnimator.setDuration(ANIMATION_DURATION);
        morphAnimator.addUpdateListener(animation -> {
            float progress = animation.getAnimatedFraction();
            List<GridLayoutCalculator.ViewportInfo> interpolatedViewports =
                layoutCalculator.interpolateLayout(fromViewports, toViewports, progress);

            // Apply interpolated positions to channel views
            applyInterpolatedLayout(interpolatedViewports);
        });

        morphAnimator.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                updateGridLayout(to);
                updateButtonStates();

                if (listener != null) {
                    listener.onLayoutChanged(to, from);
                }
            }
        });

        morphAnimator.start();
    }

    private void applyInterpolatedLayout(List<GridLayoutCalculator.ViewportInfo> viewports) {
        for (int i = 0; i < viewports.size() && i < channelSurfaces.size(); i++) {
            GridLayoutCalculator.ViewportInfo viewport = viewports.get(i);
            View channelContainer = getChannelContainer(i + 1);

            if (channelContainer != null) {
                ViewGroup.LayoutParams params = channelContainer.getLayoutParams();
                if (params instanceof ViewGroup.MarginLayoutParams) {
                    ViewGroup.MarginLayoutParams marginParams = (ViewGroup.MarginLayoutParams) params;
                    marginParams.leftMargin = viewport.bounds.left;
                    marginParams.topMargin = viewport.bounds.top;
                    marginParams.width = viewport.bounds.width();
                    marginParams.height = viewport.bounds.height();
                    channelContainer.setLayoutParams(marginParams);
                }
            }
        }
    }

    // Layout switching with gesture support
    public void handleSwipeGesture(float velocityX, float velocityY) {
        if (Math.abs(velocityX) > Math.abs(velocityY)) {
            // Horizontal swipe
            if (velocityX > 0) {
                switchToPreviousLayout();
            } else {
                switchToNextLayout();
            }
        }
    }

    public void handlePinchGesture(float scaleFactor) {
        if (scaleFactor > 1.1f) {
            // Zoom in - go to more detailed layout
            LayoutMode[] modes = LayoutMode.values();
            int currentIndex = currentMode.ordinal();
            if (currentIndex < modes.length - 1) {
                switchLayoutWithAnimation(modes[currentIndex + 1], LayoutTransitionType.ZOOM);
            }
        } else if (scaleFactor < 0.9f) {
            // Zoom out - go to less detailed layout
            LayoutMode[] modes = LayoutMode.values();
            int currentIndex = currentMode.ordinal();
            if (currentIndex > 0) {
                switchLayoutWithAnimation(modes[currentIndex - 1], LayoutTransitionType.ZOOM);
            }
        }
    }

    // Performance optimization for layout switching
    public void preloadLayout(LayoutMode mode) {
        // Pre-calculate layout to improve switching performance
        if (layoutCalculator != null) {
            GridLayoutCalculator.GridMode gridMode = convertToGridMode(mode);
            layoutCalculator.calculateLayout(gridMode);
        }
    }

    public void enableLayoutCaching(boolean enabled) {
        // Enable/disable layout caching for better performance
        if (layoutCalculator != null) {
            // This would be implemented in GridLayoutCalculator
            // layoutCalculator.enableCaching(enabled);
        }
    }

    // Layout state persistence
    public void saveLayoutState() {
        if (context != null) {
            SharedPreferences prefs = context.getSharedPreferences("nvr_layout", Context.MODE_PRIVATE);
            SharedPreferences.Editor editor = prefs.edit();
            editor.putString("current_mode", currentMode.name());
            editor.putInt("selected_channel", selectedChannel);
            editor.putBoolean("controls_visible", controlsVisible);
            editor.apply();
        }
    }

    public void restoreLayoutState() {
        if (context != null) {
            SharedPreferences prefs = context.getSharedPreferences("nvr_layout", Context.MODE_PRIVATE);
            String modeName = prefs.getString("current_mode", LayoutMode.SINGLE.name());
            selectedChannel = prefs.getInt("selected_channel", 0);
            controlsVisible = prefs.getBoolean("controls_visible", false);

            try {
                LayoutMode restoredMode = LayoutMode.valueOf(modeName);
                switchLayout(restoredMode);
            } catch (IllegalArgumentException e) {
                // Invalid mode name, use default
                switchLayout(LayoutMode.SINGLE);
            }
        }
    }

    // Advanced Layout Performance Optimization
    public class LayoutPerformanceOptimizer {
        private boolean viewportCullingEnabled = true;
        private boolean adaptiveQualityEnabled = true;
        private boolean frameSkippingEnabled = true;
        private boolean backgroundRenderingEnabled = false;

        private Map<Integer, Float> channelPriorities = new HashMap<>();
        private Map<Integer, Integer> channelQualityLevels = new HashMap<>();
        private Map<Integer, Boolean> channelVisibilityCache = new HashMap<>();

        private long lastOptimizationTime = 0;
        private static final long OPTIMIZATION_INTERVAL = 1000; // 1 second

        public void optimizeLayout() {
            long currentTime = System.currentTimeMillis();
            if (currentTime - lastOptimizationTime < OPTIMIZATION_INTERVAL) {
                return; // Skip optimization if called too frequently
            }
            lastOptimizationTime = currentTime;

            if (viewportCullingEnabled) {
                performViewportCulling();
            }

            if (adaptiveQualityEnabled) {
                adjustChannelQuality();
            }

            if (frameSkippingEnabled) {
                optimizeFrameRates();
            }

            updateRenderingPriorities();
        }

        private void performViewportCulling() {
            if (layoutCalculator == null) return;

            GridLayoutCalculator.GridMode gridMode = convertToGridMode(currentMode);
            List<GridLayoutCalculator.ViewportInfo> viewports = layoutCalculator.calculateLayout(gridMode);

            // Calculate visible area (screen bounds)
            Rect visibleArea = new Rect(0, 0, rootContainer.getWidth(), rootContainer.getHeight());

            for (GridLayoutCalculator.ViewportInfo viewport : viewports) {
                boolean wasVisible = channelVisibilityCache.getOrDefault(viewport.channelIndex, true);
                boolean isVisible = isViewportVisible(viewport, visibleArea);

                if (wasVisible != isVisible) {
                    setChannelRenderingEnabled(viewport.channelIndex + 1, isVisible);
                    channelVisibilityCache.put(viewport.channelIndex, isVisible);

                    LOGD("Channel %d visibility changed: %s", viewport.channelIndex + 1, isVisible ? "visible" : "culled");
                }
            }
        }

        private boolean isViewportVisible(GridLayoutCalculator.ViewportInfo viewport, Rect visibleArea) {
            // Check if viewport is within visible area
            if (!Rect.intersects(viewport.bounds, visibleArea)) {
                return false;
            }

            // Check minimum size threshold
            int minWidth = 64;  // Minimum useful width
            int minHeight = 48; // Minimum useful height

            if (viewport.bounds.width() < minWidth || viewport.bounds.height() < minHeight) {
                return false;
            }

            // Check if viewport area is significant enough
            float viewportArea = viewport.bounds.width() * viewport.bounds.height();
            float totalArea = visibleArea.width() * visibleArea.height();
            float areaRatio = viewportArea / totalArea;

            // Hide channels that occupy less than 1% of screen area
            return areaRatio >= 0.01f;
        }

        private void adjustChannelQuality() {
            for (int channelIndex = 1; channelIndex <= currentMode.channels; channelIndex++) {
                View channelContainer = getChannelContainer(channelIndex);
                if (channelContainer == null || channelContainer.getVisibility() != View.VISIBLE) {
                    continue;
                }

                // Calculate quality level based on viewport size
                int qualityLevel = calculateOptimalQuality(channelIndex);
                Integer currentQuality = channelQualityLevels.get(channelIndex);

                if (currentQuality == null || !currentQuality.equals(qualityLevel)) {
                    setChannelQuality(channelIndex, qualityLevel);
                    channelQualityLevels.put(channelIndex, qualityLevel);
                }
            }
        }

        private int calculateOptimalQuality(int channelIndex) {
            View channelContainer = getChannelContainer(channelIndex);
            if (channelContainer == null) return 1; // Lowest quality

            int width = channelContainer.getWidth();
            int height = channelContainer.getHeight();

            // Quality levels: 1 (lowest) to 5 (highest)
            if (width >= 800 && height >= 600) {
                return 5; // Full quality for large viewports
            } else if (width >= 400 && height >= 300) {
                return 4; // High quality for medium viewports
            } else if (width >= 200 && height >= 150) {
                return 3; // Medium quality for small viewports
            } else if (width >= 100 && height >= 75) {
                return 2; // Low quality for very small viewports
            } else {
                return 1; // Minimal quality for tiny viewports
            }
        }

        private void optimizeFrameRates() {
            float baseFps = 30.0f;

            // Adjust target FPS based on layout complexity
            float targetFps = baseFps;
            switch (currentMode) {
                case SINGLE:
                    targetFps = 30.0f;
                    break;
                case QUAD:
                    targetFps = 25.0f;
                    break;
                case NINE:
                    targetFps = 20.0f;
                    break;
                case SIXTEEN:
                    targetFps = 15.0f;
                    break;
            }

            // Apply frame rate limits to channels
            for (int channelIndex = 1; channelIndex <= currentMode.channels; channelIndex++) {
                float channelFps = targetFps;

                // Reduce FPS for non-visible channels
                if (!channelVisibilityCache.getOrDefault(channelIndex - 1, true)) {
                    channelFps = Math.min(5.0f, targetFps * 0.2f); // Very low FPS for culled channels
                }

                // Prioritize selected channel
                if (channelIndex == selectedChannel) {
                    channelFps = Math.min(30.0f, targetFps * 1.2f);
                }

                setChannelTargetFps(channelIndex, channelFps);
            }
        }

        private void updateRenderingPriorities() {
            // Set higher priority for selected channel
            for (int channelIndex = 1; channelIndex <= currentMode.channels; channelIndex++) {
                float priority = 0.5f; // Default priority

                if (channelIndex == selectedChannel) {
                    priority = 1.0f; // Highest priority
                } else if (channelVisibilityCache.getOrDefault(channelIndex - 1, true)) {
                    priority = 0.7f; // High priority for visible channels
                } else {
                    priority = 0.1f; // Low priority for culled channels
                }

                channelPriorities.put(channelIndex, priority);
                setChannelRenderingPriority(channelIndex, priority);
            }
        }

        // Configuration methods
        public void setViewportCullingEnabled(boolean enabled) {
            this.viewportCullingEnabled = enabled;
        }

        public void setAdaptiveQualityEnabled(boolean enabled) {
            this.adaptiveQualityEnabled = enabled;
        }

        public void setFrameSkippingEnabled(boolean enabled) {
            this.frameSkippingEnabled = enabled;
        }

        public void setBackgroundRenderingEnabled(boolean enabled) {
            this.backgroundRenderingEnabled = enabled;
        }

        // Performance metrics
        public Map<String, Object> getPerformanceMetrics() {
            Map<String, Object> metrics = new HashMap<>();

            int visibleChannels = 0;
            int culledChannels = 0;

            for (Boolean visible : channelVisibilityCache.values()) {
                if (visible) {
                    visibleChannels++;
                } else {
                    culledChannels++;
                }
            }

            metrics.put("visible_channels", visibleChannels);
            metrics.put("culled_channels", culledChannels);
            metrics.put("viewport_culling_enabled", viewportCullingEnabled);
            metrics.put("adaptive_quality_enabled", adaptiveQualityEnabled);
            metrics.put("frame_skipping_enabled", frameSkippingEnabled);
            metrics.put("total_channels", currentMode.channels);

            return metrics;
        }
    }

    // Performance optimizer instance
    private LayoutPerformanceOptimizer performanceOptimizer = new LayoutPerformanceOptimizer();

    // Public methods to access performance optimization
    public void enablePerformanceOptimization(boolean enabled) {
        if (enabled) {
            performanceOptimizer.optimizeLayout();

            // Schedule periodic optimization
            rootContainer.postDelayed(new Runnable() {
                @Override
                public void run() {
                    if (performanceOptimizer != null) {
                        performanceOptimizer.optimizeLayout();
                        rootContainer.postDelayed(this, 2000); // Optimize every 2 seconds
                    }
                }
            }, 2000);
        }
    }

    public void setViewportCullingEnabled(boolean enabled) {
        performanceOptimizer.setViewportCullingEnabled(enabled);
    }

    public void setAdaptiveQualityEnabled(boolean enabled) {
        performanceOptimizer.setAdaptiveQualityEnabled(enabled);
    }

    public Map<String, Object> getPerformanceMetrics() {
        return performanceOptimizer.getPerformanceMetrics();
    }

    // Native interface methods (to be implemented in JNI)
    private void setChannelRenderingEnabled(int channelIndex, boolean enabled) {
        // This would call native method to enable/disable rendering for channel
        LOGD("Setting channel %d rendering: %s", channelIndex, enabled ? "enabled" : "disabled");
    }

    private void setChannelQuality(int channelIndex, int qualityLevel) {
        // This would call native method to set quality level for channel
        LOGD("Setting channel %d quality level: %d", channelIndex, qualityLevel);
    }

    private void setChannelTargetFps(int channelIndex, float targetFps) {
        // This would call native method to set target FPS for channel
        LOGD("Setting channel %d target FPS: %.1f", channelIndex, targetFps);
    }

    private void setChannelRenderingPriority(int channelIndex, float priority) {
        // This would call native method to set rendering priority for channel
        LOGD("Setting channel %d rendering priority: %.2f", channelIndex, priority);
    }

    // Utility method for logging
    private void LOGD(String format, Object... args) {
        android.util.Log.d("NVRLayoutManager", String.format(format, args));
    }

    // Cleanup method
    public void cleanup() {
        if (channelGestureDetectors != null) {
            channelGestureDetectors.clear();
        }
        if (channelSurfaces != null) {
            channelSurfaces.clear();
        }
        if (channelLabels != null) {
            channelLabels.clear();
        }
        if (layoutButtons != null) {
            layoutButtons.clear();
        }
        layoutCalculator = null;
    }
}
