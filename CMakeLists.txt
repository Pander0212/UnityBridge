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
            tests/testdll/other.def tests/testdll/shell32.def tests/testdll/baselib.def tests/testdll/ntdll.def
            tests/testdll/build.sh)
  add_custom_target(ubtest-dll ALL DEPENDS ${UBTEST_DIR}/ubtest.dll)
  add_test(NAME win32-host-test COMMAND win32-host-test ${UBTEST_DIR}/ubtest.dll)

  # The il2cpp bridge (M3): ARM64 thunks for il2cpp_*, here driving native ms_abi functions.
  add_executable(il2cpp-bridge-test tests/host/il2cpp_bridge_test.cpp engine/il2cpp_bridge.cpp)
  target_include_directories(il2cpp-bridge-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(il2cpp-bridge-test PRIVATE -Wall -Wextra)
  add_test(NAME il2cpp-bridge-test COMMAND il2cpp-bridge-test)

  # The reverse (x64 -> native) il2cpp InternalCall thunks (M3b): here calling System V functions.
  add_executable(icall-bridge-test tests/host/icall_bridge_test.cpp engine/icall_bridge.cpp
                 engine/call_native_sysv.S win32/host_entry_x64.S)
  target_link_libraries(icall-bridge-test PRIVATE ub_win32)
  target_include_directories(icall-bridge-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(icall-bridge-test PRIVATE -Wall -Wextra)
  add_test(NAME icall-bridge-test COMMAND icall-bridge-test)

  # The intercepted host-callback il2cpp_* APIs (M3c): InternalCall signatures from fake metadata.
  add_executable(il2cpp-intercept-test tests/host/il2cpp_intercept_test.cpp engine/il2cpp_intercept.cpp
                 engine/icall_bridge.cpp engine/call_native_sysv.S win32/host_entry_x64.S)
  target_link_libraries(il2cpp-intercept-test PRIVATE ub_win32)
  target_include_directories(il2cpp-intercept-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(il2cpp-intercept-test PRIVATE -Wall -Wextra)
  add_test(NAME il2cpp-intercept-test COMMAND il2cpp-intercept-test)
endif()

# M4: HLSLcc (fetched by scripts/fetch-hlslcc.sh into build/hlslcc, MIT) and the D3D11 -> GLSL ES 3.0
# translation around it; ub-shader-xlat runs it on a case exported from Unity's own shaders.
set(HLSLCC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build/hlslcc)
if(EXISTS ${HLSLCC_DIR}/src/HLSLcc.cpp)
  file(GLOB HLSLCC_SOURCES ${HLSLCC_DIR}/src/*.cpp ${HLSLCC_DIR}/src/cbstring/*.c)
  add_library(hlslcc STATIC ${HLSLCC_SOURCES})
  target_include_directories(hlslcc SYSTEM PUBLIC ${HLSLCC_DIR} ${HLSLCC_DIR}/include
                             PRIVATE ${HLSLCC_DIR}/src ${HLSLCC_DIR}/src/cbstring ${HLSLCC_DIR}/src/internal_includes)
  target_compile_options(hlslcc PRIVATE -w)  # third-party code, built unmodified
  set_target_properties(hlslcc PROPERTIES CXX_STANDARD 11)

  add_executable(ub-shader-xlat tools/shader_xlat/main.cpp engine/shader_xlat.cpp)
  target_link_libraries(ub-shader-xlat PRIVATE hlslcc)
  target_include_directories(ub-shader-xlat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(ub-shader-xlat PRIVATE -Wall -Wextra)
  set_target_properties(ub-shader-xlat PROPERTIES CXX_STANDARD 17)

  # M4c: the whole conversion of a serialized file, as the device runs it.
  add_executable(ub-shader-convert tools/shader_convert/main.cpp engine/shader_gles3.cpp engine/shader_xlat.cpp
                 engine/unity_file.cpp engine/unity_typetree.cpp engine/shader_import.cpp engine/unity_textures.cpp)
  target_link_libraries(ub-shader-convert PRIVATE hlslcc)
  target_include_directories(ub-shader-convert PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(ub-shader-convert PRIVATE -Wall -Wextra)
  set_target_properties(ub-shader-convert PROPERTIES CXX_STANDARD 17)

  add_executable(unity-assets-test tests/host/unity_assets_test.cpp engine/shader_gles3.cpp engine/shader_xlat.cpp
                 engine/unity_file.cpp engine/unity_typetree.cpp)
  target_link_libraries(unity-assets-test PRIVATE hlslcc)
  target_include_directories(unity-assets-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(unity-assets-test PRIVATE -Wall -Wextra)
  set_target_properties(unity-assets-test PROPERTIES CXX_STANDARD 17)
  add_test(NAME unity-assets-test COMMAND unity-assets-test)
endif()
