# Note: This CMakeLists.txt is only required
# if you want to run the example. Otherwise,
# you can just #include threadpool.hpp
# into your project. However, don't forget
# to add the following to your CMakeLists.txt:
#
# find_package(Threads REQUIRED)
#
# as well as this *after* add_executable / add_library
#
# target_link_libraries(bin_or_lib_name ${CMAKE_THREAD_LIBS_INIT})
#
# See below for details.

cmake_minimum_required(VERSION 3.2)

project(threadpool)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
set(tp_bench ON CACHE BOOL "Enable benchmarking for the enqueue() operation")
set(tp_debug ON CACHE BOOL "Enable debug output")
set(bin_name "threadpool")

if(${tp_bench})
	message(STATUS "NOTE: Benchmarking is switched on. Debug output will be switched on as well.")
	add_definitions(-DTP_DEBUG -DTP_BENCH)
else()
	if(${tp_debug})
		add_definitions(-DTP_DEBUG)
	endif()
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
	include/threadpool.hpp
	src/example.cpp
	)

add_executable(${bin_name} ${src_list})
target_link_libraries(${bin_name} ${CMAKE_THREAD_LIBS_INIT})

