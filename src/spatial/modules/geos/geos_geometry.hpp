#pragma once

#include "geos_c.h"

#include "duckdb/common/vector.hpp"

namespace duckdb {

class GeosGeometry;
class PreparedGeosGeometry;

class GeosGeometry {
	friend class PreparedGeosGeometry;

public:
	// constructor
	GeosGeometry(GEOSContextHandle_t handle_p, GEOSGeometry *geom_p);

	// disable copy
	GeosGeometry(const GeosGeometry &) = delete;
	GeosGeometry &operator=(const GeosGeometry &) = delete;

	// support move
	GeosGeometry(GeosGeometry &&other) noexcept;
	GeosGeometry &operator=(GeosGeometry &&other) noexcept;

	// destructor
	~GeosGeometry();

public:
	GEOSGeomTypes type() const;
	const GEOSGeometry *get_raw() const;
	GEOSGeometry *get_raw();

	// Release the ownership of the geometry, but does NOT destroy it
	void leak();

	bool is_simple() const;
	bool is_ring() const;
	bool is_valid() const;
	bool is_empty() const;

	GeosGeometry get_boundary() const;
	GeosGeometry get_centroid() const;
	GeosGeometry get_convex_hull() const;
	GeosGeometry get_envelope() const;
	GeosGeometry get_minimum_rotated_rectangle() const;
	GeosGeometry get_reversed() const;
	GeosGeometry get_point_on_surface() const;
	GeosGeometry get_made_valid() const;
	GeosGeometry get_voronoi_diagram() const;
	GeosGeometry get_built_area() const;
	GeosGeometry get_noded() const;
	GeosGeometry get_clipped(double xmin, double ymin, double xmax, double ymax) const;

	// matrix format: [a, b, c, d, e, f]
	//	x' = a*x + b*y + e
	//	y' = c*x + d*y + f
	GeosGeometry get_transformed(const double matrix[6]) const;
	GeosGeometry get_gridded(double grid_size) const;

	bool contains(const GeosGeometry &other) const;
	bool covers(const GeosGeometry &other) const;
	bool covered_by(const GeosGeometry &other) const;
	bool crosses(const GeosGeometry &other) const;
	bool disjoint(const GeosGeometry &other) const;
	bool equals(const GeosGeometry &other) const;
	bool intersects(const GeosGeometry &other) const;
	bool overlaps(const GeosGeometry &other) const;
	bool touches(const GeosGeometry &other) const;
	bool within(const GeosGeometry &other) const;
	bool distance_within(const GeosGeometry &other, double distance) const;

	double distance_to(const GeosGeometry &other) const;

	void normalize_in_place() const;
	void orient_polygons(bool ext_cw);

	GeosGeometry get_difference(const GeosGeometry &other) const;
	GeosGeometry get_intersection(const GeosGeometry &other) const;
	GeosGeometry get_union(const GeosGeometry &other) const;
	GeosGeometry get_shortest_line(const GeosGeometry &other) const;

	GeosGeometry get_simplified(double tolerance) const;
	GeosGeometry get_simplified_topo(double tolerance) const;
	GeosGeometry get_without_repeated_points(double tolerance) const;
	GeosGeometry get_reduced_precision(double tolerance) const;
	GeosGeometry get_maximum_inscribed_circle(double tolerance) const;
	// default tolerance is max(height/width) / 1000
	GeosGeometry get_maximum_inscribed_circle() const;
	GeosGeometry get_point_n(int n) const;

	GeosGeometry get_linemerged(bool directed) const;
	GeosGeometry get_concave_hull(const double ratio, const bool allowHoles) const;
	GeosGeometry get_buffer(double distance, int quadsegs) const;
	GeosGeometry get_buffer_style(double distance, int quadsegs, int endcap_style, int join_style,
	                              double mitre_limit) const;

	GeosGeometry get_coverage_invalid_edges(double tolerance) const;
	GeosGeometry get_coverage_simplified(double tolerance, bool preserve_boundary) const;
	GeosGeometry get_coverage_union() const;

	PreparedGeosGeometry get_prepared() const;

