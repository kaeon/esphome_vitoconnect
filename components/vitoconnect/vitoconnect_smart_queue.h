/*
  vitoconnect_smart_queue.h - Smart Queue for Vitoconnect with priority and safe deduplication
  
  Based on ViessData queue system with enhancements:
  - Separate write/read queues for priority handling
  - SAFE component-aware deduplication using type enum (not pointers!)
  - Retry throttling to prevent busy-wait loops
  - Timeout protection to prevent blocking
  
  Copyright (C) 2024  Philipp Danner
  
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <vector>
#include <cstdint>
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace vitoconnect {

/**
 * @brief Represents a queued communication request
 */
struct QueuedRequest {
  uint16_t address;
  uint8_t length;
  bool is_write;
  void* callback_arg;          // Pointer to CbArg for callback
  uint8_t component_type;      // Component type for safe deduplication
  uint32_t enqueue_time;
  
  /**
   * @brief Check if this request matches given parameters
   * Safe component-aware deduplication using type enum instead of pointer comparison
   */
  bool matches(uint16_t addr, bool write, uint8_t comp_type) const {
    return (address == addr && is_write == write && component_type == comp_type);
  }
};

/**
 * @brief Smart Queue with priority, safe deduplication, and throttling
 * 
 * Features:
 * - Write requests have priority over read requests
 * - Safe component-aware deduplication using type enum (NO pointer comparison!)
 * - Retry throttling prevents busy-wait loops
 * - Timeout protection prevents queue blocking
 * - Inter-communication delay ensures protocol compliance
 */
class SmartQueue {
 public:
  static constexpr uint32_t MAX_QUEUE_SIZE = 64;
  static constexpr uint32_t INTER_COMM_DELAY_MS = 50;
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 30000;
  static constexpr uint32_t RETRY_DELAY_MS = 50;  // Minimum delay between retries
  
  SmartQueue() : has_current_(false), current_is_write_(false), last_comm_time_(0), last_retry_time_(0) {}
  
  /**
   * @brief Enqueue a new request with automatic priority and safe deduplication
   * 
   * @param address Datapoint address
   * @param length Data length in bytes
   * @param is_write True for write, false for read
   * @param arg Callback argument (pointer to CbArg)
   * @param comp_type Component type for safe deduplication
   * @return true if successfully enqueued or already queued
   * @return false if queue is full
   */
  bool enqueue(uint16_t address, uint8_t length, bool is_write, void* arg, uint8_t comp_type) {
    // Check capacity first (fast)
    if (write_queue_.size() + read_queue_.size() >= MAX_QUEUE_SIZE) {
      ESP_LOGW("vitoconnect.queue", "Queue full!");
      return false;
    }
    
    // Safe component-aware deduplication using type enum
    // For writes: always check (critical to avoid duplicate writes)
    // For reads: only check if queue is large (optimization)
    if (is_write || read_queue_.size() > 10) {
      if (has_pending(address, is_write, comp_type)) {
        ESP_LOGVV("vitoconnect.queue", "Duplicate avoided: 0x%04X %s type:%d", 
                 address, is_write ? "write" : "read", comp_type);
        return true;  // Already queued, consider it success
      }
    }
    
    QueuedRequest req;
    req.address = address;
    req.length = length;
    req.is_write = is_write;
    req.callback_arg = arg;
    req.component_type = comp_type;
    req.enqueue_time = millis();
    
    // Add to appropriate queue
    if (is_write) {
      write_queue_.push_back(req);
      ESP_LOGD("vitoconnect.queue", "Enqueued WRITE 0x%04X type:%d (writes:%d, reads:%d)", 
               address, comp_type, write_queue_.size(), read_queue_.size());
    } else {
      read_queue_.push_back(req);
      ESP_LOGV("vitoconnect.queue", "Enqueued read 0x%04X type:%d (writes:%d, reads:%d)", 
               address, comp_type, write_queue_.size(), read_queue_.size());
    }
    
    return true;
  }
  
