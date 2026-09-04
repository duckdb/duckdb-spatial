#include "spatial/geometry/cluster_data_loader.hpp"
#include <iostream>
#include <cassert>

using namespace duckdb::spatial;

int main() {
	std::cout << "[RUNNING] test_step3_data_loader...\n";

	// 1. Load 2D sample data
	std::string sample2d_path = "dbscan/sample2d.csv";
	auto points2d = ClusterDataLoader::LoadPoints2DFromCSV(sample2d_path);
	std::cout << "  Loaded " << points2d.size() << " 2D points from " << sample2d_path << "\n";
	assert(points2d.size() > 0);

	// Compute and verify bounds
	auto bbox = ClusterDataLoader::ComputeBounds(points2d);
	std::cout << "  2D Extents: X[" << bbox.min_x << ", " << bbox.max_x << "], Y[" << bbox.min_y << ", " << bbox.max_y << "]\n";
	assert(bbox.min_x < bbox.max_x);
	assert(bbox.min_y < bbox.max_y);

	// 2. Load 3D sample data
	std::string sample3d_path = "dbscan/sample3d.csv";
	auto points3d = ClusterDataLoader::LoadPoints3DFromCSV(sample3d_path);
	std::cout << "  Loaded " << points3d.size() << " 3D points from " << sample3d_path << "\n";
	assert(points3d.size() > 0);

	// 3. Test non-existent file error handling
	try {
		ClusterDataLoader::LoadPoints2DFromCSV("non_existent_file.csv");
		assert(false);
	} catch (const std::runtime_error &e) {
		// expected
	}

	std::cout << "[PASSED] test_step3_data_loader completed successfully!\n";
	return 0;
}
