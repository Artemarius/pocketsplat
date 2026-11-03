# splatcore — portable C++20 core, zero GPU/platform deps
add_library(splatcore STATIC
    src/cleaning.cpp
    src/coords.cpp
    src/coverage.cpp
    src/home_view.cpp
    src/pack.cpp
    src/ply_reader.cpp
    src/ply_writer.cpp
    src/quality_controller.cpp
    src/scene_import.cpp
    src/scene_load.cpp
    src/spz_io.cpp
    src/stats.cpp)
target_include_directories(splatcore PUBLIC include)
target_compile_features(splatcore PUBLIC cxx_std_20)
target_link_libraries(splatcore PRIVATE pocketsplat_warnings spz::spz glm::glm)

# scene_import probes legacy gzip SPZ headers with a partial inflate. Reuse whichever zlib spz
# resolved: its FetchContent zlibstatic (deps.cmake rewrites the shared target away), or the
# system/NDK zlib via find_package on platforms that ship one (Android).
if(TARGET zlibstatic)
    target_link_libraries(splatcore PRIVATE zlibstatic)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(splatcore PRIVATE ZLIB::ZLIB)
endif()

if(POCKETSPLAT_BUILD_TESTS)
    add_executable(splatcore_tests
        tests/cleaning_test.cpp
        tests/coords_test.cpp
        tests/coverage_test.cpp
        tests/home_view_test.cpp
        tests/pack_test.cpp
        tests/ply_io_test.cpp
        tests/quality_controller_test.cpp
        tests/scene_import_test.cpp
        tests/spz_io_test.cpp
        tests/stats_test.cpp)
    target_link_libraries(splatcore_tests PRIVATE splatcore GTest::gtest_main pocketsplat_warnings)
    if(TARGET zlibstatic)  # scene_import_test crafts a legacy gzip SPZ header
        target_link_libraries(splatcore_tests PRIVATE zlibstatic)
    else()
        target_link_libraries(splatcore_tests PRIVATE ZLIB::ZLIB)
    endif()
    include(GoogleTest)
    gtest_discover_tests(splatcore_tests)
endif()
