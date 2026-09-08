#pragma once

/*
 * A small, dependency-free validator for the BMP files used by PhotoPull.
 *
 * The validator consumes a forward-only stream.  This makes the exact same
 * checks usable while a file is being downloaded, by the web upload path,
 * and by host tests without having to keep a 1.1 MiB image in RAM.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BMP_VALIDATOR_OK = 0,
    BMP_VALIDATOR_INVALID_ARGUMENT,
    BMP_VALIDATOR_SHORT_READ,
    BMP_VALIDATOR_BAD_FILE_SIZE,
    BMP_VALIDATOR_BAD_SIGNATURE,
    BMP_VALIDATOR_BAD_HEADER,
    BMP_VALIDATOR_BAD_DIMENSIONS,
    BMP_VALIDATOR_BAD_LAYOUT,
    BMP_VALIDATOR_BAD_PIXEL,
} bmp_validator_result_t;

typedef size_t (*bmp_validator_read_fn)(void *context, void *buffer, size_t length);

typedef struct {
    void *context;
    uint64_t total_size;
    bmp_validator_read_fn read;
} bmp_validator_stream_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t row_stride;
    uint32_t image_size;
    uint32_t pixel_offset;
    uint64_t total_size;
} bmp_validator_info_t;

/*
 * Validate one strict PhotoPull BMP:
 *   800x480 or 480x800, 24-bit BI_RGB, positive height, offset 54,
 *   exact 1,152,054-byte length, and six-color BGR pixels.
 */
bmp_validator_result_t bmp_validator_validate_stream(const bmp_validator_stream_t *stream,
                                                       bmp_validator_info_t *info);

/* Convenience wrapper for a seekable file.  It is intentionally separate
 * from the stream API so embedded callers can use the latter for downloads. */
bmp_validator_result_t bmp_validator_validate_file(const char *path,
                                                    bmp_validator_info_t *info);

#ifdef __cplusplus
}
#endif
