cmake_minimum_required(VERSION 3.16)

# Point at the parent project's sdkconfig.defaults so the test app
# gets the same chip target / PSRAM / flash settings.
set(SDKCONFIG_DEFAULTS "../sdkconfig.defaults.esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(langoustine_tests)
