
CMAKE_MINIMUM_REQUIRED( VERSION 3.10.1 )

project(dngsdk)
set(CMAKE_CXX_STANDARD 20)
set(JPEG_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libjpeg)
set(JPEG_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/libjpeg)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    # Set the possible values of build type for cmake-gui
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(BUILD_SHARED_LIBS FALSE)

ADD_SUBDIRECTORY(libjpeg)
ADD_SUBDIRECTORY(libjxl)
ADD_SUBDIRECTORY(xmp)
ADD_SUBDIRECTORY(dng_sdk)

If (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "iOS")
    find_package(LibXml2 REQUIRED)

    add_executable(dcptool ${CMAKE_CURRENT_SOURCE_DIR}/dcptool.cpp
             ${CMAKE_CURRENT_SOURCE_DIR}/xml_camera_profile.cpp)
    # Link against the library the child built
    target_link_libraries(dcptool PRIVATE dngsdk expat LibXml2::LibXml2)

    add_executable(cube2xmp ${CMAKE_CURRENT_SOURCE_DIR}/cube2xmp.cpp)
    # Link against the library the child built
    target_link_libraries(cube2xmp PRIVATE dngsdk expat)
endif()