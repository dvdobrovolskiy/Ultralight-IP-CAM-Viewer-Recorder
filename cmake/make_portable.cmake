if(NOT DEFINED build_dir OR NOT DEFINED out_dir)
  message(FATAL_ERROR "make_portable.cmake requires -Dbuild_dir=... -Dout_dir=...")
endif()

file(MAKE_DIRECTORY "${out_dir}")

file(GLOB dist_existing "${out_dir}/*")
if(dist_existing)
  file(REMOVE_RECURSE ${dist_existing})
endif()

set(exe "${build_dir}/Ultralight_IP_CAM_Viewer_Recorder.exe")
if(NOT EXISTS "${exe}")
  message(FATAL_ERROR "Executable not found: ${exe}")
endif()

file(COPY_FILE "${exe}" "${out_dir}/Ultralight_IP_CAM_Viewer_Recorder.exe")

file(GLOB dlls "${build_dir}/*.dll")
foreach(dll IN LISTS dlls)
  get_filename_component(name "${dll}" NAME)
  file(COPY_FILE "${dll}" "${out_dir}/${name}" ONLY_IF_DIFFERENT)
endforeach()

