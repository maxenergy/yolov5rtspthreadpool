
#include <opencv2/imgproc.hpp>
#include "cv_draw.h"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "logging.h"

// 在img上画出检测结果
void DrawDetections(cv::Mat &img, const std::vector<Detection> &objects)
{
    NN_LOG_DEBUG("draw %ld objects", objects.size());
    for (const auto &object : objects)
    {
        cv::rectangle(img, object.box, object.color, 6);
        // class name with confidence
        std::string draw_string = object.className + " " + std::to_string(object.confidence);

        cv::putText(img,
                    draw_string,
                    cv::Point(object.box.x, object.box.y - 5),
                    cv::FONT_HERSHEY_SIMPLEX,
                    4,
                    cv::Scalar(255, 0, 255),
                    2);
    }
}

// Helper function to set pixel color in RGBA buffer
inline void setPixelRGBA(uint8_t* rgba_data, int x, int y, int width, int stride,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    if (x >= 0 && x < width && y >= 0) {
        int offset = y * stride + x * 4;
        rgba_data[offset] = r;     // R
        rgba_data[offset + 1] = g; // G
        rgba_data[offset + 2] = b; // B
        rgba_data[offset + 3] = a; // A
    }
}

// Draw a thick line in RGBA buffer
void drawThickLine(uint8_t* rgba_data, int width, int height, int stride,
                   int x1, int y1, int x2, int y2, int thickness,
                   uint8_t r, uint8_t g, uint8_t b) {
    // Simple thick line drawing using multiple thin lines
    int half_thick = thickness / 2;

    for (int offset = -half_thick; offset <= half_thick; offset++) {
        // Draw horizontal offset lines
        if (abs(x2 - x1) > abs(y2 - y1)) {
            // More horizontal than vertical
            int start_x = std::min(x1, x2);
            int end_x = std::max(x1, x2);
            int start_y = (x1 < x2) ? y1 : y2;
            int end_y = (x1 < x2) ? y2 : y1;

            for (int x = start_x; x <= end_x; x++) {
                int y = start_y + (end_y - start_y) * (x - start_x) / (end_x - start_x);
                setPixelRGBA(rgba_data, x, y + offset, width, stride, r, g, b);
            }
        } else {
            // More vertical than horizontal
            int start_y = std::min(y1, y2);
            int end_y = std::max(y1, y2);
            int start_x = (y1 < y2) ? x1 : x2;
            int end_x = (y1 < y2) ? x2 : x1;

            for (int y = start_y; y <= end_y; y++) {
                int x = start_x + (end_x - start_x) * (y - start_y) / (end_y - start_y);
                setPixelRGBA(rgba_data, x + offset, y, width, stride, r, g, b);
            }
        }
    }
}

// Draw rectangle outline in RGBA buffer
void drawRectangle(uint8_t* rgba_data, int width, int height, int stride,
                   int x, int y, int w, int h, int thickness,
                   uint8_t r, uint8_t g, uint8_t b) {
    // Ensure rectangle is within bounds
    x = std::max(0, std::min(x, width - 1));
    y = std::max(0, std::min(y, height - 1));
    w = std::max(1, std::min(w, width - x));
    h = std::max(1, std::min(h, height - y));

    // Draw four sides of rectangle
    drawThickLine(rgba_data, width, height, stride, x, y, x + w, y, thickness, r, g, b);         // Top
    drawThickLine(rgba_data, width, height, stride, x, y + h, x + w, y + h, thickness, r, g, b); // Bottom
    drawThickLine(rgba_data, width, height, stride, x, y, x, y + h, thickness, r, g, b);         // Left
    drawThickLine(rgba_data, width, height, stride, x + w, y, x + w, y + h, thickness, r, g, b); // Right
}

// Simple bitmap font for drawing text (8x8 pixels per character)
// This is a minimal implementation for basic ASCII characters
const uint8_t font_8x8[95][8] = {
    // Space (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ! (33)
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    // " (34)
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // # (35)
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    // $ (36)
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},
    // % (37)
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    // & (38)
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},
    // ' (39)
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ( (40)
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},
    // ) (41)
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    // * (42)
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    // + (43)
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    // , (44)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x06, 0x00},
    // - (45)
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    // . (46)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    // / (47)
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    // 0 (48)
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},
    // 1 (49)
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    // 2 (50)
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},
    // 3 (51)
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    // 4 (52)
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},
    // 5 (53)
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    // 6 (54)
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},
    // 7 (55)
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    // 8 (56)
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},
    // 9 (57)
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    // : (58)
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    // ; (59)
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x06, 0x00},
    // < (60)
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},
    // = (61)
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    // > (62)
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},
    // ? (63)
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    // @ (64)
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},
    // A (65)
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    // B (66)
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    // C (67)
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    // D (68)
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    // E (69)
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    // F (70)
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    // G (71)
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    // H (72)
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    // I (73)
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    // J (74)
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    // K (75)
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    // L (76)
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    // M (77)
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    // N (78)
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    // O (79)
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    // P (80)
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    // Q (81)
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    // R (82)
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    // S (83)
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    // T (84)
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    // U (85)
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    // V (86)
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    // W (87)
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    // X (88)
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    // Y (89)
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    // Z (90)
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},
    // Additional characters can be added here...
    // For now, we'll use space for unsupported characters
};

