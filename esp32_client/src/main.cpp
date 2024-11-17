// main.cpp
#include <M5Core2.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "config.h"
#include "ConfigManager.h"
#include "constants.h"

// Global variables
WebSocketsClient webSocket;
bool wsConnected = false;

uint8_t microphonedata0[DATA_SIZE * BUFFER_LEN];

Config config;

// SemaphoreHandle_t audioMutex = NULL;

int currentVolume = 50;
TouchPoint_t touchPos;

void processAudioInPlace(uint8_t *buffer, size_t length, int volume) {
    // Convert volume to float (0.0 - 1.0)
    float volumeScale = constrain(volume, 0, 100) / 100.0f;
    
    // Compression parameters
    const float threshold = 16384.0f;  // 50% of max amplitude
    const float ratio = 4.0f;          // Compression ratio above threshold
    const float knee = 2048.0f;        // Smooth transition around threshold
    
    for (size_t i = 0; i < length; i += 2) {
        // Get sample
        int16_t sample = (int16_t)((buffer[i + 1] << 8) | buffer[i]);
        float fsample = (float)sample;
        
        // Apply soft knee compression
        float absample = abs(fsample);
        if (absample > threshold) {
            float excess = absample - threshold;
            float reduction = excess - (excess / ratio);
            fsample = copysignf(absample - reduction, fsample);
        } else if (absample > (threshold - knee)) {
            float excess = absample - (threshold - knee);
            float reduction = excess * excess / (2.0f * knee) / ratio;
            fsample = copysignf(absample - reduction, fsample);
        }
        
        // Apply volume scaling
        fsample *= volumeScale;
        
        // Clamp to int16_t range
        if (fsample > 32767.0f) fsample = 32767.0f;
        if (fsample < -32768.0f) fsample = -32768.0f;
        
        // Write back to the same buffer
        int16_t finalSample = (int16_t)fsample;
        buffer[i] = finalSample & 0xFF;
        buffer[i + 1] = (finalSample >> 8) & 0xFF;
    }
}

// Optional debug function remains the same
void printAudioStats(const char* label, uint8_t* buffer, size_t length) {
    int16_t min_val = 32767;
    int16_t max_val = -32768;
    float rms = 0;
    
    for (size_t i = 0; i < length; i += 2) {
        int16_t sample = (int16_t)((buffer[i + 1] << 8) | buffer[i]);
        min_val = min(min_val, sample);
        max_val = max(max_val, sample);
        rms += (float)sample * sample;
    }
    
    rms = sqrt(rms / (length / 2));
    
    Serial.printf("%s - Min: %d, Max: %d, RMS: %.1f\n", 
                 label, min_val, max_val, rms);
}

void processMicAudio(uint8_t *buffer, size_t length) {
    // Microphone processing parameters
    const float gainFactor = 2.0f;      // Boost quiet signals
    const float threshold = 8192.0f;    // Lower threshold for earlier compression
    const float ratio = 2.0f;           // Gentler compression ratio
    const float knee = 4096.0f;         // Wider knee for smoother transition
    
    for (size_t i = 0; i < length; i += 2) {
        // Get sample
        int16_t sample = (int16_t)((buffer[i + 1] << 8) | buffer[i]);
        float fsample = (float)sample;
        
        // Apply gain first
        fsample *= gainFactor;
        
        // Compression for loud sounds
        float absample = abs(fsample);
        if (absample > threshold) {
            float excess = absample - threshold;
            float reduction = excess - (excess / ratio);
            fsample = copysignf(absample - reduction, fsample);
        } else if (absample > (threshold - knee)) {
            float excess = absample - (threshold - knee);
            float reduction = excess * excess / (2.0f * knee) / ratio;
            fsample = copysignf(absample - reduction, fsample);
        }
        
        // Prevent clipping
        if (fsample > 32767.0f) fsample = 32767.0f;
        if (fsample < -32768.0f) fsample = -32768.0f;
        
        // Store back
        int16_t finalSample = (int16_t)fsample;
        buffer[i] = finalSample & 0xFF;
        buffer[i + 1] = (finalSample >> 8);
    }
}

