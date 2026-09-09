cmake_minimum_required(VERSION 3.20)
project(wxl_video_host LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(MPV_ROOT "" CACHE PATH "x86 libmpv SDK root")

set(IMGUI_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)

add_executable(wxl-video-host WIN32
    VideoHost.cpp FramePublisher.cpp MpvPlayer.cpp PlexClient.cpp TokenStore.cpp NativeUi.cpp
    ${IMGUI_SOURCES})
target_include_directories(wxl-video-host PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${MPV_ROOT}/include"
    "${CMAKE_CURRENT_LIST_DIR}/third_party"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/imgui"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/imgui/backends")
target_compile_definitions(wxl-video-host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(wxl-video-host PRIVATE
    d3d11 dxgi d3dcompiler user32 gdi32 winhttp crypt32 shell32 ole32 oleaut32)
if(MPV_ROOT)
    add_custom_command(TARGET wxl-video-host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MPV_ROOT}/libmpv-2.dll"
            "$<TARGET_FILE_DIR:wxl-video-host>/libmpv-2.dll")
endif()

add_executable(mpv_probe tests/MpvProbe.cpp)
target_include_directories(mpv_probe PRIVATE "${MPV_ROOT}/include")
target_compile_definitions(mpv_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_test(NAME mpv_probe COMMAND mpv_probe)

add_executable(frame_publisher_tests tests/FramePublisherTests.cpp FramePublisher.cpp)
target_include_directories(frame_publisher_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(frame_publisher_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_test(NAME frame_publisher_tests COMMAND frame_publisher_tests)

add_executable(plex_fixture_tests tests/PlexFixtureTests.cpp PlexClient.cpp TokenStore.cpp)
target_include_directories(plex_fixture_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/third_party")
target_compile_definitions(plex_fixture_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(plex_fixture_tests PRIVATE winhttp shell32 crypt32 user32 ole32 oleaut32)
add_test(NAME plex_fixture_tests COMMAND plex_fixture_tests)

add_executable(token_store_tests tests/TokenStoreTests.cpp TokenStore.cpp)
target_include_directories(token_store_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(token_store_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(token_store_tests PRIVATE crypt32)
add_test(NAME token_store_tests COMMAND token_store_tests)

add_executable(mpv_player_tests tests/MpvPlayerTests.cpp MpvPlayer.cpp)
target_include_directories(mpv_player_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${MPV_ROOT}/include")
target_compile_definitions(mpv_player_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(mpv_player_tests PRIVATE d3d11 dxgi user32)
add_test(NAME mpv_player_tests COMMAND mpv_player_tests)

add_executable(native_ui_tests tests/NativeUiTests.cpp NativeUi.cpp MpvPlayer.cpp PlexClient.cpp TokenStore.cpp FramePublisher.cpp ${IMGUI_SOURCES})
target_include_directories(native_ui_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${MPV_ROOT}/include"
    "${CMAKE_CURRENT_LIST_DIR}/third_party"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/imgui"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/imgui/backends")
target_compile_definitions(native_ui_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(native_ui_tests PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 winhttp crypt32 shell32 ole32 oleaut32)
add_test(NAME native_ui_tests COMMAND native_ui_tests)
