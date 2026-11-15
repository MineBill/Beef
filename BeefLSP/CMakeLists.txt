cmake_minimum_required(VERSION 3.16)

project(beeflsp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This directory can be built on its own:
#   cmake -S BeefLSP -B build-lsp -G Ninja -DCMAKE_BUILD_TYPE=Release
# or as part of the root Beef project with -DBF_LSP=ON.
set(BEEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

# On Windows the compiler frontend is taken from the binaries the normal Beef
# build (bin/build.bat) produces in IDE/dist. That keeps this project to the
# language server itself and avoids re-porting the whole BeefySysLib CMake
# build. On Linux the CMake build of IDEHelper/BeefySysLib already works.
if (WIN32)
  option(BEEFLSP_USE_PREBUILT "Link the compiler from IDE/dist instead of building it" ON)
else()
  option(BEEFLSP_USE_PREBUILT "Link the compiler from IDE/dist instead of building it" OFF)
endif()

set(BEEFLSP_SRC
  src/main.cpp
  src/Log.cpp
  src/JsonRpc.cpp
  src/LspText.cpp
  src/Workspace.cpp
  src/BeefCompiler.cpp
  src/CompletionParser.cpp
  src/Server.cpp
)

add_executable(beeflsp ${BEEFLSP_SRC})
target_compile_features(beeflsp PRIVATE cxx_std_17)

target_include_directories(beeflsp PRIVATE
  "${BEEF_ROOT}"
  "${BEEF_ROOT}/BeefySysLib"
  "${BEEF_ROOT}/BeefySysLib/third_party"
  "${BEEF_ROOT}/IDEHelper"
  "${BEEF_ROOT}/IDEHelper/Compiler"
  "${BEEF_ROOT}/extern"
)

if (WIN32)
  target_include_directories(beeflsp PRIVATE "${BEEF_ROOT}/BeefySysLib/platform/win")
elseif (APPLE)
  target_include_directories(beeflsp PRIVATE "${BEEF_ROOT}/BeefySysLib/platform/osx")
else()
  target_include_directories(beeflsp PRIVATE "${BEEF_ROOT}/BeefySysLib/platform/linux")
endif()

target_compile_definitions(beeflsp PRIVATE
  UNICODE
  _UNICODE
  BF_NO_FBX
  _CRT_SECURE_NO_WARNINGS
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (BEEFLSP_USE_PREBUILT)
  # --- Windows: link the prebuilt IDEHelper DLL -----------------------------
  set(BEEF_DIST "${BEEF_ROOT}/IDE/dist")

  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(IDEHELPER_LIB "${BEEF_DIST}/IDEHelper64_d.lib")
    set(IDEHELPER_DLL "${BEEF_DIST}/IDEHelper64_d.dll")
  else()
    set(IDEHELPER_LIB "${BEEF_DIST}/IDEHelper64.lib")
    set(IDEHELPER_DLL "${BEEF_DIST}/IDEHelper64.dll")
  endif()
  set(BEEFY_STATIC_LIB "${BEEF_DIST}/BeefySysLib64_Static.lib")

  foreach(prebuilt "${IDEHELPER_LIB}" "${IDEHELPER_DLL}" "${BEEFY_STATIC_LIB}")
    if (NOT EXISTS "${prebuilt}")
      message(FATAL_ERROR
        "beeflsp needs '${prebuilt}'.\n"
        "Build the Beef repository first (bin/build.bat) so that IDE/dist is populated, "
        "or configure with -DBEEFLSP_USE_PREBUILT=OFF to compile the compiler from source.")
    endif()
  endforeach()

  # BfAst.h/BfIRBuilder.h pull in LLVM headers even though we only use the C API.
  set(LLVM_INCLUDE_HINTS
    "${BEEF_ROOT}/extern/llvm-project_22_1_5/llvm/include"
    "${BEEF_ROOT}/extern/llvm_win64_22_1_5/include"
    "${BEEF_ROOT}/extern/llvm_win64_22_1_5/lib/Target/X86"
  )
  foreach(llvmInclude ${LLVM_INCLUDE_HINTS})
    if (EXISTS "${llvmInclude}")
      target_include_directories(beeflsp PRIVATE "${llvmInclude}")
    endif()
  endforeach()

  target_compile_options(beeflsp PRIVATE /W3 /EHsc /bigobj /wd4100 /wd4244 /wd4267)
  # BeefySysLib64_Static.lib is built with the static CRT (/MT), so this project
  # has to match it.
  set_property(TARGET beeflsp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

  target_link_libraries(beeflsp PRIVATE
    "${IDEHELPER_LIB}"
    "${BEEFY_STATIC_LIB}"
    kernel32 user32 gdi32 winspool comdlg32 advapi32 shell32 ole32 oleaut32 uuid odbc32 odbccp32
    ntdll rpcrt4 cabinet winmm wininet
  )

  add_custom_command(TARGET beeflsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${IDEHELPER_DLL}" "$<TARGET_FILE_DIR:beeflsp>"
    COMMENT "Copying IDEHelper DLL next to beeflsp"
    VERBATIM)

else()
  # --- Linux/macOS: build the compiler frontend from source -----------------
  if (NOT TARGET IDEHelper)
    if (NOT TARGET BeefySysLib)
      add_subdirectory("${BEEF_ROOT}/BeefySysLib" "${CMAKE_CURRENT_BINARY_DIR}/BeefySysLib")
    endif()
    if (NOT TARGET hunspell)
      add_subdirectory("${BEEF_ROOT}/extern/hunspell" "${CMAKE_CURRENT_BINARY_DIR}/hunspell")
    endif()
    add_subdirectory("${BEEF_ROOT}/IDEHelper" "${CMAKE_CURRENT_BINARY_DIR}/IDEHelper")
  endif()

  find_package(LLVM 22.1 CONFIG REQUIRED)
  message(STATUS "beeflsp: using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

  target_include_directories(beeflsp PRIVATE ${LLVM_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})

  if (LLVM_LIBRARY_DIRS)
    target_link_directories(beeflsp PRIVATE ${LLVM_LIBRARY_DIRS})
  endif()

  target_link_libraries(beeflsp PRIVATE
    "$<TARGET_FILE:IDEHelper>"
    "$<TARGET_FILE:BeefySysLib>"
    LLVM-22 ${LLVM_SYSTEM_LIBS}
    pthread dl ${CMAKE_DL_LIBS}
  )
  add_dependencies(beeflsp IDEHelper BeefySysLib)
endif()

set_target_properties(beeflsp PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