bool InitI2SSpeakOrMic(int mode) {
    // Init I2S
    esp_err_t err = ESP_OK;

    Serial.printf("Initializing I2S with sample rate: %d Hz\n", SAMPLE_RATE);
    Serial.printf("DMA Buffer: Count=%d, Length=%d\n", DMA_BUFFER_COUNT, DMA_BUFFER_LEN);
    
    // Uninstall the I2S driver
    i2s_driver_uninstall(Speak_I2S_NUMBER);
    
    i2s_config_t i2s_config = {
        // Set the I2S operating mode
        .mode = (i2s_mode_t)(I2S_MODE_MASTER),
        
        // Set the I2S sampling rate
        .sample_rate = SAMPLE_RATE,
        
        // Fixed 12-bit stereo MSB
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        
        // Set the channel format
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 1, 0)
        // Set the format of the communication
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
#else
        .communication_format = I2S_COMM_FORMAT_I2S,
#endif
        // Set the interrupt flag
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        
        // DMA buffer count
        .dma_buf_count = DMA_BUFFER_COUNT,
        
        // DMA buffer length
        .dma_buf_len = DMA_BUFFER_LEN,
    };

    if (mode == MODE_MIC) {
        i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    } else {
        i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        // I2S clock setup
        i2s_config.use_apll = false;
        // Enables auto-cleanup descriptors for understreams
        i2s_config.tx_desc_auto_clear = true;
    }

    // Install and drive I2S
    err += i2s_driver_install(Speak_I2S_NUMBER, &i2s_config, 0, NULL);
    
    i2s_pin_config_t tx_pin_config;

#if (ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 3, 0))
    tx_pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
#endif

    // Link the BCK to the CONFIG_I2S_BCK_PIN pin
    tx_pin_config.bck_io_num = CONFIG_I2S_BCK_PIN;
    tx_pin_config.ws_io_num = CONFIG_I2S_LRCK_PIN;
    tx_pin_config.data_out_num = CONFIG_I2S_DATA_PIN;
    tx_pin_config.data_in_num = CONFIG_I2S_DATA_IN_PIN;

    // Set the I2S pin number
    err += i2s_set_pin(Speak_I2S_NUMBER, &tx_pin_config);

    // Set the clock and bitwidth used by I2S Rx and Tx
    err += i2s_set_clk(Speak_I2S_NUMBER, SAMPLE_RATE, 
                        I2S_BITS_PER_SAMPLE_16BIT, 
                        I2S_CHANNEL_MONO);

    return true;
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("###########################");
            Serial.println("WebSocket Disconnected!");
            Serial.printf("Heap: %d, WiFi Status: %d\n", ESP.getFreeHeap(), WiFi.status());
            Serial.println("###########################");
            wsConnected = false;
            break;

        case WStype_CONNECTED:
            Serial.printf("WebSocket Connected! Heap: %d\n", ESP.getFreeHeap());
            wsConnected = true;
            break;

        case WStype_BIN: { 

            if (!payload || length == 0) {
                Serial.println("Error: Invalid audio payload or length");
                break;
            }

            uint8_t *audioChunk = (uint8_t *)malloc(length);
            if (audioChunk == nullptr) {
                Serial.println("Failed to allocate memory for audio chunk");
            }

            memcpy(audioChunk, payload, length);

            printAudioStats("Before", audioChunk, length);
            processAudioInPlace(audioChunk, length, currentVolume);
            printAudioStats("After", audioChunk, length);

            // Analyze first few samples
            Serial.printf("\n--- Audio Chunk Analysis ---\n");
            Serial.printf("Chunk size: %d bytes (%d samples)\n", length, length/2);
            
            // Look at first few samples to verify data format
            if (length >= 16) {
                Serial.printf("First 8 samples (16-bit signed):\n");
                for (int i = 0; i < 16; i += 2) {
                    int16_t sample = (int16_t)((payload[i+1] << 8) | payload[i]);
                    Serial.printf("%d ", sample);
                }
                Serial.println("\n");
                
                // Also show as raw bytes
                Serial.printf("Raw bytes (first 16):\n");
                for (int i = 0; i < 16; i++) {
                    Serial.printf("%02X ", payload[i]);
                }
                Serial.println("\n");
            }

            size_t bytes_written = 0;            
            // Direct I2S write
            esp_err_t result = i2s_write(Speak_I2S_NUMBER, 
                                         audioChunk, 
                                         length, 
                                         &bytes_written, 
                                         portMAX_DELAY);
            free(audioChunk);
            if (result != ESP_OK) {
                Serial.printf("Error in i2s_write: %s\n", esp_err_to_name(result));
            } else {
                Serial.printf("Successfully played %d bytes\n", bytes_written);
            }

        }
        break;

        case WStype_TEXT:
            // Optionally handle text messages or ignore
            break;

        case WStype_ERROR:
            Serial.println("WebSocket Error!");
            Serial.printf("Error payload: %s\n", payload);  // Add error details
            Serial.printf("Heap: %d, WiFi Status: %d\n", ESP.getFreeHeap(), WiFi.status());
            break;

        default:
            Serial.printf("Unknown WebSocket event type: %d\n", type);
            if (payload && length > 0) {
                Serial.printf("Payload length: %d\n", length);
                Serial.printf("Payload: ");
                for (size_t i = 0; i < length; i++) {
                    Serial.printf("%02x ", payload[i]);
                }
                Serial.println();
            }
            break;
    }
}

