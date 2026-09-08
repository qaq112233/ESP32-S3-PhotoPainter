#pragma once
#include <atomic>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace photopull {
constexpr size_t kIoBlock = 4096;
struct HttpRequest {
    uint32_t serial = 0;
    char path[257] = {};
    char etag[257] = {};
    uint32_t limit = 32768;
    uint32_t budget_ms = 30000;
};
enum class HttpEventKind : uint8_t { Headers, Data, Complete, Failed };
struct HttpEvent {
    uint32_t serial = 0;
    HttpEventKind kind = HttpEventKind::Failed;
    int status = 0;
    int64_t content_length = -1;
    char etag[257] = {};
    size_t size = 0;
    uint8_t data[kIoBlock] = {};
};

// This worker never touches SD or display. Both queues are fixed capacity.
class HttpsIo {
public:
    bool start(const std::string& origin, const std::string& token, const std::string& ca);
    bool submit(const HttpRequest& request);
    bool receive(HttpEvent& event);
    void cancel();
    void stop();
    bool stopped() const { return stopped_.load(); }
    unsigned stack_watermark() const { return task_ && !stopped_ ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(task_)) : 0; }
    unsigned queued_blocks() const { return events_ ? static_cast<unsigned>(uxQueueMessagesWaiting(events_)) : 0; }
    void connected(bool value) { connected_.store(value); }
private:
    static void task_entry(void* ctx);
    void run();
    void perform(const HttpRequest& request);
    bool emit(const HttpEvent& event, int64_t deadline);
    bool time_ready();
    bool alive(uint32_t serial, int64_t deadline) const;
    QueueHandle_t requests_ = nullptr;
    QueueHandle_t events_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stopping_{false}, stopped_{true}, connected_{false};
    std::atomic<uint32_t> accepted_{0};
    std::atomic<unsigned> read_credits_{0};
    std::string origin_, token_, ca_;
    bool sntp_started_ = false;
    bool time_valid_ = false;
};
}
