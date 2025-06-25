package com.wulala.myyolov5rtspthreadpool;

import android.graphics.Rect;
import java.util.ArrayList;
import java.util.List;

/**
 * Grid Layout Calculator for NVR multi-channel display
 * Calculates viewport positions and sizes for different grid configurations
 */
public class GridLayoutCalculator {
    
    public static class ViewportInfo {
        public int channelIndex;
        public Rect bounds;
        public float aspectRatio;
        public boolean isVisible;
        public int row;
        public int col;
        
        public ViewportInfo(int channelIndex, int left, int top, int right, int bottom) {
            this.channelIndex = channelIndex;
            this.bounds = new Rect(left, top, right, bottom);
            this.aspectRatio = (float) bounds.width() / bounds.height();
            this.isVisible = true;
            this.row = 0;
            this.col = 0;
        }

        // Copy constructor
        public ViewportInfo(ViewportInfo other) {
            this.channelIndex = other.channelIndex;
            this.bounds = new Rect(other.bounds);
            this.aspectRatio = other.aspectRatio;
            this.isVisible = other.isVisible;
            this.row = other.row;
            this.col = other.col;
        }
        
        public int getWidth() {
            return bounds.width();
        }
        
        public int getHeight() {
            return bounds.height();
        }
        
        public int getCenterX() {
            return bounds.centerX();
        }
        
        public int getCenterY() {
            return bounds.centerY();
        }
    }
    
    public enum GridMode {
        SINGLE(1, 1, 1),
        QUAD(2, 2, 4),
        NINE(3, 3, 9),
        SIXTEEN(4, 4, 16);
        
        public final int rows;
        public final int cols;
        public final int maxChannels;
        
        GridMode(int rows, int cols, int maxChannels) {
            this.rows = rows;
            this.cols = cols;
            this.maxChannels = maxChannels;
        }
    }
    
    private int containerWidth;
    private int containerHeight;
    private int channelSpacing;
    private float targetAspectRatio;
    
    public GridLayoutCalculator(int containerWidth, int containerHeight) {
        this.containerWidth = containerWidth;
        this.containerHeight = containerHeight;
        this.channelSpacing = 2; // Default 2px spacing between channels
        this.targetAspectRatio = 16.0f / 9.0f; // Default 16:9 aspect ratio
    }
    
    public void setContainerSize(int width, int height) {
        this.containerWidth = width;
        this.containerHeight = height;
    }
    
    public void setChannelSpacing(int spacing) {
        this.channelSpacing = spacing;
    }
    
    public void setTargetAspectRatio(float aspectRatio) {
        this.targetAspectRatio = aspectRatio;
    }
    
    public List<ViewportInfo> calculateLayout(GridMode mode) {
        return calculateLayout(mode, mode.maxChannels);
    }
    
    public List<ViewportInfo> calculateLayout(GridMode mode, int activeChannels) {
        List<ViewportInfo> viewports = new ArrayList<>();
        
        if (mode == GridMode.SINGLE) {
            // Single channel takes full container
            ViewportInfo viewport = new ViewportInfo(0, 0, 0, containerWidth, containerHeight);
            viewport.row = 0;
            viewport.col = 0;
            viewports.add(viewport);
            return viewports;
        }
        
        // Calculate grid dimensions
        int rows = mode.rows;
        int cols = mode.cols;
        int channelsToShow = Math.min(activeChannels, mode.maxChannels);
        
        // Calculate cell dimensions with spacing
        int totalHorizontalSpacing = (cols - 1) * channelSpacing;
        int totalVerticalSpacing = (rows - 1) * channelSpacing;
        
        int cellWidth = (containerWidth - totalHorizontalSpacing) / cols;
        int cellHeight = (containerHeight - totalVerticalSpacing) / rows;
        
        // Adjust cell dimensions to maintain aspect ratio if needed
        if (targetAspectRatio > 0) {
            float currentAspectRatio = (float) cellWidth / cellHeight;
            
            if (currentAspectRatio > targetAspectRatio) {
                // Too wide, reduce width
                cellWidth = (int) (cellHeight * targetAspectRatio);
            } else if (currentAspectRatio < targetAspectRatio) {
                // Too tall, reduce height
                cellHeight = (int) (cellWidth / targetAspectRatio);
            }
        }
        
        // Calculate starting offsets to center the grid
        int startX = (containerWidth - (cols * cellWidth + totalHorizontalSpacing)) / 2;
        int startY = (containerHeight - (rows * cellHeight + totalVerticalSpacing)) / 2;
        
        // Generate viewport info for each channel
        int channelIndex = 0;
        for (int row = 0; row < rows && channelIndex < channelsToShow; row++) {
            for (int col = 0; col < cols && channelIndex < channelsToShow; col++) {
                int left = startX + col * (cellWidth + channelSpacing);
                int top = startY + row * (cellHeight + channelSpacing);
                int right = left + cellWidth;
                int bottom = top + cellHeight;
                
                ViewportInfo viewport = new ViewportInfo(channelIndex, left, top, right, bottom);
                viewport.row = row;
                viewport.col = col;
                viewport.isVisible = channelIndex < activeChannels;
                
                viewports.add(viewport);
                channelIndex++;
            }
        }
        
        return viewports;
    }
    
