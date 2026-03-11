#pragma once

#include "packet_context.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

namespace FFmpeg {

  /// @brief Manages thread-safe packet output
  class PacketEgress {
    public:
      /// @brief Create a packet egress manager
      PacketEgress();

      /// @brief Destructor
      ~PacketEgress();

      /// @brief Add a packet to the output queue
      /// @param packet Packet to output
      /// @return True if packet was queued successfully
      bool queuePacket(std::shared_ptr<PacketContext> packet);

      /// @brief Get next packet from the output queue
      /// @param[out] packet Output packet
      /// @param timeout Maximum time to wait for a packet
      /// @return True if packet was retrieved, false on timeout or closed queue
      bool getNextPacket(std::shared_ptr<PacketContext> & packet,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

      /// @brief Clear output queue
      void clear();

      /// @brief Check if egress is closed
      bool isClosed() const { return closed; }

      /// @brief Set closed state
      void setClosed(bool state) {
        std::lock_guard<std::mutex> lock(mutex);
        closed = state;
        if (closed) { cv.notify_all(); }
      }

      // Capacity and metrics
      void setCapacity(size_t cap) { capacity = cap ? cap : capacity; }
      size_t getCapacity() const { return capacity; }
      size_t getSize() const {
        std::lock_guard<std::mutex> lock(mutex);
        return outputQueue.size();
      }

    private:
      // Queue management
      std::queue<std::shared_ptr<PacketContext>> outputQueue; ///< Output packet queue
      mutable std::mutex mutex; ///< Queue mutex
      std::condition_variable cv; ///< Condition variable for queue synchronization
      bool closed{false}; ///< Whether egress is closed

      // Queue limits
      size_t capacity{1000}; ///< Maximum output queue size

      // Metrics
      std::atomic<uint64_t> totalProcessedPackets{0}; ///< Total packets processed
      std::atomic<uint64_t> currentQueueSize{0}; ///< Current queue size
  };

} // namespace FFmpeg
