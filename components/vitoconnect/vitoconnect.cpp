/*
  optolink.cpp - Connect Viessmann heating devices via Optolink to ESPhome

  Copyright (C) 2023  Philipp Danner

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "vitoconnect.h"

namespace esphome {
namespace vitoconnect {

static const char *TAG = "vitoconnect";

void VitoConnect::setup() {

    this->check_uart_settings(4800, 2, uart::UART_CONFIG_PARITY_EVEN, 8);

    ESP_LOGD(TAG, "Starting optolink with protocol: %s", this->protocol.c_str());
    if (this->protocol.compare("P300") == 0) {
        _optolink = new OptolinkP300(this);
    } else if (this->protocol.compare("KW") == 0) {
        _optolink = new OptolinkKW(this);
    } else {
      ESP_LOGW(TAG, "Unknown protocol.");
    }

    // optimize datapoint list
    _datapoints.shrink_to_fit();

    if (_optolink) {

      // add onData and onError callbacks
      _optolink->onData(&VitoConnect::_onData);
      _optolink->onError(&VitoConnect::_onError);
      
      // set initial state
      _optolink->begin();

    } else {
      ESP_LOGW(TAG, "Not able to initialize VitoConnect");
    }
}

void VitoConnect::register_datapoint(Datapoint *datapoint) {
    ESP_LOGD(TAG, "Adding datapoint with address %x and length %d", datapoint->getAddress(), datapoint->getLength());
    this->_datapoints.push_back(datapoint);
}

void VitoConnect::loop() {
    // Always process optolink state machine first (handles INIT, RESET, etc)
    _optolink->loop();
    
    // Only try to push requests if protocol is ready (not initializing)
    if (!_optolink->is_ready()) {
      // Protocol is initializing (RESET/INIT states), retry current request later
      // This prevents pushing to Optolink while it's not ready to accept commands
      smart_queue_.retry_current();
      return;
    }
    
    // Get next request from SmartQueue and push to Optolink if ready
    QueuedRequest* req = smart_queue_.get_next();
    
    if (req != nullptr) {
      // Validate callback arg before processing
      if (req->callback_arg == nullptr) {
        ESP_LOGW(TAG, "Invalid callback arg for 0x%04X, skipping", req->address);
        smart_queue_.release_current();
        return;
      }
      
      // Check if Optolink queue has capacity (keep below 80% to avoid blocking)
      constexpr size_t OPTOLINK_MAX = 64;  // VITOWIFI_MAX_QUEUE_LENGTH
      if (_optolink->queue_size() > OPTOLINK_MAX * 0.8) {
        static uint32_t last_full_log = 0;
        if (millis() - last_full_log > 5000) {
          ESP_LOGD(TAG, "Optolink queue busy (%d/%d), throttling requests", 
                   _optolink->queue_size(), OPTOLINK_MAX);
          last_full_log = millis();
        }
        smart_queue_.retry_current();
        return;
      }
      
      // We have a request ready to process, push to Optolink's internal queue
      if (req->is_write) {
        // For writes, encode the data from the datapoint
        CbArg* cbArg = reinterpret_cast<CbArg*>(req->callback_arg);
        uint8_t* data = new uint8_t[req->length];
        cbArg->dp->encode(data, req->length);
        
        // Push to Optolink's internal queue
        bool success = _optolink->write(req->address, req->length, data, req->callback_arg);
        delete[] data;  // Optolink copies the data
        
        if (!success) {
          ESP_LOGV(TAG, "Optolink queue full for write 0x%04X, will retry", req->address);
          // Retry current request with throttling
          smart_queue_.retry_current();
          return;
        }
      } else {
        // Read request
        if (!_optolink->read(req->address, req->length, req->callback_arg)) {
          ESP_LOGV(TAG, "Optolink queue full for read 0x%04X, will retry", req->address);
          // Retry current request with throttling
          smart_queue_.retry_current();
          return;
        }
      }
      
      // Successfully queued to Optolink, but DON'T release SmartQueue yet
      // Release will happen in _onData or _onError callbacks after Optolink completes
    }
}

void VitoConnect::update() {
  // This will be called every "update_interval" milliseconds.
  static int cleanup_counter = 0;
  static size_t last_datapoint_index = 0;  // For batching
  
  ESP_LOGD(TAG, "Schedule sensor update (queue: %d writes, %d reads, total: %d)", 
           smart_queue_.write_count(), smart_queue_.read_count(), smart_queue_.size());

  // 1. First enqueue all WRITES (they get priority automatically via SmartQueue)
  for (Datapoint* dp : this->_datapoints) {
    if (dp->getLastUpdate() != 0) {
      ESP_LOGD(TAG, "Datapoint with address 0x%04X was modified and needs to be written.", dp->getAddress());
      
      uint8_t* data = new uint8_t[dp->getLength()];
      dp->encode(data, dp->getLength());

      // Create unique component ID from datapoint pointer (safe, no pointer comparison!)
      uint8_t comp_id = (reinterpret_cast<uintptr_t>(dp) >> 4) & 0xFF;

      // Write the modified datapoint - enqueue to SmartQueue, NOT Optolink
      CbArg* writeCbArg = new CbArg(this, dp, true, dp->getLastUpdate(), TYPE_UNKNOWN);        
      if (!smart_queue_.enqueue(dp->getAddress(), dp->getLength(), true, reinterpret_cast<void*>(writeCbArg), comp_id)) {
        ESP_LOGW(TAG, "Failed to queue write for 0x%04X", dp->getAddress());
        delete writeCbArg;
        delete[] data;
        continue;
      }
      
      // Also queue verification read (will execute after write completes)
      CbArg* readCbArg = new CbArg(this, dp, false, 0, TYPE_UNKNOWN, data);
      if (!smart_queue_.enqueue(dp->getAddress(), dp->getLength(), false, reinterpret_cast<void*>(readCbArg), comp_id)) {
        ESP_LOGW(TAG, "Failed to queue verification read for 0x%04X", dp->getAddress());
        delete readCbArg;
        delete[] data;
      }
    }
  }
  
  // 2. Then enqueue READS in batches to avoid watchdog timeout
  constexpr size_t BATCH_SIZE = 20;
  size_t datapoints_processed = 0;
  
  for (size_t i = last_datapoint_index; i < this->_datapoints.size(); i++) {
    Datapoint* dp = this->_datapoints[i];
    
    // Create unique component ID from datapoint pointer (safe!)
    uint8_t comp_id = (reinterpret_cast<uintptr_t>(dp) >> 4) & 0xFF;
    
    CbArg* arg = new CbArg(this, dp, false, 0, TYPE_UNKNOWN);
    if (!smart_queue_.enqueue(dp->getAddress(), dp->getLength(), false, reinterpret_cast<void*>(arg), comp_id)) {
      // Queue full or duplicate - not critical for reads
      delete arg;
    } else {
      datapoints_processed++;
      
      // Process only BATCH_SIZE datapoints per update to avoid watchdog
      if (datapoints_processed >= BATCH_SIZE) {
        last_datapoint_index = i + 1;  // Continue from next datapoint next time
        ESP_LOGV(TAG, "Batched %d datapoints, continuing in next update cycle", datapoints_processed);
        return;
      }
    }
  }
  
  // All datapoints processed, reset for next cycle
  last_datapoint_index = 0;
  
  // 3. Cleanup stale requests every 10 update cycles
  if (++cleanup_counter >= 10) {
    smart_queue_.cleanup_stale();
    cleanup_counter = 0;
    
    if (!smart_queue_.is_empty()) {
      ESP_LOGD(TAG, "Queue status: %d writes, %d reads pending", 
               smart_queue_.write_count(), smart_queue_.read_count());
    }
  }
}

void VitoConnect::_onData(uint8_t* data, uint8_t len, void* arg) {
  CbArg* cbArg = reinterpret_cast<CbArg*>(arg);

  if (cbArg->dp->getLastUpdate() > 0) {
    if (!cbArg->w && cbArg->d == nullptr) {
      ESP_LOGD(TAG, "Datapoint with address 0x%04X is eventually being written, waiting for confirmation.", cbArg->dp->getAddress());
    } else if (cbArg->w) { // this was a write operation
      ESP_LOGD(TAG, "Write operation for datapoint with address 0x%04X %s.", 
             cbArg->dp->getAddress(), data[0] == 0x00 ? "has been completed" : "failed");
    } else if (cbArg->d != nullptr) { // cbArg->d is only set if this read is intended to verify a previous write
      ESP_LOGD(TAG, "Verifying received data for datapoint with address 0x%04X.", cbArg->dp->getAddress());

      if (len != cbArg->dp->getLength()) {
        ESP_LOGW(TAG, "Expected length of %d was not met for datapoint with address 0x%04X.", cbArg->dp->getLength(), cbArg->dp->getAddress());
      } else if (memcmp(data, cbArg->d, len) == 0) {
        ESP_LOGD(TAG, "Previous write operation for datapoint with address 0x%04X was successfully verified.", cbArg->dp->getAddress());
        cbArg->dp->clearLastUpdate();
      } else {
        ESP_LOGW(TAG, "Previous write operation for datapoint with address 0x%04X failed verification.", cbArg->dp->getAddress());
      }
      // Free the data buffer that was allocated for verification
      delete[] cbArg->d;
    }
  } else if (!cbArg->w) {
    cbArg->dp->decode(data, len, cbArg->dp);
  }

  // Release the queue so next request can be processed
  cbArg->v->smart_queue_.release_current();

  delete cbArg;
}

void VitoConnect::_onError(uint8_t error, void* arg) {
  CbArg* cbArg = reinterpret_cast<CbArg*>(arg);
  ESP_LOGW(TAG, "Error received: %d for datapoint 0x%04X", error, cbArg->dp->getAddress());
  
  if (cbArg->v->_onErrorCb) cbArg->v->_onErrorCb(error, cbArg->dp);
  
  // Free the data buffer if it was allocated for verification
  if (cbArg->d != nullptr) {
    delete[] cbArg->d;
  }
  
  // Release the queue so next request can be processed (important for non-blocking!)
  cbArg->v->smart_queue_.release_current();
  
  delete cbArg;
}

}  // namespace vitoconnect
}  // namespace esphome


