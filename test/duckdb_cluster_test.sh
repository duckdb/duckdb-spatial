#!/usr/bin/env bash
set -euo pipefail

echo "=========================================================================="
echo "          DuckDB Spatial DBSCAN Scale Test & CLI Verification             "
echo "=========================================================================="

# 1. Run the high-performance C++ benchmark across 10, 1000, 1000000 points
/workspace/test/bin/test_scale_benchmark

# 2. Query and inspect the 10-point test in DuckDB CLI
echo ""
echo ">>> [DuckDB CLI] Inspecting 10 points dataset:"
duckdb -dark-mode -c "
SELECT id, round(x, 3) AS x, round(y, 3) AS y, COALESCE(cluster_id, 'NULL (noise)') AS cluster
FROM read_csv('/tmp/clusters_10.csv');"

# 3. Query and summarize the 1,000-point clusters in DuckDB CLI
echo ""
echo ">>> [DuckDB CLI] Aggregating 1,000 points into clusters with centroids & bounds:"
duckdb -dark-mode -c "
SELECT 
    COALESCE(cluster_id::VARCHAR, 'NOISE') AS cluster, 
    count(*) AS point_count, 
    round(avg(x), 2) AS centroid_x, 
    round(avg(y), 2) AS centroid_y, 
    round(min(x), 2) AS min_x, 
    round(max(x), 2) AS max_x 
FROM read_csv('/tmp/clusters_1000.csv') 
GROUP BY cluster_id 
ORDER BY (cluster_id IS NULL), cluster_id;"

echo ""
echo "=========================================================================="
echo "           Scale Test and DuckDB CLI Verification Complete!               "
echo "=========================================================================="
