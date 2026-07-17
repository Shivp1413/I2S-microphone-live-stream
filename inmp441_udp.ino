#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "driver/i2s_std.h"

// ============================================================================
// Configuration
// ============================================================================

// --- WiFi & Network ---
const char* AP_SSID  = "mystream";
const char* AP_PASS  = "12345678";
const uint16_t UDP_PORT = 4210;
const uint32_t CLIENT_TIMEOUT_MS = 4000; // Drop client if no keep-alive for this long

// --- I2S Hardware Pins (ESP32-S3 to INMP441) ---
// VDD -> 3V3, GND -> GND, L/R -> GND (Left Channel)
#define I2S_BCLK_PIN   5
#define I2S_WS_PIN     6
#define I2S_DATA_PIN   4

// --- Audio Format ---
#define USE_STEREO_I2S    0
#define SAMPLE_RATE       16000
#define BITS_PER_SAMPLE   16

#if USE_STEREO_I2S
  #define CHANNELS          2
  #define I2S_SLOT_MODE_SEL I2S_SLOT_MODE_STEREO
  #define I2S_SLOT_MASK_SEL I2S_STD_SLOT_BOTH
#else
  #define CHANNELS          1
  #define I2S_SLOT_MODE_SEL I2S_SLOT_MODE_MONO
  #define I2S_SLOT_MASK_SEL I2S_STD_SLOT_LEFT // L/R tied to GND
#endif

#define BYTES_PER_SAMPLE  (BITS_PER_SAMPLE / 8)
#define BLOCK_ALIGN       (CHANNELS * BYTES_PER_SAMPLE)

// INMP441 outputs 24-bit left-aligned data in a 32-bit slot
#define I2S_RAW_BYTES_PS  4
#define INMP441_SHIFT     16

// --- Buffer & Latency Tuning ---
#define I2S_READ_BYTES      512   // 256 mono frames = ~16ms per I2S read
#define UDP_PACKET_BYTES    512   // Keep <= 1400 to avoid IP fragmentation
#define RING_BUFFER_BYTES   16384 // Small ring buffer; UDP doesn't need huge cushions

#define TARGET_LATENCY_MS   60
#define BYTE_RATE           (SAMPLE_RATE * BLOCK_ALIGN)
#define TARGET_LATENCY_BYTES ((BYTE_RATE * TARGET_LATENCY_MS) / 1000)

// --- Audio Processing (AGC & Filtering) ---
#define ENABLE_AGC          1
#define AGC_INITIAL_GAIN    8.0f
#define AGC_MAX_GAIN        30.0f
#define AGC_MIN_GAIN        0.25f
#define AGC_TARGET_PEAK     22000.0f
#define AGC_SILENCE_PEAK    40
#define AGC_GAIN_UP_SPEED   0.08f   // Slow attack (raise gain gradually)
#define AGC_GAIN_DOWN_SPEED 0.45f   // Fast release (drop gain quickly to prevent clipping)
#define OUTPUT_BOOST        1.20f
#define HPF_COEFF           0.970f  // 1st order high-pass filter coefficient


// ============================================================================
// Global State
// ============================================================================

// Ring Buffer
uint8_t ringBuffer[RING_BUFFER_BYTES];
portMUX_TYPE ringBufferMutex = portMUX_INITIALIZER_UNLOCKED;
size_t ringReadPos = 0;
size_t ringWritePos = 0;
size_t ringCount = 0;

// I2S Handle
i2s_chan_handle_t i2sRxHandle = NULL;

// Audio Processing State
float agcGain = AGC_INITIAL_GAIN;
float hpfStateX[2] = {0.0f, 0.0f};
float hpfStateY[2] = {0.0f, 0.0f};

// Network State
WiFiUDP udp;
IPAddress clientIP;
uint16_t clientPort = 0;
bool haveClient = false;
unsigned long lastHelloMs = 0;


// ============================================================================
// Helpers
// ============================================================================

