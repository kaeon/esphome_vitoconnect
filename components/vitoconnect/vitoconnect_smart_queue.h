/*
  vitoconnect_smart_queue.h - Smart Queue for Vitoconnect with priority and deduplication
  
  Based on ViessData queue system by Phil Oebel with enhancements:
  - Separate write/read queues for priority handling
  - Deduplication to avoid redundant requests
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
  void* callback_arg;
  uint32_t enqueue_time;
  
  /**
   * @brief Check if this request matches given parameters
   */
  bool matches(uint16_t addr, bool write) const {
    return (address == addr && is_write == write);
  }
};

/**
 * @brief Smart Queue with priority and deduplication
 * 
 * Features:
 * - Write requests have priority over read requests
 * - Automatic deduplication prevents duplicate requests
 * - Timeout protection prevents queue blocking
 * - Inter-communication delay ensures protocol compliance
 */
class SmartQueue {
 public:
  static constexpr uint32_t MAX_QUEUE_SIZE = 32;
  static constexpr uint32_t INTER_COMM_DELAY_MS = 50;
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 30000;
  
  SmartQueue() : current_request_(nullptr), last_comm_time_(0) {}
  
  /**
   * @brief Enqueue a new request with automatic priority and deduplication
   * 
   * @param address Datapoint address
   * @param length Data length in bytes
   * @param is_write True for write, false for read
   * @param arg Callback argument
   * @return true if successfully enqueued or already queued
   * @return false if queue is full
   */
  bool enqueue(uint16_t address, uint8_t length, bool is_write, void* arg) {
    // Check for duplicate
    if (has_pending(address, is_write)) {
      ESP_LOGD("vitoconnect.queue", "Duplicate avoided: 0x%04X %s", address, 
               is_write ? "write" : "read");
      return true;  // Already queued, consider it success
    }
    
    // Check capacity
    if (write_queue_.size() + read_queue_.size() >= MAX_QUEUE_SIZE) {
      ESP_LOGW("vitoconnect.queue", "Queue full!");
      return false;
    }
    
    QueuedRequest req;
    req.address = address;
    req.length = length;
    req.is_write = is_write;
    req.callback_arg = arg;
    req.enqueue_time = millis();
    
    // Add to appropriate queue
    if (is_write) {
      write_queue_.push_back(req);
      ESP_LOGD("vitoconnect.queue", "Enqueued WRITE 0x%04X (writes:%d, reads:%d)", 
               address, write_queue_.size(), read_queue_.size());
    } else {
      read_queue_.push_back(req);
      ESP_LOGV("vitoconnect.queue", "Enqueued read 0x%04X (writes:%d, reads:%d)", 
               address, write_queue_.size(), read_queue_.size());
    }
    
    return true;
  }
  
  /**
   * @brief Get next request to process
   * 
   * Respects inter-comm delay and processes writes before reads.
   * 
   * @return Pointer to next request or nullptr if nothing ready
   */
  QueuedRequest* get_next() {
    uint32_t now = millis();
    
    // Check inter-comm delay
    if (current_request_ == nullptr && 
        now - last_comm_time_ < INTER_COMM_DELAY_MS) {
      return nullptr;
    }
    
    // Already processing a request
    if (current_request_ != nullptr) {
      // Check timeout
      if (now - current_request_->enqueue_time > REQUEST_TIMEOUT_MS) {
        ESP_LOGW("vitoconnect.queue", "Request 0x%04X timed out, releasing", 
                 current_request_->address);
        release_current();
      }
      return nullptr;
    }
    
    // Priority: writes before reads
    if (!write_queue_.empty()) {
      current_request_ = &write_queue_.front();
      ESP_LOGD("vitoconnect.queue", "Processing WRITE 0x%04X", current_request_->address);
      return current_request_;
    }
    
    if (!read_queue_.empty()) {
      current_request_ = &read_queue_.front();
      ESP_LOGV("vitoconnect.queue", "Processing read 0x%04X", current_request_->address);
      return current_request_;
    }
    
    return nullptr;  // Queue empty
  }
  
  /**
   * @brief Get current request being processed
   */
  QueuedRequest* get_current() const {
    return current_request_;
  }
  
  /**
   * @brief Release current request after completion
   * 
   * Must be called after each request completes (success or error)
   */
  void release_current() {
    if (current_request_ == nullptr) {
      return;
    }
    
    last_comm_time_ = millis();
    
    // Remove from appropriate queue
    if (current_request_->is_write) {
      if (!write_queue_.empty()) {
        write_queue_.erase(write_queue_.begin());
      }
    } else {
      if (!read_queue_.empty()) {
        read_queue_.erase(read_queue_.begin());
      }
    }
    
    current_request_ = nullptr;
    
    ESP_LOGV("vitoconnect.queue", "Request released (writes:%d, reads:%d)", 
             write_queue_.size(), read_queue_.size());
  }
  
  /**
   * @brief Check if specific request is pending
   * 
   * @param address Datapoint address
   * @param is_write True for write, false for read
   * @return true if request is queued or currently processing
   */
  bool has_pending(uint16_t address, bool is_write) const {
    if (is_write) {
      for (const auto& req : write_queue_) {
        if (req.matches(address, is_write)) return true;
      }
    } else {
      for (const auto& req : read_queue_) {
        if (req.matches(address, is_write)) return true;
      }
    }
    
    // Check if currently processing
    if (current_request_ && current_request_->matches(address, is_write)) {
      return true;
    }
    
    return false;
  }
  
  /**
   * @brief Cleanup stale requests that have timed out
   * 
   * Should be called periodically to prevent memory buildup
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
    return write_queue_.size() + read_queue_.size() + 
           (current_request_ != nullptr ? 1 : 0);
  }
  
  /**
   * @brief Get write queue size
   */
  size_t write_count() const { return write_queue_.size(); }
  
  /**
   * @brief Get read queue size
   */
  size_t read_count() const { return read_queue_.size(); }
  
  /**
   * @brief Check if queue is empty
   */
  bool is_empty() const {
    return write_queue_.empty() && read_queue_.empty() && current_request_ == nullptr;
  }
  
 private:
  std::vector<QueuedRequest> write_queue_;  // Priority queue for writes
  std::vector<QueuedRequest> read_queue_;   // Normal queue for reads
  QueuedRequest* current_request_;          // Currently processing
  uint32_t last_comm_time_;                 // For inter-comm delay
};

}  // namespace vitoconnect
}  // namespace esphome

