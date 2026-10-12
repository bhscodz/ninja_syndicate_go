cmake_minimum_required(VERSION 3.28)
project(SFMLGAME LANGUAGES CXX)    
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(FetchContent)
FetchContent_Declare(
    SFML
    GIT_REPOSITORY https://github.com/SFML/SFML.git
    GIT_TAG 3.1.0
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(SFML)
add_executable(sfml_app src/main.cpp)
target_compile_features(sfml_app PRIVATE cxx_std_17)
target_link_libraries(sfml_app PRIVATE SFML::Graphics)
