# IDF `main` component; the sources are reused from ../src (not duplicated).
#
# ASCII ONLY here (see the note in the root CMakeLists.txt).
#
# To switch to the conventional IDF layout (sources directly under main/):
#   1. rename src/ to main/ (delete the current main directory first)
#   2. change "../src" to "." in SRCS and INCLUDE_DIRS below
# Nothing else in the build depends on the directory names.

idf_component_register(
    SRCS
        "../src/main.c"
        "../src/ble_api.c"
        "../src/epd_drv.c"
        "../src/power.c"
        "../src/settings.c"
        "../src/cache.c"
        "../src/store.c"
    INCLUDE_DIRS
        "../src"
    PRIV_REQUIRES
        bt
        nvs_flash
        app_update
        spi_flash
        esp_partition
        driver
        esp_adc
        esp_timer
)