static inline size_t minSize(size_t a, size_t b) { 
  return a < b ? a : b; 
}

static inline int16_t clamp16FromFloat(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)v;
}

static inline int32_t clamp32To16Range(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return v;
}

void resetAudioProcessor() {
  agcGain = AGC_INITIAL_GAIN;
  hpfStateX[0] = hpfStateX[1] = 0.0f;
  hpfStateY[0] = hpfStateY[1] = 0.0f;
}

size_t convertRawToPcm16(const int32_t *raw32, size_t rawSamples, int16_t *out16) {
  for (size_t i = 0; i < rawSamples; i++) {
    int32_t v = raw32[i] >> INMP441_SHIFT;
    out16[i] = (int16_t)clamp32To16Range(v);
  }
  return rawSamples * sizeof(int16_t);
}


// ============================================================================
// Audio Processing: HPF + AGC + Soft Limiter
// ============================================================================

void processAudioInPlace(uint8_t *buf, size_t len) {
  // Ensure we only process complete audio frames
  len -= len % BLOCK_ALIGN;
  if (len == 0) return;

  int16_t *samples = (int16_t *)buf;
  size_t sampleCount = len / sizeof(int16_t);

  // 1. High-Pass Filter & Peak Detection
  uint32_t peak = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    int ch = i % CHANNELS;
    float x = (float)samples[i];
    
    // 1st order HPF: y[n] = x[n] - x[n-1] + coeff * y[n-1]
    float y = x - hpfStateX[ch] + HPF_COEFF * hpfStateY[ch];
    hpfStateX[ch] = x;
    hpfStateY[ch] = y;
    
    int32_t yi = clamp32To16Range((int32_t)y);
    samples[i] = (int16_t)yi;
    
    uint32_t absY = (yi < 0) ? (uint32_t)(-yi) : (uint32_t)yi;
    if (absY > peak) peak = absY;
  }

  // 2. Automatic Gain Control (AGC)
#if ENABLE_AGC
  if (peak > AGC_SILENCE_PEAK) {
    float desiredGain = AGC_TARGET_PEAK / (float)peak;
    desiredGain = constrain(desiredGain, AGC_MIN_GAIN, AGC_MAX_GAIN);
    
    // Asymmetric attack/release: drop gain fast, raise it slowly
    if (desiredGain < agcGain) {
      agcGain = agcGain * (1.0f - AGC_GAIN_DOWN_SPEED) + desiredGain * AGC_GAIN_DOWN_SPEED;
    } else {
      agcGain = agcGain * (1.0f - AGC_GAIN_UP_SPEED) + desiredGain * AGC_GAIN_UP_SPEED;
    }
  }
  float totalGain = agcGain * OUTPUT_BOOST;
#else
  float totalGain = AGC_INITIAL_GAIN * OUTPUT_BOOST;
#endif

  // 3. Apply Gain & Soft Clipping Limiter
  for (size_t i = 0; i < sampleCount; i++) {
    float v = (float)samples[i] * totalGain;
    
    // Soft knee clipping to prevent harsh digital distortion
    if (v > 30000.0f) {
      v = 30000.0f + (v - 30000.0f) * 0.20f;
    } else if (v < -30000.0f) {
      v = -30000.0f + (v + 30000.0f) * 0.20f;
    }
    
    samples[i] = clamp16FromFloat(v);
  }
}


// ============================================================================
// Ring Buffer Management
// ============================================================================

void ringClear() {
  portENTER_CRITICAL(&ringBufferMutex);
  ringReadPos = ringWritePos = ringCount = 0;
  portEXIT_CRITICAL(&ringBufferMutex);
}

void ringDropOldest_NoLock(size_t bytesToDrop) {
  if (bytesToDrop == 0) return;
  bytesToDrop -= bytesToDrop % BLOCK_ALIGN;
  if (bytesToDrop == 0) return;
  if (bytesToDrop > ringCount) bytesToDrop = ringCount;
  
  ringReadPos = (ringReadPos + bytesToDrop) % RING_BUFFER_BYTES;
  ringCount -= bytesToDrop;
}