	void get_extent(double &xmin, double &ymin, double &xmax, double &ymax) const {
		GEOSGeom_getExtent_r(handle, geom, &xmin, &ymin, &xmax, &ymax);
	}

private:
	GEOSContextHandle_t handle;
	GEOSGeometry *geom;
};

class GeosCollection {
public:
	explicit GeosCollection(GEOSContextHandle_t handle_p) : handle(handle_p) {
	}

	// disable copy
	GeosCollection(const GeosCollection &) = delete;
	GeosCollection &operator=(const GeosCollection &) = delete;

	// disable move
	GeosCollection(GeosCollection &&) = delete;
	GeosCollection &operator=(GeosCollection &&) = delete;

	void add(GeosGeometry &&geom) {
		geometries.push_back(std::move(geom));
		pointers.push_back(geometries.back().get_raw());
	}

	void clear() {
		geometries.clear();
		pointers.clear();
	}

	void reserve(size_t size) {
		geometries.reserve(size);
		pointers.reserve(size);
	}

	GeosGeometry get_polygonized() const {
		return GeosGeometry(handle, GEOSPolygonize_r(handle, pointers.data(), pointers.size()));
	}

	GeosGeometry get_collection() {
		auto res = GeosGeometry(
		    handle, GEOSGeom_createCollection_r(handle, GEOS_GEOMETRYCOLLECTION, pointers.data(), pointers.size()));

		// Because the GEOSGeometry takes ownership of the geometries, we need to leak them here
		for (auto &geom : geometries) {
			geom.leak();
		}

		// Now we can clear safely
		geometries.clear();
		pointers.clear();
		return res;
	}

private:
	GEOSContextHandle_t handle;
	vector<GeosGeometry> geometries;
	vector<GEOSGeometry *> pointers;
};

class PreparedGeosGeometry {
	friend class GeosGeometry;

public:
	// constructor
	PreparedGeosGeometry(GEOSContextHandle_t handle_p, const GeosGeometry &geom);

	// disable copy
	PreparedGeosGeometry(const PreparedGeosGeometry &) = delete;
	PreparedGeosGeometry &operator=(const PreparedGeosGeometry &) = delete;

	// support move
	PreparedGeosGeometry(PreparedGeosGeometry &&other) noexcept;
	PreparedGeosGeometry &operator=(PreparedGeosGeometry &&other) noexcept;

	~PreparedGeosGeometry();

public:
	bool contains(const GeosGeometry &other) const;
	bool contains_properly(const GeosGeometry &other) const;
	bool covers(const GeosGeometry &other) const;
	bool covered_by(const GeosGeometry &other) const;
	bool crosses(const GeosGeometry &other) const;
	bool disjoint(const GeosGeometry &other) const;
	bool intersects(const GeosGeometry &other) const;
	bool overlaps(const GeosGeometry &other) const;
	bool touches(const GeosGeometry &other) const;
	bool within(const GeosGeometry &other) const;

