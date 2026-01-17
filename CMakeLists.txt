cmake_minimum_required(VERSION 3.18)

set(TARGET IoStoreAssetLoader)
project(${TARGET})

add_library(${TARGET} SHARED
	dllmain.cpp
	sigscan.cpp
)

target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)

if (MSVC)
  target_compile_options(${TARGET} PRIVATE /Zc:preprocessor /Zc:__cplusplus)
endif()
