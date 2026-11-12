foreach(component IN ITEMS common symbols expr format client console debug_adapter)
  add_subdirectory(${component})
endforeach()

add_executable(zxdb main.cc local_agent.cc)
target_link_libraries(zxdb PRIVATE zxdb_debug_adapter debug_agent)
set_target_properties(zxdb PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
install(TARGETS zxdb RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  include(Tests.cmake)
endif()
