#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <freertos/semphr.h>
#include <esp_err.h>
#include "fonts.h"
#include "imgdecode_app.h"

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite,
    ColorYellow,
    ColorRed,
    ColorBlue = 5,
    ColorGreen
};

/*Bitmap file header   14bit*/
typedef struct BMP_FILE_HEADER {
    uint16_t bType;                         //File identifier
    uint32_t bSize;                         //The size of the file
    uint16_t bReserved1;                    //Reserved value, must be set to 0
    uint16_t bReserved2;                    //Reserved value, must be set to 0
    uint32_t bOffset;                       //The offset from the beginning of the file header to the beginning of the image data bit
} __attribute__((packed)) BMPFILEHEADER;  // 14bit


/*Bitmap information header  40bit*/
typedef struct BMP_INFO {
    uint32_t biInfoSize;       //The size of the header
    int32_t  biWidth;          //The width of the image
    int32_t  biHeight;         //The height of the image
    uint16_t biPlanes;         //The number of planes in the image
    uint16_t biBitCount;       //The number of bits per pixel
    uint32_t biCompression;    //Compression type
    uint32_t bimpImageSize;    //The size of the image, in bytes
    uint32_t biXPelsPerMeter;  //Horizontal resolution
    uint32_t biYPelsPerMeter;  //Vertical resolution
    uint32_t biClrUsed;        //The number of colors used
    uint32_t biClrImportant;   //The number of important colors
} __attribute__((packed)) BMPINFOHEADER;

class ePaperPort {
  private:
    spi_device_handle_t spi = NULL;
    uint32_t            i2c_data_pdMS_TICKS = 0;
    uint32_t            i2c_done_pdMS_TICKS = 0;
    const char         *TAG                 = "Display";
    const char         *img_to_bmpName      = "/sdcard/06_user_foundation_img/sys_decode.bmp";
    ImgDecodeDither &dither_;
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 busy_;
    uint16_t            width_;
    uint16_t            height_;
    uint16_t            scale_MaxWidth_;
    uint16_t            scale_MaxHeight_;
    uint8_t            *DispBuffer = NULL;
    uint8_t            *RotationBuffer = NULL;
    uint8_t            *BmpSrcBuffer = NULL;
    int                 DisplayLen = 0;
    uint16_t            src_width = 0;
    uint16_t            src_height = 0;
    uint8_t Rotation = 0;                          //0:0 1:90 2:180 3:270
    uint8_t mirrx = 0;                             
    uint8_t mirry = 0;
    bool isEPDInit = false;
    bool needs_reinit_ = false;
    bool panel_powered_ = false;
    esp_err_t init_error_ = ESP_OK;
    spi_host_device_t spi_host_;
    bool spi_bus_initialized_ = false;
    bool spi_device_added_ = false;
    SemaphoreHandle_t display_mutex_ = NULL;

    static constexpr TickType_t kMutexWaitTicks = pdMS_TO_TICKS(1000);
    static constexpr uint32_t kInitTimeoutMs = 10000;
    static constexpr uint32_t kPowerOnTimeoutMs = 10000;
    static constexpr uint32_t kRefreshTimeoutMs = 120000;
    static constexpr uint32_t kPowerOffTimeoutMs = 10000;

    bool LockDisplay();
    void UnlockDisplay();
    esp_err_t EnsureInitializedLocked();
    esp_err_t EPD_InitLocked();
    esp_err_t EPD_DispClearLocked(uint8_t color);
    esp_err_t EPD_DisplayLocked();
    esp_err_t EPD_SDcardBmpShakingColorLocked(const char *path,uint16_t x_start,uint16_t y_start);
    esp_err_t EPD_DecodeAndPrepareImageLocked(const char *path, uint16_t x_start,
                                               uint16_t y_start, bool allow_scale);
    esp_err_t EPD_PowerOffLocked(void);
    void SetPixel4Locked(uint8_t *buf, int width, int x, int y, uint8_t px);

