# Cross-build toolchain for the MA35D1 / Ubuntu 22.04 target.
#
# The compiler is intentionally selected by name instead of downloading
# anything during configuration. Install the matching distro toolchain on the
# development or CI host before configuring this preset.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Keep target dependency discovery inside the cross sysroot. In particular,
# never let find_package(OpenSSL) silently link the host x86_64 library.
set(CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu")

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc CACHE FILEPATH
        "AArch64 C compiler")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++ CACHE FILEPATH
        "AArch64 C++ compiler")
endif()
if(NOT DEFINED CMAKE_AR)
    set(CMAKE_AR aarch64-linux-gnu-ar CACHE FILEPATH "AArch64 archiver")
endif()
if(NOT DEFINED CMAKE_RANLIB)
    set(CMAKE_RANLIB aarch64-linux-gnu-ranlib CACHE FILEPATH
        "AArch64 archive indexer")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