  /**
   * @brief Get next request to process with retry throttling
   * 
   * @return Pointer to next request or nullptr if nothing ready
   * NOTE: Pointer is only valid until next queue modification
   */
  QueuedRequest* get_next() {
    uint32_t now = millis();
    
    // Check retry throttle delay (prevents busy-wait loop)
    if (last_retry_time_ > 0 && now - last_retry_time_ < RETRY_DELAY_MS) {
      return nullptr;  // Still in throttle period, wait longer
    }
    
    // Check inter-comm delay (only when selecting NEW request)
    if (!has_current_ && now - last_comm_time_ < INTER_COMM_DELAY_MS) {
      return nullptr;
    }
    
    // Already processing a request
    if (has_current_) {
      QueuedRequest* current = get_current_ptr();
      if (current == nullptr) {
        has_current_ = false;
        last_retry_time_ = 0;
        return nullptr;
      }
      
      // Check timeout
      if (now - current->enqueue_time > REQUEST_TIMEOUT_MS) {
        ESP_LOGW("vitoconnect.queue", "Request 0x%04X timed out, releasing", current->address);
        release_current();
        last_retry_time_ = 0;
        return nullptr;
      }
      
      return current;
    }
    
    // Priority: writes before reads
    if (!write_queue_.empty()) {
      has_current_ = true;
      current_is_write_ = true;
      last_retry_time_ = 0;
      QueuedRequest* req = &write_queue_.front();
      ESP_LOGD("vitoconnect.queue", "Processing WRITE 0x%04X type:%d", req->address, req->component_type);
      return req;
    }
    
    if (!read_queue_.empty()) {
      has_current_ = true;
      current_is_write_ = false;
      last_retry_time_ = 0;
      QueuedRequest* req = &read_queue_.front();
      ESP_LOGV("vitoconnect.queue", "Processing read 0x%04X type:%d", req->address, req->component_type);
      return req;
    }
    
    return nullptr;  // Queue empty
  }
  
  /**
   * @brief Retry current request with throttling
   */
  void retry_current() {
    if (!has_current_) {
      return;
    }
    
    last_retry_time_ = millis();
    has_current_ = false;
    
    ESP_LOGV("vitoconnect.queue", "Request retry scheduled after %dms", RETRY_DELAY_MS);
  }
  
  /**
   * @brief Check if currently processing a request
   */
  bool has_current() const {
    return has_current_;
  }
  
  /**
   * @brief Release current request after completion
   */
  void release_current() {
    if (!has_current_) {
      return;
    }
    
    last_comm_time_ = millis();
    last_retry_time_ = 0;  // Reset retry throttle
    
    // Remove from appropriate queue
    if (current_is_write_) {
      if (!write_queue_.empty()) {
        write_queue_.erase(write_queue_.begin());
      }
    } else {
      if (!read_queue_.empty()) {
        read_queue_.erase(read_queue_.begin());
      }
    }
    
    has_current_ = false;
    
    ESP_LOGV("vitoconnect.queue", "Request released (writes:%d, reads:%d)", 
             write_queue_.size(), read_queue_.size());
  }
  
  /**
   * @brief Check if specific request is pending
   */
  bool has_pending(uint16_t address, bool is_write, uint8_t comp_type) const {
    const auto& queue = is_write ? write_queue_ : read_queue_;
    
    for (const auto& req : queue) {
      if (req.matches(address, is_write, comp_type)) {
        return true;
      }
    }
    
    // Check if currently processing
    if (has_current_ && current_is_write_ == is_write) {
      const QueuedRequest* current = get_current_ptr();
      if (current && current->matches(address, is_write, comp_type)) {
        return true;
      }
    }
    
    return false;
  }
  
  /**
   * @brief Cleanup stale requests
   */
  void cleanup_stale() {
    uint32_t now = millis();
    
    auto cleanup = [now](std::vector<QueuedRequest>& queue) {
      auto it = queue.begin();
      while (it != queue.end()) {
        if (now - it->enqueue_time > REQUEST_TIMEOUT_MS) {
          ESP_LOGW("vitoconnect.queue", "Removing stale request 0x%04X", it->address);
          it = queue.erase(it);
        } else {
          ++it;
        }
      }
    };
    
    cleanup(write_queue_);
    cleanup(read_queue_);
  }
  
  /**
   * @brief Get total queue size
   */
  size_t size() const {
    return write_queue_.size() + read_queue_.size();
  }
  
  size_t write_count() const { return write_queue_.size(); }
  size_t read_count() const { return read_queue_.size(); }
  
  bool is_empty() const {
    return write_queue_.empty() && read_queue_.empty();
  }
  
 private:
  /**
   * @brief Get pointer to current request (safe against reallocation)
   */
  QueuedRequest* get_current_ptr() const {
    if (!has_current_) return nullptr;
    
    if (current_is_write_) {
      return write_queue_.empty() ? nullptr : const_cast<QueuedRequest*>(&write_queue_.front());
    } else {
      return read_queue_.empty() ? nullptr : const_cast<QueuedRequest*>(&read_queue_.front());
    }
  }
  
  std::vector<QueuedRequest> write_queue_;
  std::vector<QueuedRequest> read_queue_;
  bool has_current_;
  bool current_is_write_;
  uint32_t last_comm_time_;
  uint32_t last_retry_time_;  // For retry throttling
};

}  // namespace vitoconnect
}  // namespace esphome
