cmake_minimum_required(VERSION 3.16)
project(autotop_ui LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
option(AUTOTOP_DESKTOP "Use SDL2 for interactive desktop development" OFF)
option(AUTOTOP_SNAP "Host build of the launcher screen snapshot tool only" OFF)
set(INTER_DIR "" CACHE PATH "Extracted pinned Inter fonts")
set(LVGL_SOURCE_DIR "" CACHE PATH "Extracted pinned LVGL source")
if(NOT EXISTS "${LVGL_SOURCE_DIR}/lvgl.h")
  message(FATAL_ERROR "Use make apps or make preview to fetch LVGL first")
endif()
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/ui/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_SOURCE_DIR}" lvgl)
target_compile_definitions(lvgl PUBLIC AUTOTOP_DESKTOP=$<BOOL:${AUTOTOP_DESKTOP}> AUTOTOP_SNAP=$<BOOL:${AUTOTOP_SNAP}>)
if(AUTOTOP_DESKTOP)
  find_package(SDL2 REQUIRED)
  target_link_libraries(lvgl PUBLIC SDL2::SDL2)
endif()
if(NOT AUTOTOP_SNAP)
  add_executable(autotop-ui ui/main.c ui/screen.c)
  target_compile_options(autotop-ui PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(autotop-ui PRIVATE lvgl m)
  install(TARGETS autotop-ui RUNTIME DESTINATION bin)
endif()

# The launcher's screens (design/): theme, frame and screens as a library,
# with Inter embedded, and a tool that draws one screen into a picture.
if(NOT EXISTS "${INTER_DIR}/Inter-Regular.ttf")
  message(FATAL_ERROR "Use make to fetch the Inter fonts first")
endif()
set(UI_FONTS)
foreach(weight Light Regular SemiBold)
  string(TOLOWER ${weight} lower)
  set(font_c "${CMAKE_CURRENT_BINARY_DIR}/font_inter_${lower}.c")
  add_custom_command(OUTPUT "${font_c}"
    COMMAND ${CMAKE_COMMAND} "-DIN=${INTER_DIR}/Inter-${weight}.ttf" "-DOUT=${font_c}"
            -DSYM=font_inter_${lower} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS "${INTER_DIR}/Inter-${weight}.ttf" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake")
  list(APPEND UI_FONTS "${font_c}")
endforeach()
add_library(launcher-ui STATIC launcher/ui/theme.c launcher/ui/frame.c launcher/ui/home.c launcher/ui/wait.c
            launcher/ui/list.c launcher/ui/text.c launcher/ui/steps.c launcher/ui/receive.c launcher/ui/banner.c launcher/ui/nav.c launcher/ui/display.c
            launcher/ui/icons.c ${UI_FONTS})
target_compile_options(launcher-ui PRIVATE -Wall -Wextra -Werror)
# A builder's own mark for a running projection session (tools/design_icons.py), in place of the default.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/local/icons/carplay_24.inc")
  target_compile_definitions(launcher-ui PRIVATE "ICON_CARPLAY_LOCAL=\"${CMAKE_CURRENT_SOURCE_DIR}/local/icons/carplay_24.inc\"")
endif()
target_include_directories(launcher-ui PUBLIC launcher/ui)
target_link_libraries(launcher-ui PUBLIC lvgl m)
# Without the unit: a display in memory, a clock the caller moves, a finger,
# and a stand-in for launcherd (sample phones and settings).
add_library(launcher-ui-host STATIC launcher/ui/host.c launcher/ui/sample.c)
target_compile_options(launcher-ui-host PRIVATE -Wall -Wextra -Werror)
target_link_libraries(launcher-ui-host PUBLIC launcher-ui)
add_executable(launcher-snap launcher/ui/snap.c)
target_compile_options(launcher-snap PRIVATE -Wall -Wextra -Werror)
target_link_libraries(launcher-snap PRIVATE launcher-ui-host)
add_executable(launcher-uitest launcher/ui/test_nav.c)
target_compile_options(launcher-uitest PRIVATE -Wall -Wextra -Werror)
target_link_libraries(launcher-uitest PRIVATE launcher-ui-host)

# launcherd's logic against recording doubles for the unit and the UI: the
# whole launcher exercised by scenarios, without the unit and without LVGL.
add_executable(launcher-coretest launcher/launcherd.c launcher/phones.c launcher/test_core.c)
target_compile_options(launcher-coretest PRIVATE -Wall -Wextra -Werror -D_GNU_SOURCE)
target_link_libraries(launcher-coretest PRIVATE launcher-ui)

if(NOT AUTOTOP_DESKTOP AND NOT AUTOTOP_SNAP)
  # New Launcher (docs/launcher-design.md): CarPlay control, MCU, Bluetooth
  # and the LVGL UI in one static binary.
  # launcherd.c decides; unit.c is the unit it runs on (launcher/io.h).
  add_executable(launcherd launcher/launcherd.c launcher/unit.c launcher/dbusmini.c launcher/phones.c launcher/sounds.c
                 launcher/update.c)
  target_compile_options(launcherd PRIVATE -Wall -Wextra -Werror -D_GNU_SOURCE)
  target_link_libraries(launcherd PRIVATE launcher-ui)
  add_executable(cpctl launcher/cpctl.c launcher/dbusmini.c)
  target_compile_options(cpctl PRIVATE -Wall -Wextra -Werror -D_GNU_SOURCE)
endif()
