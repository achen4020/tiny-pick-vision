cmake_minimum_required(VERSION 3.22.1)
project(tpv_jni LANGUAGES C)

# Import the pre-built libtpv.so placed under jniLibs/arm64-v8a by
# `make android-so`. IMPORTED means CMake does not try to build it —
# only links against it.
add_library(tpv SHARED IMPORTED)
set_target_properties(tpv PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libtpv.so)

# Path from this CMakeLists.txt to the repo root:
#   android/ (1) → app/ (2) → src/ (3) → main/ (4) → cpp/ (5)
# so five ".." hops.
set(TPV_REPO_ROOT ${CMAKE_SOURCE_DIR}/../../../../..)

add_library(tpv_jni SHARED tpv_jni.c)
target_include_directories(tpv_jni PRIVATE ${TPV_REPO_ROOT}/include)
target_compile_definitions(tpv_jni PRIVATE TPV_DEBUG_FEATURES)
target_link_libraries(tpv_jni PRIVATE tpv log)