    public ViewportInfo calculateSingleChannelLayout(int channelIndex, GridMode currentMode) {
        // Calculate layout for a specific channel in the current grid mode
        List<ViewportInfo> allViewports = calculateLayout(currentMode);
        
        for (ViewportInfo viewport : allViewports) {
            if (viewport.channelIndex == channelIndex) {
                return viewport;
            }
        }
        
        return null;
    }
    
    public List<ViewportInfo> calculateOptimizedLayout(int activeChannels) {
        // Automatically choose the best grid mode for the number of active channels
        GridMode optimalMode;
        
        if (activeChannels <= 1) {
            optimalMode = GridMode.SINGLE;
        } else if (activeChannels <= 4) {
            optimalMode = GridMode.QUAD;
        } else if (activeChannels <= 9) {
            optimalMode = GridMode.NINE;
        } else {
            optimalMode = GridMode.SIXTEEN;
        }
        
        return calculateLayout(optimalMode, activeChannels);
    }
    
    public ViewportInfo findChannelAtPosition(int x, int y, List<ViewportInfo> viewports) {
        // Find which channel viewport contains the given coordinates
        for (ViewportInfo viewport : viewports) {
            if (viewport.isVisible && viewport.bounds.contains(x, y)) {
                return viewport;
            }
        }
        return null;
    }
    
    public List<ViewportInfo> getVisibleViewports(List<ViewportInfo> allViewports) {
        List<ViewportInfo> visible = new ArrayList<>();
        for (ViewportInfo viewport : allViewports) {
            if (viewport.isVisible) {
                visible.add(viewport);
            }
        }
        return visible;
    }
    
    public Rect calculateBoundingRect(List<ViewportInfo> viewports) {
        if (viewports.isEmpty()) {
            return new Rect(0, 0, containerWidth, containerHeight);
        }
        
        int minLeft = Integer.MAX_VALUE;
        int minTop = Integer.MAX_VALUE;
        int maxRight = Integer.MIN_VALUE;
        int maxBottom = Integer.MIN_VALUE;
        
        for (ViewportInfo viewport : viewports) {
            if (viewport.isVisible) {
                minLeft = Math.min(minLeft, viewport.bounds.left);
                minTop = Math.min(minTop, viewport.bounds.top);
                maxRight = Math.max(maxRight, viewport.bounds.right);
                maxBottom = Math.max(maxBottom, viewport.bounds.bottom);
            }
        }
        
        return new Rect(minLeft, minTop, maxRight, maxBottom);
    }
    
    public float calculateScaleFactor(GridMode fromMode, GridMode toMode) {
        // Calculate scale factor for smooth transitions between grid modes
        float fromCellArea = calculateAverageCellArea(fromMode);
        float toCellArea = calculateAverageCellArea(toMode);
        
        return (float) Math.sqrt(toCellArea / fromCellArea);
    }
    
