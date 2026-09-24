cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Embed the git short SHA as the app version so OTA can tell builds apart.
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short=7 HEAD
                OUTPUT_VARIABLE GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GIT_SHA)
  set(PROJECT_VER "${GIT_SHA}")
endif()

project(esp32_speaker)
