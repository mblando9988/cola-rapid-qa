cmake_minimum_required(VERSION 3.16)
project(cola_label_qa CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(cola_label_qa cola_label_qa.cpp)
target_include_directories(cola_label_qa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

set(MUPDF_ROOT "" CACHE PATH "MuPDF installation or source-build root")
set(TESSERACT_ROOT "" CACHE PATH "Tesseract installation root")

if(WIN32)
    # vcpkg-style discovery: cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
    find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h)
    find_library(MUPDF_LIBRARY NAMES mupdf)
    find_path(TESSERACT_INCLUDE_DIR tesseract/baseapi.h)
    find_library(TESSERACT_LIBRARY NAMES tesseract)
    find_library(LEPTONICA_LIBRARY NAMES lept leptonica)
    if(NOT MUPDF_INCLUDE_DIR OR NOT MUPDF_LIBRARY
       OR NOT TESSERACT_INCLUDE_DIR OR NOT TESSERACT_LIBRARY)
        message(FATAL_ERROR
            "MuPDF/tesseract not found. Install them with vcpkg:\n"
            "    vcpkg install mupdf tesseract\n"
            "then configure with:\n"
            "    cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
    endif()
    target_include_directories(cola_label_qa PRIVATE
        ${MUPDF_INCLUDE_DIR} ${TESSERACT_INCLUDE_DIR})
    target_link_libraries(cola_label_qa PRIVATE
        ${MUPDF_LIBRARY} ${TESSERACT_LIBRARY} ${LEPTONICA_LIBRARY})
else()
    find_package(PkgConfig REQUIRED)
    if(TESSERACT_ROOT)
        find_path(TESSERACT_INCLUDE_DIR tesseract/baseapi.h
            HINTS ${TESSERACT_ROOT} ENV TESSERACT_ROOT
            PATH_SUFFIXES include)
        find_library(TESSERACT_LIBRARY NAMES tesseract
            HINTS ${TESSERACT_ROOT} ENV TESSERACT_ROOT
            PATH_SUFFIXES lib lib64)
        pkg_check_modules(LEPTONICA REQUIRED IMPORTED_TARGET lept)
        if(NOT TESSERACT_INCLUDE_DIR OR NOT TESSERACT_LIBRARY)
            message(FATAL_ERROR
                "Tesseract was not found under TESSERACT_ROOT=${TESSERACT_ROOT}.")
        endif()
        target_include_directories(cola_label_qa PRIVATE ${TESSERACT_INCLUDE_DIR})
        set(TESSERACT_LINK_LIBRARIES ${TESSERACT_LIBRARY} PkgConfig::LEPTONICA)
    else()
        pkg_check_modules(TESSERACT REQUIRED IMPORTED_TARGET tesseract)
        set(TESSERACT_LINK_LIBRARIES PkgConfig::TESSERACT)
    endif()

    pkg_check_modules(MUPDF QUIET IMPORTED_TARGET mupdf)
    if(MUPDF_FOUND)
        pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
        target_link_directories(cola_label_qa PRIVATE ${MUPDF_STATIC_LIBRARY_DIRS})
        target_link_options(cola_label_qa PRIVATE ${MUPDF_STATIC_LDFLAGS_OTHER})
        target_link_libraries(cola_label_qa PRIVATE
            ${MUPDF_STATIC_LIBRARIES} PkgConfig::HARFBUZZ ${TESSERACT_LINK_LIBRARIES})
    else()
        find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h
            HINTS ${MUPDF_ROOT} ENV MUPDF_ROOT
            PATH_SUFFIXES include)
        find_library(MUPDF_LIBRARY NAMES mupdf
            HINTS ${MUPDF_ROOT} ENV MUPDF_ROOT
            PATH_SUFFIXES lib build/release)
        find_library(MUPDF_THIRD_LIBRARY NAMES mupdf-third
            HINTS ${MUPDF_ROOT} ENV MUPDF_ROOT
            PATH_SUFFIXES lib build/release)
        if(NOT MUPDF_INCLUDE_DIR OR NOT MUPDF_LIBRARY)
            message(FATAL_ERROR
                "MuPDF was not found by pkg-config. Set MUPDF_ROOT to a MuPDF install "
                "or source tree containing include/mupdf/fitz.h and lib/ or build/release/.")
        endif()
        target_include_directories(cola_label_qa PRIVATE ${MUPDF_INCLUDE_DIR})
        target_link_libraries(cola_label_qa PRIVATE ${MUPDF_LIBRARY})
        if(MUPDF_THIRD_LIBRARY AND MUPDF_LIBRARY MATCHES "\\.a$")
            target_link_libraries(cola_label_qa PRIVATE ${MUPDF_THIRD_LIBRARY})
        endif()
        target_link_libraries(cola_label_qa PRIVATE ${TESSERACT_LINK_LIBRARIES})
        if(UNIX AND NOT APPLE)
            find_package(Threads REQUIRED)
            target_link_libraries(cola_label_qa PRIVATE Threads::Threads m dl)
        endif()
    endif()
endif()
