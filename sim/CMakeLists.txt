# SPDX-License-Identifier: GPL-3.0-or-later
# Desktop simulator: the player UI at 320x240 in an SDL window (or headless screenshots).
include(FetchContent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE FILEPATH "LVGL config" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG        v9.2.2
  GIT_SHALLOW    TRUE
  # our fixes (patches/lvgl)
  PATCH_COMMAND sh ${CMAKE_SOURCE_DIR}/patches/lvgl/apply.sh)
FetchContent_MakeAvailable(lvgl)
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)

add_executable(fathom-sim main.c ../app/ui/ui.c ../app/ui/fonts.c ../app/ui/assets.c ../app/audio/out_sdl.c)
target_link_libraries(fathom-sim PRIVATE fathom_player lvgl PkgConfig::SDL2)
target_include_directories(fathom-sim PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_compile_definitions(fathom-sim PRIVATE FATHOM_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_options(fathom-sim PRIVATE ${FATHOM_WARNINGS})

# make-logo: draws the boot logo (assets/logo.png, and the kernel's copy); run by hand
find_package(ZLIB)
if(ZLIB_FOUND)
  add_executable(make-logo ../tools/logo/make_logo.c)
  target_include_directories(make-logo PRIVATE ${lvgl_SOURCE_DIR}/src/libs/tiny_ttf)
  target_link_libraries(make-logo PRIVATE ZLIB::ZLIB m)
endif()

# The UI redraws only what changed: nothing at rest or with the screen off, and
# the panel (direct mode, as on the H2) always matches the screen drawn whole.
add_test(NAME ui_redraw COMMAND fathom-sim --bench 1 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
