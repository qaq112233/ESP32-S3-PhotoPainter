// Copyright 2024 Espressif Systems (Shanghai) CO., LTD.
// All rights reserved.

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "image_io.h"
#include "test_decoder.h"

static jpeg_pixel_format_t j_type     = JPEG_PIXEL_FORMAT_RGB888;
static jpeg_rotate_t       j_rotation = JPEG_ROTATE_0D;
static const uint64_t k_max_jpeg_pixels = 16ULL * 1024ULL * 1024ULL;
static const uint64_t k_max_jpeg_input_bytes = 8ULL * 1024ULL * 1024ULL;

static jpeg_error_t checked_output_length(int width, int height,
                                          jpeg_pixel_format_t format, int *out_len)
{
    if (width <= 0 || height <= 0 || out_len == NULL ||
        (uint64_t)width * (uint64_t)height > k_max_jpeg_pixels) {
        return JPEG_ERR_INVALID_PARAM;
    }

    uint64_t bytes_per_pixel;
    if (format == JPEG_PIXEL_FORMAT_RGB565_LE ||
        format == JPEG_PIXEL_FORMAT_RGB565_BE ||
        format == JPEG_PIXEL_FORMAT_CbYCrY) {
        bytes_per_pixel = 2;
    } else if (format == JPEG_PIXEL_FORMAT_RGB888) {
        bytes_per_pixel = 3;
    } else {
        return JPEG_ERR_INVALID_PARAM;
    }

    const uint64_t bytes = (uint64_t)width * (uint64_t)height * bytes_per_pixel;
    if (bytes == 0 || bytes > (uint64_t)INT_MAX) {
        return JPEG_ERR_INVALID_PARAM;
    }
    *out_len = (int)bytes;
    return JPEG_ERR_OK;
}

jpeg_error_t esp_jpeg_decode_one_picture(uint8_t *input_buf, int len, uint8_t **output_buf, int *out_len, int *s_width, int *s_height)
{
    if (input_buf == NULL || len <= 0 || (uint64_t)len > k_max_jpeg_input_bytes ||
        output_buf == NULL || out_len == NULL) {
        return JPEG_ERR_INVALID_PARAM;
    }
    *output_buf = NULL;
    *out_len = 0;
    if (s_width != NULL) *s_width = 0;
    if (s_height != NULL) *s_height = 0;
    uint8_t *out_buf = NULL;
    jpeg_error_t ret = JPEG_ERR_OK;
    jpeg_dec_io_t *jpeg_io = NULL;
    jpeg_dec_header_info_t *out_info = NULL;

    // Generate default configuration
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = j_type;
    config.rotate = j_rotation;
    // config.scale.width       = 0;
    // config.scale.height      = 0;
    // config.clipper.width     = 0;
    // config.clipper.height    = 0;

    // Create jpeg_dec handle
    jpeg_dec_handle_t jpeg_dec = NULL;
    ret = jpeg_dec_open(&config, &jpeg_dec);
    if (ret != JPEG_ERR_OK) {
        return ret;
    }

    // Create io_callback handle
    jpeg_io = calloc(1, sizeof(jpeg_dec_io_t));
    if (jpeg_io == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }

    // Create out_info handle
    out_info = calloc(1, sizeof(jpeg_dec_header_info_t));
    if (out_info == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }

    // Set input buffer and buffer len to io_callback
    jpeg_io->inbuf = input_buf;
    jpeg_io->inbuf_len = len;

    // Parse jpeg picture header and get picture for user and decoder
    ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);
    if (ret != JPEG_ERR_OK) {
        goto jpeg_dec_failed;
    }

    ret = checked_output_length(out_info->width, out_info->height,
                                config.output_type, out_len);
    if (ret != JPEG_ERR_OK) goto jpeg_dec_failed;
    if(s_width != NULL) {*s_width = out_info->width;}
    if(s_height != NULL) {*s_height = out_info->height;}
    // Calloc out_put data buffer and update inbuf ptr and inbuf_len
    out_buf = jpeg_calloc_align(*out_len, 16);
    if (out_buf == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }
    jpeg_io->outbuf = out_buf;
    *output_buf = out_buf;

    // Start decode jpeg
    ret = jpeg_dec_process(jpeg_dec, jpeg_io);
    if (ret != JPEG_ERR_OK) {
        goto jpeg_dec_failed;
    }

    // Decoder deinitialize
