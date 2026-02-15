cmake_minimum_required(VERSION 3.25.0)

project(XShare)

include(ExternalProject)

find_program(MAKE_EXE make)

ExternalProject_Add(
    Lua 
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lua
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${MAKE_EXE} CFLAGS="-fPIC" a
    BUILD_IN_SOURCE TRUE
    INSTALL_COMMAND ""
)

add_library(XShare SHARED)

target_sources(XShare
    PRIVATE
        src/XShare.c
        src/shared_table.c
        src/GC.c
        src/stored_object.c
    PUBLIC
        FILE_SET HEADERS
        TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src 
        FILES src/shared_table.h src/GC.h src/stored_object.h
)

target_include_directories(XShare PRIVATE lua)
target_link_directories(XShare PRIVATE lua)

add_dependencies(XShare Lua)
target_link_libraries(XShare PRIVATE lua m)

if(BUILD_STATIC_LIB)
    set(STATIC_LIB_NAME ${PROJECT_NAME}-static)    #设置静态库的原始名称

    add_library(${STATIC_LIB_NAME} STATIC $<TARGET_OBJECTS:${PROJECT_NAME}>)
    set_target_properties(${STATIC_LIB_NAME} PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
    
    target_link_libraries(${STATIC_LIB_NAME} PRIVATE lua m)
endif()