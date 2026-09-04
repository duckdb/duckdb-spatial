#pragma once

#include "spatial/geometry/spatial_index_interface.hpp"
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <string>

namespace duckdb {
namespace spatial {

// Point cluster assignment status
enum class ClusterStatus : int32_t {
	UNVISITED = -2,
	NOISE = -1
	// Non-negative values (0, 1, 2, ...) represent valid cluster assignments
};

// Input parameters for DBSCAN clustering
struct DBSCANParams {
	double eps;
	int64_t min_points;

	DBSCANParams() : eps(0.0), min_points(1) {}
	DBSCANParams(double eps_p, int64_t min_points_p) : eps(eps_p), min_points(min_points_p) {}

	void Validate() const {
		if (eps <= 0.0) {
			throw std::invalid_argument("DBSCAN parameter 'eps' must be greater than 0.0, got " + std::to_string(eps));
		}
		if (min_points <= 0) {
			throw std::invalid_argument("DBSCAN parameter 'min_points' must be at least 1, got " + std::to_string(min_points));
		}
	}
};

// Summary metrics for an individual cluster
struct ClusterSummary2D {
	int32_t cluster_id;
	size_t point_count;
	BoundingBox2D bbox;
	Point2D centroid;

	ClusterSummary2D() : cluster_id(-1), point_count(0), centroid(0.0, 0.0) {}
	ClusterSummary2D(int32_t id) : cluster_id(id), point_count(0), centroid(0.0, 0.0) {}
};

// Result container for DBSCAN clustering execution
class DBSCANResult {
public:
	DBSCANResult() : num_clusters_(0), num_noise_(0) {}

	explicit DBSCANResult(size_t point_count)
	    : cluster_ids_(point_count, static_cast<int32_t>(ClusterStatus::UNVISITED)),
	      num_clusters_(0),
	      num_noise_(0) {}

	size_t Size() const { return cluster_ids_.size(); }
	size_t NumClusters() const { return num_clusters_; }
	size_t NumNoise() const { return num_noise_; }

	int32_t GetClusterId(size_t idx) const {
		return cluster_ids_[idx];
	}

	void SetClusterId(size_t idx, int32_t cid) {
		cluster_ids_[idx] = cid;
	}

	bool IsNoise(size_t idx) const {
		return cluster_ids_[idx] == static_cast<int32_t>(ClusterStatus::NOISE);
	}

	bool IsUnvisited(size_t idx) const {
		return cluster_ids_[idx] == static_cast<int32_t>(ClusterStatus::UNVISITED);
	}

	bool IsClustered(size_t idx) const {
		return cluster_ids_[idx] >= 0;
	}

	const std::vector<int32_t> &GetClusterIds() const {
		return cluster_ids_;
	}

	std::vector<int32_t> &GetClusterIdsMutable() {
		return cluster_ids_;
	}

	void FinalizeMetrics(size_t cluster_count) {
		num_clusters_ = cluster_count;
		num_noise_ = 0;
		for (size_t i = 0; i < cluster_ids_.size(); ++i) {
			if (cluster_ids_[i] == static_cast<int32_t>(ClusterStatus::NOISE)) {
				num_noise_++;
			}
		}
	}

	// Compute summaries for each cluster (bounding box, centroid, point count)
	std::vector<ClusterSummary2D> ComputeSummaries(const ArrayView<Point2D> &points) const {
		if (points.size() != cluster_ids_.size()) {
			throw std::runtime_error("Point count mismatch in ComputeSummaries");
		}

		std::vector<ClusterSummary2D> summaries(num_clusters_);
		for (size_t c = 0; c < num_clusters_; ++c) {
			summaries[c].cluster_id = static_cast<int32_t>(c);
		}

		for (size_t i = 0; i < points.size(); ++i) {
			int32_t cid = cluster_ids_[i];
			if (cid >= 0 && static_cast<size_t>(cid) < num_clusters_) {
				summaries[cid].point_count++;
				summaries[cid].bbox.ExpandToInclude(points[i]);
				summaries[cid].centroid.x += points[i].x;
				summaries[cid].centroid.y += points[i].y;
			}
		}

		for (size_t c = 0; c < num_clusters_; ++c) {
			if (summaries[c].point_count > 0) {
				summaries[c].centroid.x /= summaries[c].point_count;
				summaries[c].centroid.y /= summaries[c].point_count;
			}
		}

		return summaries;
	}

private:
	std::vector<int32_t> cluster_ids_;
	size_t num_clusters_;
	size_t num_noise_;
};

} // namespace spatial
} // namespace duckdb
