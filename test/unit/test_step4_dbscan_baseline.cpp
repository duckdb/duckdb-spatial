#include "spatial/geometry/dbscan_engine.hpp"
#include "spatial/geometry/cluster_data_loader.hpp"
#include <iostream>
#include <cassert>

using namespace duckdb::spatial;

class NaiveLinearSpatialIndex2D : public SpatialIndex2D {
public:
	void Build(const ArrayView<Point2D> &points) override {
		points_.clear();
		for (size_t i = 0; i < points.size(); ++i) {
			points_.push_back(points[i]);
		}
	}

	void RadiusSearch(const Point2D &center, double eps, std::vector<size_t> &matches) const override {
		matches.clear();
		const double eps_sq = eps * eps;
		for (size_t i = 0; i < points_.size(); ++i) {
			if (center.DistanceSquared(points_[i]) <= eps_sq) {
				matches.push_back(i);
			}
		}
	}

	size_t Count() const override {
		return points_.size();
	}

private:
	std::vector<Point2D> points_;
};

int main() {
	std::cout << "[RUNNING] test_step4_dbscan_baseline...\n";

	// 1. Synthetic dataset with 2 clusters and 1 noise point
	// Cluster A: (0,0), (0.1, 0), (0, 0.1), (0.1, 0.1)
	// Cluster B: (5,5), (5.1, 5), (5, 5.1), (5.1, 5.1)
	// Noise: (2.5, 2.5)
	// Border point: (0.2, 0.2) which has distance ~0.14 from (0.1, 0.1) but only 1 neighbor
	std::vector<Point2D> pts = {
		Point2D(0.0, 0.0), Point2D(0.1, 0.0), Point2D(0.0, 0.1), Point2D(0.1, 0.1),
		Point2D(5.0, 5.0), Point2D(5.1, 5.0), Point2D(5.0, 5.1), Point2D(5.1, 5.1),
		Point2D(2.5, 2.5),
		Point2D(0.2, 0.2) // border point near Cluster A
	};

	NaiveLinearSpatialIndex2D index;
	index.Build(ArrayView<Point2D>(pts));

	// eps = 0.15, min_points = 3
	DBSCANParams params(0.15, 3);
	auto result = DBSCANEngine::Cluster2D(ArrayView<Point2D>(pts), index, params);

	assert(result.NumClusters() == 2);
	assert(result.NumNoise() == 1);
	assert(result.IsNoise(8)); // point (2.5, 2.5) is noise
	assert(result.GetClusterId(0) == 0); // Cluster A
	assert(result.GetClusterId(4) == 1); // Cluster B
	assert(result.GetClusterId(9) == 0); // Border point correctly absorbed into Cluster A!

	// 2. Real dataset test: sample2d.csv
	auto sample2d = ClusterDataLoader::LoadPoints2DFromCSV("dbscan/sample2d.csv");
	NaiveLinearSpatialIndex2D sample_index;
	sample_index.Build(ArrayView<Point2D>(sample2d));

	DBSCANParams sample_params(0.2, 10);
	auto sample_result = DBSCANEngine::Cluster2D(ArrayView<Point2D>(sample2d), sample_index, sample_params);

	std::cout << "  sample2d.csv DBSCAN result: " << sample_result.NumClusters() 
	          << " clusters, " << sample_result.NumNoise() << " noise points\n";

	assert(sample_result.NumClusters() == 4);
	assert(sample_result.NumNoise() > 0);

	std::cout << "[PASSED] test_step4_dbscan_baseline completed successfully!\n";
	return 0;
}
