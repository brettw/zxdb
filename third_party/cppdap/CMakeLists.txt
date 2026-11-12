# Source selection follows the upstream host build.
add_library(cppdap STATIC
  src/content_stream.cpp
  src/io.cpp
  src/network.cpp
  src/null_json_serializer.cpp
  src/protocol_events.cpp
  src/protocol_requests.cpp
  src/protocol_response.cpp
  src/protocol_types.cpp
  src/rapid_json_serializer.cpp
  src/session.cpp
  src/socket.cpp
  src/typeinfo.cpp
  src/typeof.cpp
)
target_link_libraries(cppdap PUBLIC rapidjson Threads::Threads)
target_include_directories(cppdap PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(cppdap PRIVATE CPPDAP_JSON_RAPID)
