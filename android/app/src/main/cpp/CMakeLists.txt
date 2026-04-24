cmake_minimum_required(VERSION 3.22.1)
project(tpv_jni LANGUAGES C)

# Import the pre-built libtpv.so placed under jniLibs/arm64-v8a by
# `make android-so`. IMPORTED means CMake does not try to build it —
# only links against it.
add_library(tpv SHARED IMPORTED)
# IMPORTED_SONAME forces libtpv_jni.so's DT_NEEDED to the short name
# `libtpv.so`, so the Android linker resolves it under the APK's
# /data/app/.../lib/arm64/ at runtime. Without it the linker writes
# IMPORTED_LOCATION's absolute build-machine path into DT_NEEDED, and
# dlopen fails on device with "library ... not found".
set_target_properties(tpv PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libtpv.so
    IMPORTED_SONAME   libtpv.so)

# Path from this CMakeLists.txt to the repo root:
#   android/ (1) → app/ (2) → src/ (3) → main/ (4) → cpp/ (5)
# so five ".." hops.
set(TPV_REPO_ROOT ${CMAKE_SOURCE_DIR}/../../../../..)

add_library(tpv_jni SHARED tpv_jni.c)
target_include_directories(tpv_jni PRIVATE ${TPV_REPO_ROOT}/include)
target_compile_definitions(tpv_jni PRIVATE TPV_DEBUG_FEATURES)
target_link_libraries(tpv_jni PRIVATE tpv log)
