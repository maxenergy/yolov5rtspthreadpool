#ifndef MY_YOLOV5_RTSP_THREAD_POOL_USER_COMM_H
#define MY_YOLOV5_RTSP_THREAD_POOL_USER_COMM_H

#include "mpp_decoder.h"
#include <memory>
#include <vector>

// Include Detection structure
#include "yolo_datatype.h"

typedef struct g_frame_data_t {
    std::unique_ptr<char[]> data;  // Use smart pointer for automatic memory management
    long dataSize;
    int screenStride;
    int screenW;
    int screenH;
    int widthStride;
    int heightStride;
    int frameId;
    int frameFormat;

    // Detection results for this frame
    std::vector<Detection> detections;
    bool hasDetections;

    // Constructor
    g_frame_data_t() : dataSize(0), screenStride(0), screenW(0), screenH(0),
                       widthStride(0), heightStride(0), frameId(0), frameFormat(0), hasDetections(false) {}

    // Move constructor
    g_frame_data_t(g_frame_data_t&& other) noexcept
        : data(std::move(other.data)), dataSize(other.dataSize),
          screenStride(other.screenStride), screenW(other.screenW), screenH(other.screenH),
          widthStride(other.widthStride), heightStride(other.heightStride),
          frameId(other.frameId), frameFormat(other.frameFormat),
          detections(std::move(other.detections)), hasDetections(other.hasDetections) {}

    // Move assignment operator
    g_frame_data_t& operator=(g_frame_data_t&& other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            dataSize = other.dataSize;
            screenStride = other.screenStride;
            screenW = other.screenW;
            screenH = other.screenH;
            widthStride = other.widthStride;
            heightStride = other.heightStride;
            frameId = other.frameId;
            frameFormat = other.frameFormat;
            detections = std::move(other.detections);
            hasDetections = other.hasDetections;
        }
        return *this;
    }

    // Delete copy constructor and copy assignment to prevent accidental copying
    g_frame_data_t(const g_frame_data_t&) = delete;
    g_frame_data_t& operator=(const g_frame_data_t&) = delete;
} frame_data_t;

#endif //MY_YOLOV5_RTSP_THREAD_POOL_USER_COMM_H
