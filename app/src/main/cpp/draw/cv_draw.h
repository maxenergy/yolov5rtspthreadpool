

#ifndef RK3588_DEMO_CV_DRAW_H
#define RK3588_DEMO_CV_DRAW_H

#include <opencv2/opencv.hpp>
#include "yolo_datatype.h"

// Viewport-aware detection rendering configuration
struct ViewportRenderConfig {
    int viewportWidth;
    int viewportHeight;
    float scaleFactor;
    bool isSmallViewport;
    bool adaptiveBoxThickness;
    bool adaptiveTextSize;
    bool showConfidenceInSmallViewport;
    bool showClassNamesInSmallViewport;
    int minBoxThickness;
    int maxBoxThickness;
    float minTextScale;
    float maxTextScale;

    ViewportRenderConfig() :
        viewportWidth(1920), viewportHeight(1080), scaleFactor(1.0f),
        isSmallViewport(false), adaptiveBoxThickness(true), adaptiveTextSize(true),
        showConfidenceInSmallViewport(true), showClassNamesInSmallViewport(true),
        minBoxThickness(1), maxBoxThickness(6), minTextScale(0.3f), maxTextScale(1.0f) {}
};

// draw detections on img
void DrawDetections(cv::Mat& img, const std::vector<Detection>& objects);

// draw detections directly on RGBA buffer for better performance
void DrawDetectionsOnRGBA(uint8_t* rgba_data, int width, int height, int stride,
                         const std::vector<Detection>& objects);

// Enhanced viewport-aware detection rendering
void DrawDetectionsOnRGBAViewportOptimized(uint8_t* rgba_data, int width, int height, int stride,
                                          const std::vector<Detection>& objects,
                                          const ViewportRenderConfig& config);

// Adaptive detection rendering for multi-channel environments
void DrawDetectionsAdaptive(uint8_t* rgba_data, int width, int height, int stride,
                           const std::vector<Detection>& objects, int channelIndex,
                           bool isActiveChannel, float systemLoad);

// Utility functions for viewport optimization
ViewportRenderConfig calculateViewportConfig(int width, int height, bool isActiveChannel);
bool shouldShowDetectionDetails(const Detection& detection, const ViewportRenderConfig& config);
int calculateAdaptiveThickness(int width, int height, const ViewportRenderConfig& config);
float calculateAdaptiveTextScale(int width, int height, const ViewportRenderConfig& config);

#endif //RK3588_DEMO_CV_DRAW_H
