LIBRARY()

PEERDIR(
    ydb/core/blobstorage/base
    ydb/core/blobstorage/groupinfo
    ydb/core/blobstorage/vdisk/common
    ydb/core/blobstorage/vdisk/hulldb
    ydb/core/blobstorage/vdisk/ingress
    ydb/core/blobstorage/vdisk/repl
)

SRCS(
    balancing_actor.cpp
)

END()

RECURSE_FOR_TESTS(
    ut
)