// Draw a single character at position (x, y)
void drawChar(uint8_t* rgba_data, int width, int height, int stride,
              int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
    if (c < 32 || c > 126) c = 32; // Use space for unsupported characters

    const uint8_t* char_data = font_8x8[c - 32];

    for (int row = 0; row < 8; row++) {
        uint8_t byte = char_data[row];
        for (int col = 0; col < 8; col++) {
            if (byte & (0x80 >> col)) {
                setPixelRGBA(rgba_data, x + col, y + row, width, stride, r, g, b);
            }
        }
    }
}

// Draw text string
void drawText(uint8_t* rgba_data, int width, int height, int stride,
              int x, int y, const std::string& text, uint8_t r, uint8_t g, uint8_t b) {
    int char_x = x;
    for (char c : text) {
        if (char_x + 8 > width) break; // Stop if we exceed width
        drawChar(rgba_data, width, height, stride, char_x, y, c, r, g, b);
        char_x += 8; // Move to next character position
    }
}

// Get color for different object classes
void getClassColor(int class_id, uint8_t& r, uint8_t& g, uint8_t& b) {
    // Define colors for different classes
    static const uint8_t colors[][3] = {
        {0, 255, 0},    // Green for class 0
        {255, 0, 0},    // Red for class 1
        {0, 0, 255},    // Blue for class 2
        {255, 255, 0},  // Yellow for class 3
        {255, 0, 255},  // Magenta for class 4
        {0, 255, 255},  // Cyan for class 5
        {255, 128, 0},  // Orange for class 6
        {128, 0, 255},  // Purple for class 7
        {255, 192, 203}, // Pink for class 8
        {128, 128, 128}, // Gray for class 9
    };

    int color_index = class_id % 10; // Cycle through colors
    r = colors[color_index][0];
    g = colors[color_index][1];
    b = colors[color_index][2];
}

// Main function to draw detections on RGBA buffer
void DrawDetectionsOnRGBA(uint8_t* rgba_data, int width, int height, int stride,
                         const std::vector<Detection>& objects) {
    if (!rgba_data || objects.empty()) {
        return;
    }

    LOGD("Drawing %zu detections on RGBA buffer (%dx%d)", objects.size(), width, height);

    for (const auto& detection : objects) {
        // Get bounding box coordinates
        int x = detection.box.x;
        int y = detection.box.y;
        int w = detection.box.width;
        int h = detection.box.height;

        // Ensure coordinates are within bounds
        if (x < 0 || y < 0 || x >= width || y >= height || w <= 0 || h <= 0) {
            continue;
        }

        // Get color for this class
        uint8_t r, g, b;
        getClassColor(detection.class_id, r, g, b);

        // Draw bounding box with appropriate thickness (scale with image size)
        int thickness = std::max(2, std::min(6, width / 200));
        drawRectangle(rgba_data, width, height, stride, x, y, w, h, thickness, r, g, b);

        // Prepare label text with confidence (2 decimal places)
        std::ostringstream label_stream;
        label_stream << detection.className << " " << std::fixed << std::setprecision(2) << detection.confidence;
        std::string label = label_stream.str();

        // Calculate text position (above the box, or inside if too close to top)
        int text_x = x;
        int text_y = (y > 12) ? (y - 4) : (y + 12);

        // Ensure text doesn't go outside image bounds
        text_x = std::max(0, std::min(text_x, width - static_cast<int>(label.length()) * 8));
        text_y = std::max(8, std::min(text_y, height - 8));

        // Draw text background (semi-transparent black rectangle)
        int text_width = label.length() * 8;
        int text_height = 8;

        // Draw background rectangle for better text visibility
        for (int bg_y = text_y - 1; bg_y < text_y + text_height + 1; bg_y++) {
            for (int bg_x = text_x - 1; bg_x < text_x + text_width + 1; bg_x++) {
                if (bg_x >= 0 && bg_x < width && bg_y >= 0 && bg_y < height) {
                    int offset = bg_y * stride + bg_x * 4;
                    // Semi-transparent black background
                    rgba_data[offset] = rgba_data[offset] / 2;     // R
                    rgba_data[offset + 1] = rgba_data[offset + 1] / 2; // G
                    rgba_data[offset + 2] = rgba_data[offset + 2] / 2; // B
                    // Keep alpha unchanged
                }
            }
        }

        // Draw the label text in white for good contrast
        drawText(rgba_data, width, height, stride, text_x, text_y, label, 255, 255, 255);
    }

    LOGD("Finished drawing detections on RGBA buffer");
}

