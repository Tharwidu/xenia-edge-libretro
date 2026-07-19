# Cross-compile for Windows x64 from Linux using clang-cl + lld-link against
# the MSVC CRT / Windows SDK fetched by xwin (winsdk/sdk: crt/ and sdk/).
#
# Usage:
#   cmake -B build-win -GNinja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-clang-cl-xwin.cmake \
#     -DXWIN_ROOT=/path/to/winsdk/sdk ...

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED XWIN_ROOT)
  set(XWIN_ROOT "$ENV{XWIN_ROOT}")
endif()
if(NOT XWIN_ROOT)
  message(FATAL_ERROR "Set XWIN_ROOT to the xwin splat output (containing crt/ and sdk/)")
endif()
# Propagate into try_compile scratch projects, which re-include this file
# without the command-line cache variables.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_ROOT)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER lld-link)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_ASM_MASM_COMPILER llvm-ml)

# MSVC/SDK headers. /imsvc = system include (no warnings).
foreach(inc crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt sdk/include/cppwinrt)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:/imsvc ${XWIN_ROOT}/${inc}>")
endforeach()

# Import libs. xwin --preserve-ms-arch-notation names the arch dirs x64.
add_link_options(
  "/libpath:${XWIN_ROOT}/crt/lib/x64"
  "/libpath:${XWIN_ROOT}/sdk/lib/um/x64"
  "/libpath:${XWIN_ROOT}/sdk/lib/ucrt/x64"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH "${XWIN_ROOT}")
