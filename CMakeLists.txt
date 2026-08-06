cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  # macOS 26 SDK removed the legacy AGL framework that prebuilt Qt6 still
  # references; strip it from the fetched deps before Qt is resolved.
  if(APPLE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/patch-qt-agl.cmake")
  endif()

  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/beacon.cpp
    src/log-capture.cpp
    src/connection-monitor.cpp
    src/diagnostics-dock.cpp
    src/log-window.cpp
    src/packet-probe.cpp
    src/beacon-rtmp.c
)

# Vendored copy of OBS's stock RTMP output (plain RTMP, no TLS/SWF) — registered
# as "beacon_rtmp_output" so we can add custom AMF connect data without touching
# OBS. Built with NO_CRYPTO so all TLS/crypto/SWF paths compile out.
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/rtmp-output/rtmp-stream.c
    src/rtmp-output/flv-mux.c
    src/rtmp-output/rtmp-av1.c
    src/rtmp-output/rtmp-hevc.c
    src/rtmp-output/net-if.c
    src/rtmp-output/happy-eyeballs/happy-eyeballs.c
    src/rtmp-output/librtmp/amf.c
    src/rtmp-output/librtmp/log.c
    src/rtmp-output/librtmp/parseurl.c
    src/rtmp-output/librtmp/rtmp.c
    src/rtmp-output/librtmp/md5.c
    src/rtmp-output/librtmp/cencode.c
)
target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/rtmp-output src/rtmp-output/librtmp src/rtmp-output/happy-eyeballs
)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NO_CRYPTO)

# Windows-specific bits of the vendored RTMP output (new-socket-loop, interface
# logging) + the system libs it needs. Verified on macOS/Linux locally; the
# Windows link set mirrors obs-outputs and is confirmed by CI.
if(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/rtmp-output/rtmp-windows.c)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::w32-pthreads ws2_32 iphlpapi winmm)
endif()

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
