#include "../bmp_validator.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace {

constexpr size_t kFileSize = 1152054;

void put16(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
    data[offset + 0] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

std::vector<uint8_t> make_bmp(uint32_t width, uint32_t height) {
    const uint32_t row_stride = width * 3U;
    std::vector<uint8_t> data(kFileSize, 0);
    put16(data, 0, 0x4d42);
    put32(data, 2, kFileSize);
    put32(data, 10, 54);
    put32(data, 14, 40);
    put32(data, 18, width);
    put32(data, 22, height);
    put16(data, 26, 1);
    put16(data, 28, 24);
    put32(data, 34, width * height * 3U);
    /* All-black BGR pixels are a valid member of the six-color palette. */
    (void)row_stride;
    return data;
}

struct ChunkReader {
    const std::vector<uint8_t> *data;
    size_t offset;
    size_t max_chunk;
    size_t stop_after;
};

size_t read_chunk(void *context, void *buffer, size_t length) {
    auto &reader = *static_cast<ChunkReader *>(context);
    if (reader.offset >= reader.stop_after || reader.offset >= reader.data->size()) return 0;
    const size_t available = std::min(reader.data->size(), reader.stop_after) - reader.offset;
    const size_t count = std::min({length, reader.max_chunk, available});
    memcpy(buffer, reader.data->data() + reader.offset, count);
    reader.offset += count;
    return count;
}

bmp_validator_result_t validate(const std::vector<uint8_t> &data, size_t chunk,
                                size_t stop_after = kFileSize) {
    ChunkReader reader{&data, 0, chunk, stop_after};
    bmp_validator_stream_t stream{&reader, kFileSize, read_chunk};
    bmp_validator_info_t info{};
    return bmp_validator_validate_stream(&stream, &info);
}

bmp_validator_result_t validate_with_info(const std::vector<uint8_t> &data,
                                           size_t chunk,
                                           bmp_validator_info_t *info) {
    ChunkReader reader{&data, 0, chunk, kFileSize};
    bmp_validator_stream_t stream{&reader, kFileSize, read_chunk};
    return bmp_validator_validate_stream(&stream, info);
}

size_t overreporting_reader(void *, void *, size_t length) {
    return length + 1U;
}

} // namespace

int main() {
    const std::vector<uint8_t> landscape = make_bmp(800, 480);
    const std::vector<uint8_t> portrait = make_bmp(480, 800);
    assert(validate(landscape, 17) == BMP_VALIDATOR_OK);
    assert(validate(portrait, 4096) == BMP_VALIDATOR_OK);
    bmp_validator_info_t info{1, 2, 3, 4, 5, 6};
    assert(validate_with_info(landscape, 17, &info) == BMP_VALIDATOR_OK);
    assert(info.width == 800 && info.height == 480 && info.row_stride == 2400 &&
           info.image_size == 1152000 && info.pixel_offset == 54 &&
           info.total_size == kFileSize);

    std::vector<uint8_t> bad_pixel = landscape;
    bad_pixel[54] = 1;
    assert(validate(bad_pixel, 31) == BMP_VALIDATOR_BAD_PIXEL);

    std::vector<uint8_t> bad_offset = landscape;
    put32(bad_offset, 10, 55);
    assert(validate(bad_offset, 64) == BMP_VALIDATOR_BAD_HEADER);

    std::vector<uint8_t> bad_reserved = landscape;
    put16(bad_reserved, 6, 1);
    info = {1, 2, 3, 4, 5, 6};
    assert(validate_with_info(bad_reserved, 64, &info) == BMP_VALIDATOR_BAD_HEADER);
    assert(info.width == 0 && info.height == 0 && info.total_size == 0);

    std::vector<uint8_t> bad_image_size = landscape;
    put32(bad_image_size, 34, 0);
    assert(validate(bad_image_size, 64) == BMP_VALIDATOR_BAD_HEADER);

    assert(validate(landscape, 128, kFileSize - 1) == BMP_VALIDATOR_SHORT_READ);
    ChunkReader reader{&landscape, 0, 32, kFileSize};
    bmp_validator_stream_t bad_size{&reader, kFileSize - 1, read_chunk};
    info = {};
    assert(bmp_validator_validate_stream(&bad_size, &info) == BMP_VALIDATOR_BAD_FILE_SIZE);

    bmp_validator_stream_t overreport{nullptr, kFileSize, overreporting_reader};
    assert(bmp_validator_validate_stream(&overreport, &info) == BMP_VALIDATOR_SHORT_READ);
    assert(bmp_validator_validate_stream(nullptr, &info) == BMP_VALIDATOR_INVALID_ARGUMENT);
    return 0;
}
