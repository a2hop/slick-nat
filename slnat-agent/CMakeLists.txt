cmake_minimum_required(VERSION 3.10)
project(SlickNatAgent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use nlohmann/json header-only library
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Daemon executable
add_executable(slick-nat-daemon
    net-agent/slnat-daemon.cpp
)

# Network client executable
add_executable(slnatc
    net-client/slnatc.cpp
)

# Link libraries
target_link_libraries(slick-nat-daemon nlohmann_json::nlohmann_json)
target_link_libraries(slnatc nlohmann_json::nlohmann_json)

# Add pthread for threading support
find_package(Threads REQUIRED)
target_link_libraries(slick-nat-daemon Threads::Threads)
target_link_libraries(slnatc Threads::Threads)

# Set build type specific flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(slick-nat-daemon PRIVATE -g -O0 -Wall -Wextra)
    target_compile_options(slnatc PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(slick-nat-daemon PRIVATE -O2 -DNDEBUG)
    target_compile_options(slnatc PRIVATE -O2 -DNDEBUG)
endif()

# Install targets
install(TARGETS slick-nat-daemon slnatc
    RUNTIME DESTINATION bin
)
