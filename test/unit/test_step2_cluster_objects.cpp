#include "spatial/geometry/cluster_types.hpp"
#include <iostream>
#include <cassert>

using namespace duckdb::spatial;

int main() {
	std::cout << "[RUNNING] test_step2_cluster_objects...\n";

	// 1. Test Parameter Validation
	DBSCANParams valid_params(0.5, 5);
	valid_params.Validate(); // should not throw

	try {
		DBSCANParams invalid_eps(-0.1, 5);
		invalid_eps.Validate();
		assert(false); // should not reach here
	} catch (const std::invalid_argument &) {
		// expected
	}

	try {
		DBSCANParams invalid_pts(0.5, 0);
		invalid_pts.Validate();
		assert(false);
	} catch (const std::invalid_argument &) {
		// expected
	}

	// 2. Test DBSCANResult initialization and mutations
	DBSCANResult result(5);
	assert(result.Size() == 5);
	for (size_t i = 0; i < 5; ++i) {
		assert(result.IsUnvisited(i));
		assert(!result.IsClustered(i));
		assert(!result.IsNoise(i));
	}

	// Simulate cluster assignment
	result.SetClusterId(0, 0);
	result.SetClusterId(1, 0);
	result.SetClusterId(2, 1);
	result.SetClusterId(3, static_cast<int32_t>(ClusterStatus::NOISE));
	result.SetClusterId(4, 1);

	assert(result.IsClustered(0));
	assert(result.IsClustered(1));
	assert(result.IsClustered(2));
	assert(result.IsNoise(3));
	assert(result.IsClustered(4));

	result.FinalizeMetrics(2);
	assert(result.NumClusters() == 2);
	assert(result.NumNoise() == 1);

	// 3. Test Cluster Summaries (Centroid & Bbox)
	std::vector<Point2D> pts = {
		Point2D(0.0, 0.0), // cluster 0
		Point2D(2.0, 0.0), // cluster 0 -> centroid (1.0, 0.0)
		Point2D(10.0, 10.0), // cluster 1
		Point2D(100.0, 100.0), // noise
		Point2D(12.0, 12.0)  // cluster 1 -> centroid (11.0, 11.0)
	};

	auto summaries = result.ComputeSummaries(ArrayView<Point2D>(pts));
	assert(summaries.size() == 2);

	// Cluster 0
	assert(summaries[0].cluster_id == 0);
	assert(summaries[0].point_count == 2);
	assert(std::abs(summaries[0].centroid.x - 1.0) < 1e-9);
	assert(std::abs(summaries[0].centroid.y - 0.0) < 1e-9);
	assert(summaries[0].bbox.min_x == 0.0 && summaries[0].bbox.max_x == 2.0);

	// Cluster 1
	assert(summaries[1].cluster_id == 1);
	assert(summaries[1].point_count == 2);
	assert(std::abs(summaries[1].centroid.x - 11.0) < 1e-9);
	assert(std::abs(summaries[1].centroid.y - 11.0) < 1e-9);

	std::cout << "[PASSED] test_step2_cluster_objects completed successfully!\n";
	return 0;
}
