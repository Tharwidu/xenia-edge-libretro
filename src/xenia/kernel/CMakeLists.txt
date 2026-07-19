add_library(xenia-kernel STATIC)
xe_platform_sources(xenia-kernel ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
# The xam UI implementations open real dialogs; the libretro build swaps them
# for stubs. The *_stub.cc files live in the tree, so the config not using
# them must exclude them from the recursive glob.
get_target_property(_kernel_srcs xenia-kernel SOURCES)
if(XENIA_BUILD_LIBRETRO)
  list(FILTER _kernel_srcs EXCLUDE REGEX "xam/(xam_ui|xam_nui)\\.(cc|h)$")
  list(FILTER _kernel_srcs EXCLUDE REGEX "xam/ui/")
  list(FILTER _kernel_srcs EXCLUDE REGEX "_stub\\.cc$")
  set_target_properties(xenia-kernel PROPERTIES SOURCES "${_kernel_srcs}")
  target_sources(xenia-kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/xam/xam_ui_stub.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/xam/xam_nui_stub.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/xam/ui/disc_swap_ui_stub.cc
  )
else()
  list(FILTER _kernel_srcs EXCLUDE REGEX "_stub\\.cc$")
  set_target_properties(xenia-kernel PROPERTIES SOURCES "${_kernel_srcs}")
endif()
target_compile_definitions(xenia-kernel PRIVATE WITH_GZFILEOP)
if(XE_TARGET_X86_64)
  target_compile_definitions(xenia-kernel PRIVATE
    X86_FEATURES
    X86_HAVE_XSAVE_INTRIN
    X86_SSE2
    X86_SSSE3
    X86_SSE42
    X86_AVX2
    X86_AVX512
    X86_AVX512VNNI
    X86_PCLMULQDQ_CRC
    X86_VPCLMULQDQ_CRC
    _WINSOCK_DEPRECATED_NO_WARNINGS
  )
endif()
target_link_libraries(xenia-kernel PUBLIC
  aes_128 asio fmt zlib-ng pugixml xenia-apu xenia-base xenia-cpu xenia-hid xenia-vfs
)
xe_target_defaults(xenia-kernel)

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
