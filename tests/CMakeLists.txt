function(clipitto_add_test name)
    add_executable(${name} ${name}.cpp TestDb.h)
    target_link_libraries(${name} PRIVATE clipitto_core Qt6::Test)
    clipitto_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

clipitto_add_test(tst_config)
clipitto_add_test(tst_ftsquery)
clipitto_add_test(tst_migrator)
clipitto_add_test(tst_repository)

# Not part of the default ctest run: ctest -C bench -L bench (or run bench_search directly).
add_executable(bench_search bench_search.cpp TestDb.h)
target_link_libraries(bench_search PRIVATE clipitto_core Qt6::Test)
clipitto_set_warnings(bench_search)
add_test(NAME bench_search COMMAND bench_search CONFIGURATIONS bench)
set_tests_properties(bench_search PROPERTIES LABELS bench)
