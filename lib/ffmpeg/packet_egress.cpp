#include "packet_egress.h"
extern "C" {
#include <libavutil/time.h>
}

namespace FFmpeg {

  PacketEgress::PacketEgress() {}

  PacketEgress::~PacketEgress() {
    // Ensure all packets are processed before destruction
    clear();
  }

  bool PacketEgress::queuePacket(std::shared_ptr<PacketContext> packet) {
    if (!packet) {
      ERROR_MSG("PacketEgress: Null packet");
      return false;
    }

    // Add to queue with thread safety
    {
      std::unique_lock<std::mutex> lock(mutex);

      // Wait for space in queue
      if (!cv.wait_for(lock, std::chrono::milliseconds(1000), [this] { return outputQueue.size() < capacity || closed; })) {
        ERROR_MSG("PacketEgress: Output queue full");
        return false;
      }

      if (!closed) {
        outputQueue.push(packet);
        currentQueueSize.store(outputQueue.size());
        ++totalProcessedPackets;
        cv.notify_one();
        VERYHIGH_MSG("PacketEgress: Queued output packet: size=%d, PTS=%" PRIu64 ", queue_size=%zu/%zu",
                     packet->getSize(), packet->getPts(), outputQueue.size(), capacity);
        return true;
      }
    }

    return false;
  }

  bool PacketEgress::getNextPacket(std::shared_ptr<PacketContext> & packet, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);

    VERYHIGH_MSG("PacketEgress: Waiting for next output packet (timeout: %" PRIu64 "ms, queue size: %zu)",
                 timeout.count(), outputQueue.size());

    // Wait with proper predicate
    auto waitResult = cv.wait_for(lock, timeout, [this]() { return !outputQueue.empty() || closed; });

    if (!waitResult) {
      MEDIUM_MSG("PacketEgress: Output packet wait timeout");
      return false;
    }

    if (outputQueue.empty()) {
      if (closed) { MEDIUM_MSG("PacketEgress: Output queue closed and empty"); }
      return false;
    }

    packet = std::move(outputQueue.front());
    outputQueue.pop();
    currentQueueSize.store(outputQueue.size());

    // Notify producers if queue was full
    if (outputQueue.size() == capacity - 1) { cv.notify_one(); }

    VERYHIGH_MSG("PacketEgress: Retrieved output packet: size=%d, PTS=%" PRIu64 ", queue_size=%zu/%zu",
                 packet->getSize(), packet->getPts(), outputQueue.size(), capacity);

    return true;
  }

  void PacketEgress::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    std::queue<std::shared_ptr<PacketContext>>().swap(outputQueue);
    currentQueueSize.store(0);
    cv.notify_all();
  }

} // namespace FFmpeg
