include(${CMAKE_SOURCE_DIR}/CMakeSettings.txt)
include(${CMAKE_SOURCE_DIR}/CMakeIncludes.txt)

file(GLOB_RECURSE
    sources
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cc
)

add_library(${project_name} STATIC ${sources})
