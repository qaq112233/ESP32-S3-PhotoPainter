#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "imgdecode_app.h"
#include "test_decoder.h"

#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

/* Decoding is fed by SD-card and web-controlled paths.  Keep allocation and
 * arithmetic bounded before handing dimensions to libjpeg/libpng. */
static constexpr uint32_t kMaxDecodeDimension = 4096;
static constexpr uint64_t kMaxDecodePixels = 16ULL * 1024ULL * 1024ULL;
static constexpr uint64_t kMaxJpegInputBytes = 8ULL * 1024ULL * 1024ULL;

static const uint8_t PALETTE[6][3] = {
    {0, 0, 0},       // Black
    {255, 255, 255}, // White
    {255, 0, 0},     // Red
    {0, 255, 0},     // Green
    {0, 0, 255},     // Blue
    {255, 255, 0}    // Yellow
};

void ImgDecodeDither::png_read_callback(png_structp png_ptr, png_bytep data, png_size_t length) {
    FILE *fp = (FILE *)png_get_io_ptr(png_ptr);
    if (fp == NULL || fread(data, 1, length, fp) != length) {
        /* png_error longjmps through the setjmp installed by the caller. */
        png_error(png_ptr, "truncated PNG stream");
    }
}

ImgDecodeDither::ImgDecodeDither() {

}

ImgDecodeDither::~ImgDecodeDither() {

}

esp_err_t ImgDecodeDither::ImgDecode_OneJPGPicture(uint8_t *inbuffer, int inlen, uint8_t **outbuffer, int *outlen) {
    if (inbuffer == NULL || inlen <= 0 || static_cast<uint64_t>(inlen) > kMaxJpegInputBytes ||
        outbuffer == NULL || outlen == NULL) {
        ESP_LOGE(TAG, "jpeg_decode fill inbuffer is NULL");
        return ESP_FAIL;
    }
    *outbuffer = NULL;
    *outlen = 0;
    if (esp_jpeg_decode_one_picture(inbuffer, inlen, outbuffer, outlen,NULL,NULL) == JPEG_ERR_OK) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t ImgDecodeDither::ImgDecode_TFOneJPGPicture(const char *path,uint8_t **outbuffer, int *outlen, int *s_width, int *s_height) {
    if (path == NULL || outbuffer == NULL || outlen == NULL || s_width == NULL || s_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *outbuffer = NULL;
    *outlen = 0;
    *s_width = 0;
    *s_height = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_FAIL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long file_size = ftell(f);
    if (file_size <= 0 || static_cast<uint64_t>(file_size) > kMaxJpegInputBytes) {
        ESP_LOGE(TAG, "Invalid file size");
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buffer = (uint8_t *)malloc(static_cast<size_t>(file_size));
    if (buffer == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t bytes_read = fread(buffer, 1, static_cast<size_t>(file_size), f);
    fclose(f);
    if(bytes_read == static_cast<size_t>(file_size)) {
        if (esp_jpeg_decode_one_picture(buffer, bytes_read, outbuffer, outlen,s_width,s_height) == JPEG_ERR_OK) {
            free(buffer);
            buffer = NULL;
            return ESP_OK;
        } else {
            ESP_LOGE(TAG,"JPG Decode fill");
        }
    }
    if(buffer) {
        free(buffer);
        buffer = NULL;
    }
    return ESP_FAIL;
}

esp_err_t ImgDecodeDither::ImgDecode_TFOnePNGPicture(const char *png_path, uint8_t **out_rgb888,int *out_width, int *out_height) {
    FILE * volatile fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    png_bytep volatile row = NULL;
    png_uint_32 png_width = 0;
    png_uint_32 png_height = 0;
    png_byte bit_depth = 0;
    png_byte color_type = 0;
    png_size_t row_bytes = 0;
    uint64_t rgb_size = 0;
    bool source_has_alpha = false;

    if (png_path == NULL || out_rgb888 == NULL || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_rgb888 = NULL;
    *out_width = 0;
    *out_height = 0;
    fp = fopen(png_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Unable to open PNG file");
        return ESP_FAIL;
    }

    uint8_t png_header[8];
    if (fread(png_header, 1, sizeof(png_header), fp) != sizeof(png_header) ||
        !png_check_sig(png_header, sizeof(png_header))) {
        ESP_LOGE(TAG, "Not a valid PNG file");
        goto clean_up;
    }

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        goto clean_up;
    }
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        goto clean_up;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decoding failed");
        goto clean_up;
    }

    png_set_read_fn(png_ptr, (void *)fp, png_read_callback);
    png_set_sig_bytes(png_ptr, sizeof(png_header));
    png_read_info(png_ptr, info_ptr);

    png_width = png_get_image_width(png_ptr, info_ptr);
    png_height = png_get_image_height(png_ptr, info_ptr);
    if (png_width == 0 || png_height == 0 || png_width > kMaxDecodeDimension ||
        png_height > kMaxDecodeDimension ||
        static_cast<uint64_t>(png_width) * png_height > kMaxDecodePixels) {
        ESP_LOGE(TAG, "PNG dimensions are outside the supported range");
        goto clean_up;
    }
    *out_width = static_cast<int>(png_width);
    *out_height = static_cast<int>(png_height);

    bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);
    source_has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
                       png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) != 0;
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    /* Every supported input is normalized to RGBA before row conversion. */
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (!source_has_alpha && (color_type == PNG_COLOR_TYPE_RGB ||
                              color_type == PNG_COLOR_TYPE_GRAY ||
                              color_type == PNG_COLOR_TYPE_PALETTE)) {
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }
    png_read_update_info(png_ptr, info_ptr);

    row_bytes = png_get_rowbytes(png_ptr, info_ptr);
    if (row_bytes < static_cast<png_size_t>(png_width) * 4U ||
        row_bytes > static_cast<png_size_t>(kMaxDecodeDimension) * 4U) {
        ESP_LOGE(TAG, "PNG row size is outside the supported range");
        goto clean_up;
    }
    rgb_size = static_cast<uint64_t>(png_width) * png_height * 3U;
    if (rgb_size > SIZE_MAX) {
        goto clean_up;
    }
    *out_rgb888 = (uint8_t *)heap_caps_malloc(static_cast<size_t>(rgb_size), MALLOC_CAP_SPIRAM);
    if (!*out_rgb888) {
        ESP_LOGE(TAG, "Failed to allocate RGB888 cache");
        goto clean_up;
    }
    row = (png_bytep)heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM);
    if (!row) {
        goto clean_up;
    }

    for (png_uint_32 y = 0; y < png_height; ++y) {
        png_read_row(png_ptr, (png_bytep)row, NULL);
        uint8_t *rgb888_row = *out_rgb888 + static_cast<size_t>(y) * png_width * 3U;
        for (png_uint_32 x = 0; x < png_width; ++x) {
            rgb888_row[x * 3U + 0] = row[x * 4U + 0];
            rgb888_row[x * 3U + 1] = row[x * 4U + 1];
            rgb888_row[x * 3U + 2] = row[x * 4U + 2];
        }
    }
    png_read_end(png_ptr, NULL);
    heap_caps_free((png_bytep)row);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);
    return ESP_OK;

