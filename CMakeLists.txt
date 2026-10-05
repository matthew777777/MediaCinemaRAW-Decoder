cmake_minimum_required(VERSION 3.16)
project(MediaCinemaRAWDecoder VERSION 0.1.0 LANGUAGES CXX)

# Static library, no dependencies beyond the C++ standard library.
add_library(MediaCinemaRAWDecoder
    src/ContainerReader.cpp
    src/Decoder.cpp)
add_library(MediaCinemaRAW::Decoder ALIAS MediaCinemaRAWDecoder)
set_target_properties(MediaCinemaRAWDecoder PROPERTIES
    EXPORT_NAME Decoder
    POSITION_INDEPENDENT_CODE ON)
target_compile_features(MediaCinemaRAWDecoder PUBLIC cxx_std_17)
target_include_directories(MediaCinemaRAWDecoder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(MediaCinemaRAWDecoder PRIVATE /W4)
else()
    target_compile_options(MediaCinemaRAWDecoder PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS MediaCinemaRAWDecoder EXPORT MediaCinemaRAWDecoderTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MediaCinemaRAWDecoderTargets
    NAMESPACE MediaCinemaRAW::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWDecoder)

configure_package_config_file(
    cmake/MediaCinemaRAWDecoderConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWDecoderConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWDecoder)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWDecoderConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWDecoderConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MediaCinemaRAWDecoderConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWDecoder)

option(MEDIACINEMARAW_BUILD_TESTS "Build decoder self-tests" ON)
# Optional path to a MediaCinemaRAW-Encoder checkout; enables the
# encode/decode round-trip suite (equivalent to tools/verify_interop.py).
set(MEDIACINEMARAW_ENCODER_DIR ""
    CACHE PATH "MediaCinemaRAW-Encoder checkout for interop tests")

if(MEDIACINEMARAW_BUILD_TESTS)
    enable_testing()
    add_executable(MediaCinemaRAWDecoderTests tests/DecoderTests.cpp)
    target_link_libraries(MediaCinemaRAWDecoderTests PRIVATE MediaCinemaRAW::Decoder)
    if(MSVC)
        target_compile_options(MediaCinemaRAWDecoderTests PRIVATE /W4 /UNDEBUG)
    else()
        target_compile_options(MediaCinemaRAWDecoderTests PRIVATE -Wall -Wextra -Wpedantic)
        # The suite is assert()-based, but Release/RelWithDebInfo builds
        # define NDEBUG, which would silently compile every check out.
        # Target options come after the config flags on the command line,
        # so this -U wins and ctest actually tests. PRIVATE: the library
        # itself keeps NDEBUG.
        target_compile_options(MediaCinemaRAWDecoderTests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME decoder_validation COMMAND MediaCinemaRAWDecoderTests)

    if(MEDIACINEMARAW_ENCODER_DIR)
        foreach(source Encoder.cpp ContainerWriter.cpp)
            if(NOT EXISTS ${MEDIACINEMARAW_ENCODER_DIR}/src/${source})
                message(FATAL_ERROR
                    "missing encoder source: ${MEDIACINEMARAW_ENCODER_DIR}/src/${source}")
            endif()
        endforeach()
        add_executable(InteropTests
            tests/InteropTests.cpp
            ${MEDIACINEMARAW_ENCODER_DIR}/src/Encoder.cpp
            ${MEDIACINEMARAW_ENCODER_DIR}/src/ContainerWriter.cpp)
        target_include_directories(InteropTests PRIVATE
            ${MEDIACINEMARAW_ENCODER_DIR}/include)
        target_link_libraries(InteropTests PRIVATE MediaCinemaRAW::Decoder)
        if(NOT MSVC)
            target_compile_options(InteropTests PRIVATE -UNDEBUG)
        endif()
        add_test(NAME interop COMMAND InteropTests)
    endif()
endif()

option(MEDIACINEMARAW_BUILD_TOOLS "Build dump tool" ON)
if(MEDIACINEMARAW_BUILD_TOOLS)
    add_executable(mcraw_dump tools/mcraw_dump.cpp)
    target_link_libraries(mcraw_dump PRIVATE MediaCinemaRAW::Decoder)
endif()
