# PersonalDepth_pi - paquet du gestionnaire de plugins d'OpenCPN (.tar.gz)
# Copyright (C) 2026 Marcaurelio Franzetti - GPL-2.0-or-later
#
# cmake -DLIB=<bibliothèque> -DSRC=<sources> -DBIN=<build> -DNAME=<nom du paquet>
#       -DLAYOUT=<msvc|darwin|flatpak|linux> -DOUT=<dossier de sortie> -P PdPackage.cmake
#
# Arborescence attendue par l'import d'OpenCPN (Options › Plugins › Importer) :
#   msvc    : plugins/PersonalDepth_pi.dll, plugins/PersonalDepth_pi/data/
#   darwin  : OpenCPN.app/Contents/PlugIns/libPersonalDepth_pi.dylib,
#             OpenCPN.app/Contents/SharedSupport/plugins/PersonalDepth_pi/data/
#   flatpak : lib/opencpn/libPersonalDepth_pi.so, share/opencpn/plugins/PersonalDepth_pi/data/
#   linux   : usr/lib/opencpn/libPersonalDepth_pi.so, usr/share/opencpn/plugins/PersonalDepth_pi/data/
foreach(v LIB SRC BIN NAME LAYOUT OUT)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "PdPackage.cmake : ${v} manquant")
  endif()
endforeach()
get_filename_component(libname "${LIB}" NAME)
set(stage "${BIN}/package")
set(top "${stage}/${NAME}")
file(REMOVE_RECURSE "${stage}")
if(LAYOUT STREQUAL "msvc")
  set(libdir "${top}/plugins")
  set(datadir "${top}/plugins/PersonalDepth_pi/data")
elseif(LAYOUT STREQUAL "darwin")
  set(libdir "${top}/OpenCPN.app/Contents/PlugIns")
  set(datadir "${top}/OpenCPN.app/Contents/SharedSupport/plugins/PersonalDepth_pi/data")
elseif(LAYOUT STREQUAL "flatpak")
  set(libdir "${top}/lib/opencpn")
  set(datadir "${top}/share/opencpn/plugins/PersonalDepth_pi/data")
else()
  set(libdir "${top}/usr/lib/opencpn")
  set(datadir "${top}/usr/share/opencpn/plugins/PersonalDepth_pi/data")
endif()
file(MAKE_DIRECTORY "${libdir}" "${datadir}")
file(COPY "${LIB}" DESTINATION "${libdir}"
     FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(COPY "${SRC}/data/personaldepth.svg" DESTINATION "${datadir}")
configure_file("${SRC}/LICENSE" "${datadir}/license.txt" COPYONLY)
configure_file("${BIN}/metadata.xml" "${top}/metadata.xml" COPYONLY)
file(MAKE_DIRECTORY "${OUT}")
set(tgz "${OUT}/${NAME}.tar.gz")
file(REMOVE "${tgz}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${tgz}" "${NAME}"
                WORKING_DIRECTORY "${stage}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Création du paquet impossible : ${tgz}")
endif()
message(STATUS "Paquet : ${tgz}")
