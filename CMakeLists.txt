cmake_minimum_required(VERSION 3.16)
project(PersonalDepth_pi VERSION 0.1.0 LANGUAGES C CXX)

if(MSVC)
  # OpenCPN 5.14 (Windows) livre une msvcp140.dll de 2017 (14.12), chargée en
  # priorité : sans cette macro, un std::mutex compilé avec Visual Studio
  # 2022 17.10+ plante au premier verrouillage (rupture d'ABI de Microsoft).
  add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

# Cible par défaut du paquet = système de compilation (à préciser si besoin
# avec -DPD_TARGET=… -DPD_TARGET_VERSION=… -DPD_TARGET_ARCH=…)
function(pd_default_target out_t out_v out_a)
  if(WIN32)
    set(t msvc-wx32)
    set(v 10)
    set(a x86_64)
  elseif(APPLE)
    set(t darwin-wx32)
    set(v 10)
    set(a "arm64;x86_64")
    if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
      set(a "${CMAKE_OSX_ARCHITECTURES}")
    endif()
  else()
    set(id linux)
    set(v "")
    if(EXISTS /etc/os-release)
      file(STRINGS /etc/os-release _id REGEX "^ID=")
      file(STRINGS /etc/os-release _ver REGEX "^VERSION_ID=")
      string(REGEX REPLACE "^ID=\"?([^\"]*)\"?$" "\\1" id "${_id}")
      string(REGEX REPLACE "^VERSION_ID=\"?([^\"]*)\"?$" "\\1" v "${_ver}")
    endif()
    if(NOT id STREQUAL "ubuntu")  # Debian, Raspberry Pi OS : version majeure
      string(REGEX REPLACE "\\..*$" "" v "${v}")
    endif()
    set(a "${CMAKE_SYSTEM_PROCESSOR}")
    if(a MATCHES "^(aarch64|arm64)$")
      set(a arm64)
    elseif(a MATCHES "^arm")
      set(a armhf)
    elseif(a MATCHES "^(AMD64|amd64)$")
      set(a x86_64)
    endif()
    set(t "${id}-${a}")
  endif()
  set(${out_t} "${t}" PARENT_SCOPE)
  set(${out_v} "${v}" PARENT_SCOPE)
  set(${out_a} "${a}" PARENT_SCOPE)
endfunction()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PD_WITH_GDAL   "Lecture GeoTIFF/BAG/NetCDF via GDAL" ON)
option(PD_BUILD_TESTS "Tests du cœur (sans OpenCPN)" ON)
option(PD_BUILD_PLUGIN "Construire le plugin (wxWidgets requis)" ON)
set(OCPN_API_DIR "" CACHE PATH
    "Dossier contenant ocpn_plugin.h + CMakeLists.txt (opencpn-libs/api-21). Vide = téléchargement")

if(MSVC)
  add_compile_options(/utf-8 /W3)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
elseif(UNIX)
  add_compile_definitions(_FILE_OFFSET_BITS=64)  # fichiers > 2 Go en 32 bits (armhf)
endif()

# ------------------------------------------------- bibliothèques tierces
# SQLite (domaine public) et miniz (MIT) : base de bathymétrie, compression
add_library(pd_thirdparty STATIC
  third_party/sqlite/sqlite3.c
  third_party/miniz/miniz.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c)
