cmake_minimum_required(VERSION 3.2)

project(threadpool)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
set(bin_name "threadpool")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	add_definitions(-DTP_DEBUG)
endif()

set(
	CMAKE_RUNTIME_OUTPUT_DIRECTORY
	"${CMAKE_CURRENT_SOURCE_DIR}/bin"
	CACHE
	PATH
	"Executable output directory"
)

include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/include
	)

set(
	src_list
	include/dlog.hpp
	include/threadpool.hpp
	src/example.cpp
	)

add_executable(${bin_name} ${src_list})
target_link_libraries(${bin_name} ${CMAKE_THREAD_LIBS_INIT})