// Enhanced viewport-aware detection rendering
void DrawDetectionsOnRGBAViewportOptimized(uint8_t* rgba_data, int width, int height, int stride,
                                          const std::vector<Detection>& objects,
                                          const ViewportRenderConfig& config) {
    if (!rgba_data || objects.empty()) {
        return;
    }

    LOGD("Drawing %zu detections with viewport optimization (%dx%d, scale: %.2f)",
         objects.size(), width, height, config.scaleFactor);

    // Calculate adaptive parameters based on viewport
    int adaptiveThickness = calculateAdaptiveThickness(width, height, config);
    float adaptiveTextScale = calculateAdaptiveTextScale(width, height, config);

    for (const auto& detection : objects) {
        // Skip low-confidence detections in small viewports
        if (config.isSmallViewport && detection.confidence < 0.7f) {
            continue;
        }

        // Get bounding box coordinates
        int x = detection.box.x;
        int y = detection.box.y;
        int w = detection.box.width;
        int h = detection.box.height;

        // Ensure coordinates are within bounds
        if (x < 0 || y < 0 || x >= width || y >= height || w <= 0 || h <= 0) {
            continue;
        }

        // Skip very small boxes in small viewports
        if (config.isSmallViewport && (w < 10 || h < 10)) {
            continue;
        }

        // Get color for this class
        uint8_t r, g, b;
        getClassColor(detection.class_id, r, g, b);

        // Draw bounding box with adaptive thickness
        drawRectangle(rgba_data, width, height, stride, x, y, w, h, adaptiveThickness, r, g, b);

        // Determine what text to show based on viewport size and configuration
        bool showDetails = shouldShowDetectionDetails(detection, config);
        if (!showDetails) {
            continue; // Skip text rendering for small viewports or low-priority detections
        }

        // Prepare label text
        std::ostringstream label_stream;
        if (config.showClassNamesInSmallViewport) {
            label_stream << detection.className;
        }
        if (config.showConfidenceInSmallViewport) {
            if (config.showClassNamesInSmallViewport) {
                label_stream << " ";
            }
            label_stream << std::fixed << std::setprecision(2) << detection.confidence;
        }
        std::string label = label_stream.str();

        if (label.empty()) {
            continue;
        }

        // Calculate text position with adaptive scaling
        int text_x = x;
        int text_y = (y > 12) ? (y - 4) : (y + 12);

        // Adaptive text size calculation
        int text_width = static_cast<int>(label.length() * 6 * adaptiveTextScale);
        int text_height = static_cast<int>(8 * adaptiveTextScale);

        // Ensure text fits within viewport
        if (text_x + text_width >= width) {
            text_x = width - text_width - 2;
        }
        if (text_y + text_height >= height) {
            text_y = height - text_height - 2;
        }

        // Draw background rectangle for better text visibility (only if text is large enough)
        if (adaptiveTextScale > 0.5f) {
            for (int bg_y = text_y - 1; bg_y < text_y + text_height + 1; bg_y++) {
                for (int bg_x = text_x - 1; bg_x < text_x + text_width + 1; bg_x++) {
                    if (bg_x >= 0 && bg_x < width && bg_y >= 0 && bg_y < height) {
                        int offset = bg_y * stride + bg_x * 4;
                        // Semi-transparent black background
                        rgba_data[offset] = rgba_data[offset] / 2;     // R
                        rgba_data[offset + 1] = rgba_data[offset + 1] / 2; // G
                        rgba_data[offset + 2] = rgba_data[offset + 2] / 2; // B
                    }
                }
            }
        }

        // Draw the label text with adaptive scaling
        drawText(rgba_data, width, height, stride, text_x, text_y, label, 255, 255, 255);
    }

    LOGD("Finished viewport-optimized detection rendering");
}

