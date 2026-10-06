if(NOT DEFINED exe OR NOT DEFINED out)
  message(FATAL_ERROR "copy_runtime_deps.cmake requires -Dexe=... -Dout=...")
endif()

if(NOT EXISTS "${exe}")
  message(FATAL_ERROR "Executable not found: ${exe}")
endif()

if(NOT DEFINED search_dir)
  set(search_dir "")
endif()

file(MAKE_DIRECTORY "${out}")

file(GLOB existing_dlls "${out}/*.dll")
if(existing_dlls)
  file(REMOVE ${existing_dlls})
endif()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${exe}"
  DIRECTORIES "${search_dir}"
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  POST_EXCLUDE_REGEXES
    "api-ms-win-.*"
    "ext-ms-win-.*"
    ".*System32/.*"
    ".*System32\\\\.*"
)

string(REPLACE "\\" "/" search_dir_norm "${search_dir}")
string(TOLOWER "${search_dir_norm}" search_dir_norm_lc)

foreach(dll IN LISTS resolved)
  if(EXISTS "${dll}")
    string(REPLACE "\\" "/" dll_norm "${dll}")
    string(TOLOWER "${dll_norm}" dll_norm_lc)
    if(search_dir_norm_lc AND NOT dll_norm_lc MATCHES "^${search_dir_norm_lc}/")
      continue()
    endif()
    get_filename_component(name "${dll}" NAME)
    file(COPY_FILE "${dll}" "${out}/${name}" ONLY_IF_DIFFERENT)
  endif()
endforeach()

set(unresolved_filtered "")
foreach(name IN LISTS unresolved)
  # Ignore Windows API-set style forwarders and OneCore/ext-ms shims.
  if(name MATCHES "^api-ms-" OR name MATCHES "^ext-ms-")
    continue()
  endif()
  # Ignore optional Windows security/attestation components that are not redistributable and
  # commonly appear as "unresolved" on some Windows versions.
  if(name MATCHES "^AzureAttest" OR name MATCHES "^HvsiFileTrust\\.dll$" OR
     name MATCHES "^PdmUtilities\\.dll$" OR name MATCHES "^wpaxholder\\.dll$")
    continue()
  endif()
  list(APPEND unresolved_filtered "${name}")
endforeach()

if(unresolved_filtered)
  message(WARNING "Unresolved runtime dependencies: ${unresolved_filtered}")
endif()