target_include_directories(pd_thirdparty PUBLIC third_party/sqlite third_party/miniz)
target_compile_definitions(pd_thirdparty PUBLIC
  SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED
  MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(pd_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(pd_thirdparty PRIVATE /W0)
else()
  target_compile_options(pd_thirdparty PRIVATE -w)
endif()

# ---------------------------------------------------------------- cœur
add_library(pd_core STATIC
  src/core/PointCloud.cpp src/core/BathyGrid.cpp src/core/ColorRamp.cpp
  src/core/Soundings.cpp src/core/BathyLayer.cpp src/core/Contours.cpp
  src/core/Projection.cpp src/core/VerticalRef.cpp src/core/Triangulate.cpp
  src/core/Inflate.cpp src/core/TideFES.cpp
  src/core/WebMerc.cpp src/core/TileCodec.cpp src/core/BathyDb.cpp src/core/Lang.cpp src/core/FileUtil.cpp
  src/core/WaterLevel.cpp src/core/SurveyShape.cpp)
target_include_directories(pd_core PUBLIC src third_party)
set_target_properties(pd_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(pd_core PUBLIC Threads::Threads pd_thirdparty)

if(PD_WITH_GDAL)
  find_package(GDAL QUIET)
  if(GDAL_FOUND)
    target_link_libraries(pd_core PUBLIC GDAL::GDAL)
    target_compile_definitions(pd_core PUBLIC PD_HAVE_GDAL)
    message(STATUS "PersonalDepth : GDAL activé")
  else()
    message(STATUS "PersonalDepth : GDAL absent -> XYZ, ASC et LandXML seulement")
  endif()
endif()

if(PD_BUILD_TESTS)
  enable_testing()
  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE pd_core)
  add_test(NAME core COMMAND test_core 400000)
endif()

# --------------------------------------------------------------- plugin
if(PD_BUILD_PLUGIN)
  find_package(wxWidgets COMPONENTS core base gl QUIET)
  find_package(OpenGL QUIET)
  if(NOT wxWidgets_FOUND OR NOT OPENGL_FOUND)
    message(WARNING "wxWidgets/OpenGL introuvables : seul le cœur est construit")
  else()
    include(${wxWidgets_USE_FILE})
    if(OCPN_API_DIR)
      add_subdirectory(${OCPN_API_DIR} ${CMAKE_BINARY_DIR}/ocpn-api)
    else()
      include(FetchContent)
      FetchContent_Declare(opencpn_libs
        GIT_REPOSITORY https://github.com/OpenCPN/opencpn-libs.git
        GIT_TAG main GIT_SHALLOW TRUE)
      FetchContent_GetProperties(opencpn_libs)
      if(NOT opencpn_libs_POPULATED)
        FetchContent_Populate(opencpn_libs)
      endif()
      add_subdirectory(${opencpn_libs_SOURCE_DIR}/api-21 ${CMAKE_BINARY_DIR}/ocpn-api)
    endif()

    add_library(PersonalDepth_pi SHARED
      src/PersonalDepth_pi.cpp src/BathyRenderer.cpp src/BathyDialog.cpp src/ImportDialog.cpp src/DataDialog.cpp src/TideChart.cpp src/TextFormatDialog.cpp
      src/WaterLevelDialog.cpp)
    target_link_libraries(PersonalDepth_pi PRIVATE pd_core ocpn::api
                          ${wxWidgets_LIBRARIES} OpenGL::GL)
    if(APPLE)
      # symboles OpenCPN résolus au chargement ; wxWidgets : celui d'OpenCPN.app
      target_link_options(PersonalDepth_pi PRIVATE -undefined dynamic_lookup)
      target_compile_definitions(PersonalDepth_pi PRIVATE GL_SILENCE_DEPRECATION)
      add_custom_command(TARGET PersonalDepth_pi POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DDYLIB=$<TARGET_FILE:PersonalDepth_pi>
                -P ${CMAKE_SOURCE_DIR}/cmake/PdMacFixWx.cmake
        COMMAND codesign --force -s - $<TARGET_FILE:PersonalDepth_pi>
        VERBATIM)
    endif()
    if(WIN32)
      set_target_properties(PersonalDepth_pi PROPERTIES PREFIX "")
      install(TARGETS PersonalDepth_pi RUNTIME DESTINATION plugins)
      install(FILES data/personaldepth.svg DESTINATION plugins/PersonalDepth_pi/data)
    else()
      install(TARGETS PersonalDepth_pi LIBRARY DESTINATION lib/opencpn)
      install(FILES data/personaldepth.svg
              DESTINATION share/opencpn/plugins/PersonalDepth_pi/data)
    endif()

    # ------------------------------------- paquet pour OpenCPN (.tar.gz)
    # cmake --build build --target pd-package  ->  dist/PersonalDepth-<version>_<cible>.tar.gz
    # La cible doit être celle de l'OpenCPN qui importera le paquet.
    pd_default_target(_t _tv _ta)
    set(PD_TARGET "${_t}" CACHE STRING "metadata.xml <target> (ex. debian-x86_64, darwin-wx32)")
    set(PD_TARGET_VERSION "${_tv}" CACHE STRING "metadata.xml <target-version> (ex. 12, 24.04, 10)")
    set(PD_TARGET_ARCH "${_ta}" CACHE STRING "metadata.xml <target-arch> (x86_64, arm64, armhf…)")
    set(PD_PRE "" CACHE STRING "suffixe de pré-version du paquet (ex. beta.4)")
    set(PD_VERSION "${PROJECT_VERSION}")
    if(PD_PRE)
      set(PD_VERSION "${PROJECT_VERSION}-${PD_PRE}")
    endif()
    configure_file(packaging/metadata.xml.in ${CMAKE_BINARY_DIR}/metadata.xml @ONLY)
    if(PD_TARGET MATCHES "^msvc")
      set(_layout msvc)
    elseif(PD_TARGET MATCHES "^darwin")
      set(_layout darwin)
    elseif(PD_TARGET MATCHES "^flatpak")
      set(_layout flatpak)
    else()
      set(_layout linux)
    endif()
    # Debian 13 : OpenCPN annonce la version « n/a » ; le nom du fichier
    # prend alors la version majeure du système de compilation
    set(_tvf "${PD_TARGET_VERSION}")
    if(_tvf STREQUAL "n/a" AND EXISTS /etc/os-release)
      file(STRINGS /etc/os-release _ver REGEX "^VERSION_ID=")
      if(_ver MATCHES "^VERSION_ID=\"?([0-9]+)")
        set(_tvf "${CMAKE_MATCH_1}")
      endif()
    endif()
    string(REPLACE "/" "-" _tvf "${_tvf}")
    if(_layout STREQUAL "darwin")
      set(_arch "universal")
      if(NOT PD_TARGET_ARCH MATCHES ";")
        set(_arch "${PD_TARGET_ARCH}")
      endif()
    else()
      set(_arch "${PD_TARGET_ARCH}")
    endif()
    set(PD_PACKAGE_NAME "PersonalDepth-${PD_VERSION}_${PD_TARGET}-${_tvf}-${_arch}")
    add_custom_target(pd-package
      COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:PersonalDepth_pi> -DSRC=${CMAKE_SOURCE_DIR}
              -DBIN=${CMAKE_BINARY_DIR} -DNAME=${PD_PACKAGE_NAME} -DLAYOUT=${_layout}
              -DOUT=${CMAKE_SOURCE_DIR}/dist -P ${CMAKE_SOURCE_DIR}/cmake/PdPackage.cmake
      DEPENDS PersonalDepth_pi VERBATIM)
    message(STATUS "PersonalDepth : paquet ${PD_PACKAGE_NAME} (cible ${PD_TARGET} ${PD_TARGET_VERSION} ${PD_TARGET_ARCH})")
  endif()
endif()
