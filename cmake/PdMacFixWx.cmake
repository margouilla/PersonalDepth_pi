# PersonalDepth_pi - macOS : les bibliothèques wxWidgets sont celles livrées
# dans OpenCPN.app (Contents/Frameworks). Chaque référence libwx_*-3.2*.dylib
# de la bibliothèque du plugin est réécrite vers ce dossier.
# Copyright (C) 2026 Marcaurelio Franzetti - GPL-2.0-or-later
# cmake -DDYLIB=<libPersonalDepth_pi.dylib> -P PdMacFixWx.cmake
execute_process(COMMAND otool -L "${DYLIB}" OUTPUT_VARIABLE refs RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "otool -L a échoué sur ${DYLIB}")
endif()
string(REGEX MATCHALL "[^ \t\n]*libwx_[A-Za-z0-9_]+-3\\.2[0-9.]*\\.dylib" wx "${refs}")
list(REMOVE_DUPLICATES wx)
foreach(old IN LISTS wx)
  string(REGEX REPLACE "^.*(libwx_[A-Za-z0-9_]+)-3\\.2[0-9.]*\\.dylib$" "\\1" base "${old}")
  set(new "@executable_path/../Frameworks/${base}-3.2.dylib")
  if(NOT old STREQUAL new)
    execute_process(COMMAND install_name_tool -change "${old}" "${new}" "${DYLIB}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "install_name_tool -change ${old} a échoué")
    endif()
  endif()
endforeach()