void ringTrimTo(size_t maxBytes) {
  maxBytes -= maxBytes % BLOCK_ALIGN;
  portENTER_CRITICAL(&ringBufferMutex);
  if (ringCount > maxBytes) {
    size_t dropBytes = ringCount - maxBytes;
    dropBytes -= dropBytes % BLOCK_ALIGN;
    ringReadPos = (ringReadPos + dropBytes) % RING_BUFFER_BYTES;
    ringCount -= dropBytes;
  }
  portEXIT_CRITICAL(&ringBufferMutex);
}

void ringWriteDropOldest(const uint8_t *data, size_t len) {
  if (len == 0) return;
  len -= len % BLOCK_ALIGN;
  if (len == 0) return;
  
  // If incoming data exceeds buffer size, just keep the newest portion
  if (len > RING_BUFFER_BYTES) { 
    data += len - RING_BUFFER_BYTES; 
    len = RING_BUFFER_BYTES; 
  }

  portENTER_CRITICAL(&ringBufferMutex);
  size_t freeSpace = RING_BUFFER_BYTES - ringCount;
  
  // Make room if necessary
  if (len > freeSpace) {
    size_t dropBytes = len - freeSpace;
    dropBytes -= dropBytes % BLOCK_ALIGN;
    ringDropOldest_NoLock(dropBytes);
  }
  
  // Write data (handle wrap-around)
  size_t firstPart = minSize(len, RING_BUFFER_BYTES - ringWritePos);
  memcpy(&ringBuffer[ringWritePos], data, firstPart);
  
  size_t secondPart = len - firstPart;
  if (secondPart > 0) {
    memcpy(&ringBuffer[0], data + firstPart, secondPart);
  }
  
  ringWritePos = (ringWritePos + len) % RING_BUFFER_BYTES;
  ringCount += len;
  portEXIT_CRITICAL(&ringBufferMutex);
}

size_t ringRead(uint8_t *out, size_t maxLen) {
  maxLen -= maxLen % BLOCK_ALIGN;
  if (maxLen == 0) return 0;
  
  portENTER_CRITICAL(&ringBufferMutex);
  size_t len = minSize(maxLen, ringCount);
  len -= len % BLOCK_ALIGN;
  
  if (len == 0) { 
    portEXIT_CRITICAL(&ringBufferMutex); 
    return 0; 
  }
  
  // Read data (handle wrap-around)
  size_t firstPart = minSize(len, RING_BUFFER_BYTES - ringReadPos);
  memcpy(out, &ringBuffer[ringReadPos], firstPart);
  
  size_t secondPart = len - firstPart;
  if (secondPart > 0) {
    memcpy(out + firstPart, &ringBuffer[0], secondPart);
  }
  
  ringReadPos = (ringReadPos + len) % RING_BUFFER_BYTES;
  ringCount -= len;
  portEXIT_CRITICAL(&ringBufferMutex);
  
  return len;
}


// ============================================================================
// I2S Microphone Setup
// ============================================================================

bool setupI2sMic() {
  Serial.println("Initializing INMP441 I2S microphone...");

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = 8;
  chanCfg.dma_frame_num = 256;

  if (i2s_new_channel(&chanCfg, NULL, &i2sRxHandle) != ESP_OK) {
    Serial.println("ERROR: Failed to create I2S channel");
    return false;
  }

  i2s_std_config_t stdCfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_SEL),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_BCLK_PIN,
      .ws   = (gpio_num_t)I2S_WS_PIN,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_DATA_PIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  stdCfg.slot_cfg.slot_mask = I2S_SLOT_MASK_SEL;

  if (i2s_channel_init_std_mode(i2sRxHandle, &stdCfg) != ESP_OK) {
    Serial.println("ERROR: Failed to initialize I2S standard mode");
    return false;
  }

  if (i2s_channel_enable(i2sRxHandle) != ESP_OK) {
    Serial.println("ERROR: Failed to enable I2S channel");
    return false;
  }

  Serial.println("INMP441 initialized successfully.");
  return true;
}


