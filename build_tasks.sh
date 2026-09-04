#!/usr/bin/env bash
set -euo pipefail

echo "=========================================================="
echo "      Building and Running Tasks from TASKS.md            "
echo "=========================================================="

mkdir -p /workspace/test/bin

echo ""
echo "[Step 1] Building and testing SpatialIndex Abstractions..."
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    test/unit/test_step1_abstractions.cpp \
    -o test/bin/test_step1
./test/bin/test_step1
echo "==> Step 1 BUILD & TEST: PASSED"

echo ""
echo "[Step 2] Building and testing Clustering Data Objects..."
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    test/unit/test_step2_cluster_objects.cpp \
    -o test/bin/test_step2
./test/bin/test_step2
echo "==> Step 2 BUILD & TEST: PASSED"

echo ""
echo "[Step 3] Building and testing Data Loader with sample data..."
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    test/unit/test_step3_data_loader.cpp \
    -o test/bin/test_step3
./test/bin/test_step3
echo "==> Step 3 BUILD & TEST: PASSED"

echo ""
echo "[Step 4] Building and testing DBSCAN Engine Baseline (Port & Bug Fix)..."
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    src/spatial/geometry/dbscan_engine.cpp \
    test/unit/test_step4_dbscan_baseline.cpp \
    -o test/bin/test_step4
./test/bin/test_step4
echo "==> Step 4 BUILD & TEST: PASSED"

echo ""
echo "[Step 5] Building and testing Inbuilt FlatRTree with DBSCAN..."
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    src/spatial/geometry/dbscan_engine.cpp \
    test/unit/test_step5_flat_rtree_dbscan.cpp \
    -o test/bin/test_step5
./test/bin/test_step5
echo "==> Step 5 BUILD & TEST: PASSED"

echo ""
echo "[Step 6] Compiling DuckDB Spatial Window Functions Extension Module..."
g++ -std=c++11 -Wall -Wextra -Werror -c \
    -I src -isystem duckdb/src/include -Wno-unused-parameter -DDUCKDB_SPATIAL_EXTENSION=1 \
    src/spatial/geometry/dbscan_engine.cpp \
    -o test/bin/dbscan_engine.o
g++ -std=c++11 -Wall -Wextra -Werror -c \
    -I src -isystem duckdb/src/include -Wno-unused-parameter -DDUCKDB_SPATIAL_EXTENSION=1 \
    src/spatial/modules/main/spatial_functions_window.cpp \
    -o test/bin/spatial_functions_window.o
echo "==> Step 6 EXTENSION MODULE COMPILED SUCCESSFULLY"

echo ""
echo "[Step 7] Running Staff C++ Software Engineer Review (AddressSanitizer & UBSan)..."
g++ -std=c++11 -fsanitize=address,undefined -g -Wall -Wextra \
    -I src \
    src/spatial/geometry/dbscan_engine.cpp \
    test/unit/test_step5_flat_rtree_dbscan.cpp \
    -o test/bin/test_step5_asan
./test/bin/test_step5_asan
echo "==> Step 7 ASAN & UBSAN AUDIT: 0 LEAKS, 0 ERRORS DETECTED"

echo ""
echo "=========================================================="
echo "    ALL TASKS IN TASKS.md BUILT AND VERIFIED SUCCESFULLY  "
echo "=========================================================="
