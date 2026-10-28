# Host build (Linux/macOS): the PE loader and the mini Win32 as a library, the inspect tool and,
# on x86-64 Linux, the native test that runs tests/testdll/ubtest.dll. The iPhone build of the
# engine goes through scripts/build-fexcore-ios.sh instead.
cmake_minimum_required(VERSION 3.20)
project(UnityBridge LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ub_win32 STATIC
  loader/pe_image.cpp
  win32/hle.cpp
  win32/support.cpp
  win32/kernel32.cpp
  win32/other_dlls.cpp
  win32/baselib.cpp
  win32/kernel32_sync.cpp
  win32/kernel32_files.cpp
  win32/kernel32_misc.cpp
  win32/tls.cpp
  win32/ntdll_unwind.cpp
  win32/ntdll_exception.cpp)
target_include_directories(ub_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ub_win32 PRIVATE -Wall -Wextra)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  target_compile_options(ub_win32 PRIVATE -mcx16)  # 128-bit compare-and-swap (InterlockedFlushSList)
endif()

add_executable(ub-pe-inspect tools/pe_inspect.cpp)
target_link_libraries(ub-pe-inspect PRIVATE ub_win32)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  enable_testing()
  add_executable(win32-host-test tests/host/win32_host_test.cpp win32/host_entry_x64.S)
  target_link_libraries(win32-host-test PRIVATE ub_win32)
  set(UBTEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/testdll)
  add_custom_command(
    OUTPUT ${UBTEST_DIR}/ubtest.dll
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdll/build.sh ${UBTEST_DIR}
    DEPENDS tests/testdll/ubtest.c tests/testdll/kernel32.def tests/testdll/oleaut32.def
            tests/testdll/other.def tests/testdll/shell32.def tests/testdll/baselib.def
            tests/testdll/build.sh)
  add_custom_target(ubtest-dll ALL DEPENDS ${UBTEST_DIR}/ubtest.dll)
  add_test(NAME win32-host-test COMMAND win32-host-test ${UBTEST_DIR}/ubtest.dll)

  # The il2cpp bridge (M3): ARM64 thunks for il2cpp_*, here driving native ms_abi functions.
  add_executable(il2cpp-bridge-test tests/host/il2cpp_bridge_test.cpp engine/il2cpp_bridge.cpp)
  target_include_directories(il2cpp-bridge-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(il2cpp-bridge-test PRIVATE -Wall -Wextra)
  add_test(NAME il2cpp-bridge-test COMMAND il2cpp-bridge-test)
endif()
