cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# The mask filter's icon browser needs the frontend API, Qt (Widgets/Svg/Xml),
# libcurl and nlohmann_json.
if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "Spectrum requires ENABLE_FRONTEND_API and ENABLE_QT to be ON.")
endif()

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Svg Xml)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg Qt6::Xml)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  # Path where nlohmann_json is expected inside the Flatpak runtime
  set(JSON_TARBALL_PATH "${CMAKE_SOURCE_DIR}/json.tar.xz")
  if(EXISTS "${JSON_TARBALL_PATH}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xJf "${JSON_TARBALL_PATH}"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${CMAKE_BINARY_DIR}/json/single_include")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
  else()
    include(FetchContent)
    FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(json)
  endif()
endif()

find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)

if(APPLE)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "$<LINK_LIBRARY:FRAMEWORK,Security.framework>" "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

# Blur
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/blur/blur-filter.c
    src/blur/blur-filter.h
    src/blur/obs-utils.c
    src/blur/obs-utils.h
    src/blur/algorithms/box.c
    src/blur/algorithms/box.h
    src/blur/algorithms/dual_kawase.c
    src/blur/algorithms/dual_kawase.h
    src/blur/algorithms/gaussian-kernel.c
    src/blur/algorithms/gaussian-kernel.h
    src/blur/algorithms/gaussian.c
    src/blur/algorithms/gaussian.h
    src/blur/algorithms/pixelate.c
    src/blur/algorithms/pixelate.h
    src/blur/algorithms/temporal.c
    src/blur/algorithms/temporal.h
)

# Stroke / Glow / Shadow
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/stroke/defines.h
    src/stroke/glow.c
    src/stroke/glow.h
    src/stroke/glow-filter.c
    src/stroke/glow-filter.h
    src/stroke/glow-types.h
    src/stroke/obs-utils.c
    src/stroke/obs-utils.h
    src/stroke/stroke.c
    src/stroke/stroke.h
    src/stroke/stroke-filter.c
    src/stroke/stroke-filter.h
    src/stroke/stroke-types.h
    src/stroke/blur/alpha-blur.c
    src/stroke/blur/alpha-blur.h
    src/stroke/blur/dual-kawase.c
    src/stroke/blur/dual-kawase.h
)

# Masks
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/masks/base-filter.h
    src/masks/color-adjustments.c
    src/masks/color-adjustments.h
    src/masks/mask-bsm.c
    src/masks/mask-bsm.h
    src/masks/mask-chroma-key.c
    src/masks/mask-chroma-key.h
    src/masks/mask-feather.c
    src/masks/mask-feather.h
    src/masks/mask-font-awesome.cpp
    src/masks/mask-font-awesome.hpp
    src/masks/mask-gradient.c
    src/masks/mask-gradient.h
    src/masks/mask-shape.c
    src/masks/mask-shape.h
    src/masks/mask-source.c
    src/masks/mask-source.h
    src/masks/mask-svg.c
    src/masks/mask-svg.h
    src/masks/masks.h
    src/masks/masks-filter.c
    src/masks/masks-filter.h
    src/masks/obs-utils.c
    src/masks/obs-utils.h
    src/masks/svg-utils.cpp
    src/masks/svg-utils.hpp
    src/masks/utils.c
    src/masks/utils.h
)

# Gradient
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/gradient/gradient.c
    src/gradient/gradient.h
    src/gradient/gradient-editor.cpp
    src/gradient/gradient-editor.hpp
    src/gradient/gradient-source.c
)

# Single-pass effects
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/common/fx-filter.c src/common/fx-filter.h src/fx/gradient-map.c src/fx/shape-source.c
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
