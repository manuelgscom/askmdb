#!/usr/bin/env bash
# Only aggregate, and only in the executor: no write or admin function of libmongoc in src/.
# Usage: mongoc_check.sh SRC_DIR
set -o pipefail

src="${1:?usage: mongoc_check.sh SRC_DIR}"
if [ ! -d "$src" ]; then
    echo "mongoc_check: not a directory: $src" >&2
    exit 2
fi
src="${src%/}"
status=0

if hits=$(grep -rnE 'mongoc_\w*(insert|update|delete|remove|drop|command|create_index|rename|bulk|find_and_modify)' "$src"); then
    echo "mongoc_check: write or admin libmongoc functions in $src:" >&2
    echo "$hits" >&2
    status=1
fi

if hits=$(grep -rn 'mongoc_collection_aggregate' "$src" | grep -v "^$src/mongo/execute.cpp:"); then
    echo "mongoc_check: mongoc_collection_aggregate outside $src/mongo/execute.cpp:" >&2
    echo "$hits" >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "mongoc_check: ok"
fi
exit "$status"
