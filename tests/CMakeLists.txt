add_executable(logsift_tests
    test_main.cpp
    test_eval.cpp
    test_formats.cpp
    test_json.cpp
    test_output.cpp
    test_pipeline.cpp
    test_query_parser.cpp
    test_store.cpp
    test_time.cpp
    test_value.cpp
)
target_include_directories(logsift_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
# doctest names test cases with __COUNTER__, which Clang 22 and later report
# under -Wpedantic as a C2y extension; with LOGSIFT_WARNINGS_AS_ERRORS that
# fails the build. The warning concerns doctest's macros, not our code, so
# turn off just that one, for the tests only. It must come after -Wpedantic
# on the command line, hence a separate interface target linked after
# logsift_build_flags. (Check the positive flag: GCC accepts any unknown
# -Wno-... silently.)
add_library(logsift_test_flags INTERFACE)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wc2y-extensions LOGSIFT_HAVE_WC2Y_EXTENSIONS)
if(LOGSIFT_HAVE_WC2Y_EXTENSIONS)
    target_compile_options(logsift_test_flags INTERFACE -Wno-c2y-extensions)
endif()

target_link_libraries(logsift_tests PRIVATE logsift logsift_build_flags logsift_test_flags)
target_compile_definitions(logsift_tests PRIVATE LOGSIFT_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
add_test(NAME logsift_tests COMMAND logsift_tests)

# Smoke tests for the command-line tool against the example logs.
if(TARGET logsift_cli)
    set(EX ${PROJECT_SOURCE_DIR}/examples)

    add_test(NAME cli_json_stats COMMAND logsift_cli "level == \"error\" | count by service" ${EX}/app.jsonl)
    set_tests_properties(cli_json_stats PROPERTIES PASS_REGULAR_EXPRESSION "billing")

    add_test(NAME cli_access_top COMMAND logsift_cli "status >= 400 | top 3 path" ${EX}/access.log)
    set_tests_properties(cli_access_top PROPERTIES PASS_REGULAR_EXPRESSION "/admin")

    add_test(NAME cli_syslog COMMAND logsift_cli --year 2026 "app == \"sshd\" and message =~ \"^Failed\" | count" ${EX}/syslog.log)
    set_tests_properties(cli_syslog PROPERTIES PASS_REGULAR_EXPRESSION "count")

    add_test(NAME cli_logfmt_json COMMAND logsift_cli -o json "ok == false | fields job, attempt | head 1" ${EX}/worker.logfmt)
    set_tests_properties(cli_logfmt_json PROPERTIES PASS_REGULAR_EXPRESSION "^\\{\"job\":")

    add_test(NAME cli_indexed_explain COMMAND logsift_cli --index service --explain "service == \"api\" and _time >= \"2026-11-15T09:30:00Z\"" ${EX}/app.jsonl)
    set_tests_properties(cli_indexed_explain PROPERTIES PASS_REGULAR_EXPRESSION "index scan")

    add_test(NAME cli_schema COMMAND logsift_cli --schema ${EX}/app.jsonl)
    set_tests_properties(cli_schema PROPERTIES PASS_REGULAR_EXPRESSION "http.status")

    add_test(NAME cli_bad_query COMMAND logsift_cli "status >=" ${EX}/app.jsonl)
    set_tests_properties(cli_bad_query PROPERTIES WILL_FAIL TRUE)
endif()
