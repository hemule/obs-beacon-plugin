# patch-qt-agl.cmake
#
# Durable workaround for the legacy AGL framework, which was removed from the
# macOS 26 (Tahoe) SDK. The prebuilt Qt6 in obs-deps still references
# "-framework AGL" in two places, which breaks linking on newer SDKs:
#
#   1. lib/cmake/Qt6/FindWrapOpenGL.cmake       (WrapOpenGL imported target)
#   2. lib/*.framework/.../*.prl                 (per-module link interface)
#
# AGL is not needed by modern Qt/OpenGL apps, so we strip both references from
# the fetched dependency tree. This runs at configure time (before
# find_package(Qt6)) and is idempotent, so it survives `rm -rf .deps`, a fresh
# clone, or CI — unlike a manual edit of the gitignored .deps tree.
#
# See the project memory "macos-sdk26-build-workarounds" for background.

include_guard(GLOBAL)

if(NOT APPLE)
  return()
endif()

file(GLOB _qt6_dep_dirs "${CMAKE_SOURCE_DIR}/.deps/obs-deps-qt6-*")
if(NOT _qt6_dep_dirs)
  message(STATUS "patch-qt-agl: no obs-deps qt6 directory yet; skipping")
  return()
endif()

set(_patched_prl 0)

foreach(_qt6_dir IN LISTS _qt6_dep_dirs)
  # 1. FindWrapOpenGL.cmake — neutralise the AGL interface link.
  set(_wrapgl "${_qt6_dir}/lib/cmake/Qt6/FindWrapOpenGL.cmake")
  if(EXISTS "${_wrapgl}")
    file(READ "${_wrapgl}" _content)
    string(
      REPLACE
      "target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE \${__opengl_agl_fw_path})"
      "# AGL interface link removed by patch-qt-agl.cmake (legacy framework, absent on macOS 26 SDK)"
      _content
      "${_content}"
    )
    # Also neutralise the hardcoded fallback so WrapOpenGL_AGL can't reintroduce it.
    string(
      REPLACE
      "set(__opengl_agl_fw_path \"-framework AGL\")"
      "set(__opengl_agl_fw_path \"\")"
      _content
      "${_content}"
    )
    file(READ "${_wrapgl}" _orig)
    if(NOT _content STREQUAL _orig)
      file(WRITE "${_wrapgl}" "${_content}")
      message(STATUS "patch-qt-agl: patched FindWrapOpenGL.cmake in ${_qt6_dir}")
    endif()
  endif()

  # 2. Qt module .prl files — strip "-framework AGL" from the link interface.
  file(GLOB_RECURSE _prl_files "${_qt6_dir}/lib/*.prl")
  foreach(_prl IN LISTS _prl_files)
    file(READ "${_prl}" _prl_content)
    if(_prl_content MATCHES "AGL")
      set(_new "${_prl_content}")
      string(REPLACE "-framework AGL;" "" _new "${_new}")
      string(REPLACE "-framework AGL " "" _new "${_new}")
      if(NOT _new STREQUAL _prl_content)
        file(WRITE "${_prl}" "${_new}")
        math(EXPR _patched_prl "${_patched_prl}+1")
      endif()
    endif()
  endforeach()
endforeach()

if(_patched_prl GREATER 0)
  message(STATUS "patch-qt-agl: stripped -framework AGL from ${_patched_prl} Qt .prl file(s)")
endif()
