#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <esp_camera.h>
#include <driver/i2s.h>
#include <esp_bt.h>
#include <esp_wifi.h>
#include <time.h>

// NOTE: Continuous audio recording requires the MCU and I2S/PDM path to remain active.
// Deep sleep cannot be used while continuous audio capture is running.

namespace cfg {
constexpr int SD_CS_PIN = 21;

constexpr int MIC_DATA_PIN = 41;
constexpr int MIC_CLK_PIN = 42;

constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint8_t AUDIO_BITS_PER_SAMPLE = 16;
constexpr uint8_t AUDIO_CHANNELS = 1;
constexpr size_t AUDIO_BUFFER_BYTES = 2048;
constexpr uint32_t AUDIO_ROTATE_MS = 60000;
constexpr uint32_t AUDIO_FLUSH_MS = 1000;
constexpr uint32_t PHOTO_INTERVAL_MS = 60000;

constexpr int CPU_FREQ_MHZ = 80;
}  // namespace cfg

static SemaphoreHandle_t sdMutex = nullptr;
static volatile uint32_t monotonicFileCounter = 0;
static bool sdMounted = false;

struct WavHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t chunkSize = 36;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = cfg::AUDIO_CHANNELS;
  uint32_t sampleRate = cfg::AUDIO_SAMPLE_RATE;
  uint32_t byteRate = cfg::AUDIO_SAMPLE_RATE * cfg::AUDIO_CHANNELS * (cfg::AUDIO_BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = cfg::AUDIO_CHANNELS * (cfg::AUDIO_BITS_PER_SAMPLE / 8);
  uint16_t bitsPerSample = cfg::AUDIO_BITS_PER_SAMPLE;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t subchunk2Size = 0;
};

bool ensureSdMounted(uint8_t retries = 3) {
  if (sdMounted) {
    return true;
  }

  for (uint8_t i = 0; i < retries; ++i) {
    if (SD.begin(cfg::SD_CS_PIN)) {
      sdMounted = true;
      SD.mkdir("/audio");
      SD.mkdir("/photo");
      Serial.println("[SD] Mounted");
      return true;
    }
    Serial.printf("[SD] Mount failed (%u/%u)\n", i + 1, retries);
    delay(500);
  }

  return false;
}

String timestampString(const char *prefix, const char *ext) {
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 5)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &timeInfo);
    return String(prefix) + "/" + String(buf) + "." + ext;
  }

  uint32_t id = ++monotonicFileCounter;
  return String(prefix) + "/M" + String(millis()) + "_" + String(id) + "." + ext;
}

bool writeWavHeader(File &file, uint32_t dataBytes = 0) {
  WavHeader header;
  header.subchunk2Size = dataBytes;
  header.chunkSize = 36 + dataBytes;

  size_t written = file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  return written == sizeof(header);
}

bool finalizeWavFile(File &file, uint32_t dataBytes) {
  if (!file.seek(0)) {
    return false;
  }
  if (!writeWavHeader(file, dataBytes)) {
    return false;
  }
  file.flush();
  return true;
}

bool initMicI2S() {
  i2s_config_t i2sConfig = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
      .sample_rate = static_cast<int>(cfg::AUDIO_SAMPLE_RATE),
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
      .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,
      .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
#endif
  };

  i2s_pin_config_t pinConfig = {
      .bck_io_num = I2S_PIN_NO_CHANGE,
      .ws_io_num = cfg::MIC_CLK_PIN,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = cfg::MIC_DATA_PIN,
  };

  if (i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr) != ESP_OK) {
    Serial.println("[AUDIO] i2s_driver_install failed");
    return false;
  }

  if (i2s_set_pin(I2S_NUM_0, &pinConfig) != ESP_OK) {
    Serial.println("[AUDIO] i2s_set_pin failed");
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  if (i2s_zero_dma_buffer(I2S_NUM_0) != ESP_OK) {
    Serial.println("[AUDIO] i2s_zero_dma_buffer failed");
  }

  return true;
}

void deinitMicI2S() {
  i2s_stop(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 15;
  config.pin_d1 = 17;
  config.pin_d2 = 18;
  config.pin_d3 = 16;
  config.pin_d4 = 14;
  config.pin_d5 = 12;
  config.pin_d6 = 11;
  config.pin_d7 = 48;
  config.pin_xclk = 10;
  config.pin_pclk = 13;
  config.pin_vsync = 38;
  config.pin_href = 47;
  config.pin_sccb_sda = 40;
  config.pin_sccb_scl = 39;
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 14;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed, err=0x%x\n", err);
    return false;
  }

  return true;
}

void deinitCamera() {
  esp_camera_deinit();
}

