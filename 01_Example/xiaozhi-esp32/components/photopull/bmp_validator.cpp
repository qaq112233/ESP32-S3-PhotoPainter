#include "bmp_validator.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr uint64_t kExpectedFileSize = 1152054ULL;
constexpr uint32_t kExpectedImageSize = 1152000U;
constexpr uint32_t kHeaderSize = 54U;

uint16_t read_u16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool read_exact(const bmp_validator_stream_t &stream, void *buffer, size_t length) {
    auto *out = static_cast<uint8_t *>(buffer);
    size_t done = 0;
    while (done < length) {
        const size_t got = stream.read(stream.context, out + done, length - done);
        if (got == 0 || got > length - done) {
            return false;
        }
        done += got;
    }
    return true;
}

bool is_photo_color(uint8_t b, uint8_t g, uint8_t r) {
    return (b == 0x00 && g == 0x00 && r == 0x00) || // black
           (b == 0xff && g == 0xff && r == 0xff) || // white
           (b == 0x00 && g == 0x00 && r == 0xff) || // red
           (b == 0x00 && g == 0xff && r == 0x00) || // green
           (b == 0xff && g == 0x00 && r == 0x00) || // blue
           (b == 0x00 && g == 0xff && r == 0xff);   // yellow
}

size_t file_read(void *context, void *buffer, size_t length) {
    return fread(buffer, 1, length, static_cast<FILE *>(context));
}

} // namespace

extern "C" bmp_validator_result_t bmp_validator_validate_stream(
    const bmp_validator_stream_t *stream, bmp_validator_info_t *info) {
    if (info != nullptr) {
        *info = {};
    }
    if (stream == nullptr || info == nullptr || stream->read == nullptr ||
        stream->total_size != kExpectedFileSize) {
        return stream == nullptr || info == nullptr || stream->read == nullptr
                   ? BMP_VALIDATOR_INVALID_ARGUMENT
                   : BMP_VALIDATOR_BAD_FILE_SIZE;
    }

    uint8_t header[kHeaderSize];
    if (!read_exact(*stream, header, sizeof(header))) {
        return BMP_VALIDATOR_SHORT_READ;
    }

    if (read_u16(header + 0) != 0x4d42) {
        return BMP_VALIDATOR_BAD_SIGNATURE;
    }
    if (read_u32(header + 2) != kExpectedFileSize ||
        read_u16(header + 6) != 0 || read_u16(header + 8) != 0 ||
        read_u32(header + 10) != kHeaderSize) {
        return BMP_VALIDATOR_BAD_HEADER;
    }

    const uint32_t dib_size = read_u32(header + 14);
    const uint32_t width = read_u32(header + 18);
    const uint32_t height = read_u32(header + 22);
    const uint16_t planes = read_u16(header + 26);
    const uint16_t bits = read_u16(header + 28);
    const uint32_t compression = read_u32(header + 30);
    const uint32_t image_size = read_u32(header + 34);
    if (dib_size != 40 || (width != 800 && width != 480) ||
        (height != 480 && height != 800) || width * height != 384000 ||
        planes != 1 || bits != 24 || compression != 0 ||
        image_size != kExpectedImageSize) {
        return (width == 800 || width == 480) && (height == 480 || height == 800)
                   ? BMP_VALIDATOR_BAD_HEADER
                   : BMP_VALIDATOR_BAD_DIMENSIONS;
    }

    const uint32_t row_stride = ((width * 3U) + 3U) & ~3U;
    if (row_stride * height != kExpectedImageSize) {
        return BMP_VALIDATOR_BAD_LAYOUT;
    }

    uint8_t row[2400];
    for (uint32_t y = 0; y < height; ++y) {
        if (!read_exact(*stream, row, row_stride)) {
            return BMP_VALIDATOR_SHORT_READ;
        }
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *pixel = row + x * 3U;
            if (!is_photo_color(pixel[0], pixel[1], pixel[2])) {
                return BMP_VALIDATOR_BAD_PIXEL;
            }
        }
    }

    *info = {};
    info->width = width;
    info->height = height;
    info->row_stride = row_stride;
    info->image_size = kExpectedImageSize;
    info->pixel_offset = kHeaderSize;
    info->total_size = kExpectedFileSize;
    return BMP_VALIDATOR_OK;
}

extern "C" bmp_validator_result_t bmp_validator_validate_file(
    const char *path, bmp_validator_info_t *info) {
    if (path == nullptr || info == nullptr) {
        return BMP_VALIDATOR_INVALID_ARGUMENT;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return BMP_VALIDATOR_SHORT_READ;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return BMP_VALIDATOR_SHORT_READ;
    }
    const long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return BMP_VALIDATOR_SHORT_READ;
    }

    bmp_validator_stream_t stream = {
        .context = file,
        .total_size = static_cast<uint64_t>(end),
        .read = file_read,
    };
    bmp_validator_result_t result = bmp_validator_validate_stream(&stream, info);
    if (ferror(file)) result = BMP_VALIDATOR_SHORT_READ;
    if (fclose(file) != 0) result = BMP_VALIDATOR_SHORT_READ;
    return result;
}
