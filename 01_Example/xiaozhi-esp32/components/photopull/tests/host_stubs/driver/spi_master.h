#pragma once
#include <cstddef>
using spi_host_device_t = int;
using spi_device_handle_t = void*;
struct spi_bus_config_t { int miso_io_num, mosi_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; };
struct spi_device_interface_config_t { int spics_io_num, clock_speed_hz, mode, queue_size, flags; };
struct spi_transaction_t { unsigned length; const void* tx_buffer; };
constexpr int SPI3_HOST = 3, SPI_DMA_CH_AUTO = 1, SPI_DEVICE_HALFDUPLEX = 1;
inline int spi_bus_initialize(int, spi_bus_config_t*, int) { return 0; }
inline int spi_bus_add_device(int, spi_device_interface_config_t*, void** handle) {
    *handle = reinterpret_cast<void*>(1); return 0;
}
inline int spi_bus_remove_device(void*) { return 0; }
inline int spi_bus_free(int) { return 0; }
inline int spi_device_polling_transmit(void*, spi_transaction_t*) { return 0; }
