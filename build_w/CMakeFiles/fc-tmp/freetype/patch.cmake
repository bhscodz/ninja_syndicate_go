cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

message(VERBOSE "Executing patch step for freetype")

block(SCOPE_FOR VARIABLES)

execute_process(
  WORKING_DIRECTORY "D:/cloud_computing_game/build_w/_deps/freetype-src"
  COMMAND_ERROR_IS_FATAL LAST
  COMMAND  [====[C:/Program Files/CMake/bin/cmake.exe]====] [====[-DFREETYPE_DIR=D:/cloud_computing_game/build_w/_deps/freetype-src]====] [====[-P]====] [====[D:/cloud_computing_game/build/_deps/sfml-src/tools/freetype/PatchFreetype.cmake]====]
)

endblock()