// ============================================================================
// FreeRTOS Tasks
// ============================================================================

// Core 0 Task: Time-critical I2S reading and audio processing
void micTask(void *param) {
  // Allocate buffers statically to avoid stack fragmentation
  static int32_t rawBuffer[I2S_READ_BYTES / sizeof(int16_t)];
  static int16_t pcmBuffer[I2S_READ_BYTES / sizeof(int16_t)];
  size_t bytesRead = 0;

  while (true) {
    esp_err_t err = i2s_channel_read(i2sRxHandle, rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
    
    if (err == ESP_OK && bytesRead >= I2S_RAW_BYTES_PS) {
      size_t rawSamples = bytesRead / sizeof(int32_t);
      size_t pcmBytes = convertRawToPcm16(rawBuffer, rawSamples, pcmBuffer);
      
      processAudioInPlace((uint8_t *)pcmBuffer, pcmBytes);
      ringWriteDropOldest((uint8_t *)pcmBuffer, pcmBytes);
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

// Core 1 Task: Network handling and UDP streaming
void streamTask(void *param) {
  uint8_t outBuffer[UDP_PACKET_BYTES];

  while (true) {
    // 1. Check for incoming client "keep-alive" packets
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      uint8_t discardBuffer[64];
      udp.read(discardBuffer, sizeof(discardBuffer)); // Payload doesn't matter, packet itself is the signal
      
      clientIP = udp.remoteIP();
      clientPort = udp.remotePort();
      lastHelloMs = millis();

      if (!haveClient) {
        haveClient = true;
        resetAudioProcessor();
        ringClear(); // Flush stale audio so we start streaming live data immediately
        Serial.printf("Client registered: %s:%d\n", clientIP.toString().c_str(), clientPort);
      }
    }

    // 2. Timeout check
    if (haveClient && (millis() - lastHelloMs > CLIENT_TIMEOUT_MS)) {
      haveClient = false;
      Serial.println("Client timed out. Streaming paused.");
    }

    // 3. Stream audio if connected
    if (!haveClient) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // Prevent buffer bloat: drop old audio if network lags to maintain low latency
    ringTrimTo(TARGET_LATENCY_BYTES);

    size_t n = ringRead(outBuffer, sizeof(outBuffer));
    if (n > 0) {
      udp.beginPacket(clientIP, clientPort);
      udp.write(outBuffer, n);
      udp.endPacket();
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}


// ============================================================================
// Arduino Entry Points
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- ESP32-S3 INMP441 UDP Realtime Mic ---");

  resetAudioProcessor();

  if (!setupI2sMic()) {
    Serial.println("FATAL: INMP441 setup failed. Halting.");
    while (true) delay(1000);
  }

  ringClear();

  // Pin mic task to Core 0 for deterministic I2S timing
  xTaskCreatePinnedToCore(micTask, "micTask", 6144, NULL, 3, NULL, 0);

  // Start WiFi Access Point
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false); // Disable modem sleep for lower latency
  if (!WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 2)) {
    Serial.println("FATAL: WiFi AP failed to start. Halting.");
    while (true) delay(1000);
  }
  Serial.printf("WiFi AP started. IP: %s\n", WiFi.softAPIP().toString().c_str());

  udp.begin(UDP_PORT);
  Serial.printf("UDP listening on port %d. Ready for client.\n", UDP_PORT);

  // Pin streaming task to Core 1 (handles WiFi stack, can tolerate minor jitter)
  xTaskCreatePinnedToCore(streamTask, "streamTask", 4096, NULL, 2, NULL, 1);
}

void loop() {
  // Main loop is intentionally empty; all work is handled by FreeRTOS tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}
