#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "display_bsp.h"

namespace {
constexpr uint32_t kBmpHeaderSize = 54;
constexpr uint64_t kMaxImagePixels = 16ULL * 1024ULL * 1024ULL;

bool read_exact(FILE *file, void *buffer, size_t length) {
    return file != nullptr && fread(buffer, 1, length, file) == length;
}
}

ePaperPort::ePaperPort(ImgDecodeDither &dither,int mosi, int scl, int dc, int cs, int rst, int busy, uint16_t width, uint16_t height,uint16_t scale_MaxWidth, uint16_t scale_MaxHeight, spi_host_device_t spihost) : 
dither_(dither),
mosi_(mosi), 
scl_(scl), 
dc_(dc), 
cs_(cs), 
rst_(rst), 
busy_(busy), 
width_(width), 
height_(height),
scale_MaxWidth_(scale_MaxWidth),
scale_MaxHeight_(scale_MaxHeight),
spi_host_(spihost) {
    const uint64_t pixels = static_cast<uint64_t>(width_) * height_;
    if (width_ == 0 || height_ == 0 || (pixels & 1U) != 0 || pixels > kMaxImagePixels) {
        init_error_ = ESP_ERR_INVALID_ARG;
        return;
    }
    display_mutex_ = xSemaphoreCreateMutex();
    if (display_mutex_ == NULL) {
        init_error_ = ESP_ERR_NO_MEM;
        return;
    }
    DisplayLen = static_cast<int>(pixels / 2U);
    DispBuffer = (uint8_t *)heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    RotationBuffer = (uint8_t *)heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    BmpSrcBuffer = (uint8_t *)heap_caps_malloc(static_cast<size_t>(pixels * 3U), MALLOC_CAP_SPIRAM);
    if (DispBuffer == NULL || RotationBuffer == NULL || BmpSrcBuffer == NULL) {
        init_error_ = ESP_ERR_NO_MEM;
        return;
    }
    memset(DispBuffer, (ColorWhite << 4) | ColorWhite, DisplayLen);
    memset(RotationBuffer, (ColorWhite << 4) | ColorWhite, DisplayLen);

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = -1;
    buscfg.mosi_io_num = mosi_;
    buscfg.sclk_io_num = scl_;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = DisplayLen;
    spi_device_interface_config_t devcfg = {};
    devcfg.spics_io_num = -1;
    devcfg.clock_speed_hz = 40 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.queue_size = 1;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;
    esp_err_t ret = spi_bus_initialize(spi_host_, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        init_error_ = ret;
        return;
    }
    spi_bus_initialized_ = true;
    ret = spi_bus_add_device(spi_host_, &devcfg, &spi);
    if (ret != ESP_OK) {
        init_error_ = ret;
        return;
    }
    spi_device_added_ = true;

    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << rst_) | (0x1ULL << dc_) | (0x1ULL << cs_);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ret = gpio_config(&gpio_conf);
    if (ret != ESP_OK) {
        init_error_ = ret;
        return;
    }
    gpio_conf.mode = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << busy_);
    ret = gpio_config(&gpio_conf);
    if (ret != ESP_OK) {
        init_error_ = ret;
        return;
    }
    Set_ResetIOLevel(1);
}

ePaperPort::~ePaperPort() {
    if (spi_device_added_) {
        spi_bus_remove_device(spi);
    }
    if (spi_bus_initialized_) {
        spi_bus_free(spi_host_);
    }
    if (DispBuffer != NULL) heap_caps_free(DispBuffer);
    if (RotationBuffer != NULL) heap_caps_free(RotationBuffer);
    if (BmpSrcBuffer != NULL) heap_caps_free(BmpSrcBuffer);
    if (display_mutex_ != NULL) vSemaphoreDelete(display_mutex_);
}

bool ePaperPort::LockDisplay() {
    return display_mutex_ != NULL && xSemaphoreTake(display_mutex_, kMutexWaitTicks) == pdTRUE;
}

void ePaperPort::UnlockDisplay() {
    if (display_mutex_ != NULL) xSemaphoreGive(display_mutex_);
}

void ePaperPort::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}

void ePaperPort::Set_CSIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) cs_, level ? 1 : 0);
}

void ePaperPort::Set_DCIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) dc_, level ? 1 : 0);
}

uint8_t ePaperPort::Get_BusyIOLevel() {
    return gpio_get_level((gpio_num_t) busy_);
}

