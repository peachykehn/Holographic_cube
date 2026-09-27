#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <SD.h>
#include <SPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TFT_DC   2
#define TFT_CS   15
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_RST  -1
#define TFT_BL   21

#define SD_CS    5
#define SD_SCK   18
#define SD_MOSI  23
#define SD_MISO  19

#define VIDEO_FILE   "/video.bin"
#define TARGET_FPS   12
#define MAX_JPEG_SIZE (24 * 1024)

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, HSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, TFT_RST, 1);

SPIClass sdSPI(VSPI);
JPEGDEC jpeg;

static uint8_t jpegBuf[2][MAX_JPEG_SIZE];
static size_t jpegSize[2];
static SemaphoreHandle_t freeSem[2];
static SemaphoreHandle_t readySem[2];
static const uint32_t frameIntervalMs = 1000 / TARGET_FPS;

int jpegDrawCallback(JPEGDRAW *pDraw) {
  gfx->draw16bitRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  return 1;
}

static size_t readFully(File &f, uint8_t *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    size_t n = f.read(buf + total, len - total);
    if (n == 0) break; // EOF or error
    total += n;
  }
  return total;
}


void readerTask(void *pv) {
  int idx = 0;
  File video = SD.open(VIDEO_FILE, FILE_READ);

  for (;;) {
    xSemaphoreTake(freeSem[idx], portMAX_DELAY);

    uint32_t len = 0;
    size_t n = readFully(video, (uint8_t *)&len, sizeof(len));
    if (n != sizeof(len) || len == 0 || len > MAX_JPEG_SIZE) {
      video.seek(0);
      n = readFully(video, (uint8_t *)&len, sizeof(len));
    }

    if (n == sizeof(len) && len > 0 && len <= MAX_JPEG_SIZE) {
      size_t got = readFully(video, jpegBuf[idx], len);
      jpegSize[idx] = (got == len) ? len : 0; 
    } else {
      jpegSize[idx] = 0; 
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    xSemaphoreGive(readySem[idx]);
    idx ^= 1;
  }
}
//black and white so contrast is better!
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  bool gfxOk = gfx->begin(40000000);
  Serial.printf("gfx->begin() returned %d\n", gfxOk);
  gfx->fillScreen(0x0000); // black

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 20000000)) { 
    Serial.println("SD card mount failed");
    gfx->setCursor(10, 10);
    gfx->setTextColor(0xFFFF); // white
    gfx->println("SD card mount failed");
    while (true) delay(1000);
  }

  if (!SD.exists(VIDEO_FILE)) {
    Serial.println("Video file not found: " VIDEO_FILE);
    gfx->setCursor(10, 10);
    gfx->setTextColor(0xFFFF); // white
    gfx->println("Video file not found on SD card");
    while (true) delay(1000);
  }

  for (int i = 0; i < 2; i++) {
    freeSem[i] = xSemaphoreCreateBinary();
    readySem[i] = xSemaphoreCreateBinary();
    xSemaphoreGive(freeSem[i]); // both slots start out free
  }
  xTaskCreatePinnedToCore(readerTask, "reader", 8192, NULL, 1, NULL, 0);
}

void loop() {
  static int drawIdx = 0;
  uint32_t frameStart = millis();

  xSemaphoreTake(readySem[drawIdx], portMAX_DELAY);
  size_t size = jpegSize[drawIdx];
  if (size > 0) {
    if (jpeg.openRAM(jpegBuf[drawIdx], size, jpegDrawCallback)) {
      jpeg.decode(0, 0, 0);
      jpeg.close();
    }
  }
  xSemaphoreGive(freeSem[drawIdx]);
  drawIdx ^= 1;

  int32_t elapsed = millis() - frameStart;
  int32_t wait = (int32_t)frameIntervalMs - elapsed;
  if (wait > 0) delay(wait);
}
