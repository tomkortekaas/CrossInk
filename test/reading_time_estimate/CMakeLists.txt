add_executable(ReadingTimeEstimateTest
  ReadingTimeEstimateTest.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingTimeEstimate.cpp
)

target_include_directories(ReadingTimeEstimateTest PRIVATE ${REPO_ROOT}/src)
target_compile_options(ReadingTimeEstimateTest PRIVATE -UNDEBUG)
add_test(NAME ReadingTimeEstimateTest COMMAND ReadingTimeEstimateTest)
