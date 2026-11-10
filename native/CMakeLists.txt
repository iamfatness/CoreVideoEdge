cmake_minimum_required(VERSION 3.24)
project(CoreVideoEdge VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "CoreVideo Edge v1 builds for Windows x64 only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Every executable lands in <build>/bin for single-config generators (Ninja) and
# <build>/bin/<Config> for Visual Studio, so a Debug build can never silently
# overwrite the Release binary (a trap CoreVideo Pro hit; see its CLAUDE.md).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_compile_options(/utf-8 /EHsc /W3 /permissive-)
add_compile_definitions(NOMINMAX _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)

set(ZOOM_SDK_DIR "$ENV{ZOOM_SDK_DIR}" CACHE PATH
    "Zoom Meeting SDK x64 folder (contains h/ and lib/). Never committed.")
option(EDGE_BUILD_ZOOM_ENGINE "Build the real corevideo-zoom-engine (needs ZOOM_SDK_DIR)" ON)

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

# ---------------------------------------------------------------------------
# CoreVideo Edge: supervisor, subscriptions, ISO record.
# ---------------------------------------------------------------------------
add_library(edge_core STATIC
  src/edge/EdgeController.cpp
  src/edge/EngineSupervisor.cpp
  src/edge/IsoMp4Writer.cpp
  src/edge/IsoSourceRecorder.cpp
  src/edge/IsoSession.cpp
  src/edge/EdgeConfig.cpp
  src/edge/CommsRelay.cpp
  src/edge/PreviewPublisher.cpp
  src/edge/NdiRuntime.cpp
  src/edge/FeedSender.cpp
  src/edge/SrtFeedSink.cpp
)
target_include_directories(edge_core PUBLIC src)
target_link_libraries(edge_core PUBLIC edge_lifted mfplat mfreadwrite mfuuid mf ole32)
target_compile_definitions(edge_core PUBLIC EDGE_VERSION="${PROJECT_VERSION}")

add_executable(corevideo-edge src/edge/main.cpp)
target_link_libraries(corevideo-edge PRIVATE edge_core)

# Verification probe for Edge's NDI feeds (loads the NDI runtime at run time).
add_executable(corevideo-edge-ndi-probe tools/ndi-probe.cpp)
target_link_libraries(corevideo-edge-ndi-probe PRIVATE edge_core)

# ---------------------------------------------------------------------------
# Vendored Zoom engine (native/zoom-engine, see its README for provenance).
# ---------------------------------------------------------------------------
# Comms (Zoom talkback), ported from ZComms into zoom-engine/engine/comms (see
# its PORTED.md). The SDK-free core -- talkback channel ladder, eligibility,
# duck planner, pacer, the orchestration in comms_core -- is shared by the
# real engine, the fake engine and the tests, so all three run the same laws.
set(EDGE_COMMS_DIR zoom-engine/engine/comms)
add_library(edge_comms_core STATIC
  ${EDGE_COMMS_DIR}/comms_core.cpp
  ${EDGE_COMMS_DIR}/talkback_sdk.cpp
  ${EDGE_COMMS_DIR}/talkback_channels.cpp
  ${EDGE_COMMS_DIR}/reach.cpp
  ${EDGE_COMMS_DIR}/room_plan.cpp
  ${EDGE_COMMS_DIR}/duck_plan.cpp
  ${EDGE_COMMS_DIR}/signal_gate.cpp
  ${EDGE_COMMS_DIR}/tx_pacer.cpp
  ${EDGE_COMMS_DIR}/frame_ring.cpp
  ${EDGE_COMMS_DIR}/frame_accumulator.cpp
  ${EDGE_COMMS_DIR}/envelope.cpp
  ${EDGE_COMMS_DIR}/limiter.cpp
)
target_include_directories(edge_comms_core PUBLIC ${EDGE_COMMS_DIR})
# The mic chain (miniaudio: WASAPI capture + sidetone). Real engine only. The
# MA_NO_* macros change miniaudio's struct layouts, so they are PUBLIC: every
# translation unit that sees miniaudio.h must see the same ones (ZComms law).
add_library(edge_comms_audio STATIC
  ${EDGE_COMMS_DIR}/devices.cpp
  ${EDGE_COMMS_DIR}/engine.cpp
  ${EDGE_COMMS_DIR}/miniaudio_impl.cpp
)
target_include_directories(edge_comms_audio PUBLIC ${EDGE_COMMS_DIR} zoom-engine/third_party/miniaudio)
target_compile_definitions(edge_comms_audio PUBLIC MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE MA_NO_NODE_GRAPH)
target_link_libraries(edge_comms_audio PUBLIC edge_comms_core)

# The synthetic engine needs no SDK and is always built: it is how Edge is
# tested without a meeting.
add_executable(corevideo-zoom-engine-fake zoom-engine/fake/fake-engine.cpp zoom-engine/fake/fake-comms.cpp)
target_include_directories(corevideo-zoom-engine-fake PRIVATE zoom-engine/shared zoom-engine/engine)
target_link_libraries(corevideo-zoom-engine-fake PRIVATE edge_comms_core)

if(EDGE_BUILD_ZOOM_ENGINE)
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
    zoom-engine/engine/engine-comms.cpp
    ${EDGE_COMMS_DIR}/talkback_sdk_win.cpp
  )
  target_include_directories(corevideo-zoom-engine PRIVATE
    zoom-engine/engine
    zoom-engine/shared
    "${ZOOM_SDK_DIR}/h"
    "${ZOOM_SDK_DIR}/h/rawdata"
    "${ZOOM_SDK_DIR}/h/meeting_service_components"
  )
  file(GLOB EDGE_ZOOM_SDK_LIBS "${ZOOM_SDK_DIR}/lib/*.lib")
  target_link_libraries(corevideo-zoom-engine PRIVATE edge_comms_audio ws2_32 crypt32 winhttp shlwapi ${EDGE_ZOOM_SDK_LIBS})
  # The SDK runtime must sit next to the engine. Copied at build time from
  # ZOOM_SDK_DIR; .gitignore keeps it out of the repository.
  add_custom_command(TARGET corevideo-zoom-engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ZOOM_SDK_DIR}/bin" "$<TARGET_FILE_DIR:corevideo-zoom-engine>")
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
  tests/CommsProtocolTest.cpp
  tests/CommsCoreTest.cpp
  tests/CommsRelayTest.cpp
  # ZComms' own tests for the ported pieces (tests/comms/test_util.h maps
  # their ZC_CHECK onto this harness).
  tests/comms/ZCommsPortedTest.cpp
  tests/comms/test_talkback_channels.cpp
  tests/comms/test_talkback_eligibility.cpp
  tests/comms/test_room_plan.cpp
  tests/comms/test_reach.cpp
  tests/comms/test_duck_plan.cpp
  tests/comms/test_envelope.cpp
  tests/comms/test_limiter.cpp
  tests/comms/test_frame_accumulator.cpp
  tests/comms/test_sample_ring.cpp
  tests/comms/test_frame_ring.cpp
  tests/comms/test_signal_gate.cpp
)
target_include_directories(corevideo-edge-tests PRIVATE src tests)
target_link_libraries(corevideo-edge-tests PRIVATE edge_core edge_comms_core)
add_test(NAME corevideo-edge-tests COMMAND corevideo-edge-tests)
