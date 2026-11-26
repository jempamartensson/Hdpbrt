# hdPbrt: Hydra render delegate for pbrt-v4.
#
# Two USD flavors (choose with -DHDPBRT_FLAVOR=houdini|blender):
#   houdini: headers and libs from a Houdini install (-DHFS=/opt/hfs22.0.508)
#   blender: Blender's monolithic libusd_ms.so (-DBLENDER_LIB=<blender>/lib),
#            with headers from the matching OpenUSD source tree
#            (-DUSD_SOURCE=..., -DUSD_GENERATED=<dir with pxr/pxr.h>),
#            oneTBB headers (-DTBB_INCLUDE=...) and Python 3.13 headers
#            (-DPYTHON_INCLUDE=...).
cmake_minimum_required(VERSION 3.16)
project(hdPbrt CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(HDPBRT_FLAVOR "houdini" CACHE STRING "houdini or blender")

add_library(hdPbrt SHARED
  hdPbrt/session.cpp
  hdPbrt/renderBuffer.cpp
  hdPbrt/renderDelegate.cpp
  hdPbrt/renderPass.cpp
  hdPbrt/rendererPlugin.cpp
  hdPbrt/geom.cpp
  hdPbrt/mesh.cpp
  hdPbrt/curves.cpp
  hdPbrt/points.cpp
  hdPbrt/implicit.cpp
  hdPbrt/subdiv.cpp
  hdPbrt/volume.cpp
  hdPbrt/camera.cpp
  hdPbrt/ies.cpp
  third_party/cycles_ies/ies.cpp
  hdPbrt/material.cpp
  hdPbrt/light.cpp
  hdPbrt/instancer.cpp
  hdPbrt/sceneIndexPlugin.cpp)
set_target_properties(hdPbrt PROPERTIES PREFIX "")
target_compile_definitions(hdPbrt PRIVATE TBB_SUPPRESS_DEPRECATED_MESSAGES=1 NDEBUG)
target_compile_options(hdPbrt PRIVATE -Wno-deprecated -Wno-deprecated-declarations)

if(HDPBRT_FLAVOR STREQUAL "houdini")
  set(HFS "/opt/hfs22.0.508" CACHE PATH "Houdini install")
  # The HDK's VERSION define (houdiniImage.cpp): from the install's sys header.
  file(STRINGS ${HFS}/toolkit/include/SYS/SYS_Version.h _hv REGEX "define SYS_VERSION_FULL ")
  string(REGEX REPLACE ".*\"(.*)\".*" "\\1" HDPBRT_HOUDINI_VERSION "${_hv}")
  target_link_options(hdPbrt PRIVATE -Wl,--no-undefined)
  target_compile_definitions(hdPbrt PRIVATE HDPBRT_HOUDINI=1)
  target_include_directories(hdPbrt SYSTEM PRIVATE
    ${HFS}/toolkit/include ${HFS}/python/include/python3.13)
  set(_libs hdsi hd hf geomUtil pxOsd cameraUtil sdf ar plug trace work vt gf tf arch js python)
  foreach(l ${_libs})
    target_link_libraries(hdPbrt PRIVATE ${HFS}/dsolib/libpxr_${l}.so)
  endforeach()
  target_link_libraries(hdPbrt PRIVATE ${HFS}/dsolib/libtbb.so ${HFS}/python/lib/libpython3.13.so
    ${HFS}/dsolib/libosdCPU.so)
  set_target_properties(hdPbrt PROPERTIES INSTALL_RPATH "${HFS}/dsolib" BUILD_RPATH "${HFS}/dsolib")
  # op: images (COP nodes) through the HDK's TIL_CopResolver. Only this file
  # sees the HDK's defines (from `hcustom -c`); it includes no USD headers.
  target_sources(hdPbrt PRIVATE hdPbrt/houdiniImage.cpp)
  set_source_files_properties(hdPbrt/houdiniImage.cpp PROPERTIES COMPILE_DEFINITIONS
    "VERSION=\"${HDPBRT_HOUDINI_VERSION}\";_GNU_SOURCE;LINUX;AMD64;SIZEOF_VOID_P=8;SESI_LITTLE_ENDIAN;HBOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT=1;HBOOST_BIND_GLOBAL_PLACEHOLDERS=1;ENABLE_THREADS;USE_PTHREADS;_REENTRANT;_FILE_OFFSET_BITS=64;OPENCL_ENABLED=1;OPENVDB_ENABLED=1"
    COMPILE_OPTIONS "-faligned-new")
  target_link_libraries(hdPbrt PRIVATE ${HFS}/dsolib/libHoudiniUT.so)
elseif(HDPBRT_FLAVOR STREQUAL "blender")
  set(BLENDER_LIB "/opt/blender-5.3.0-linux-x64/lib" CACHE PATH "Blender lib dir")
  set(USD_SOURCE "" CACHE PATH "OpenUSD source tree matching Blender's USD")
  set(USD_GENERATED "" CACHE PATH "Directory containing the generated pxr/pxr.h")
  set(TBB_INCLUDE "" CACHE PATH "oneTBB include directory")
  set(PYTHON_INCLUDE "" CACHE PATH "Python 3.13 include directory")
  # OpenSubdiv headers matching Blender's libosdCPU (3.7): a directory that
  # contains opensubdiv/ (e.g. a link to Houdini's toolkit/include/opensubdiv).
  set(OSD_INCLUDE "" CACHE PATH "Directory containing opensubdiv/far/...")
  target_include_directories(hdPbrt SYSTEM BEFORE PRIVATE
    ${USD_GENERATED} ${USD_SOURCE} ${TBB_INCLUDE} ${PYTHON_INCLUDE} ${OSD_INCLUDE})
  target_link_libraries(hdPbrt PRIVATE ${BLENDER_LIB}/libusd_ms.so ${BLENDER_LIB}/libtbb.so
    ${BLENDER_LIB}/libosdCPU.so)
  # Python symbols come from the Blender executable at run time.
else()
  message(FATAL_ERROR "HDPBRT_FLAVOR must be houdini or blender")
endif()

# hdpbrt_vdb2nvdb: OpenVDB -> NanoVDB for volumes (Houdini flavor; it's a
# standalone tool, so the Blender flavor can use the same binary). Built with
# pbrt's own NanoVDB headers (-DPBRT_SOURCE=<pbrt-v4 source>) so the file
# version matches pbrt's reader; pbrt's OpenToNanoVDB.h predates OpenVDB 11's
# 64-bit nodeCount(), so a fixed copy is generated.
set(PBRT_SOURCE "" CACHE PATH "pbrt-v4 source tree (for NanoVDB headers)")
if(HDPBRT_FLAVOR STREQUAL "houdini" AND PBRT_SOURCE AND EXISTS ${HFS}/dsolib/libopenvdb_sesi.so)
  set(_nano ${PBRT_SOURCE}/src/ext/openvdb/nanovdb)
  file(READ ${_nano}/nanovdb/util/OpenToNanoVDB.h _o2n)
  string(REPLACE "std::vector<uint32_t> nodeCount = openTree.nodeCount();"
                 "auto nodeCount = openTree.nodeCount();" _o2n "${_o2n}")
  string(REGEX REPLACE "#include \"([A-Za-z]+\\.h)\"" "#include <nanovdb/util/\\1>" _o2n "${_o2n}")
  file(WRITE ${CMAKE_BINARY_DIR}/nanovdb-compat/nanovdb/util/OpenToNanoVDB.h "${_o2n}")
  add_executable(hdpbrt_vdb2nvdb tools/vdb2nvdb.cpp)
  target_include_directories(hdpbrt_vdb2nvdb SYSTEM BEFORE PRIVATE
    ${CMAKE_BINARY_DIR}/nanovdb-compat ${_nano} ${HFS}/toolkit/include)
  target_compile_options(hdpbrt_vdb2nvdb PRIVATE -Wno-deprecated -Wno-deprecated-declarations)
  target_link_libraries(hdpbrt_vdb2nvdb PRIVATE ${HFS}/dsolib/libopenvdb_sesi.so ${HFS}/dsolib/libtbb.so)
  set_target_properties(hdpbrt_vdb2nvdb PROPERTIES INSTALL_RPATH "${HFS}/dsolib" BUILD_RPATH "${HFS}/dsolib")
  install(TARGETS hdpbrt_vdb2nvdb DESTINATION .)
endif()

configure_file(plugInfo.json.in ${CMAKE_BINARY_DIR}/plugInfo.json @ONLY)

install(TARGETS hdPbrt LIBRARY DESTINATION .)
install(FILES ${CMAKE_BINARY_DIR}/plugInfo.json DESTINATION .)