    private float calculateAverageCellArea(GridMode mode) {
        if (mode == GridMode.SINGLE) {
            return containerWidth * containerHeight;
        }
        
        int totalHorizontalSpacing = (mode.cols - 1) * channelSpacing;
        int totalVerticalSpacing = (mode.rows - 1) * channelSpacing;
        
        int cellWidth = (containerWidth - totalHorizontalSpacing) / mode.cols;
        int cellHeight = (containerHeight - totalVerticalSpacing) / mode.rows;
        
        return cellWidth * cellHeight;
    }
    
    // Utility methods for layout validation and debugging
    public boolean isValidLayout(List<ViewportInfo> viewports) {
        // Check if all viewports are within container bounds
        for (ViewportInfo viewport : viewports) {
            if (viewport.bounds.left < 0 || viewport.bounds.top < 0 ||
                viewport.bounds.right > containerWidth || viewport.bounds.bottom > containerHeight) {
                return false;
            }
        }
        return true;
    }
    
    public String getLayoutDebugInfo(List<ViewportInfo> viewports) {
        StringBuilder sb = new StringBuilder();
        sb.append("Container: ").append(containerWidth).append("x").append(containerHeight).append("\n");
        sb.append("Spacing: ").append(channelSpacing).append("px\n");
        sb.append("Target Aspect Ratio: ").append(targetAspectRatio).append("\n");
        sb.append("Viewports (").append(viewports.size()).append("):\n");

        for (ViewportInfo viewport : viewports) {
            sb.append("  Channel ").append(viewport.channelIndex)
              .append(": [").append(viewport.bounds.left).append(",").append(viewport.bounds.top)
              .append(",").append(viewport.bounds.right).append(",").append(viewport.bounds.bottom)
              .append("] ").append(viewport.bounds.width()).append("x").append(viewport.bounds.height())
              .append(" visible=").append(viewport.isVisible).append("\n");
        }

        return sb.toString();
    }

    // Enhanced layout calculation methods
    public List<ViewportInfo> calculateAdaptiveLayout(int activeChannels, boolean maintainAspectRatio) {
        GridMode optimalMode = selectOptimalGridMode(activeChannels);

        if (maintainAspectRatio) {
            return calculateLayoutWithAspectRatio(optimalMode, activeChannels);
        } else {
            return calculateLayout(optimalMode, activeChannels);
        }
    }

    private GridMode selectOptimalGridMode(int activeChannels) {
        // Enhanced logic for selecting optimal grid mode
        if (activeChannels <= 1) {
            return GridMode.SINGLE;
        } else if (activeChannels <= 4) {
            return GridMode.QUAD;
        } else if (activeChannels <= 9) {
            return GridMode.NINE;
        } else {
            return GridMode.SIXTEEN;
        }
    }

    private List<ViewportInfo> calculateLayoutWithAspectRatio(GridMode mode, int activeChannels) {
        List<ViewportInfo> viewports = new ArrayList<>();

        if (mode == GridMode.SINGLE) {
            // For single channel, maintain aspect ratio within container
            ViewportInfo viewport = calculateCenteredViewport(0, containerWidth, containerHeight);
            viewports.add(viewport);
            return viewports;
        }

        // Calculate optimal cell size maintaining aspect ratio
        int rows = mode.rows;
        int cols = mode.cols;
        int channelsToShow = Math.min(activeChannels, mode.maxChannels);

        // Calculate available space
        int totalHorizontalSpacing = (cols - 1) * channelSpacing;
        int totalVerticalSpacing = (rows - 1) * channelSpacing;
        int availableWidth = containerWidth - totalHorizontalSpacing;
        int availableHeight = containerHeight - totalVerticalSpacing;

        // Calculate cell size maintaining aspect ratio
        int cellWidth = availableWidth / cols;
        int cellHeight = availableHeight / rows;

        // Adjust to maintain target aspect ratio
        float currentAspectRatio = (float) cellWidth / cellHeight;
        if (targetAspectRatio > 0 && Math.abs(currentAspectRatio - targetAspectRatio) > 0.1f) {
            if (currentAspectRatio > targetAspectRatio) {
                cellWidth = (int) (cellHeight * targetAspectRatio);
            } else {
                cellHeight = (int) (cellWidth / targetAspectRatio);
            }
        }

        // Calculate grid starting position to center it
        int gridWidth = cols * cellWidth + totalHorizontalSpacing;
        int gridHeight = rows * cellHeight + totalVerticalSpacing;
        int startX = (containerWidth - gridWidth) / 2;
        int startY = (containerHeight - gridHeight) / 2;

        // Generate viewports
        int channelIndex = 0;
        for (int row = 0; row < rows && channelIndex < channelsToShow; row++) {
            for (int col = 0; col < cols && channelIndex < channelsToShow; col++) {
                int left = startX + col * (cellWidth + channelSpacing);
                int top = startY + row * (cellHeight + channelSpacing);
                int right = left + cellWidth;
                int bottom = top + cellHeight;

                ViewportInfo viewport = new ViewportInfo(channelIndex, left, top, right, bottom);
                viewport.row = row;
                viewport.col = col;
                viewport.isVisible = channelIndex < activeChannels;

                viewports.add(viewport);
                channelIndex++;
            }
        }

        return viewports;
    }

