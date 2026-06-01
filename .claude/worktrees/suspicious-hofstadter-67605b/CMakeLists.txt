cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults.esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(langoustine)

# Pre-flash a valid LittleFS image so first boot does not need runtime formatting.
littlefs_create_partition_image(littlefs littlefs_data FLASH_IN_PROJECT)
