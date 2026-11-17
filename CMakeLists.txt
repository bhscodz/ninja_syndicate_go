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

# --- Authoritative server (net core: coordinates, status, chat) ---
add_executable(game_server src/server/ServerMain.cpp)
target_compile_features(game_server PRIVATE cxx_std_17)
target_include_directories(game_server PRIVATE src)
target_link_libraries(game_server PRIVATE SFML::Network SFML::System)

# --- Client (rendering, input, local network thread) ---
add_executable(game_client src/client/ClientMain.cpp)
target_compile_features(game_client PRIVATE cxx_std_17)
target_include_directories(game_client PRIVATE src)
target_link_libraries(game_client PRIVATE SFML::Graphics SFML::Window SFML::Audio SFML::System SFML::Network)

# MinGW: link the C++ runtime statically so the exes run without MSYS2 on PATH
if(MINGW)
    target_link_options(game_server PRIVATE -static)
    target_link_options(game_client PRIVATE -static)
endif()

# Encrypted channel (src/game/SecureChannel.hpp): X25519 + HKDF + ChaCha20-Poly1305 from SFML's bundled mbedTLS
target_link_libraries(game_server PRIVATE mbedcrypto)
target_link_libraries(game_client PRIVATE mbedcrypto)
if(WIN32)
    target_link_libraries(game_server PRIVATE bcrypt)   # BCryptGenRandom (OS CSPRNG)
    target_link_libraries(game_client PRIVATE bcrypt)
endif()