void ePaperPort::EPD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t ePaperPort::EPD_LoopBusy(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (true) {
        if (Get_BusyIOLevel()) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t ePaperPort::SPI_Write(uint8_t data) {
    if (spi == NULL) return ESP_ERR_INVALID_STATE;
    spi_transaction_t t = {};
    memset(&t, 0, sizeof(t));
    t.length    = 8;
    t.tx_buffer = &data;
    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    return ret;
}

esp_err_t ePaperPort::EPD_SendCommand(uint8_t Reg) {
    Set_DCIOLevel(0);
    Set_CSIOLevel(0);
    esp_err_t ret = SPI_Write(Reg);
    Set_CSIOLevel(1);
    return ret;
}

esp_err_t ePaperPort::EPD_SendData(uint8_t Data) {
    Set_DCIOLevel(1);
    Set_CSIOLevel(0);
    esp_err_t ret = SPI_Write(Data);
    Set_CSIOLevel(1);
    return ret;
}

esp_err_t ePaperPort::EPD_Sendbuffera(uint8_t *Data, int len) {
    if (Data == NULL || len <= 0 || spi == NULL) return ESP_ERR_INVALID_ARG;
    Set_DCIOLevel(1);
    Set_CSIOLevel(0);
    esp_err_t ret = ESP_OK;
    spi_transaction_t t = {};
    memset(&t, 0, sizeof(t));
    int      len_scl = len / 5000;
    int      len_dcl = len % 5000;
    uint8_t *ptr     = Data;
    while (len_scl) {
        t.length    = 8 * 5000;
        t.tx_buffer = ptr;
        ret = spi_device_polling_transmit(spi, &t);
        if (ret != ESP_OK) break;
        len_scl--;
        ptr += 5000;
    }
    if (ret == ESP_OK && len_dcl > 0) {
        t.length = 8 * len_dcl;
        t.tx_buffer = ptr;
        ret = spi_device_polling_transmit(spi, &t);
    }
    Set_CSIOLevel(1);
    return ret;
}

esp_err_t ePaperPort::EPD_TurnOnDisplay(void) {
    /* Mark powered before issuing POWER_ON: a transport error can happen
     * after the controller accepted the command, so cleanup must still try
     * POWER_OFF. */
    panel_powered_ = true;
    esp_err_t ret = EPD_SendCommand(0x04); // POWER_ON
    if (ret != ESP_OK) goto failed;
    ret = EPD_LoopBusy(kPowerOnTimeoutMs);
    if (ret != ESP_OK) goto failed;

    // Second setting
    if ((ret = EPD_SendCommand(0x06)) != ESP_OK ||
        (ret = EPD_SendData(0x6F)) != ESP_OK ||
        (ret = EPD_SendData(0x1F)) != ESP_OK ||
        (ret = EPD_SendData(0x17)) != ESP_OK ||
        (ret = EPD_SendData(0x49)) != ESP_OK) goto failed;

    if ((ret = EPD_SendCommand(0x12)) != ESP_OK || // DISPLAY_REFRESH
        (ret = EPD_SendData(0x00)) != ESP_OK ||
        (ret = EPD_LoopBusy(kRefreshTimeoutMs)) != ESP_OK) goto failed;

    return EPD_PowerOffLocked();

failed: {
        const esp_err_t off_ret = EPD_PowerOffLocked();
        if (ret == ESP_OK) ret = off_ret;
        return ret;
    }
}

esp_err_t ePaperPort::EPD_PowerOffLocked(void) {
    esp_err_t ret = EPD_SendCommand(0x02); // POWER_OFF
    if (ret == ESP_OK) ret = EPD_SendData(0X00);
    if (ret == ESP_OK) ret = EPD_LoopBusy(kPowerOffTimeoutMs);
    if (ret == ESP_OK) panel_powered_ = false;
    return ret;
}

void ePaperPort::Set_Rotation(uint8_t rot) {
    Rotation = rot;
}

void ePaperPort::Set_Mirror(uint8_t mirr_x,uint8_t mirr_y) {
    mirrx = mirr_x;
    mirry = mirr_y;
}

esp_err_t ePaperPort::EPD_InitLocked() {
    if (isEPDInit && !needs_reinit_) return ESP_OK;
    if (init_error_ != ESP_OK || spi == NULL || DispBuffer == NULL || RotationBuffer == NULL) {
        return init_error_ == ESP_OK ? ESP_ERR_INVALID_STATE : init_error_;
    }

    /* A previous transaction may have failed while the controller's power
     * state was uncertain.  Establish the known-off state before resetting
     * and sending the configuration sequence. */
    if (panel_powered_) {
        esp_err_t ret = EPD_PowerOffLocked();
        if (ret != ESP_OK) {
            needs_reinit_ = true;
            return ret;
        }
    }

    EPD_Reset();
    esp_err_t ret = EPD_LoopBusy(kInitTimeoutMs);
    if (ret != ESP_OK) goto failed;
    vTaskDelay(pdMS_TO_TICKS(50));

#define DISPLAY_SEND(cmd, data) do { \
    ret = EPD_SendCommand((cmd)); if (ret != ESP_OK) goto failed; \
    ret = EPD_SendData((data)); if (ret != ESP_OK) goto failed; \
} while (0)
    ret = EPD_SendCommand(0xAA); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x49); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x55); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x20); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x08); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x09); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x18); if (ret != ESP_OK) goto failed;
    DISPLAY_SEND(0x01, 0x3F);
    ret = EPD_SendCommand(0x00); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x5F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x69); if (ret != ESP_OK) goto failed;
    ret = EPD_SendCommand(0x03); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x00); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x54); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x00); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x44); if (ret != ESP_OK) goto failed;
    ret = EPD_SendCommand(0x05); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x40); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x1F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x1F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x2C); if (ret != ESP_OK) goto failed;
    ret = EPD_SendCommand(0x06); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x6F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x1F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x17); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x49); if (ret != ESP_OK) goto failed;
    ret = EPD_SendCommand(0x08); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x6F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x1F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x1F); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x22); if (ret != ESP_OK) goto failed;
    DISPLAY_SEND(0x30, 0x03);
    DISPLAY_SEND(0x50, 0x3F);
    ret = EPD_SendCommand(0x60); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x02); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x00); if (ret != ESP_OK) goto failed;
    ret = EPD_SendCommand(0x61); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x03); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x20); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0x01); if (ret != ESP_OK) goto failed;
    ret = EPD_SendData(0xE0); if (ret != ESP_OK) goto failed;
    DISPLAY_SEND(0x84, 0x01);
    DISPLAY_SEND(0xE3, 0x2F);
