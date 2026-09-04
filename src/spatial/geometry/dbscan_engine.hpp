#pragma once

#include "spatial/geometry/spatial_index_interface.hpp"
#include "spatial/geometry/cluster_types.hpp"
#include <queue>

namespace duckdb {
namespace spatial {

class DBSCANEngine {
public:
	// Execute DBSCAN clustering for 2D points using any SpatialIndex2D
	static DBSCANResult Cluster2D(const ArrayView<Point2D> &points,
	                             const SpatialIndex2D &index,
	                             const DBSCANParams &params);

	// Execute DBSCAN clustering for 3D points using any SpatialIndex3D
	static DBSCANResult Cluster3D(const ArrayView<Point3D> &points,
	                             const SpatialIndex3D &index,
	                             const DBSCANParams &params);
};

} // namespace spatial
} // namespace duckdb
