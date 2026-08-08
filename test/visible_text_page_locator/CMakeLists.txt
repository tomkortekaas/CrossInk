add_executable(VisibleTextPageLocatorTest
  VisibleTextPageLocatorTest.cpp
)

target_include_directories(VisibleTextPageLocatorTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(VisibleTextPageLocatorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VisibleTextPageLocatorTest)
