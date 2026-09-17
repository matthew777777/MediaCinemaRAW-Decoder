cmake_minimum_required(VERSION 3.16)
project(MediaCinemaRAWDecoder VERSION 0.1.0 LANGUAGES CXX)

add_library(MediaCinemaRAWDecoder src/Decoder.cpp src/ContainerReader.cpp)
add_library(MediaCinemaRAW::Decoder ALIAS MediaCinemaRAWDecoder)
set_target_properties(MediaCinemaRAWDecoder PROPERTIES EXPORT_NAME Decoder)
target_compile_features(MediaCinemaRAWDecoder PUBLIC cxx_std_17)
target_compile_options(MediaCinemaRAWDecoder PRIVATE -O3 -Wall -Wextra)
target_include_directories(MediaCinemaRAWDecoder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

include(GNUInstallDirs)
install(TARGETS MediaCinemaRAWDecoder EXPORT MediaCinemaRAWDecoderTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MediaCinemaRAWDecoderTargets
    NAMESPACE MediaCinemaRAW::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MediaCinemaRAWDecoder)

option(MEDIACINEMARAW_BUILD_TESTS "Build decoder self-tests" ON)
if(MEDIACINEMARAW_BUILD_TESTS)
    enable_testing()
    add_executable(MediaCinemaRAWDecoderTests tests/DecoderTests.cpp)
    target_link_libraries(MediaCinemaRAWDecoderTests PRIVATE MediaCinemaRAW::Decoder)
    add_test(NAME decoder_validation COMMAND MediaCinemaRAWDecoderTests)
endif()

option(MEDIACINEMARAW_BUILD_TOOLS "Build dump tool" ON)
if(MEDIACINEMARAW_BUILD_TOOLS)
    add_executable(mcraw_dump tools/mcraw_dump.cpp)
    target_link_libraries(mcraw_dump PRIVATE MediaCinemaRAW::Decoder)
endif()