clean_up:
    if (row != NULL) {
        heap_caps_free((png_bytep)row);
    }
    if (*out_rgb888 != NULL) {
        heap_caps_free(*out_rgb888);
        *out_rgb888 = NULL;
    }
    if (png_ptr != NULL) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    }
    if (fp != NULL) {
        fclose((FILE *)fp);
    }
    *out_width = 0;
    *out_height = 0;
    return ESP_FAIL;
}

esp_err_t ImgDecodeDither::ImgDecodebmp_TFOneBMPPicture(const char *bmp_path, uint8_t **out_rgb888, int *out_width, int *out_height) {
    if (bmp_path == NULL || out_rgb888 == NULL || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_rgb888 = NULL;
    *out_width = 0;
    *out_height = 0;

    FILE *fp = fopen(bmp_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open BMP file");
        return ESP_FAIL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    const long end = ftell(fp);
    if (end < 54 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    const uint64_t file_size = static_cast<uint64_t>(end);

    BITMAPFILEHEADER file_header = {};
    BITMAPINFOHEADER info_header = {};
    if (fread(&file_header, sizeof(file_header), 1, fp) != 1 ||
        fread(&info_header, sizeof(info_header), 1, fp) != 1 ||
        file_header.bfType != 0x4D42 || file_header.bfOffBits < 54 ||
        file_header.bfOffBits > file_size || file_header.bfSize > file_size ||
        info_header.biSize != sizeof(BITMAPINFOHEADER) || info_header.biWidth <= 0 ||
        info_header.biHeight == 0 || info_header.biPlanes != 1 ||
        info_header.biBitCount != 24 || info_header.biCompression != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    const int64_t signed_height = info_header.biHeight;
    const uint64_t image_height = signed_height < 0
                                      ? static_cast<uint64_t>(-(signed_height + 1)) + 1U
                                      : static_cast<uint64_t>(signed_height);
    const uint64_t image_width = static_cast<uint64_t>(info_header.biWidth);
    const uint64_t pixels = image_width * image_height;
    if (image_width > kMaxDecodeDimension || image_height > kMaxDecodeDimension ||
        pixels == 0 || pixels > kMaxDecodePixels || image_width > (UINT64_MAX - 3U) / 3U) {
        fclose(fp);
        return ESP_FAIL;
    }
    const uint64_t row_bytes = (image_width * 3U + 3U) & ~3U;
    const uint64_t image_bytes = row_bytes * image_height;
    if (image_bytes > file_size - file_header.bfOffBits ||
        file_header.bfSize < static_cast<uint64_t>(file_header.bfOffBits) + image_bytes ||
        (info_header.biSizeImage != 0 && info_header.biSizeImage < image_bytes)) {
        fclose(fp);
        return ESP_FAIL;
    }
    const uint64_t rgb_size = pixels * 3U;
    if (rgb_size > SIZE_MAX || row_bytes > SIZE_MAX ||
        fseek(fp, static_cast<long>(file_header.bfOffBits), SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    *out_width = static_cast<int>(image_width);
    *out_height = static_cast<int>(image_height);
    *out_rgb888 = (uint8_t *)heap_caps_malloc(static_cast<size_t>(rgb_size), MALLOC_CAP_SPIRAM);
    uint8_t *bmp_row_buf = (uint8_t *)malloc(static_cast<size_t>(row_bytes));
    if (*out_rgb888 == NULL || bmp_row_buf == NULL) {
        if (*out_rgb888 != NULL) {
            heap_caps_free(*out_rgb888);
            *out_rgb888 = NULL;
        }
        free(bmp_row_buf);
        fclose(fp);
        *out_width = 0;
        *out_height = 0;
        return ESP_ERR_NO_MEM;
    }

    const bool bottom_up = info_header.biHeight > 0;
    for (uint64_t y = 0; y < image_height; ++y) {
        if (fread(bmp_row_buf, 1, static_cast<size_t>(row_bytes), fp) != row_bytes) {
            free(bmp_row_buf);
            heap_caps_free(*out_rgb888);
            *out_rgb888 = NULL;
            *out_width = 0;
            *out_height = 0;
            fclose(fp);
            return ESP_FAIL;
        }
        const uint64_t rgb_y = bottom_up ? image_height - 1U - y : y;
        uint8_t *rgb888_row = *out_rgb888 + static_cast<size_t>(rgb_y * image_width * 3U);
        for (uint64_t x = 0; x < image_width; ++x) {
            const uint8_t *pixel = bmp_row_buf + x * 3U;
            rgb888_row[x * 3U + 0] = pixel[2];
            rgb888_row[x * 3U + 1] = pixel[1];
            rgb888_row[x * 3U + 2] = pixel[0];
        }
    }

    free(bmp_row_buf);
    fclose(fp);
    return ESP_OK;
}

void ImgDecodeDither::ImgDecode_JPGBufferFree(uint8_t *buffer) {
    if (buffer != NULL) {
        jpeg_free_align(buffer);
        buffer = NULL;
    }
}

void ImgDecodeDither::ImgDecode_PNGBufferFree(uint8_t *buffer) {
    if (buffer != NULL) {
        heap_caps_free(buffer);
        buffer = NULL;
    }
}

void  ImgDecodeDither::ImgDecode_BMPBufferFree(uint8_t *buffer) {
    if (buffer != NULL) {
        heap_caps_free(buffer);
        buffer = NULL;
    }
}

esp_err_t ImgDecodeDither::ImgDecode_DitherRgb888(uint8_t *in_img, uint8_t *out_img, int w, int h) {
    if (in_img == NULL || out_img == NULL || w <= 0 || h <= 0 ||
        static_cast<uint64_t>(w) > kMaxDecodeDimension ||
        static_cast<uint64_t>(h) > kMaxDecodeDimension ||
        static_cast<uint64_t>(w) * h > kMaxDecodePixels) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t image_size = static_cast<size_t>(static_cast<uint64_t>(w) * h * 3U);
    uint8_t *work = (uint8_t *) malloc(image_size);
    if (!work) {
        /* Keep legacy callers that ignored the return value from publishing
         * uninitialized bytes as a valid image. */
        memset(out_img, 0xFF, image_size);
        return ESP_ERR_NO_MEM;
    }
    memcpy(work, in_img, image_size);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int     idx = (y * w + x) * 3;
            uint8_t r   = work[idx + 0];
            uint8_t g   = work[idx + 1];
            uint8_t b   = work[idx + 2];

            // Find the nearest color
            int     ci = ImgDecode_NearestColor(r, g, b);
            uint8_t rr = PALETTE[ci][0];
            uint8_t gg = PALETTE[ci][1];
            uint8_t bb = PALETTE[ci][2];

            // Output result
            out_img[idx + 0] = rr;
            out_img[idx + 1] = gg;
            out_img[idx + 2] = bb;

            // Error
            int err_r = (int) r - rr;
            int err_g = (int) g - gg;
            int err_b = (int) b - bb;

            // Floyd–Steinberg diffusion
            //     *   7
            // 3   5   1
            if (x + 1 < w) {
                int n       = idx + 3;
                work[n + 0] = CLAMP(work[n + 0] + (err_r * 7) / 16, 0, 255);
                work[n + 1] = CLAMP(work[n + 1] + (err_g * 7) / 16, 0, 255);
                work[n + 2] = CLAMP(work[n + 2] + (err_b * 7) / 16, 0, 255);
            }
            if (y + 1 < h) {
                if (x > 0) {
                    int n       = ((y + 1) * w + (x - 1)) * 3;
                    work[n + 0] = CLAMP(work[n + 0] + (err_r * 3) / 16, 0, 255);
                    work[n + 1] = CLAMP(work[n + 1] + (err_g * 3) / 16, 0, 255);
                    work[n + 2] = CLAMP(work[n + 2] + (err_b * 3) / 16, 0, 255);
                }
                int n       = ((y + 1) * w + x) * 3;
                work[n + 0] = CLAMP(work[n + 0] + (err_r * 5) / 16, 0, 255);
                work[n + 1] = CLAMP(work[n + 1] + (err_g * 5) / 16, 0, 255);
                work[n + 2] = CLAMP(work[n + 2] + (err_b * 5) / 16, 0, 255);

                if (x + 1 < w) {
                    int n2       = ((y + 1) * w + (x + 1)) * 3;
                    work[n2 + 0] = CLAMP(work[n2 + 0] + (err_r * 1) / 16, 0, 255);
                    work[n2 + 1] = CLAMP(work[n2 + 1] + (err_g * 1) / 16, 0, 255);
                    work[n2 + 2] = CLAMP(work[n2 + 2] + (err_b * 1) / 16, 0, 255);
                }
            }
        }
    }

    free(work);
    return ESP_OK;
}

esp_err_t ImgDecodeDither::ImgDecode_EncodingBmpToSdcard(const char *filename, const uint8_t *inRgb, int width, int height) {
    if (filename == NULL || inRgb == NULL || width <= 0 || height <= 0 ||
        static_cast<uint64_t>(width) > kMaxDecodeDimension ||
        static_cast<uint64_t>(height) > kMaxDecodeDimension ||
        static_cast<uint64_t>(width) * height > kMaxDecodePixels) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("fopen");
        return ESP_FAIL;
    }

    // Each line must be aligned at 4-byte intervals (as required by BMP)
    const uint64_t row_stride64 = (static_cast<uint64_t>(width) * 3U + 3U) & ~3U;
    const uint64_t img_size64 = row_stride64 * static_cast<uint64_t>(height);
    if (row_stride64 > SIZE_MAX || img_size64 > UINT32_MAX ||
        img_size64 > SIZE_MAX - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER)) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t row_stride = static_cast<size_t>(row_stride64);
    const uint32_t img_size = static_cast<uint32_t>(img_size64);

    // Construct the file header
    BITMAPFILEHEADER file_header;
    file_header.bfType      = 0x4D42; // 'BM'
    file_header.bfSize      = static_cast<uint32_t>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + img_size);
    file_header.bfReserved1 = 0;
    file_header.bfReserved2 = 0;
    file_header.bfOffBits   = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    // Construction of information header
    BITMAPINFOHEADER info_header;
    memset(&info_header, 0, sizeof(info_header));
    info_header.biSize        = sizeof(BITMAPINFOHEADER);
    info_header.biWidth       = width;
    info_header.biHeight      = height; // Positive numbers = Stored in reverse order (from bottom to top)
    info_header.biPlanes      = 1;
    info_header.biBitCount    = 24;
    info_header.biCompression = 0; // BI_RGB
    info_header.biSizeImage   = img_size;

    // Write the file header and information header
    if (fwrite(&file_header, sizeof(file_header), 1, f) != 1 ||
        fwrite(&info_header, sizeof(info_header), 1, f) != 1) {
        fclose(f);
        return ESP_FAIL;
    }

    // Write pixel data (BMP requires BGR order, each row is aligned at 4 bytes, and written in reverse order)
    uint8_t *row_buf = (uint8_t *) malloc(row_stride);
    if (!row_buf) {
        fclose(f);
        return ESP_FAIL;
    }

    for (int y = 0; y < height; y++) {
        int            src_row = height - 1 - y; // 倒序
        const uint8_t *src     = inRgb + src_row * width * 3;

        // 转 RGB888 -> BGR888
        for (int x = 0; x < width; x++) {
            row_buf[x * 3 + 0] = src[x * 3 + 2]; // B
            row_buf[x * 3 + 1] = src[x * 3 + 1]; // G
            row_buf[x * 3 + 2] = src[x * 3 + 0]; // R
        }
        // Fill-aligned bytes
        for (int p = width * 3; p < row_stride; p++) {
            row_buf[p] = 0;
        }
        if (fwrite(row_buf, 1, row_stride, f) != row_stride) {
            free(row_buf);
            fclose(f);
            return ESP_FAIL;
        }
    }

    free(row_buf);
    return fclose(f) == 0 ? ESP_OK : ESP_FAIL;
}

// Find the closest color from the palette (RGB888)
int ImgDecodeDither::ImgDecode_NearestColor(uint8_t r, uint8_t g, uint8_t b) {
    int best      = 0;
    int best_dist = 999999;

    for (int i = 0; i < 6; i++) {
        int dr   = (int) r - PALETTE[i][0];
        int dg   = (int) g - PALETTE[i][1];
        int db   = (int) b - PALETTE[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best      = i;
        }
    }
    return best;
}

esp_err_t ImgDecodeDither::ImgDecode_ScaleRgb888Nearest(const uint8_t *src, int src_w, int src_h, uint8_t *dst, int dst_w, int dst_h) {
    if (src == NULL || dst == NULL || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        static_cast<uint64_t>(src_w) > kMaxDecodeDimension ||
        static_cast<uint64_t>(src_h) > kMaxDecodeDimension ||
        static_cast<uint64_t>(dst_w) > kMaxDecodeDimension ||
        static_cast<uint64_t>(dst_h) > kMaxDecodeDimension ||
        static_cast<uint64_t>(src_w) * src_h > kMaxDecodePixels ||
        static_cast<uint64_t>(dst_w) * dst_h > kMaxDecodePixels) {
        return ESP_ERR_INVALID_ARG;
    }
    // 定点数缩放比例（×1024，精度1/1024，平衡精度和速度）
    const int32_t scale_x = (src_w * 1024) / dst_w;
    const int32_t scale_y = (src_h * 1024) / dst_h;

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            // 目标像素对应原图像的定点数坐标（×1024）
            int32_t fx = x * scale_x;
            int32_t fy = y * scale_y;

            // 取4个相邻像素的整数坐标
            int x1 = fx / 1024;
            int y1 = fy / 1024;
            int x2 = x1 + 1;
            int y2 = y1 + 1;

            // 边界处理
            x2 = (x2 >= src_w) ? (src_w - 1) : x2;
            y2 = (y2 >= src_h) ? (src_h - 1) : y2;

            // 计算权重（0~1024，替代浮点0~1）
            int wx = fx - x1 * 1024; // 权重x = fx - floor(fx)
            int wy = fy - y1 * 1024; // 权重y = fy - floor(fy)
            int wx1 = 1024 - wx;
            int wy1 = 1024 - wy;

            // 计算4个相邻像素的偏移
            int off1 = (y1 * src_w + x1) * 3; // 左上
            int off2 = (y1 * src_w + x2) * 3; // 右上
            int off3 = (y2 * src_w + x1) * 3; // 左下
            int off4 = (y2 * src_w + x2) * 3; // 右下

            // 加权计算R/G/B通道（定点数运算，最后÷1024²=1048576）
            int r = (src[off1] * wx1 * wy1 + src[off2] * wx * wy1 +
                     src[off3] * wx1 * wy + src[off4] * wx * wy) / 1048576;
            int g = (src[off1+1] * wx1 * wy1 + src[off2+1] * wx * wy1 +
                     src[off3+1] * wx1 * wy + src[off4+1] * wx * wy) / 1048576;
            int b = (src[off1+2] * wx1 * wy1 + src[off2+2] * wx * wy1 +
                     src[off3+2] * wx1 * wy + src[off4+2] * wx * wy) / 1048576;

            // 限制取值范围0~255，防止溢出
            r = (r < 0) ? 0 : (r > 255) ? 255 : r;
            g = (g < 0) ? 0 : (g > 255) ? 255 : g;
            b = (b < 0) ? 0 : (b > 255) ? 255 : b;

            // 写入目标像素
            int dst_off = (y * dst_w + x) * 3;
            dst[dst_off] = r;
            dst[dst_off+1] = g;
            dst[dst_off+2] = b;
        }
    }
    return ESP_OK;
}