	double distance_to(const GeosGeometry &other) const;
	bool distance_within(const GeosGeometry &other, double distance) const;

private:
	GEOSContextHandle_t handle;
	const GEOSPreparedGeometry *prepared;
};

//------------------------------------------------------------------------------
// Lifecycle methods
//------------------------------------------------------------------------------

//-- GeosGeometry --//
inline GeosGeometry::GeosGeometry(GEOSContextHandle_t handle_p, GEOSGeometry *geom_p) : handle(handle_p), geom(geom_p) {
}
inline GeosGeometry::GeosGeometry(GeosGeometry &&other) noexcept : handle(other.handle), geom(other.geom) {
	other.geom = nullptr;
	other.handle = nullptr;
}
inline GeosGeometry &GeosGeometry::operator=(GeosGeometry &&other) noexcept {
	if (this != &other) {
		if (geom) {
			GEOSGeom_destroy_r(handle, geom);
		}
		handle = other.handle;
		geom = other.geom;
		other.geom = nullptr;
	}
	return *this;
}

inline GeosGeometry::~GeosGeometry() {
	if (geom) {
		GEOSGeom_destroy_r(handle, geom);
	}
}

//-- PreparedGeosGeometry --//
inline PreparedGeosGeometry::PreparedGeosGeometry(GEOSContextHandle_t handle_p, const GeosGeometry &geom)
    : handle(handle_p) {
	prepared = GEOSPrepare_r(handle, geom.geom);
}
inline PreparedGeosGeometry::PreparedGeosGeometry(PreparedGeosGeometry &&other) noexcept
    : handle(other.handle), prepared(other.prepared) {
	other.prepared = nullptr;
}

inline PreparedGeosGeometry &PreparedGeosGeometry::operator=(PreparedGeosGeometry &&other) noexcept {
	if (this != &other) {
		if (prepared) {
			GEOSPreparedGeom_destroy_r(handle, prepared);
		}
		handle = other.handle;
		prepared = other.prepared;
		other.prepared = nullptr;
	}
	return *this;
}

inline PreparedGeosGeometry::~PreparedGeosGeometry() {
	if (prepared) {
		GEOSPreparedGeom_destroy_r(handle, prepared);
	}
}

//------------------------------------------------------------------------------
// Methods
//------------------------------------------------------------------------------

//-- GeosGeometry --//
inline GEOSGeomTypes GeosGeometry::type() const {
	return static_cast<GEOSGeomTypes>(GEOSGeomTypeId_r(handle, geom));
}

inline const GEOSGeometry *GeosGeometry::get_raw() const {
	return geom;
}

inline GEOSGeometry *GeosGeometry::get_raw() {
	return geom;
}

inline void GeosGeometry::leak() {
	handle = nullptr;
	geom = nullptr;
}

inline bool GeosGeometry::is_simple() const {
	return GEOSisSimple_r(handle, geom);
}

inline bool GeosGeometry::is_ring() const {
	return GEOSisRing_r(handle, geom);
}

inline bool GeosGeometry::is_valid() const {
	return GEOSisValid_r(handle, geom);
}

inline bool GeosGeometry::is_empty() const {
	return GEOSisEmpty_r(handle, geom);
}

inline GeosGeometry GeosGeometry::get_boundary() const {
	return GeosGeometry(handle, GEOSBoundary_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_centroid() const {
	return GeosGeometry(handle, GEOSGetCentroid_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_concave_hull(const double ratio, const bool allowHoles) const {
	return GeosGeometry(handle, GEOSConcaveHull_r(handle, geom, ratio, allowHoles));
}

inline GeosGeometry GeosGeometry::get_convex_hull() const {
	return GeosGeometry(handle, GEOSConvexHull_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_envelope() const {
	return GeosGeometry(handle, GEOSEnvelope_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_reversed() const {
	return GeosGeometry(handle, GEOSReverse_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_point_on_surface() const {
	return GeosGeometry(handle, GEOSPointOnSurface_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_made_valid() const {
	return GeosGeometry(handle, GEOSMakeValid_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_minimum_rotated_rectangle() const {
	return GeosGeometry(handle, GEOSMinimumRotatedRectangle_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_voronoi_diagram() const {
	return GeosGeometry(handle, GEOSVoronoiDiagram_r(handle, geom, nullptr, 0, 0));
}

inline GeosGeometry GeosGeometry::get_built_area() const {
	return GeosGeometry(handle, GEOSBuildArea_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_noded() const {
	return GeosGeometry(handle, GEOSNode_r(handle, geom));
}

inline GeosGeometry GeosGeometry::get_clipped(double xmin, double ymin, double xmax, double ymax) const {
	return GeosGeometry(handle, GEOSClipByRect_r(handle, geom, xmin, ymin, xmax, ymax));
}

inline GeosGeometry GeosGeometry::get_transformed(const double matrix[6]) const {
	// x' = a*x + b*y + e
	// y' = c*x + d*y + f
	return GeosGeometry(handle, GEOSGeom_transformXY_r(
	                                handle, geom,
	                                [](double *x_ptr, double *y_ptr, void *data) -> int {
		                                const auto m = static_cast<const double *>(data);
		                                const auto &x = *x_ptr;
		                                const auto &y = *y_ptr;

		                                const auto new_x = m[0] * x + m[1] * y + m[4];
		                                const auto new_y = m[2] * x + m[3] * y + m[5];

		                                *x_ptr = new_x;
		                                *y_ptr = new_y;
		                                return 1;
	                                },
	                                const_cast<double *>(matrix)));
}

inline GeosGeometry GeosGeometry::get_gridded(double grid_size) const {
	return GeosGeometry(handle, GEOSGeom_setPrecision_r(handle, geom, grid_size, GEOS_PREC_VALID_OUTPUT));
}

inline GeosGeometry GeosGeometry::get_maximum_inscribed_circle() const {
	double xmin = 0;
	double ymin = 0;
	double xmax = 0;
	double ymax = 0;

	GEOSGeom_getExtent_r(handle, geom, &xmin, &ymin, &xmax, &ymax);

	const auto width = xmax - xmin;
	const auto height = ymax - ymin;
	const auto length = (width > height ? height : width);
	const auto tolerance = length == 0 ? 0 : length / 1000;

	return get_maximum_inscribed_circle(tolerance);
}

inline GeosGeometry GeosGeometry::get_maximum_inscribed_circle(double tolerance) const {
	return GeosGeometry(handle, GEOSMaximumInscribedCircle_r(handle, geom, tolerance));
}

inline GeosGeometry GeosGeometry::get_point_n(int n) const {
	const auto point = GEOSGeomGetPointN_r(handle, geom, n);
	return GeosGeometry(handle, point);
}

inline bool GeosGeometry::contains(const GeosGeometry &other) const {
	return GEOSContains_r(handle, geom, other.geom);
}

inline bool GeosGeometry::covers(const GeosGeometry &other) const {
	return GEOSCovers_r(handle, geom, other.geom);
}

inline bool GeosGeometry::covered_by(const GeosGeometry &other) const {
	return GEOSCoveredBy_r(handle, geom, other.geom);
}

inline bool GeosGeometry::crosses(const GeosGeometry &other) const {
	return GEOSCrosses_r(handle, geom, other.geom);
}

inline bool GeosGeometry::disjoint(const GeosGeometry &other) const {
	return GEOSDisjoint_r(handle, geom, other.geom);
}

inline bool GeosGeometry::equals(const GeosGeometry &other) const {
	return GEOSEquals_r(handle, geom, other.geom);
}

inline bool GeosGeometry::intersects(const GeosGeometry &other) const {
	return GEOSIntersects_r(handle, geom, other.geom);
}

inline bool GeosGeometry::overlaps(const GeosGeometry &other) const {
	return GEOSOverlaps_r(handle, geom, other.geom);
}

inline bool GeosGeometry::touches(const GeosGeometry &other) const {
	return GEOSTouches_r(handle, geom, other.geom);
}

inline bool GeosGeometry::within(const GeosGeometry &other) const {
	return GEOSWithin_r(handle, geom, other.geom);
}

inline bool GeosGeometry::distance_within(const GeosGeometry &other, double distance) const {
	return GEOSDistanceWithin_r(handle, geom, other.geom, distance);
}

inline double GeosGeometry::distance_to(const GeosGeometry &other) const {
	double distance = 0;
	GEOSDistance_r(handle, geom, other.geom, &distance);
	return distance;
}

inline void GeosGeometry::normalize_in_place() const {
	GEOSNormalize_r(handle, geom);
}

inline void GeosGeometry::orient_polygons(bool ext_cw) {
	GEOSOrientPolygons_r(handle, geom, ext_cw ? 1 : 0);
}

inline GeosGeometry GeosGeometry::get_difference(const GeosGeometry &other) const {
	return GeosGeometry(handle, GEOSDifference_r(handle, geom, other.geom));
}

inline GeosGeometry GeosGeometry::get_intersection(const GeosGeometry &other) const {
	return GeosGeometry(handle, GEOSIntersection_r(handle, geom, other.geom));
}

inline GeosGeometry GeosGeometry::get_union(const GeosGeometry &other) const {
	return GeosGeometry(handle, GEOSUnion_r(handle, geom, other.geom));
}

inline GeosGeometry GeosGeometry::get_shortest_line(const GeosGeometry &other) const {
	const auto line = GEOSNearestPoints_r(handle, geom, other.geom);
	const auto line_geom = GEOSGeom_createLineString_r(handle, line);
	return GeosGeometry(handle, line_geom);
}

inline GeosGeometry GeosGeometry::get_simplified(double tolerance) const {
	return GeosGeometry(handle, GEOSSimplify_r(handle, geom, tolerance));
}

inline GeosGeometry GeosGeometry::get_simplified_topo(double tolerance) const {
	return GeosGeometry(handle, GEOSTopologyPreserveSimplify_r(handle, geom, tolerance));
}

inline GeosGeometry GeosGeometry::get_without_repeated_points(double tolerance) const {
	const auto simplified = GEOSRemoveRepeatedPoints_r(handle, geom, tolerance);
	return GeosGeometry(handle, simplified);
}

inline GeosGeometry GeosGeometry::get_reduced_precision(double tolerance) const {
	const auto reduced = GEOSGeom_setPrecision_r(handle, geom, tolerance, 0);
	return GeosGeometry(handle, reduced);
}

inline GeosGeometry GeosGeometry::get_linemerged(bool directed) const {
	const auto merged = directed ? GEOSLineMergeDirected_r(handle, geom) : GEOSLineMerge_r(handle, geom);
	return GeosGeometry(handle, merged);
}

inline GeosGeometry GeosGeometry::get_buffer(double distance, int quadsegs) const {
	const auto buffer = GEOSBuffer_r(handle, geom, distance, quadsegs);
	return GeosGeometry(handle, buffer);
}

inline GeosGeometry GeosGeometry::get_buffer_style(double distance, int quadsegs, int endcap_style, int join_style,
                                                   double mitre_limit) const {
	const auto buffer = GEOSBufferWithStyle_r(handle, geom, distance, quadsegs, endcap_style, join_style, mitre_limit);
	return GeosGeometry(handle, buffer);
}

inline GeosGeometry GeosGeometry::get_coverage_invalid_edges(double tolerance) const {
	GEOSGeometry *output = nullptr;
	GEOSCoverageIsValid_r(handle, geom, tolerance, &output);
	return GeosGeometry(handle, output);
}
inline GeosGeometry GeosGeometry::get_coverage_simplified(double tolerance, bool preserve_boundary) const {
	return GeosGeometry(handle, GEOSCoverageSimplifyVW_r(handle, geom, tolerance, preserve_boundary));
}

inline GeosGeometry GeosGeometry::get_coverage_union() const {
	return GeosGeometry(handle, GEOSCoverageUnion_r(handle, geom));
}

inline PreparedGeosGeometry GeosGeometry::get_prepared() const {
	return PreparedGeosGeometry(handle, *this);
}

//-- PreparedGeosGeometry --//

inline bool PreparedGeosGeometry::contains(const GeosGeometry &other) const {
	return GEOSPreparedContains_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::contains_properly(const GeosGeometry &other) const {
	return GEOSPreparedContainsProperly_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::covers(const GeosGeometry &other) const {
	return GEOSPreparedCovers_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::covered_by(const GeosGeometry &other) const {
	return GEOSPreparedCoveredBy_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::crosses(const GeosGeometry &other) const {
	return GEOSPreparedCrosses_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::disjoint(const GeosGeometry &other) const {
	return GEOSPreparedDisjoint_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::intersects(const GeosGeometry &other) const {
	return GEOSPreparedIntersects_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::overlaps(const GeosGeometry &other) const {
	return GEOSPreparedOverlaps_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::touches(const GeosGeometry &other) const {
	return GEOSPreparedTouches_r(handle, prepared, other.geom);
}

inline bool PreparedGeosGeometry::within(const GeosGeometry &other) const {
	return GEOSPreparedWithin_r(handle, prepared, other.geom);
}

inline double PreparedGeosGeometry::distance_to(const GeosGeometry &other) const {
	double distance = 0;
	GEOSPreparedDistance_r(handle, prepared, other.geom, &distance);
	return distance;
}

inline bool PreparedGeosGeometry::distance_within(const GeosGeometry &other, double distance) const {
	return GEOSPreparedDistanceWithin_r(handle, prepared, other.geom, distance);
}

} // namespace duckdb