jpeg_dec_failed:
    jpeg_dec_close(jpeg_dec);
    if (jpeg_io) {
        free(jpeg_io);
    }
    if (out_info) {
        free(out_info);
    }
    if (ret != JPEG_ERR_OK && out_buf != NULL) {
        jpeg_free_align(out_buf);
        out_buf = NULL;
        *output_buf = NULL;
        *out_len = 0;
        if (s_width != NULL) *s_width = 0;
        if (s_height != NULL) *s_height = 0;
    }
    return ret;
}

jpeg_error_t esp_jpeg_decode_one_picture_block(unsigned char *input_buf, int len)
{
    if (input_buf == NULL || len <= 0 || (uint64_t)len > k_max_jpeg_input_bytes) {
        return JPEG_ERR_INVALID_PARAM;
    }
    unsigned char *output_block = NULL;
    jpeg_error_t ret = JPEG_ERR_OK;
    jpeg_dec_io_t *jpeg_io = NULL;
    jpeg_dec_header_info_t *out_info = NULL;
#if TEST_USE_SDCARD
    FILE *f_out = NULL;
#endif
    // Generate default configuration
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.block_enable = true;

    // Empty handle to jpeg_decoder
    jpeg_dec_handle_t jpeg_dec = NULL;
    ret = jpeg_dec_open(&config, &jpeg_dec);
    if (ret != JPEG_ERR_OK) {
        return ret;
    }

    // Create io_callback handle
    jpeg_io = calloc(1, sizeof(jpeg_dec_io_t));
    if (jpeg_io == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }

    // Create out_info handle
    out_info = calloc(1, sizeof(jpeg_dec_header_info_t));
    if (out_info == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }

    // Set input buffer and buffer len to io_callback
    jpeg_io->inbuf = input_buf;
    jpeg_io->inbuf_len = len;

    // Parse jpeg picture header and get picture for user and decoder
    ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);
    if (ret != JPEG_ERR_OK) {
        goto jpeg_dec_failed;
    }
    if (out_info->width <= 0 || out_info->height <= 0 ||
        (uint64_t)out_info->width * (uint64_t)out_info->height > k_max_jpeg_pixels) {
        ret = JPEG_ERR_INVALID_PARAM;
        goto jpeg_dec_failed;
    }

    // Calloc block output data buffer
    int output_len = 0;
    ret = jpeg_dec_get_outbuf_len(jpeg_dec, &output_len);
    if (ret != JPEG_ERR_OK || output_len <= 0) {
        goto jpeg_dec_failed;
    }

    output_block = jpeg_calloc_align(output_len, 16);
    if (output_block == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }
    jpeg_io->outbuf = output_block;

    // get process count
    int process_count = 0;
    ret = jpeg_dec_get_process_count(jpeg_dec, &process_count);
    if (ret != JPEG_ERR_OK || process_count == 0) {
        goto jpeg_dec_failed;
    }

#if TEST_USE_SDCARD
    // Open output file on SDCard, should init SDcard first
    f_out = fopen("/sdcard/esp_jpeg_decode_one_picture_block.bin", "wb");
    if (f_out == NULL) {
        ret = JPEG_ERR_FAIL;
        goto jpeg_dec_failed;
    }
#endif  /* TEST_USE_SDCARD */

    // Decode jpeg data
    for (int block_cnt = 0; block_cnt < process_count; block_cnt++) {
        ret = jpeg_dec_process(jpeg_dec, jpeg_io);
        if (ret != JPEG_ERR_OK) {
            goto jpeg_dec_failed;
        }

#if TEST_USE_SDCARD
        // do something - to sdcard
        int written_data = fwrite(jpeg_io->outbuf, 1, jpeg_io->out_size, f_out);
        if (written_data != jpeg_io->out_size) {
            ret = JPEG_ERR_FAIL;
            goto jpeg_dec_failed;
        }
#endif  /* TEST_USE_SDCARD */
    }

    // Decoder deinitialize
jpeg_dec_failed:
    jpeg_dec_close(jpeg_dec);
    if (jpeg_io) {
        free(jpeg_io);
    }
    if (out_info) {
        free(out_info);
    }
    jpeg_free_align(output_block);
#if TEST_USE_SDCARD
    if (f_out) {
        fclose(f_out);
    }
#endif  /* TEST_USE_SDCARD */
    return ret;
}

