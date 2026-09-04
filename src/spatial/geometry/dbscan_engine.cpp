#include "spatial/geometry/dbscan_engine.hpp"

namespace duckdb {
namespace spatial {

DBSCANResult DBSCANEngine::Cluster2D(const ArrayView<Point2D> &points,
                                    const SpatialIndex2D &index,
                                    const DBSCANParams &params) {
	params.Validate();

	const size_t n_points = points.size();
	DBSCANResult result(n_points);

	if (n_points == 0) {
		result.FinalizeMetrics(0);
		return result;
	}

	size_t cluster_count = 0;
	std::vector<size_t> neighbors;
	std::vector<size_t> sub_neighbors;
	std::queue<size_t> worklist;
	std::vector<bool> in_queue(n_points, false);

	for (size_t i = 0; i < n_points; ++i) {
		if (!result.IsUnvisited(i)) {
			continue;
		}

		index.RadiusSearch(points[i], params.eps, neighbors);

		if (neighbors.size() < static_cast<size_t>(params.min_points)) {
			result.SetClusterId(i, static_cast<int32_t>(ClusterStatus::NOISE));
			continue;
		}

		// Point i is a core point: create a new cluster
		const int32_t current_cluster = static_cast<int32_t>(cluster_count++);
		result.SetClusterId(i, current_cluster);

		for (size_t nb : neighbors) {
			if (nb != i && !in_queue[nb]) {
				worklist.push(nb);
				in_queue[nb] = true;
			}
		}

		while (!worklist.empty()) {
			size_t q = worklist.front();
			worklist.pop();
			in_queue[q] = false;

			int32_t q_status = result.GetClusterId(q);

			// If point q was previously labelled as NOISE, it is a valid border point!
			if (q_status == static_cast<int32_t>(ClusterStatus::NOISE)) {
				result.SetClusterId(q, current_cluster);
			}

			// If point q was already processed or clustered, skip expanding it
			if (q_status != static_cast<int32_t>(ClusterStatus::UNVISITED)) {
				continue;
			}

			// Point q is unvisited: assign it to the current cluster
			result.SetClusterId(q, current_cluster);

			// Check if point q is also a core point
			index.RadiusSearch(points[q], params.eps, sub_neighbors);
			if (sub_neighbors.size() >= static_cast<size_t>(params.min_points)) {
				for (size_t sub_nb : sub_neighbors) {
					int32_t sub_status = result.GetClusterId(sub_nb);
					if ((sub_status == static_cast<int32_t>(ClusterStatus::UNVISITED) ||
					     sub_status == static_cast<int32_t>(ClusterStatus::NOISE)) &&
					    !in_queue[sub_nb]) {
						worklist.push(sub_nb);
						in_queue[sub_nb] = true;
					}
				}
			}
		}
	}

	result.FinalizeMetrics(cluster_count);
	return result;
}

DBSCANResult DBSCANEngine::Cluster3D(const ArrayView<Point3D> &points,
                                    const SpatialIndex3D &index,
                                    const DBSCANParams &params) {
	params.Validate();

	const size_t n_points = points.size();
	DBSCANResult result(n_points);

	if (n_points == 0) {
		result.FinalizeMetrics(0);
		return result;
	}

	size_t cluster_count = 0;
	std::vector<size_t> neighbors;
	std::vector<size_t> sub_neighbors;
	std::queue<size_t> worklist;
	std::vector<bool> in_queue(n_points, false);

	for (size_t i = 0; i < n_points; ++i) {
		if (!result.IsUnvisited(i)) {
			continue;
		}

		index.RadiusSearch(points[i], params.eps, neighbors);

		if (neighbors.size() < static_cast<size_t>(params.min_points)) {
			result.SetClusterId(i, static_cast<int32_t>(ClusterStatus::NOISE));
			continue;
		}

		const int32_t current_cluster = static_cast<int32_t>(cluster_count++);
		result.SetClusterId(i, current_cluster);

		for (size_t nb : neighbors) {
			if (nb != i && !in_queue[nb]) {
				worklist.push(nb);
				in_queue[nb] = true;
			}
		}

		while (!worklist.empty()) {
			size_t q = worklist.front();
			worklist.pop();
			in_queue[q] = false;

			int32_t q_status = result.GetClusterId(q);

			if (q_status == static_cast<int32_t>(ClusterStatus::NOISE)) {
				result.SetClusterId(q, current_cluster);
			}

			if (q_status != static_cast<int32_t>(ClusterStatus::UNVISITED)) {
				continue;
			}

			result.SetClusterId(q, current_cluster);

			index.RadiusSearch(points[q], params.eps, sub_neighbors);
			if (sub_neighbors.size() >= static_cast<size_t>(params.min_points)) {
				for (size_t sub_nb : sub_neighbors) {
					int32_t sub_status = result.GetClusterId(sub_nb);
					if ((sub_status == static_cast<int32_t>(ClusterStatus::UNVISITED) ||
					     sub_status == static_cast<int32_t>(ClusterStatus::NOISE)) &&
					    !in_queue[sub_nb]) {
						worklist.push(sub_nb);
						in_queue[sub_nb] = true;
					}
				}
			}
		}
	}

	result.FinalizeMetrics(cluster_count);
	return result;
}

} // namespace spatial
} // namespace duckdb
