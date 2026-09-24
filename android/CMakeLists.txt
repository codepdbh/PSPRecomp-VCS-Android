cmake_minimum_required(VERSION 3.22.1)
project(VCSAndroid LANGUAGES CXX)

set(PSPRECOMP_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(PSPRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PSPRECOMP_BUILD_PROFILE_TESTS OFF CACHE BOOL "" FORCE)
set(PSPRECOMP_PROFILE "vcs" CACHE STRING "" FORCE)
set(PSPRECOMP_NATIVE_AVX2 OFF CACHE BOOL "" FORCE)
set(PSPRECOMP_HOST_AVX OFF CACHE BOOL "" FORCE)

add_subdirectory("${PSPRECOMP_REPO_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/psprecomp")

add_library(vcs_android SHARED
    app/src/main/cpp/android_game_runtime.cpp)
target_include_directories(vcs_android PRIVATE
    "${PSPRECOMP_REPO_DIR}/include"
    "${PSPRECOMP_REPO_DIR}/profiles/vcs/host")
target_link_libraries(vcs_android PRIVATE vcs_guest_core psprecomp_core android log)
