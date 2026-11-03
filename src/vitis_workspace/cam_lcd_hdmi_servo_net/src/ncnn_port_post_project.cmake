# Runs at the end of project() (via CMAKE_PROJECT_INCLUDE, set in UserConfig.cmake),
# before the generated CMakeLists.txt re-injects implicit include directories as
# -isystem. We strip the toolchain libstdc++ include directories from
# CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES so that -nostdinc++ stays effective:
# ncnn is built with NCNN_SIMPLESTL which provides operator new/delete itself,
# and mixing in libstdc++ headers breaks the build.
set(_ncnn_filtered_implicit_inc "")
foreach(_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    if(_dir MATCHES "c\\+\\+")
        # skip the libstdc++ header directory
    else()
        list(APPEND _ncnn_filtered_implicit_inc "${_dir}")
    endif()
endforeach()
set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES ${_ncnn_filtered_implicit_inc})
