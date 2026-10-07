# Toolchain for the llvm-mingw cross compilers that setup-llvm-mingw.sh fetches.
# CMake's RC compiler detection only looks for a program named "rc", which the
# llvm-mingw toolchains do not provide, so the compiler has to be named here.
# Without this file a Windows target build fails at enable_language(RC) on a
# Linux host, where the windres fallback of Platform/Windows-GNU.cmake never
# runs because the host itself is not Windows.

set(CMAKE_SYSTEM_NAME Windows)

set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_RC_FLAGS "-C 65001")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
