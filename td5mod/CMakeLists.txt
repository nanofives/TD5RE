cmake_minimum_required(VERSION 3.15)
project(td5mod C)

# Force 32-bit build
if(MSVC)
    # MSVC: set via -A Win32 on cmake command line
elseif(MINGW)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
endif()

# MinHook (fetch automatically)
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# Our mod DLL
add_library(td5mod SHARED
    src/td5_mod.c
)

target_include_directories(td5mod PRIVATE
    src
    ${minhook_SOURCE_DIR}/include
)

target_link_libraries(td5mod PRIVATE minhook)

# Output as .asi instead of .dll
set_target_properties(td5mod PROPERTIES
    SUFFIX ".asi"
    PREFIX ""
    OUTPUT_NAME "td5_mod"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/scripts"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/scripts"
)

# Strip release builds
if(MINGW)
    target_link_options(td5mod PRIVATE -s)
endif()