jpeg_error_t esp_jpeg_stream_open(esp_jpeg_stream_handle_t jpeg_handle)
{
    if (jpeg_handle == NULL) {
        return JPEG_ERR_INVALID_PARAM;
    }
    jpeg_handle->jpeg_dec = NULL;
    jpeg_handle->jpeg_io = NULL;
    jpeg_handle->out_info = NULL;
    jpeg_handle->output_type = j_type;
    jpeg_error_t ret = JPEG_ERR_OK;

    // Generate default configuration
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = j_type;
    config.rotate = j_rotation;
    // config.scale.width       = 0;
    // config.scale.height      = 0;
    // config.clipper.width     = 0;
    // config.clipper.height    = 0;
    // Create jpeg_dec handle
    ret = jpeg_dec_open(&config, &jpeg_handle->jpeg_dec);
    if (ret != JPEG_ERR_OK) {
        return ret;
    }

    // Create io_callback handle
    jpeg_handle->jpeg_io = calloc(1, sizeof(jpeg_dec_io_t));
    if (jpeg_handle->jpeg_io == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }

    // Create out_info handle
    jpeg_handle->out_info = calloc(1, sizeof(jpeg_dec_header_info_t));
    if (jpeg_handle->out_info == NULL) {
        ret = JPEG_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }
    return JPEG_ERR_OK;

    // Decoder deinitialize
jpeg_dec_failed:
    if (jpeg_handle->jpeg_dec != NULL) {
        jpeg_dec_close(jpeg_handle->jpeg_dec);
    }
    if (jpeg_handle->jpeg_io) {
        free(jpeg_handle->jpeg_io);
    }
    if (jpeg_handle->out_info) {
        free(jpeg_handle->out_info);
    }
    jpeg_handle->jpeg_dec = NULL;
    jpeg_handle->jpeg_io = NULL;
    jpeg_handle->out_info = NULL;
    return ret;
}

jpeg_error_t esp_jpeg_stream_decode(esp_jpeg_stream_handle_t jpeg_handle, uint8_t *input_buf, int len, uint8_t **output_buf, int *out_len)
{
    jpeg_error_t ret = JPEG_ERR_OK;
    unsigned char *out_buf = NULL;

    if (jpeg_handle == NULL || jpeg_handle->jpeg_dec == NULL ||
        jpeg_handle->jpeg_io == NULL || jpeg_handle->out_info == NULL ||
        input_buf == NULL || len <= 0 || (uint64_t)len > k_max_jpeg_input_bytes ||
        output_buf == NULL || out_len == NULL) {
        return JPEG_ERR_INVALID_PARAM;
    }
    *output_buf = NULL;
    *out_len = 0;

    // Set input buffer and buffer len to io_callback
    jpeg_handle->jpeg_io->inbuf = input_buf;
    jpeg_handle->jpeg_io->inbuf_len = len;

    // Parse jpeg picture header and get picture for user and decoder
    ret = jpeg_dec_parse_header(jpeg_handle->jpeg_dec, jpeg_handle->jpeg_io, jpeg_handle->out_info);
    if (ret != JPEG_ERR_OK) {
        return ret;
    }

    ret = checked_output_length(jpeg_handle->out_info->width,
                                jpeg_handle->out_info->height,
                                jpeg_handle->output_type, out_len);
    if (ret != JPEG_ERR_OK) return ret;
    // Calloc output data buffer and update inbuf ptr and inbuf_len
    out_buf = jpeg_calloc_align(*out_len, 16);
    if (out_buf == NULL) {
        ret = JPEG_ERR_NO_MEM;
        return ret;
    }
    jpeg_handle->jpeg_io->outbuf = out_buf;
    *output_buf = out_buf;

    // Start decode jpeg
    ret = jpeg_dec_process(jpeg_handle->jpeg_dec, jpeg_handle->jpeg_io);
    if (ret != JPEG_ERR_OK) {
        jpeg_free_align(out_buf);
        jpeg_handle->jpeg_io->outbuf = NULL;
        *output_buf = NULL;
        *out_len = 0;
        return ret;
    }
    return ret;
}

jpeg_error_t esp_jpeg_stream_close(esp_jpeg_stream_handle_t jpeg_handle)
{
    if (jpeg_handle == NULL) {
        return JPEG_ERR_INVALID_PARAM;
    }
    jpeg_error_t ret = JPEG_ERR_OK;

    if (jpeg_handle->jpeg_dec != NULL) {
        ret = jpeg_dec_close(jpeg_handle->jpeg_dec);
    }
    if (jpeg_handle->jpeg_io) {
        free(jpeg_handle->jpeg_io);
    }
    if (jpeg_handle->out_info) {
        free(jpeg_handle->out_info);
    }
    jpeg_handle->jpeg_dec = NULL;
    jpeg_handle->jpeg_io = NULL;
    jpeg_handle->out_info = NULL;
    return ret;
}