    void    Set_ResetIOLevel(uint8_t level);
    void    Set_CSIOLevel(uint8_t level);
    void    Set_DCIOLevel(uint8_t level);
    uint8_t Get_BusyIOLevel();
    void    EPD_Reset(void);
    esp_err_t EPD_LoopBusy(uint32_t timeout_ms);
    esp_err_t SPI_Write(uint8_t data);
    esp_err_t EPD_SendCommand(uint8_t Reg);
    esp_err_t EPD_SendData(uint8_t Data);
    esp_err_t EPD_Sendbuffera(uint8_t *Data, int len);
    esp_err_t EPD_TurnOnDisplay(void);
    uint8_t EPD_ColorToePaperColor(uint8_t b,uint8_t g,uint8_t r);
    uint8_t* EPD_ParseBMPImage(const char *path);
    uint8_t EPD_GetPixel4(const uint8_t* buf, int width, int x, int y);
    void    EPD_SetPixel4(uint8_t* buf, int width, int x, int y, uint8_t px);
    void EPD_Rotate180_Fast(const uint8_t* src, uint8_t* dst, int width, int height);
    void EPD_Rotate90CCW_Fast(const uint8_t* src, uint8_t* dst, int width, int height);
    void EPD_Rotate90CW_Fast(const uint8_t* src, uint8_t* dst, int width, int height);
    void EPD_PixelRotate();
    void EPD_DrawChar(uint16_t Xpoint, uint16_t Ypoint, const char Acsii_Char,sFONT* Font, uint16_t Color_Foreground, uint16_t Color_Background);

  public:
    ePaperPort(ImgDecodeDither &dither,int mosi, int scl, int dc, int cs, int rst, int busy, uint16_t width, uint16_t height, uint16_t scale_MaxWidth, uint16_t scale_MaxHeight, spi_host_device_t spihost = SPI3_HOST);
    ~ePaperPort();
    bool Ready() const { return init_error_ == ESP_OK && spi_device_added_; }

    esp_err_t EPD_Init();
    esp_err_t EPD_DispClear(uint8_t color);
    esp_err_t EPD_Display();
    esp_err_t EPD_SrcDisplayCopy(uint8_t *buffer,uint32_t len,uint32_t addlen);
    void Set_Rotation(uint8_t rot); // 0:no 1:90 2:180 3:270
    void Set_Mirror(uint8_t mirr_x,uint8_t mirr_y);
    uint8_t* EPD_GetIMGBuffer();
    esp_err_t EPD_SetPixel(uint16_t x, uint16_t y, uint16_t color);
    esp_err_t EPD_SDcardBmpShakingColor(const char *path,uint16_t x_start, uint16_t y_start);        /*只能用于经过抖动之后的 480x800/800x480 BMP图片显示*/
    esp_err_t EPD_SDcardIMGShakingColor(const char *path,uint16_t x_start, uint16_t y_start);        /*可以显示jpg,bmp,png格式图片 480x800/800x480*/
    esp_err_t EPD_SDcardScaleIMGShakingColor(const char *path,uint16_t x_start, uint16_t y_start);   /*可以显示jpg,bmp,png格式图片,带自动拉伸缩放的*/
    /* Atomic convenience entries for the manager task. */
    esp_err_t EPD_ShowBmp(const char *path, uint16_t x_start = 0, uint16_t y_start = 0);
    esp_err_t EPD_ShowClear(uint8_t color);
	esp_err_t EPD_DrawStringCN(uint16_t Xstart, uint16_t Ystart, const char * pString, cFONT* font,uint16_t Color_Foreground, uint16_t Color_Background);
    esp_err_t EPD_DrawStringEN(uint16_t Xstart, uint16_t Ystart, const char * pString,sFONT* Font, uint16_t Color_Foreground, uint16_t Color_Background);
};
