cmake_minimum_required(VERSION 3.27)
project(mv_extraction LANGUAGES CXX)

find_package(OpenCV REQUIRED)

# set default c++ standard for compile targets
set(CMAKE_CXX_STANDARD 20)

# require C++20, don't allow falling back to older version of standard
set(CXX_STANDARD_REQUIRED ON)

# disable C++ extensions
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)

find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
find_library(AVFORMAT_LIBRARY avformat)

find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h)
find_library(AVUTIL_LIBRARY avutil)

find_path(SWSCALE_INCLUDE_DIR libswscale/swscale.h)
find_library(SWSCALE_LIBRARY swscale)

add_library(mvextractor STATIC video_cap.cpp)
target_include_directories(mvextractor PUBLIC include ${AVCODEC_INCLUDE_DIR} ${AVFORMAT_INCLUDE_DIR} ${AVUTIL_INCLUDE_DIR} ${SWSCALE_INCLUDE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(mvextractor PRIVATE ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${OpenCV_LIBS})
