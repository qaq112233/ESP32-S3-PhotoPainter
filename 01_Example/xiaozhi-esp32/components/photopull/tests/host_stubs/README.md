# ESP boundary stubs for host regression tests

Only `manager_integration_test` and `wifi_maintenance_test` include this directory.
The tests compile the production manager, display buffer code, Wi-Fi callbacks,
and maintenance loop. They use controlled time, coalesced event bits, a memory
filesystem, and no-op hardware calls to reproduce state/ordering failures.

These stubs do not emulate FreeRTOS concurrency, Wi-Fi, TLS, SD media, or panel
power. Those behaviors still require ESP-IDF builds and hardware tests. The real
certificate test uses the pinned mbedTLS library and does not see these stubs.