#undef DISPLAY_SEND

    /* Keep the controller powered off after configuration.  The first
     * POWER_ON belongs to the refresh transaction, after its image buffer is
     * complete.  Do not clear DispBuffer here: lazy initialization must never
     * discard a prepared image. */
    panel_powered_ = false;
    isEPDInit = true;
    needs_reinit_ = false;
    return ESP_OK;

failed:
    isEPDInit = false;
    needs_reinit_ = true;
    if (panel_powered_) {
        const esp_err_t off_ret = EPD_PowerOffLocked();
        if (ret == ESP_OK) ret = off_ret;
    }
    return ret;
}

esp_err_t ePaperPort::EnsureInitializedLocked() {
    return EPD_InitLocked();
}

esp_err_t ePaperPort::EPD_Init() {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = EPD_InitLocked();
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_DispClearLocked(uint8_t color) {
    if (DispBuffer == NULL) return ESP_ERR_INVALID_STATE;
    color &= 0x0F;
    memset(DispBuffer, (color << 4) | color, DisplayLen);
    return ESP_OK;
}

esp_err_t ePaperPort::EPD_DispClear(uint8_t color) {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = EPD_DispClearLocked(color);
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_DisplayLocked() {
    esp_err_t ret = EnsureInitializedLocked();
    if (ret != ESP_OK) return ret;
    EPD_PixelRotate();
    ret = EPD_SendCommand(0x10);
    if (ret == ESP_OK) ret = EPD_Sendbuffera(RotationBuffer, DisplayLen);
    if (ret == ESP_OK) ret = EPD_TurnOnDisplay();
    if (ret != ESP_OK) {
        isEPDInit = false;
        needs_reinit_ = true;
        if (panel_powered_) {
            const esp_err_t off_ret = EPD_PowerOffLocked();
            if (ret == ESP_OK) ret = off_ret;
        }
    }
    return ret;
}

esp_err_t ePaperPort::EPD_Display() {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = EPD_DisplayLocked();
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_ShowClear(uint8_t color) {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    esp_err_t ret = EPD_DispClearLocked(color);
    if (ret == ESP_OK) ret = EPD_DisplayLocked();
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_ShowBmp(const char *path, uint16_t x_start, uint16_t y_start) {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    esp_err_t ret = EPD_SDcardBmpShakingColorLocked(path, x_start, y_start);
    if (ret == ESP_OK) ret = EPD_DisplayLocked();
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_SrcDisplayCopy(uint8_t *buffer,uint32_t len,uint32_t addlen) {
    if (buffer == NULL || addlen > static_cast<uint32_t>(DisplayLen) ||
        len > static_cast<uint32_t>(DisplayLen) - addlen) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    memcpy(RotationBuffer + addlen, buffer, len);
    UnlockDisplay();
    return ESP_OK;
}

uint8_t* ePaperPort::EPD_GetIMGBuffer() {
    return DispBuffer;
}

void ePaperPort::SetPixel4Locked(uint8_t *buf, int width, int x, int y, uint8_t px) {
    const int logical_height = (width == 480 && width_ == 800 && height_ == 480) ? 800 : height_;
    if (buf == NULL || width <= 0 || (width & 1) != 0 || x < 0 || y < 0 ||
        x >= width || y >= logical_height) {
        return;
    }
    const int index = y * (width >> 1) + (x >> 1);
    const uint8_t old = buf[index];
    if (x & 1) {
        buf[index] = (old & 0xF0) | (px & 0x0F);
    } else {
        buf[index] = (old & 0x0F) | static_cast<uint8_t>((px & 0x0F) << 4);
    }
}

esp_err_t ePaperPort::EPD_SetPixel(uint16_t x, uint16_t y, uint16_t color) {
    if(x >= width_ || y >= height_ || DispBuffer == NULL) {
        ESP_LOGE("Pixel","Beyond the limit: (%d,%d)",x,y);
        return ESP_ERR_INVALID_ARG;
    }
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    SetPixel4Locked(DispBuffer, width_, x, y, static_cast<uint8_t>(color));
    UnlockDisplay();
    return ESP_OK;
}

uint8_t* ePaperPort::EPD_ParseBMPImage(const char *path) {
    if (path == NULL || BmpSrcBuffer == NULL) {
        return NULL;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == NULL || fseek(fp, 0, SEEK_END) != 0) {
        if (fp != NULL) fclose(fp);
        return NULL;
    }
    const long end = ftell(fp);
    if (end < static_cast<long>(kBmpHeaderSize) || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    const uint64_t file_size = static_cast<uint64_t>(end);
    BMPFILEHEADER bmpFileHeader = {};
    BMPINFOHEADER bmpInfoHeader = {};
    if (!read_exact(fp, &bmpFileHeader, sizeof(bmpFileHeader)) ||
        !read_exact(fp, &bmpInfoHeader, sizeof(bmpInfoHeader)) ||
        bmpFileHeader.bType != 0x4D42 || bmpFileHeader.bReserved1 != 0 ||
        bmpFileHeader.bReserved2 != 0 || bmpFileHeader.bOffset != kBmpHeaderSize ||
        bmpFileHeader.bSize > file_size || bmpInfoHeader.biInfoSize != 40 ||
        bmpInfoHeader.biWidth <= 0 || bmpInfoHeader.biHeight <= 0 ||
        bmpInfoHeader.biPlanes != 1 || bmpInfoHeader.biBitCount != 24 ||
        bmpInfoHeader.biCompression != 0 ||
        (bmpInfoHeader.biWidth != 480 && bmpInfoHeader.biWidth != 800) ||
        (bmpInfoHeader.biHeight != 480 && bmpInfoHeader.biHeight != 800) ||
        static_cast<uint64_t>(bmpInfoHeader.biWidth) * bmpInfoHeader.biHeight !=
            static_cast<uint64_t>(width_) * height_) {
        fclose(fp);
        return NULL;
    }
    const uint32_t image_width = static_cast<uint32_t>(bmpInfoHeader.biWidth);
    const uint32_t image_height = static_cast<uint32_t>(bmpInfoHeader.biHeight);
    const uint32_t row_bytes = (image_width * 3U + 3U) & ~3U;
    const uint64_t image_bytes = static_cast<uint64_t>(row_bytes) * image_height;
    if (image_bytes > file_size - kBmpHeaderSize ||
        bmpFileHeader.bSize < static_cast<uint64_t>(kBmpHeaderSize) + image_bytes ||
        (bmpInfoHeader.bimpImageSize != 0 && bmpInfoHeader.bimpImageSize < image_bytes) ||
        fseek(fp, kBmpHeaderSize, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t row[2400];
    for (uint32_t file_y = 0; file_y < image_height; ++file_y) {
        if (!read_exact(fp, row, row_bytes)) {
            fclose(fp);
            return NULL;
        }
        /* Positive BMP heights are bottom-up; expose rows top-down. */
        const uint32_t output_y = image_height - 1U - file_y;
        memcpy(BmpSrcBuffer + static_cast<size_t>(output_y) * image_width * 3U,
               row, image_width * 3U);
    }
    fclose(fp);
    src_width = static_cast<uint16_t>(image_width);
    src_height = static_cast<uint16_t>(image_height);
    Rotation = (src_width == 480) ? 3 : 2;
    return BmpSrcBuffer;
}

uint8_t ePaperPort::EPD_ColorToePaperColor(uint8_t b,uint8_t g,uint8_t r) {
    if(b == 0xff && g == 0xff && r == 0xff) {
        return ColorWhite;
    }
    if(b == 0x0 && g == 0x0 && r == 0x0) {
        return ColorBlack;
    }
    if(b == 0x0 && g == 0x0 && r == 0xff) {
        return ColorRed;
    }
    if(b == 0xff && g == 0x0 && r == 0x0) {
        return ColorBlue;
    }
    if(b == 0x0 && g == 0xff && r == 0x0) {
        return ColorGreen;
    }
    if(b == 0x0 && g == 0xff && r == 0xff) {
        return ColorYellow;
    }
    return ColorWhite;
}

esp_err_t ePaperPort::EPD_SDcardBmpShakingColorLocked(const char *path,uint16_t x_start, uint16_t y_start) {
    uint8_t r,g,b;
    uint8_t *buffer = EPD_ParseBMPImage(path);
    if(NULL == buffer) {
        return ESP_FAIL;
    }
    const uint32_t logical_width = src_width;
    const uint32_t logical_height = src_height;
    if (x_start >= logical_width || y_start >= logical_height) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t* scapeBuffer = (uint8_t*)buffer;
    for(int y = 0; y < src_height; y++) {
        for(int x = 0; x < src_width; x++) {
            int idx = (y * src_width + x) * 3;
            b = scapeBuffer[idx + 0];
            g = scapeBuffer[idx + 1];
            r = scapeBuffer[idx + 2];
            uint8_t color = EPD_ColorToePaperColor(b, g, r);
            if (x < logical_width - x_start && y < logical_height - y_start) {
                SetPixel4Locked(DispBuffer, src_width, x_start + x, y_start + y, color);
            }
        }
    }
    return ESP_OK;
}

esp_err_t ePaperPort::EPD_SDcardBmpShakingColor(const char *path,uint16_t x_start, uint16_t y_start) {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = EPD_SDcardBmpShakingColorLocked(path, x_start, y_start);
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_DecodeAndPrepareImageLocked(const char *path, uint16_t x_start,
                                                              uint16_t y_start, bool allow_scale) {
    if (path == NULL || DispBuffer == NULL) return ESP_ERR_INVALID_ARG;

    enum ImageKind { IMAGE_JPEG, IMAGE_PNG, IMAGE_BMP };
    const char *dot = strrchr(path, '.');
    ImageKind kind;
    if (dot == NULL) return ESP_ERR_NOT_SUPPORTED;
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        kind = IMAGE_JPEG;
    } else if (strcasecmp(dot, ".png") == 0) {
        kind = IMAGE_PNG;
    } else if (strcasecmp(dot, ".bmp") == 0) {
        kind = IMAGE_BMP;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *decoded = NULL;
    uint8_t *scaled = NULL;
    uint8_t *dithered = NULL;
    int decoded_len = 0;
    int image_width = 0;
    int image_height = 0;
    int target_width = 0;
    int target_height = 0;
    bool exact_size = false;
    uint64_t scaled_size = 0;
    uint64_t output_size = 0;
    esp_err_t ret = ESP_FAIL;

    if (kind == IMAGE_JPEG) {
        ret = dither_.ImgDecode_TFOneJPGPicture(path, &decoded, &decoded_len,
                                                &image_width, &image_height);
    } else if (kind == IMAGE_PNG) {
        ret = dither_.ImgDecode_TFOnePNGPicture(path, &decoded, &image_width, &image_height);
    } else {
        ret = dither_.ImgDecodebmp_TFOneBMPPicture(path, &decoded, &image_width, &image_height);
    }
    if (ret != ESP_OK || decoded == NULL || image_width <= 0 || image_height <= 0) {
        goto cleanup;
    }
    if (image_width > static_cast<int>(scale_MaxWidth_) ||
        image_height > static_cast<int>(scale_MaxHeight_)) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    target_width = image_width;
    target_height = image_height;
    exact_size = (image_width == static_cast<int>(width_) &&
                  image_height == static_cast<int>(height_)) ||
                 (image_width == static_cast<int>(height_) &&
                  image_height == static_cast<int>(width_));
    if (!allow_scale && !exact_size) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (allow_scale && !exact_size) {
        if (image_width > image_height) {
            target_width = width_;
            target_height = height_;
        } else {
            target_width = height_;
            target_height = width_;
        }
        scaled_size = static_cast<uint64_t>(target_width) * target_height * 3U;
        if (scaled_size > SIZE_MAX) {
            ret = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        scaled = (uint8_t *)malloc(static_cast<size_t>(scaled_size));
        if (scaled == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        ret = dither_.ImgDecode_ScaleRgb888Nearest(decoded, image_width, image_height, scaled,
                                                   target_width, target_height);
        if (ret != ESP_OK) goto cleanup;
    }

    output_size = static_cast<uint64_t>(target_width) * target_height * 3U;
    if (output_size > SIZE_MAX) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    dithered = (uint8_t *)malloc(static_cast<size_t>(output_size));
    if (dithered == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ret = dither_.ImgDecode_DitherRgb888(scaled != NULL ? scaled : decoded, dithered,
                                         target_width, target_height);
    if (ret == ESP_OK) {
        ret = dither_.ImgDecode_EncodingBmpToSdcard(img_to_bmpName, dithered,
                                                     target_width, target_height);
    }
    if (ret == ESP_OK) {
        ret = EPD_SDcardBmpShakingColorLocked(img_to_bmpName, x_start, y_start);
    }

cleanup:
    if (kind == IMAGE_JPEG) {
        dither_.ImgDecode_JPGBufferFree(decoded);
    } else if (kind == IMAGE_PNG) {
        dither_.ImgDecode_PNGBufferFree(decoded);
    } else {
        dither_.ImgDecode_BMPBufferFree(decoded);
    }
    free(scaled);
    free(dithered);
    return ret;
}

esp_err_t ePaperPort::EPD_SDcardIMGShakingColor(const char *path,uint16_t x_start, uint16_t y_start) {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = EPD_DecodeAndPrepareImageLocked(path, x_start, y_start, false);
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_SDcardScaleIMGShakingColor(const char *path,uint16_t x_start, uint16_t y_start) {
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = EPD_DecodeAndPrepareImageLocked(path, x_start, y_start, true);
    UnlockDisplay();
    return ret;
}

esp_err_t ePaperPort::EPD_DrawStringCN(uint16_t Xstart, uint16_t Ystart, const char * pString, cFONT* font,uint16_t Color_Foreground, uint16_t Color_Background) {
    if (pString == NULL || font == NULL || DispBuffer == NULL) return ESP_ERR_INVALID_ARG;
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    const char* p_text = pString;
    int x = Xstart, y = Ystart;
    int i, j,Num;
    uint8_t FONT_BACKGROUND = 0xff;
    /* Send the string character by character on EPD */
    while (*p_text != 0) {
        const uint8_t first = static_cast<uint8_t>(*p_text);
        if (first < 0x80) {
            for(Num = 0; Num < font->size; Num++) {
                if(*p_text== font->table[Num].index[0]) {
                    const char* ptr = &font->table[Num].matrix[0];
                    for (j = 0; j < font->Height; j++) {
                        for (i = 0; i < font->Width; i++) {
                            if (FONT_BACKGROUND == Color_Background) { //this process is to speed up the scan
                                if (*ptr & (0x80 >> (i % 8))) {
                                    SetPixel4Locked(DispBuffer, width_, x + i, y + j, Color_Foreground);
                                }
                            } else {
                                if (*ptr & (0x80 >> (i % 8))) {
                                    SetPixel4Locked(DispBuffer, width_, x + i, y + j, Color_Foreground);
                                } else {
                                    SetPixel4Locked(DispBuffer, width_, x + i, y + j, Color_Background);
                                }
                            }
                            if (i % 8 == 7) {
                                ptr++;
                            }
                        }
                        if (font->Width % 8 != 0) {
                            ptr++;
                        }
                    }
                    break;
                }
            }
            /* Point on the next character */
            p_text += 1;
            /* Decrement the column position by 16 */
            x += font->ASCII_Width;
        } else if (first >= 0xE0 && first <= 0xEF && p_text[1] != '\0' &&
                   p_text[2] != '\0' &&
                   static_cast<uint8_t>(p_text[1]) >= 0x80 &&
                   static_cast<uint8_t>(p_text[1]) <= 0xBF &&
                   static_cast<uint8_t>(p_text[2]) >= 0x80 &&
                   static_cast<uint8_t>(p_text[2]) <= 0xBF) {        //Chinese
            for(Num = 0; Num < font->size; Num++) {
                if((*p_text== font->table[Num].index[0]) && (*(p_text+1) == font->table[Num].index[1])  && (*(p_text+2) == font->table[Num].index[2])) {
                    const char* ptr = &font->table[Num].matrix[0];

                    for (j = 0; j < font->Height; j++) {
                        for (i = 0; i < font->Width; i++) {
                            if (FONT_BACKGROUND == Color_Background) { //this process is to speed up the scan
                                if (*ptr & (0x80 >> (i % 8))) {
                                    SetPixel4Locked(DispBuffer, width_, x + i, y + j, Color_Foreground);
                                }
                            } else {
                                if (*ptr & (0x80 >> (i % 8))) {
                                    SetPixel4Locked(DispBuffer, width_, x + i, y + j, Color_Foreground);
                                } else {
                                    SetPixel4Locked(DispBuffer, width_, x + i, y + j, Color_Background);
                                }
                            }
                            if (i % 8 == 7) {
                                ptr++;
                            }
                        }
                        if (font->Width % 8 != 0) {
                            ptr++;
                        }
                    }
                    break;
                }
            }
            /* Point on the next character */
            p_text += 3;
            /* Decrement the column position by 16 */
            x += font->Width;
        } else {
            /* Skip malformed or unsupported UTF-8 without reading past the
             * string terminator. */
            if ((first & 0xE0) == 0xC0 && p_text[1] != '\0' &&
                (static_cast<uint8_t>(p_text[1]) & 0xC0) == 0x80) {
                p_text += 2;
            } else if ((first & 0xF0) == 0xF0 && p_text[1] != '\0' &&
                       p_text[2] != '\0' && p_text[3] != '\0' &&
                       (static_cast<uint8_t>(p_text[1]) & 0xC0) == 0x80 &&
                       (static_cast<uint8_t>(p_text[2]) & 0xC0) == 0x80 &&
                       (static_cast<uint8_t>(p_text[3]) & 0xC0) == 0x80) {
                p_text += 4;
            } else {
                ++p_text;
            }
            x += font->Width;
        }
    }
    UnlockDisplay();
    return ESP_OK;
}

uint8_t ePaperPort::EPD_GetPixel4(const uint8_t* buf, int width, int x, int y) {
    const int buffer_height = (width == 480 && width_ == 800 && height_ == 480) ? 800 : height_;
    if (buf == NULL || width <= 0 || (width & 1) != 0 || x < 0 || y < 0 ||
        x >= width || y >= buffer_height) return 0;
    int index = y * (width >> 1) + (x >> 1);
    uint8_t byte = buf[index];
    return (x & 1) ? (byte & 0x0F) : (byte >> 4);
}

void ePaperPort::EPD_SetPixel4(uint8_t* buf, int width, int x, int y, uint8_t px) {
    const int buffer_height = (width == 480 && width_ == 800 && height_ == 480) ? 800 : height_;
    if (buf == NULL || width <= 0 || (width & 1) != 0 || x < 0 || y < 0 ||
        x >= width || y >= buffer_height) return;
    int index = y * (width >> 1) + (x >> 1);
    uint8_t old = buf[index];
    if (x & 1)
        buf[index] = (old & 0xF0) | (px & 0x0F);
    else
        buf[index] = (old & 0x0F) | (px << 4);
}

void ePaperPort::EPD_PixelRotate() {
    if(Rotation == 3) {
        EPD_Rotate90CCW_Fast(DispBuffer,RotationBuffer,480,800);
    } else if(Rotation == 1) {
        EPD_Rotate90CW_Fast(DispBuffer,RotationBuffer,480,800);
    } else if(Rotation == 2) {
        EPD_Rotate180_Fast(DispBuffer,RotationBuffer,800,480);
    } else {
        memcpy(RotationBuffer, DispBuffer, DisplayLen);
    }
}

void ePaperPort::EPD_Rotate180_Fast(const uint8_t* src, uint8_t* dst, int width, int height)
{
    const int bytesPerRow = width >> 1;
    const int totalRows   = height;    
    for (int y = 0; y < totalRows; y++) {
        const uint8_t* srcRow = src + y * bytesPerRow;
        uint8_t* dstRow = dst + (totalRows - 1 - y) * bytesPerRow;
        for (int x = 0; x < bytesPerRow; x++) {
            uint8_t b = srcRow[x];
            b = (b << 4) | (b >> 4);
            dstRow[bytesPerRow - 1 - x] = b;
        }
    }
}

void ePaperPort::EPD_Rotate90CCW_Fast(const uint8_t* src, uint8_t* dst, int width, int height)
{
    const int srcBytesPerRow = width >> 1;
    for (int y = 0; y < height; y++) {
        const uint8_t* srcRow = src + y * srcBytesPerRow;
        for (int x = 0; x < width; x += 2) {

            uint8_t b = srcRow[x >> 1];
            uint8_t p0 = b >> 4;
            uint8_t p1 = b & 0x0F;
            int ny0 = width - 1 - x;
            int nx0 = y;
            int ny1 = width - 2 - x;
            int nx1 = y;
            EPD_SetPixel4(dst, height, nx0, ny0, p0);
            EPD_SetPixel4(dst, height, nx1, ny1, p1);
        }
    }
}

void ePaperPort::EPD_Rotate90CW_Fast(const uint8_t* src, uint8_t* dst, int width, int height)
{
    const int srcBytesPerRow = width >> 1;
    for (int y = 0; y < height; y++) {
        const uint8_t* srcRow = src + y * srcBytesPerRow;
        for (int x = 0; x < width; x += 2) {

            uint8_t b = srcRow[x >> 1];
            uint8_t p0 = b >> 4;  
            uint8_t p1 = b & 0x0F;
            int ny0 = x;
            int nx0 = height - 1 - y;
            int ny1 = x + 1;
            int nx1 = height - 1 - y;
            EPD_SetPixel4(dst, height, nx0, ny0, p0);
            EPD_SetPixel4(dst, height, nx1, ny1, p1);
        }
    }
}

void ePaperPort::EPD_DrawChar(uint16_t Xpoint, uint16_t Ypoint, const char Acsii_Char,sFONT* Font, uint16_t Color_Foreground, uint16_t Color_Background) {
    uint16_t Page, Column;

    if (Xpoint > width_ || Ypoint > height_) {
        ESP_LOGE(TAG,"Paint_DrawChar Input exceeds the normal display range");
        return;
    }

    uint32_t Char_Offset = (Acsii_Char - ' ') * Font->Height * (Font->Width / 8 + (Font->Width % 8 ? 1 : 0));
    const unsigned char *ptr = &Font->table[Char_Offset];

    for (Page = 0; Page < Font->Height; Page ++ ) {
        for (Column = 0; Column < Font->Width; Column ++ ) {

            //To determine whether the font background color and screen background color is consistent
            if (0XFF == Color_Background) { //this process is to speed up the scan
                if (*ptr & (0x80 >> (Column % 8)))
                    SetPixel4Locked(DispBuffer, width_, Xpoint + Column, Ypoint + Page, Color_Foreground);
                    // Paint_DrawPoint(Xpoint + Column, Ypoint + Page, Color_Foreground, DOT_PIXEL_DFT, DOT_STYLE_DFT);
            } else {
                if (*ptr & (0x80 >> (Column % 8))) {
                    SetPixel4Locked(DispBuffer, width_, Xpoint + Column, Ypoint + Page, Color_Foreground);
                    // Paint_DrawPoint(Xpoint + Column, Ypoint + Page, Color_Foreground, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                } else {
                    SetPixel4Locked(DispBuffer, width_, Xpoint + Column, Ypoint + Page, Color_Background);
                    // Paint_DrawPoint(Xpoint + Column, Ypoint + Page, Color_Background, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                }
            }
            //One pixel is 8 bits
            if (Column % 8 == 7)
                ptr++;
        }// Write a line
        if (Font->Width % 8 != 0)
            ptr++;
    }// Write all
}

esp_err_t ePaperPort::EPD_DrawStringEN(uint16_t Xstart, uint16_t Ystart, const char * pString,sFONT* Font, uint16_t Color_Foreground, uint16_t Color_Background) {
    if (pString == NULL || Font == NULL || DispBuffer == NULL) return ESP_ERR_INVALID_ARG;
    if (!LockDisplay()) return ESP_ERR_TIMEOUT;
    uint16_t Xpoint = Xstart;
    uint16_t Ypoint = Ystart;

    if (Xstart > width_ || Ystart > height_) {
        ESP_LOGE(TAG,"Paint_DrawString_EN Input exceeds the normal display range");
        UnlockDisplay();
        return ESP_ERR_INVALID_ARG;
    }

    while (* pString != '\0') {
        //if X direction filled , reposition to(Xstart,Ypoint),Ypoint is Y direction plus the Height of the character
        if ((Xpoint + Font->Width ) > width_ ) {
            Xpoint = Xstart;
            Ypoint += Font->Height;
        }

        // If the Y direction is full, reposition to(Xstart, Ystart)
        if ((Ypoint  + Font->Height ) > height_ ) {
            Xpoint = Xstart;
            Ypoint = Ystart;
        }
        EPD_DrawChar(Xpoint, Ypoint, * pString, Font, Color_Foreground, Color_Background);

        //The next character of the address
        pString ++;

        //The next word of the abscissa increases the font of the broadband
        Xpoint += Font->Width;
    }
    UnlockDisplay();
    return ESP_OK;
}