void audioTask(void *param) {
  (void)param;

  if (!initMicI2S()) {
    Serial.println("[AUDIO] Mic init failed, task stopping");
    vTaskDelete(nullptr);
    return;
  }

  std::unique_ptr<uint8_t[]> buffer(new uint8_t[cfg::AUDIO_BUFFER_BYTES]);
  if (!buffer) {
    Serial.println("[AUDIO] buffer allocation failed");
    deinitMicI2S();
    vTaskDelete(nullptr);
    return;
  }

  File wavFile;
  uint32_t dataBytes = 0;
  uint32_t fileStartMs = 0;
  uint32_t lastFlushMs = 0;

  auto openNewWav = [&]() -> bool {
    if (!ensureSdMounted()) {
      return false;
    }

    if (wavFile) {
      finalizeWavFile(wavFile, dataBytes);
      wavFile.close();
    }

    String path = timestampString("/audio", "wav");
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      Serial.println("[AUDIO] SD mutex timeout while opening WAV");
      return false;
    }
    wavFile = SD.open(path, FILE_WRITE);
    xSemaphoreGive(sdMutex);

    if (!wavFile) {
      Serial.printf("[AUDIO] open failed: %s\n", path.c_str());
      return false;
    }

    if (!writeWavHeader(wavFile, 0)) {
      Serial.printf("[AUDIO] WAV header write failed: %s\n", path.c_str());
      wavFile.close();
      return false;
    }

    dataBytes = 0;
    fileStartMs = millis();
    lastFlushMs = millis();
    Serial.printf("[AUDIO] recording %s\n", path.c_str());
    return true;
  };

  if (!openNewWav()) {
    Serial.println("[AUDIO] Initial WAV open failed; retrying in loop");
  }

  while (true) {
    if (!wavFile && !openNewWav()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    size_t bytesRead = 0;
    esp_err_t readErr = i2s_read(I2S_NUM_0, buffer.get(), cfg::AUDIO_BUFFER_BYTES, &bytesRead, pdMS_TO_TICKS(1000));
    if (readErr != ESP_OK) {
      Serial.printf("[AUDIO] i2s_read error: 0x%x\n", readErr);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (bytesRead > 0) {
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        size_t wrote = wavFile.write(buffer.get(), bytesRead);
        xSemaphoreGive(sdMutex);

        if (wrote != bytesRead) {
          Serial.printf("[AUDIO] short write: %u/%u\n", static_cast<unsigned>(wrote),
                        static_cast<unsigned>(bytesRead));
        } else {
          dataBytes += wrote;
        }
      } else {
        Serial.println("[AUDIO] SD mutex timeout during write");
      }
    }

    uint32_t now = millis();
    if (now - lastFlushMs >= cfg::AUDIO_FLUSH_MS) {
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        wavFile.flush();
        xSemaphoreGive(sdMutex);
      }
      lastFlushMs = now;
    }

    if (now - fileStartMs >= cfg::AUDIO_ROTATE_MS) {
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        finalizeWavFile(wavFile, dataBytes);
        wavFile.close();
        xSemaphoreGive(sdMutex);
      }
      if (!openNewWav()) {
        Serial.println("[AUDIO] rotate failed, will retry");
      }
    }
  }
}

void photoTask(void *param) {
  (void)param;

  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(cfg::PHOTO_INTERVAL_MS));

    if (!ensureSdMounted()) {
      Serial.println("[PHOTO] SD unavailable, skipping capture");
      continue;
    }

    if (!initCamera()) {
      Serial.println("[PHOTO] camera init failed");
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[PHOTO] capture failed");
      deinitCamera();
      continue;
    }

    String path = timestampString("/photo", "jpg");
    bool saved = false;

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
      File photo = SD.open(path, FILE_WRITE);
      if (photo) {
        size_t wrote = photo.write(fb->buf, fb->len);
        photo.flush();
        photo.close();
        saved = (wrote == fb->len);
      }
      xSemaphoreGive(sdMutex);
    } else {
      Serial.println("[PHOTO] SD mutex timeout");
    }

    if (saved) {
      Serial.printf("[PHOTO] saved %s (%u bytes)\n", path.c_str(), static_cast<unsigned>(fb->len));
    } else {
      Serial.printf("[PHOTO] save failed %s\n", path.c_str());
    }

    esp_camera_fb_return(fb);
    deinitCamera();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  setCpuFrequencyMhz(cfg::CPU_FREQ_MHZ);

  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
  esp_wifi_stop();
  btStop();
  esp_bt_controller_disable();

  sdMutex = xSemaphoreCreateMutex();
  if (!sdMutex) {
    Serial.println("[SYS] Failed to create SD mutex");
    while (true) {
      delay(1000);
    }
  }

  if (!ensureSdMounted()) {
    Serial.println("[SYS] SD mount failed at startup; tasks will keep retrying");
  }

  xTaskCreatePinnedToCore(audioTask, "audioTask", 8192, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(photoTask, "photoTask", 8192, nullptr, 1, nullptr, 0);

  Serial.println("[SYS] Started audio + photo logger");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
