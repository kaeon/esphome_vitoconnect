# Smart Queue Implementation - VitoConnect

## Overview

This implementation adds an intelligent queue system to the VitoConnect ESPHome component, based on the ViessData C# application's proven approach with additional enhancements for embedded systems.

## Key Features

### 1. **Write Priority**

- Write requests are processed before read requests
- Ensures critical configuration changes happen immediately
- Separate internal queues for writes and reads

### 2. **Automatic Deduplication**

- Prevents the same datapoint from being queued multiple times
- Reduces unnecessary communication overhead
- Checks both pending and currently processing requests

### 3. **Non-Blocking Operation**

- 30-second timeout protection prevents queue deadlock
- Automatic cleanup of stale requests
- Inter-communication delay (50ms) ensures protocol compliance
- Protocol ready check prevents queue buildup during heater startup

### 4. **Heater Startup Protection**

- Detects when protocol is initializing (RESET/INIT states)
- Pauses request processing until protocol is ready
- Prevents communication errors during heater boot sequence
- SmartQueue retains requests until heater is operational

### 5. **Memory Efficient**

- Maximum queue size: 32 requests (~1KB memory)
- Uses std::vector (already available in ESPHome)
- Smart cleanup prevents memory buildup

## Architecture

```
┌─────────────────────────────────────────────────────┐
│ VitoConnect::update() (every update_interval)      │
│ - Enqueues writes (PRIORITY)                       │
│ - Enqueues reads (NORMAL)                          │
│ - Periodic cleanup                                 │
└────────────────┬────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────┐
│ SmartQueue                                          │
│ ┌─────────────────────┐  ┌────────────────────────┐│
│ │ Write Queue         │  │ Read Queue             ││
│ │ (Priority)          │  │ (Normal)               ││
│ │ - Addr: 0x2323      │  │ - Addr: 0x0800         ││
│ │ - Addr: 0x6500      │  │ - Addr: 0x0802         ││
│ └─────────────────────┘  └────────────────────────┘│
│                                                     │
│ Features:                                           │
│ - Deduplication                                     │
│ - Timeout protection (30s)                          │
│ - Inter-comm delay (50ms)                           │
└────────────────┬────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────┐
│ VitoConnect::loop() (called continuously)           │
│ - Gets next request from SmartQueue                 │
│ - Pushes to Optolink's internal queue               │
│ - Processes Optolink state machine                  │
└────────────────┬────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────┐
│ Optolink (Protocol Layer)                           │
│ - Internal SimpleQueue for protocol state           │
│ - P300/KW protocol implementation                   │
│ - UART communication                                │
└────────────────┬────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────┐
│ Callbacks (_onData / _onError)                      │
│ - Process received data                             │
│ - Update datapoints                                 │
│ - Release SmartQueue → enables next request         │
└─────────────────────────────────────────────────────┘
```

## Implementation Details

### Files Modified/Created

1. **vitoconnect_smart_queue.h** (NEW)

   - SmartQueue class with priority and deduplication
   - QueuedRequest structure
   - Configuration constants

2. **vitoconnect.h** (MODIFIED)

   - Added `#include "vitoconnect_smart_queue.h"`
   - Added `SmartQueue smart_queue_;` member

3. **vitoconnect.cpp** (MODIFIED)

   - `update()`: Enqueues to SmartQueue instead of directly to Optolink
   - `loop()`: Checks protocol readiness, retrieves from SmartQueue and forwards to Optolink
   - `_onData()`: Releases SmartQueue after successful completion
   - `_onError()`: Releases SmartQueue after error

4. **vitoconnect_optolink.h** (MODIFIED)

   - Added virtual `is_ready()` method to base class

5. **vitoconnect_optolinkP300.h** (MODIFIED)

   - Implemented `is_ready()` - returns true when in IDLE or active communication state

6. **vitoconnect_optolinkKW.h** (MODIFIED)
   - Implemented `is_ready()` - returns true when in IDLE or active communication state

### Request Flow

**Before (old system):**

```
update() → Optolink→write/read() → SimpleQueue → loop() → Protocol
```

**After (new system):**

```
update() → SmartQueue→enqueue() → loop() → get_next() → Optolink→write/read() → SimpleQueue → Protocol
                                                                                     ↓
                                                                                  _onData/_onError
                                                                                     ↓
                                                                              release_current()
```

## Configuration Constants

```cpp
MAX_QUEUE_SIZE = 32         // Maximum pending requests
INTER_COMM_DELAY_MS = 50    // Minimum delay between messages (protocol requirement)
REQUEST_TIMEOUT_MS = 30000  // Request timeout (prevents blocking)
```

## Logging

The implementation provides detailed logging at different levels:

- **DEBUG**: Queue operations, request processing
- **VERBOSE**: Individual read requests (reduces log spam)
- **WARNING**: Queue full, timeouts, errors

Example logs:

```
[D][vitoconnect:103]: Schedule sensor update (queue: 1 writes, 15 reads)
[D][vitoconnect.queue:091]: Enqueued WRITE 0x2323 (writes:1, reads:0)
[V][vitoconnect.queue:096]: Enqueued read 0x0800 (writes:1, reads:1)
[D][vitoconnect.queue:130]: Processing WRITE 0x2323
[V][vitoconnect.queue:134]: Processing read 0x0800
[D][vitoconnect.queue:079]: Duplicate avoided: 0x0800 read
```

## Benefits Over Old System

| Feature             | Old System                 | New System                        |
| ------------------- | -------------------------- | --------------------------------- |
| Write Priority      | Manual check, skip reads   | Automatic via separate queues     |
| Deduplication       | None                       | Automatic for same address+type   |
| Timeout Protection  | None                       | 30s timeout with auto-cleanup     |
| Queue Full Handling | Returns false, silent fail | Logged warning, retry support     |
| Inter-comm Delay    | SimpleQueue only           | SmartQueue + SimpleQueue (50ms)   |
| Blocking Prevention | Could block on errors      | Timeout + release in callbacks    |
| Startup Protection  | None                       | Protocol ready check              |
| Heater Boot Issues  | Could flood with requests  | Waits for protocol initialization |
| Memory Usage        | ~2KB (64 items)            | ~1KB (32 items with dedup)        |

## Heater Startup Protection Details

### Problem Scenario

When a Viessmann heater starts up:

1. Heater sends spontaneous status messages on Optolink
2. Protocol needs time to initialize (RESET → INIT → IDLE)
3. ESPHome's `update()` is called, trying to queue 10-20+ datapoint reads
4. Without protection: Queue floods, communication errors, possible lockup

### Solution

The new `is_ready()` check in `loop()`:

```cpp
void VitoConnect::loop() {
    _optolink->loop();  // Process protocol state machine

    if (!_optolink->is_ready()) {
      return;  // Protocol initializing, don't push requests yet
    }

    // Now safe to push requests to Optolink
}
```

### Protocol States

**P300 Protocol:**

- ❌ NOT READY: `RESET`, `RESET_ACK`, `INIT`, `INIT_ACK`, `UNDEF`
- ✅ READY: `IDLE`, `SEND`, `SEND_ACK`, `RECEIVE`, `RECEIVE_ACK`

**KW Protocol:**

- ❌ NOT READY: `INIT`, `SYNC`, `UNDEF`
- ✅ READY: `IDLE`, `SEND`, `RECEIVE`

### Behavior During Startup

1. **Time 0s**: Heater powers on
2. **Time 0-5s**: Protocol in RESET/INIT, `is_ready()` = false
3. **SmartQueue**: Retains all queued requests (max 32)
4. **Time 5s**: Protocol reaches IDLE, `is_ready()` = true
5. **Processing begins**: Requests are now pushed to Optolink safely
6. **Inter-comm delay**: 50ms between each request prevents flooding

### Recovery After Communication Loss

If Optolink times out (e.g., heater briefly offline):

1. Protocol automatically goes to RESET state
2. `is_ready()` returns false
3. SmartQueue pauses new requests
4. Protocol re-initializes
5. When IDLE again, normal operation resumes
6. Stale requests (>30s) are automatically cleaned up

## Testing Checklist

- [ ] Write operations execute before reads
- [ ] Duplicate reads are prevented
- [ ] Write verification works correctly
- [ ] Timeout protection activates after 30s
- [ ] Inter-comm delay prevents protocol violations
- [ ] Queue statistics are logged correctly
- [ ] System recovers from Optolink queue full condition
- [ ] Error handling releases queue properly
- [ ] Multiple simultaneous writes are handled
- [ ] System performance is acceptable
- [ ] **Heater startup doesn't cause communication errors**
- [ ] **Protocol ready check prevents queue flooding**
- [ ] **System recovers gracefully after heater reboot**

## Compatibility

- ✅ Compatible with existing ESPHome configurations
- ✅ No changes required to YAML configuration
- ✅ Works with both P300 and KW protocols
- ✅ Backward compatible with existing datapoint definitions

## Performance Considerations

- **Memory**: ~1KB for queue (32 × 32 bytes per request)
- **CPU**: Minimal overhead, O(n) deduplication check where n ≤ 32
- **Latency**: +50ms minimum between requests (protocol requirement)
- **Throughput**: Unchanged, limited by protocol speed (4800 baud)

## Future Enhancements (Optional)

- Configurable queue size via YAML
- Configurable inter-comm delay
- Queue statistics exposed as sensors
- Retry mechanism for failed writes
- Priority levels beyond write/read (critical/high/normal/low)

## Credits

- Original queue concept: Phil Oebel (ViessData C# application)
- VitoWiFi library: Bert Melis
- ESPHome integration: Philipp Danner
- Smart Queue enhancement: Based on analysis of ViessData source code