// Adaptive detection rendering for multi-channel environments
void DrawDetectionsAdaptive(uint8_t* rgba_data, int width, int height, int stride,
                           const std::vector<Detection>& objects, int channelIndex,
                           bool isActiveChannel, float systemLoad) {
    if (!rgba_data || objects.empty()) {
        return;
    }

    // Calculate viewport configuration based on channel state and system load
    ViewportRenderConfig config = calculateViewportConfig(width, height, isActiveChannel);

    // Adjust configuration based on system load
    if (systemLoad > 0.8f) {
        // High system load - reduce rendering complexity
        config.showConfidenceInSmallViewport = false;
        config.showClassNamesInSmallViewport = isActiveChannel; // Only show for active channel
        config.minBoxThickness = 1;
        config.maxBoxThickness = 3;
    } else if (systemLoad > 0.6f) {
        // Medium system load - moderate complexity
        config.showConfidenceInSmallViewport = isActiveChannel;
        config.showClassNamesInSmallViewport = true;
    }
    // Low system load - full rendering (default config)

    LOGD("Adaptive rendering for channel %d (active: %s, load: %.2f, viewport: %dx%d)",
         channelIndex, isActiveChannel ? "yes" : "no", systemLoad, width, height);

    // Use viewport-optimized rendering
    DrawDetectionsOnRGBAViewportOptimized(rgba_data, width, height, stride, objects, config);
}

// Calculate viewport configuration based on dimensions and channel state
ViewportRenderConfig calculateViewportConfig(int width, int height, bool isActiveChannel) {
    ViewportRenderConfig config;

    config.viewportWidth = width;
    config.viewportHeight = height;

    // Calculate scale factor based on viewport size
    float baseArea = 1920.0f * 1080.0f; // Full HD reference
    float currentArea = static_cast<float>(width * height);
    config.scaleFactor = std::sqrt(currentArea / baseArea);

    // Determine if this is a small viewport
    config.isSmallViewport = (width < 480 || height < 320) || (!isActiveChannel && (width < 960 || height < 540));

    // Adjust settings for small viewports
    if (config.isSmallViewport) {
        config.showConfidenceInSmallViewport = isActiveChannel; // Only show confidence for active small channels
        config.showClassNamesInSmallViewport = true; // Always show class names for identification
        config.minBoxThickness = 1;
        config.maxBoxThickness = 3;
        config.minTextScale = 0.3f;
        config.maxTextScale = 0.6f;
    } else {
        // Normal or large viewport
        config.showConfidenceInSmallViewport = true;
        config.showClassNamesInSmallViewport = true;
        config.minBoxThickness = 2;
        config.maxBoxThickness = 6;
        config.minTextScale = 0.5f;
        config.maxTextScale = 1.0f;
    }

    return config;
}

// Determine if detection details should be shown based on viewport and detection properties
bool shouldShowDetectionDetails(const Detection& detection, const ViewportRenderConfig& config) {
    // Always show details for high-confidence detections
    if (detection.confidence > 0.9f) {
        return true;
    }

    // In small viewports, be more selective
    if (config.isSmallViewport) {
        // Only show details for medium-high confidence detections
        if (detection.confidence < 0.6f) {
            return false;
        }

        // Check if the detection box is large enough to warrant text
        float boxArea = detection.box.width * detection.box.height;
        float viewportArea = config.viewportWidth * config.viewportHeight;
        float relativeArea = boxArea / viewportArea;

        // Only show text for detections that occupy at least 1% of viewport
        return relativeArea > 0.01f;
    }

    // For normal viewports, show details for most detections
    return detection.confidence > 0.4f;
}

// Calculate adaptive box thickness based on viewport size
int calculateAdaptiveThickness(int width, int height, const ViewportRenderConfig& config) {
    if (!config.adaptiveBoxThickness) {
        return config.minBoxThickness;
    }

    // Base thickness on viewport size
    int baseThickness = std::max(1, std::min(width, height) / 200);

    // Apply scale factor
    int adaptiveThickness = static_cast<int>(baseThickness * config.scaleFactor);

    // Clamp to configured range
    return std::max(config.minBoxThickness, std::min(config.maxBoxThickness, adaptiveThickness));
}

// Calculate adaptive text scale based on viewport size
float calculateAdaptiveTextScale(int width, int height, const ViewportRenderConfig& config) {
    if (!config.adaptiveTextSize) {
        return config.minTextScale;
    }

    // Base scale on viewport size
    float baseScale = std::min(width, height) / 1000.0f; // Reference: 1000px = 1.0 scale

    // Apply scale factor
    float adaptiveScale = baseScale * config.scaleFactor;

    // Clamp to configured range
    return std::max(config.minTextScale, std::min(config.maxTextScale, adaptiveScale));
}