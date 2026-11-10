cmake_minimum_required(VERSION 3.24)
project(CoreVideoEdge VERSION 0.1.0 LANGUAGES CXX)

# Windows x64 (Media Foundation) and macOS on Apple Silicon (AVFoundation +
# VideoToolbox). Nothing else is supported.
if(APPLE)
  enable_language(OBJCXX)
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64)
  endif()
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)
  endif()
elseif(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "CoreVideo Edge builds for Windows x64 and macOS (Apple Silicon) only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
# Windows: every executable lands in <build>/bin for single-config generators
# (Ninja) and <build>/bin/<Config> for Visual Studio, so a Debug build can never
# silently overwrite the Release binary (a trap CoreVideo Pro hit; see its
# CLAUDE.md). macOS: the build root itself (build/mac, build/engine), which is
# what the CI workflow, make-macos-engine-bundle.sh and the app packager read;
# Ninja is single-config there too.
if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
else()
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

if(WIN32)
  add_compile_options(/utf-8 /EHsc /W3 /permissive-)
  add_compile_definitions(NOMINMAX _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers)
endif()

if(WIN32)
  set(ZOOM_SDK_DIR "$ENV{ZOOM_SDK_DIR}" CACHE PATH
      "Zoom Meeting SDK x64 folder (contains h/ and lib/). Never committed.")
  option(EDGE_BUILD_ZOOM_ENGINE "Build the real corevideo-zoom-engine (needs ZOOM_SDK_DIR)" ON)
else()
  set(ZOOM_SDK_DIR "$ENV{ZOOM_SDK_DIR}" CACHE PATH
      "Folder CONTAINING ZoomSDK.framework (Zoom Meeting SDK for macOS). Never committed.")
  # Off by default on macOS: the SDK reaches CI only from the private draft
  # release, so the node/tests job builds without it.
  option(EDGE_BUILD_ZOOM_ENGINE "Build the real corevideo-zoom-engine (needs ZOOM_SDK_DIR)" OFF)
endif()

# ---------------------------------------------------------------------------
# Code lifted from CoreVideo Pro (modules/, core/, rpc/ keep Pro's layout so
# the lifted #include lines are untouched). No Zoom SDK dependency.
# ---------------------------------------------------------------------------
add_library(edge_lifted STATIC
  src/rpc/Json.cpp
  src/core/BoundedAsyncLog.cpp
  src/modules/ZoomEngineClient.cpp
  src/modules/ZoomEngineProcess.cpp
)
target_include_directories(edge_lifted PUBLIC src zoom-engine/shared)
if(APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(edge_lifted PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# CoreVideo Edge: supervisor, subscriptions, ISO record.
# ---------------------------------------------------------------------------
set(EDGE_CORE_SOURCES
  src/edge/EdgeController.cpp
  src/edge/EngineSupervisor.cpp
  src/edge/IsoMp4Writer.cpp
  src/edge/IsoSourceRecorder.cpp
  src/edge/IsoSession.cpp
  src/edge/EdgeConfig.cpp
  src/edge/PreviewPublisher.cpp
  src/edge/NdiRuntime.cpp
  src/edge/FeedSender.cpp
  src/edge/SrtFeedSink.cpp
)
if(WIN32)
  list(APPEND EDGE_CORE_SOURCES src/edge/IsoEncoderMediaFoundation.cpp)
else()
  list(APPEND EDGE_CORE_SOURCES src/edge/IsoEncoderAVFoundation.mm)
endif()
add_library(edge_core STATIC ${EDGE_CORE_SOURCES})
target_include_directories(edge_core PUBLIC src)
target_compile_definitions(edge_core PUBLIC EDGE_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_link_libraries(edge_core PUBLIC edge_lifted mfplat mfreadwrite mfuuid mf ole32)
else()
  set_source_files_properties(src/edge/IsoEncoderAVFoundation.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(edge_core PUBLIC edge_lifted
    "-framework Foundation" "-framework AVFoundation" "-framework CoreMedia"
    "-framework CoreVideo" "-framework VideoToolbox" "-framework CoreFoundation")
endif()

add_executable(corevideo-edge src/edge/main.cpp)
target_link_libraries(corevideo-edge PRIVATE edge_core)

# Verification probe for Edge's NDI feeds (loads the NDI runtime at run time).
add_executable(corevideo-edge-ndi-probe tools/ndi-probe.cpp)
target_link_libraries(corevideo-edge-ndi-probe PRIVATE edge_core)

# ---------------------------------------------------------------------------
# Vendored Zoom engine (native/zoom-engine, see its README for provenance).
# ---------------------------------------------------------------------------
# The synthetic engine needs no SDK and is always built: it is how Edge is
# tested without a meeting.
add_executable(corevideo-zoom-engine-fake zoom-engine/fake/fake-engine.cpp)
target_include_directories(corevideo-zoom-engine-fake PRIVATE zoom-engine/shared zoom-engine/engine)
if(APPLE)
  target_link_libraries(corevideo-zoom-engine-fake PRIVATE Threads::Threads)
endif()

if(EDGE_BUILD_ZOOM_ENGINE AND WIN32)
  if(NOT ZOOM_SDK_DIR OR NOT EXISTS "${ZOOM_SDK_DIR}/h/zoom_sdk.h")
    message(FATAL_ERROR
      "EDGE_BUILD_ZOOM_ENGINE=ON needs ZOOM_SDK_DIR pointing at the Zoom Meeting SDK x64 folder "
      "(the one containing h/zoom_sdk.h and lib/sdk.lib). Pass -DEDGE_BUILD_ZOOM_ENGINE=OFF to build "
      "only the fake engine.")
  endif()
  add_executable(corevideo-zoom-engine
    zoom-engine/engine/main.cpp
    zoom-engine/engine/engine-video.cpp
    zoom-engine/engine/engine-audio.cpp
    zoom-engine/engine/engine-share.cpp
  )
  target_include_directories(corevideo-zoom-engine PRIVATE
    zoom-engine/engine
    zoom-engine/shared
    "${ZOOM_SDK_DIR}/h"
    "${ZOOM_SDK_DIR}/h/rawdata"
    "${ZOOM_SDK_DIR}/h/meeting_service_components"
  )
  file(GLOB EDGE_ZOOM_SDK_LIBS "${ZOOM_SDK_DIR}/lib/*.lib")
  target_link_libraries(corevideo-zoom-engine PRIVATE ws2_32 crypt32 winhttp shlwapi ${EDGE_ZOOM_SDK_LIBS})
  # The SDK runtime must sit next to the engine. Copied at build time from
  # ZOOM_SDK_DIR; .gitignore keeps it out of the repository.
  add_custom_command(TARGET corevideo-zoom-engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ZOOM_SDK_DIR}/bin" "$<TARGET_FILE_DIR:corevideo-zoom-engine>")
elseif(EDGE_BUILD_ZOOM_ENGINE AND APPLE)
  if(NOT ZOOM_SDK_DIR OR NOT EXISTS "${ZOOM_SDK_DIR}/ZoomSDK.framework")
    message(FATAL_ERROR
      "EDGE_BUILD_ZOOM_ENGINE=ON needs ZOOM_SDK_DIR pointing at the folder that CONTAINS "
      "ZoomSDK.framework (Zoom Meeting SDK for macOS).")
  endif()
  # The macOS Meeting SDK is Objective-C only, so the engine is the
  # self-contained ObjC++ rewrite (main-macos.mm, vendored from CoreVideo Pro,
  # see zoom-engine/README.md). Built WITHOUT ARC: the file manages retain and
  # release itself.
  add_executable(corevideo-zoom-engine zoom-engine/engine/main-macos.mm)
  target_include_directories(corevideo-zoom-engine PRIVATE zoom-engine/engine zoom-engine/shared)
  # -F must be on BOTH the compile line (so <ZoomSDK/ZoomSDK.h> resolves) and
  # the link line.
  target_compile_options(corevideo-zoom-engine PRIVATE "-F${ZOOM_SDK_DIR}" -fno-objc-arc)
  target_link_libraries(corevideo-zoom-engine PRIVATE
    "-F${ZOOM_SDK_DIR}" "-framework ZoomSDK" "-framework Foundation" "-framework AppKit" Threads::Threads)
  # BUILD_RPATH: the framework is linked through raw -F flags, so CMake infers
  # no rpath and dyld would abort at launch. make-macos-engine-bundle.sh swaps
  # it for @executable_path/../Frameworks inside corevideo-zoom-engine.app,
  # which is the only layout the SDK can authenticate from.
  set_target_properties(corevideo-zoom-engine PROPERTIES
    BUILD_WITH_INSTALL_RPATH OFF
    BUILD_RPATH "${ZOOM_SDK_DIR}"
    INSTALL_RPATH "@executable_path/../Frameworks")
endif()

# ---------------------------------------------------------------------------
# Tests (no SDK, no meeting). Run: ctest --test-dir <build> -C Release
# ---------------------------------------------------------------------------
enable_testing()
add_executable(corevideo-edge-tests
  tests/TestMain.cpp
  tests/MeetingInputTest.cpp
  tests/SubscriptionPolicyTest.cpp
  tests/IsoAudioTimelineTest.cpp
  tests/IsoNamingTest.cpp
  tests/EngineProtocolTest.cpp
  tests/IsoMp4WriterTest.cpp
  tests/EdgeConfigTest.cpp
  tests/FastConformTest.cpp
  tests/PreviewPublisherTest.cpp
  tests/FeedTargetsTest.cpp
  tests/PlatformTest.cpp
)
target_include_directories(corevideo-edge-tests PRIVATE src tests)
target_link_libraries(corevideo-edge-tests PRIVATE edge_core)
add_test(NAME corevideo-edge-tests COMMAND corevideo-edge-tests)
