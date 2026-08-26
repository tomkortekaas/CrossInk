cmake_minimum_required(VERSION 3.16)
project(dashboard_v3_preview CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(ZLIB REQUIRED)

add_executable(dashboard-v3-preview
  main.cpp
  PreviewCanvas.cpp
  PreviewFonts.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/BleHandoffRecord.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardV3.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardV3Layout.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardV3Renderer.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardDateFields.cpp
)

target_include_directories(dashboard-v3-preview PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/spikes/ble_handoff
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
)

target_link_libraries(dashboard-v3-preview PRIVATE ZLIB::ZLIB)

# The generated font headers leave the optional SD-card hooks unset, which is
# correct for a built-in font but trips -Wextra on every one of them.
target_compile_options(dashboard-v3-preview PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
