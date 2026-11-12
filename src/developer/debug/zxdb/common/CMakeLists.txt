# Source selection follows the upstream host build.
add_library(zxdb_common STATIC
  address_ranges.cc
  cache_dir.cc
  curl.cc
  data_extractor.cc
  err.cc
  file_util.cc
  fuzzy_matcher.cc
  host_util.cc
  inet_util.cc
  int128_t.cc
  leb.cc
  scoped_temp_file.cc
  string_util.cc
  tagged_data.cc
  tagged_data_builder.cc
  worker_pool.cc
)
target_link_libraries(zxdb_common PUBLIC debug::shared containers unwinder CURL::libcurl)