    private ViewportInfo calculateCenteredViewport(int channelIndex, int maxWidth, int maxHeight) {
        int viewportWidth = maxWidth;
        int viewportHeight = maxHeight;

        // Maintain aspect ratio if specified
        if (targetAspectRatio > 0) {
            float containerAspectRatio = (float) maxWidth / maxHeight;

            if (containerAspectRatio > targetAspectRatio) {
                // Container is wider than target, limit width
                viewportWidth = (int) (maxHeight * targetAspectRatio);
            } else {
                // Container is taller than target, limit height
                viewportHeight = (int) (maxWidth / targetAspectRatio);
            }
        }

        // Center the viewport
        int left = (maxWidth - viewportWidth) / 2;
        int top = (maxHeight - viewportHeight) / 2;
        int right = left + viewportWidth;
        int bottom = top + viewportHeight;

        ViewportInfo viewport = new ViewportInfo(channelIndex, left, top, right, bottom);
        viewport.row = 0;
        viewport.col = 0;
        viewport.isVisible = true;

        return viewport;
    }

    // Performance optimization methods
    public List<ViewportInfo> calculateVisibleViewports(List<ViewportInfo> allViewports, Rect visibleArea) {
        List<ViewportInfo> visibleViewports = new ArrayList<>();

        for (ViewportInfo viewport : allViewports) {
            if (viewport.isVisible && Rect.intersects(viewport.bounds, visibleArea)) {
                visibleViewports.add(viewport);
            }
        }

        return visibleViewports;
    }

    public boolean isViewportVisible(ViewportInfo viewport, Rect visibleArea) {
        return viewport.isVisible && Rect.intersects(viewport.bounds, visibleArea);
    }

    // Layout transition helpers
    public List<ViewportInfo> interpolateLayout(List<ViewportInfo> fromViewports,
                                               List<ViewportInfo> toViewports,
                                               float progress) {
        List<ViewportInfo> interpolatedViewports = new ArrayList<>();

        int maxChannels = Math.max(fromViewports.size(), toViewports.size());

        for (int i = 0; i < maxChannels; i++) {
            ViewportInfo fromViewport = i < fromViewports.size() ? fromViewports.get(i) : null;
            ViewportInfo toViewport = i < toViewports.size() ? toViewports.get(i) : null;

            if (fromViewport != null && toViewport != null) {
                // Interpolate between two viewports
                ViewportInfo interpolated = interpolateViewport(fromViewport, toViewport, progress);
                interpolatedViewports.add(interpolated);
            } else if (toViewport != null) {
                // Viewport appearing
                ViewportInfo interpolated = new ViewportInfo(toViewport);
                interpolated.bounds.set(
                    (int) (toViewport.bounds.centerX() * (1 - progress) + toViewport.bounds.left * progress),
                    (int) (toViewport.bounds.centerY() * (1 - progress) + toViewport.bounds.top * progress),
                    (int) (toViewport.bounds.centerX() * (1 - progress) + toViewport.bounds.right * progress),
                    (int) (toViewport.bounds.centerY() * (1 - progress) + toViewport.bounds.bottom * progress)
                );
                interpolatedViewports.add(interpolated);
            } else if (fromViewport != null) {
                // Viewport disappearing
                ViewportInfo interpolated = new ViewportInfo(fromViewport);
                interpolated.bounds.set(
                    (int) (fromViewport.bounds.left * (1 - progress) + fromViewport.bounds.centerX() * progress),
                    (int) (fromViewport.bounds.top * (1 - progress) + fromViewport.bounds.centerY() * progress),
                    (int) (fromViewport.bounds.right * (1 - progress) + fromViewport.bounds.centerX() * progress),
                    (int) (fromViewport.bounds.bottom * (1 - progress) + fromViewport.bounds.centerY() * progress)
                );
                interpolatedViewports.add(interpolated);
            }
        }

        return interpolatedViewports;
    }

