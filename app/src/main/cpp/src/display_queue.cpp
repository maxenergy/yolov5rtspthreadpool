#include "display_queue.h"

void RenderFrameQueue::push(std::shared_ptr<frame_data_t> &frameDataPtr) {
    // Check queue size before acquiring lock for better performance
    if (m_queue.size() > DISPLAY_QUEUE_MAX_SIZE) {
        LOGW("RenderFrameQueue::push queue size > %d, dropping frame %d",
             DISPLAY_QUEUE_MAX_SIZE, frameDataPtr->frameId);
        // No need to manually delete - smart pointer will handle cleanup automatically
        return;
    }

    // Validate frame data before adding to queue
    if (!frameDataPtr || !frameDataPtr->data) {
        LOGE("RenderFrameQueue::push received invalid frame data");
        return;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_queue.push(frameDataPtr);
    lock.unlock();
    m_cond.notify_one();

    LOGD("RenderFrameQueue::push added frame %d, queue size: %d",
         frameDataPtr->frameId, static_cast<int>(m_queue.size()));
}

std::shared_ptr<frame_data_t> RenderFrameQueue::pop() {
    std::unique_lock<std::mutex> lock(m_mutex);

    // Wait with timeout to prevent indefinite blocking
    if (!m_cond.wait_for(lock, std::chrono::milliseconds(100), [this] { return !m_queue.empty(); })) {
        // Timeout occurred, return nullptr
        return nullptr;
    }

    if (m_queue.empty()) {
        return nullptr;
    }

    auto data = m_queue.front();
    m_queue.pop();

    LOGD("RenderFrameQueue::pop retrieved frame %d, remaining queue size: %d",
         data ? data->frameId : -1, static_cast<int>(m_queue.size()));

    return data;
}

int RenderFrameQueue::size() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void RenderFrameQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);

    int clearedCount = m_queue.size();
    // Clear the queue - shared_ptr will automatically handle memory cleanup
    while (!m_queue.empty()) {
        m_queue.pop();
    }

    LOGD("RenderFrameQueue::clear() removed %d frames", clearedCount);
}