void DisplayInit(void) {       // Initialize the display. 
    M5.Lcd.fillScreen(WHITE);  // Set the screen background color to white.
    M5.Lcd.setTextColor(
        BLACK);  // Set the text color to black.  
    M5.Lcd.setTextSize(2);  // Set font size to 2.  
}

void setup() {
    M5.begin(true, true, true, true, kMBusModeOutput, true); // Init M5Core2. 
    Serial.printf("Free heap before setup: %d bytes\n", ESP.getFreeHeap());

    M5.Axp.SetSpkEnable(true);  // Enable speaker power. 
    Serial.println("Speaker enabled");

    DisplayInit();
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(10,
                     10);  // Set the cursor at (10,10). 
    M5.Lcd.printf("Gojo Koala");  // The screen prints the formatted string and
                                 // wraps it. 
    M5.Lcd.setTextColor(BLACK);
    M5.Lcd.setCursor(10, 26);
    M5.Lcd.printf("I'm ready to go!");
    delay(100);  // delay 100ms. 

    if (!ConfigManager::init() || !ConfigManager::loadConfig(config)) {
        Serial.println("Configuration initialization failed!");
        return;
    }

    // Connect to WiFi and WebSocket
    WiFi.begin(config.wifi_ssid.c_str(), config.wifi_password.c_str());
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nWiFi connected!");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Connecting to WebSocket: %s:%d\n", config.server_ip.c_str(), config.server_port);
    
    webSocket.begin(config.server_ip.c_str(),
                    config.server_port,
                    "/ws");

    webSocket.onEvent(webSocketEvent);
    webSocket.enableHeartbeat(WEBSOCKET_PING_INTERVAL, 
                              WEBSOCKET_PONG_TIMEOUT, 
                              WEBSOCKET_DISCONNECT_THRESHOLD);

    InitI2SSpeakOrMic(MODE_SPK);     // init in speaker mode 
}

void loop() {
    static int data_offset; 
    touchPos = M5.Touch.getPressPoint();  // Stores the touch coordinates in pos.

    webSocket.loop();

    if (touchPos.y > 240) {
        if (touchPos.x < 109) {
            Serial.println("Recording started!");
            M5.Axp.SetVibration(true);  // Open the vibration. 

            data_offset = 0;
            InitI2SSpeakOrMic(MODE_MIC);
            Serial.println("Microphone initialized");

            size_t byte_read;
            while (1) {
                esp_err_t result = i2s_read(Speak_I2S_NUMBER,
                                            (char *)(microphonedata0 + data_offset), 
                                            DATA_SIZE,
                                            &byte_read, 
                                            pdMS_TO_TICKS(100));
                if (result != ESP_OK) {
                    Serial.printf("Error in i2s_read: %s\n", esp_err_to_name(result));
                    break; 
                }
                processMicAudio(microphonedata0 + data_offset, byte_read);
                data_offset += DATA_SIZE;
                Serial.printf("Bytes read: %d\n", byte_read);

                if (data_offset == DATA_SIZE * BUFFER_LEN) {
                    Serial.println("Recording stopped - Buffer full");
                    // Send remaining data to WebSocket if necessary
                    if (data_offset > 0) {
                        webSocket.sendBIN((uint8_t *)microphonedata0, data_offset);
                        Serial.printf("Sent %d bytes to server\n", data_offset);
                    }
                    break;
                }
                
                if (M5.Touch.ispressed() != true) {
                    Serial.println("Recording stopped - Touch released");
                    // send audio to server
                    webSocket.sendBIN((uint8_t *)microphonedata0, data_offset);
                    Serial.printf("Sent %d bytes to server\n", data_offset);
                    break;
                } 
            }
            InitI2SSpeakOrMic(MODE_SPK);
        }
    }
    M5.Axp.SetVibration(false);

    // Small delay to prevent tight looping
    vTaskDelay(pdMS_TO_TICKS(10));
}

