#include "spatial/geometry/flat_rtree.hpp"
#include "spatial/geometry/dbscan_engine.hpp"
#include "spatial/geometry/cluster_data_loader.hpp"
#include <iostream>
#include <cassert>
#include <chrono>

using namespace duckdb::spatial;

int main() {
	std::cout << "[RUNNING] test_step5_flat_rtree_dbscan...\n";

	// 1. Test FlatRTree2D RadiusSearch precision on synthetic grid
	std::vector<Point2D> grid;
	for (int x = 0; x < 20; ++x) {
		for (int y = 0; y < 20; ++y) {
			grid.emplace_back(static_cast<double>(x), static_cast<double>(y));
		}
	}

	FlatRTree2D rtree(16);
	rtree.Build(ArrayView<Point2D>(grid));
	assert(rtree.Count() == 400);

	// Radius search around (10.0, 10.0) with eps = 1.1
	// Should find (10,10), (10,11), (10,9), (11,10), (9,10) -> exactly 5 points
	std::vector<size_t> matches;
	rtree.RadiusSearch(Point2D(10.0, 10.0), 1.1, matches);
	assert(matches.size() == 5);

	// 2. Real dataset benchmark & verification: sample2d.csv
	auto sample2d = ClusterDataLoader::LoadPoints2DFromCSV("dbscan/sample2d.csv");
	FlatRTree2D sample_rtree(32);

	auto t0 = std::chrono::high_resolution_clock::now();
	sample_rtree.Build(ArrayView<Point2D>(sample2d));
	auto t1 = std::chrono::high_resolution_clock::now();

	DBSCANParams sample_params(0.2, 10);
	auto sample_result = DBSCANEngine::Cluster2D(ArrayView<Point2D>(sample2d), sample_rtree, sample_params);
	auto t2 = std::chrono::high_resolution_clock::now();

	double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	double cluster_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

	std::cout << "  FlatRTree build time: " << build_ms << " ms\n";
	std::cout << "  DBSCAN clustering time: " << cluster_ms << " ms\n";
	std::cout << "  Result: " << sample_result.NumClusters() << " clusters, " 
	          << sample_result.NumNoise() << " noise points\n";

	// Verify exact match with ground truth!
	assert(sample_result.NumClusters() == 4);
	assert(sample_result.NumNoise() == 31);

	// Verify cluster point distribution matches prototype
	std::vector<size_t> cluster_sizes(4, 0);
	for (size_t i = 0; i < sample_result.Size(); ++i) {
		if (sample_result.IsClustered(i)) {
			cluster_sizes[sample_result.GetClusterId(i)]++;
		}
	}
	std::sort(cluster_sizes.begin(), cluster_sizes.end());
	// ground truth cluster sizes: 252, 253, 257, 257
	assert(cluster_sizes[0] == 252);
	assert(cluster_sizes[1] == 253);
	assert(cluster_sizes[2] == 257);
	assert(cluster_sizes[3] == 257);

	std::cout << "[PASSED] test_step5_flat_rtree_dbscan completed successfully with 100% accuracy!\n";
	return 0;
}
