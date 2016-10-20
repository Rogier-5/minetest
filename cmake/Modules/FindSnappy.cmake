mark_as_advanced(SNAPPY_LIBRARY SNAPPY_INCLUDE_DIR)

find_path(SNAPPY_INCLUDE_DIR NAMES snappy.h snappy-c.h)

find_library(SNAPPY_LIBRARY NAMES snappy)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SNAPPY DEFAULT_MSG SNAPPY_LIBRARY SNAPPY_INCLUDE_DIR)
