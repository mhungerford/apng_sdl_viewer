project(PNG_SDLVIEWER)
cmake_minimum_required(VERSION 2.6)

find_package(SDL REQUIRED)
include_directories(${SDL_INCLUDE_DIR})

include_directories(.)

add_executable(png_player
  main_png.c
  upng.c
)

target_link_libraries(png_player
  ${SDL_LIBRARY}
)

add_executable(apng_player
  main_apng.c
  upng.c
)

target_link_libraries(apng_player
  ${SDL_LIBRARY}
)