    private ViewportInfo interpolateViewport(ViewportInfo from, ViewportInfo to, float progress) {
        ViewportInfo interpolated = new ViewportInfo(to.channelIndex, 0, 0, 0, 0);

        interpolated.bounds.left = (int) (from.bounds.left * (1 - progress) + to.bounds.left * progress);
        interpolated.bounds.top = (int) (from.bounds.top * (1 - progress) + to.bounds.top * progress);
        interpolated.bounds.right = (int) (from.bounds.right * (1 - progress) + to.bounds.right * progress);
        interpolated.bounds.bottom = (int) (from.bounds.bottom * (1 - progress) + to.bounds.bottom * progress);

        interpolated.row = to.row;
        interpolated.col = to.col;
        interpolated.isVisible = to.isVisible;

        return interpolated;
    }

    // Layout validation and optimization
    public LayoutValidationResult validateLayout(List<ViewportInfo> viewports) {
        LayoutValidationResult result = new LayoutValidationResult();

        for (ViewportInfo viewport : viewports) {
            // Check bounds
            if (viewport.bounds.left < 0 || viewport.bounds.top < 0 ||
                viewport.bounds.right > containerWidth || viewport.bounds.bottom > containerHeight) {
                result.addError("Viewport " + viewport.channelIndex + " is outside container bounds");
            }

            // Check minimum size
            if (viewport.bounds.width() < 50 || viewport.bounds.height() < 50) {
                result.addWarning("Viewport " + viewport.channelIndex + " is very small");
            }

            // Check aspect ratio deviation
            if (targetAspectRatio > 0) {
                float actualRatio = (float) viewport.bounds.width() / viewport.bounds.height();
                float deviation = Math.abs(actualRatio - targetAspectRatio) / targetAspectRatio;
                if (deviation > 0.2f) { // 20% deviation threshold
                    result.addWarning("Viewport " + viewport.channelIndex + " aspect ratio deviates significantly");
                }
            }
        }

        // Check for overlaps
        for (int i = 0; i < viewports.size(); i++) {
            for (int j = i + 1; j < viewports.size(); j++) {
                if (Rect.intersects(viewports.get(i).bounds, viewports.get(j).bounds)) {
                    result.addError("Viewports " + i + " and " + j + " overlap");
                }
            }
        }

        return result;
    }

    public static class LayoutValidationResult {
        private List<String> errors = new ArrayList<>();
        private List<String> warnings = new ArrayList<>();

        public void addError(String error) { errors.add(error); }
        public void addWarning(String warning) { warnings.add(warning); }

        public boolean isValid() { return errors.isEmpty(); }
        public List<String> getErrors() { return errors; }
        public List<String> getWarnings() { return warnings; }

        public String getReport() {
            StringBuilder sb = new StringBuilder();
            if (!errors.isEmpty()) {
                sb.append("Errors:\n");
                for (String error : errors) {
                    sb.append("  - ").append(error).append("\n");
                }
            }
            if (!warnings.isEmpty()) {
                sb.append("Warnings:\n");
                for (String warning : warnings) {
                    sb.append("  - ").append(warning).append("\n");
                }
            }
            return sb.toString();
        }
    }
}
