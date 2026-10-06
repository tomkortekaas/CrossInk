function(add_library_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${REPO_ROOT}/src)
  target_link_libraries(${name} PRIVATE crosspoint_test_common)
  # Release defines NDEBUG; the tests rely on assert().
  target_compile_options(${name} PRIVATE -UNDEBUG)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_library_test(LibraryGridLayoutTest
  LibraryGridLayoutTest.cpp
  ${REPO_ROOT}/src/activities/library/LibraryGridLayout.cpp)

add_library_test(LibrarySortTest
  LibrarySortTest.cpp
  ${REPO_ROOT}/src/activities/library/LibrarySort.cpp)

add_library_test(LibraryIndexCodecTest
  LibraryIndexCodecTest.cpp
  ${REPO_ROOT}/src/activities/library/LibraryIndexCodec.cpp
  ${REPO_ROOT}/src/activities/library/LibrarySort.cpp)
