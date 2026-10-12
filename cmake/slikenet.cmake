# Builds SLikeNet's core as a static library target `slikenet`.
#
# Upstream's CMakeLists hard-requires OpenSSL for features we don't use, so
# the sources are compiled directly instead of via add_subdirectory().

include_guard(GLOBAL)
include(FetchContent)

FetchContent_Declare(
    slikenet
    GIT_REPOSITORY https://github.com/SLikeSoft/SLikeNet.git
    GIT_TAG d5f775d789563a2d505e2afbf99a550d990bb49e
    SOURCE_SUBDIR do-not-add-subdirectory
)
FetchContent_MakeAvailable(slikenet)

# Pinned tag: the source list never changes between builds, so no re-glob.
file(GLOB SLIKENET_SOURCES "${slikenet_SOURCE_DIR}/Source/src/*.cpp")
# Unused plugin; its DR_SHA1.h #defines TCHAR ahead of winnt.h and breaks mingw.
list(FILTER SLIKENET_SOURCES EXCLUDE REGEX "/TwoWayAuthentication\\.cpp$")
add_library(slikenet STATIC ${SLIKENET_SOURCES})
target_include_directories(slikenet SYSTEM PUBLIC "${slikenet_SOURCE_DIR}/Source/include")
set_target_properties(slikenet PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(slikenet PRIVATE /w)
    target_compile_definitions(slikenet PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    target_compile_options(slikenet PRIVATE -w)
endif()

if(MINGW AND CMAKE_CROSSCOMPILING)
    # SLikeNet includes <IPHlpApi.h>; mingw ships iphlpapi.h, which only a
    # case-insensitive filesystem forgives.
    set(SLIKENET_SHIM_DIR "${CMAKE_BINARY_DIR}/slikenet-case-shim")
    file(WRITE "${SLIKENET_SHIM_DIR}/IPHlpApi.h" "#include_next <iphlpapi.h>\n")
    target_include_directories(slikenet SYSTEM PUBLIC "${SLIKENET_SHIM_DIR}")
endif()

if(WIN32)
    target_link_libraries(slikenet PUBLIC ws2_32 iphlpapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(slikenet PUBLIC Threads::Threads)
endif()
