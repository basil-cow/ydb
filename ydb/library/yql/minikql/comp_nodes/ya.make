OWNER(
    g:yql
    g:yql_ydb_core
)

RECURSE(
    llvm
    llvm14
    no_llvm
)

RECURSE_FOR_TESTS(
    llvm/ut
    llvm14/ut
)
