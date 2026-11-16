cmake_minimum_required(VERSION 3.20)

project(logsift
    VERSION 0.1.0
    DESCRIPTION "Query structured and semi-structured logs"
    LANGUAGES CXX)

option(LOGSIFT_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(LOGSIFT_BUILD_CLI "Build the logsift command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(LOGSIFT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LOGSIFT_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(logsift
    src/eval.cpp
    src/json.cpp
    src/output.cpp
    src/pipeline.cpp
    src/query_parser.cpp
    src/record.cpp
    src/store.cpp
    src/text.cpp
    src/time.cpp
    src/value.cpp
    src/formats/clf.cpp
    src/formats/logfmt.cpp
    src/formats/reader.cpp
    src/formats/syslog.cpp
)
add_library(logsift::logsift ALIAS logsift)

target_include_directories(logsift PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(logsift PUBLIC cxx_std_20)
set_target_properties(logsift PROPERTIES CXX_EXTENSIONS OFF)

add_library(logsift_build_flags INTERFACE)
if(MSVC)
    target_compile_options(logsift_build_flags INTERFACE /W4 /permissive- /utf-8 /EHsc)
    if(LOGSIFT_WARNINGS_AS_ERRORS)
        target_compile_options(logsift_build_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(logsift_build_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Wold-style-cast)
    if(LOGSIFT_WARNINGS_AS_ERRORS)
        target_compile_options(logsift_build_flags INTERFACE -Werror)
    endif()
    if(LOGSIFT_SANITIZE)
        target_compile_options(logsift_build_flags INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(logsift_build_flags INTERFACE -fsanitize=address,undefined)
    endif()
endif()
target_link_libraries(logsift PRIVATE $<BUILD_INTERFACE:logsift_build_flags>)

if(LOGSIFT_BUILD_CLI)
    add_executable(logsift_cli tools/logsift_cli.cpp)
    set_target_properties(logsift_cli PROPERTIES OUTPUT_NAME logsift)
    target_link_libraries(logsift_cli PRIVATE logsift logsift_build_flags)
endif()

if(LOGSIFT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS logsift EXPORT logsiftTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(LOGSIFT_BUILD_CLI)
    install(TARGETS logsift_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/logsift DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT logsiftTargets NAMESPACE logsift:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/logsift)
