#include "sgl.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace sgl {
// We don't care if we leak memory here, this is just testing.
// In the future, we should provide a real arena allocator and move it into the library
class arena_allocator final : public allocator {
public:
	void *alloc(size_t size) override {
		return ::malloc(size);
	}

	void dealloc(void *ptr, size_t size) override {
		return ::free(ptr);
	}

	void *realloc(void *ptr, size_t old_size, size_t new_size) override {
		return ::realloc(ptr, new_size);
	}
};
} // namespace sgl

void test_allocator() {
	// Coverage
	sgl::arena_allocator allocator;
	void *ptr = allocator.alloc(100);
	assert(ptr != nullptr);
	void *new_ptr = allocator.realloc(ptr, 100, 200);
	assert(new_ptr != nullptr);
	allocator.dealloc(new_ptr, 200);
}

void test_wkt_parsing() {

	sgl::arena_allocator alloc;

	sgl::wkt_reader reader(alloc);

	sgl::geometry geom;
	const auto point_wkt = "POINT(1 2)";

	const auto line_wkt = "LINESTRING(1 2, 3 4)";
	const auto polygon_wkt = "POLYGON((1 2, 3 4, 5 6, 1 2))";
	const auto multi_point_wkt = "MULTIPOINT(1 2, 3 4)";
	const auto multi_line_wkt = "MULTILINESTRING((1 2, 3 4), (5 6, 7 8))";
	const auto multi_polygon_wkt = "MULTIPOLYGON(((1 2, 3 4, 5 6, 1 2)), ((7 8, 9 10, 11 12, 7 8)))";
	const auto geometry_collection_wkt = "GEOMETRYCOLLECTION(POINT(1 2), LINESTRING(3 4, 5 6))";

	const auto multipoint_extra_paren_wkt = "MULTIPOINT((1 2), (3 4))";

	assert(reader.try_parse(geom, point_wkt));
	assert(geom.get_type() == sgl::geometry_type::POINT);
	assert(!geom.is_multi_part() && !geom.is_multi_geom());

	assert(reader.try_parse(geom, line_wkt));
	assert(geom.get_type() == sgl::geometry_type::LINESTRING);
	assert(!geom.is_multi_part() && !geom.is_multi_geom());

	assert(reader.try_parse(geom, polygon_wkt));
	assert(geom.get_type() == sgl::geometry_type::POLYGON);
	assert(geom.is_multi_part() && !geom.is_multi_geom());

	assert(reader.try_parse(geom, multi_point_wkt));
	assert(geom.get_type() == sgl::geometry_type::MULTI_POINT);
	assert(geom.is_multi_part() && geom.is_multi_geom());

	assert(reader.try_parse(geom, multi_line_wkt));
	assert(geom.get_type() == sgl::geometry_type::MULTI_LINESTRING);
	assert(geom.is_multi_part() && geom.is_multi_geom());

	assert(reader.try_parse(geom, multi_polygon_wkt));
	assert(geom.get_type() == sgl::geometry_type::MULTI_POLYGON);
	assert(geom.is_multi_part() && geom.is_multi_geom());

	assert(reader.try_parse(geom, geometry_collection_wkt));
	assert(geom.get_type() == sgl::geometry_type::GEOMETRY_COLLECTION);
	assert(geom.is_multi_part() && geom.is_multi_geom());

	assert(geom.get_part_count() == 2);
	assert(geom.get_first_part()->get_type() == sgl::geometry_type::POINT);
	assert(geom.get_last_part()->get_type() == sgl::geometry_type::LINESTRING);
	assert(geom.get_first_part()->get_next() == geom.get_last_part());
	assert(geom.get_first_part()->get_parent() == &geom);
	assert(geom.get_last_part()->get_parent() == &geom);

	assert(reader.try_parse(geom, multipoint_extra_paren_wkt));
	assert(geom.get_type() == sgl::geometry_type::MULTI_POINT);
	assert(geom.is_multi_part() && geom.is_multi_geom());

	// Test failures
	assert(!reader.try_parse(geom, "FOOBAR(1 2 3)"));                                         // Invalid type
	assert(!reader.try_parse(geom, "INVALID (1 2"));                                          // Invalid type
	assert(!reader.try_parse(geom, "GEOMETRYCOLLECTION (POINT Z (1 2 3), POINT M (4 5 6))")); // Mixed ZM
	assert(reader.try_parse(geom, "SRID=1234;POINT(1 2)"));                                   // SRID is ignored

	// This is just for coverage
	assert(geom.get_extra() == 0);
}

void test_euclidean_length() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);

	sgl::geometry geom;

	// Test length of a point
	const auto point_wkt = "POINT(1 2)";
	assert(reader.try_parse(geom, point_wkt));
	assert(sgl::ops::get_length(geom) == 0.0);

	// Test length of a linestring
	const auto line_wkt = "LINESTRING(1 1, 1 3, 3 3)";
	assert(reader.try_parse(geom, line_wkt));
	assert(sgl::ops::get_length(geom) == 4.0);

	// Test length of a multilinestring
	const auto mline_wkt = "MULTILINESTRING((1 1, 1 3), (3 3, 3 1))";
	assert(reader.try_parse(geom, mline_wkt));
	assert(sgl::ops::get_length(geom) == 4.0);

	// Test length of an empty line
	const auto empty_wkt = "LINESTRING EMPTY";
	assert(reader.try_parse(geom, empty_wkt));
	assert(sgl::ops::get_length(geom) == 0.0);

	// Test length of an empty multilinestring
	const auto empty_mline_wkt = "MULTILINESTRING EMPTY";
	assert(reader.try_parse(geom, empty_mline_wkt));
	assert(sgl::ops::get_length(geom) == 0.0);

	// Test length of an empty geometrycollection
	const auto empty_geom_wkt = "GEOMETRYCOLLECTION EMPTY";
	assert(reader.try_parse(geom, empty_geom_wkt));
	assert(sgl::ops::get_length(geom) == 0.0);
}

void test_euclidean_area() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);
	sgl::geometry geom;

	// Test area of a point
	const auto point_wkt = "POINT(1 2)";
	assert(reader.try_parse(geom, point_wkt));
	assert(sgl::ops::get_area(geom) == 0.0);

	// Test area of a polygon (no holes)
	const auto polygon_wkt = "POLYGON((1 1, 1 3, 3 3, 3 1, 1 1))";
	assert(reader.try_parse(geom, polygon_wkt));
	assert(sgl::ops::get_area(geom) == 4.0);

	// Test area of a polygon (with holes)
	const auto polygon_with_hole_wkt = "POLYGON((1 1, 1 3, 3 3, 3 1, 1 1), (2 2, 2 2.5, 2.5 2.5, 2.5 2, 2 2))";
	assert(reader.try_parse(geom, polygon_with_hole_wkt));
	assert(sgl::ops::get_area(geom) == 3.75);

	// Test area of multipolygon
	const auto multipolygon_wkt = "MULTIPOLYGON(((1 1, 1 3, 3 3, 3 1, 1 1)), ((4 4, 4 6, 6 6, 6 4, 4 4)))";
	assert(reader.try_parse(geom, multipolygon_wkt));
	assert(sgl::ops::get_area(geom) == 8.0);

	// Test area of an empty polygon
	const auto empty_polygon_wkt = "POLYGON EMPTY";
	assert(reader.try_parse(geom, empty_polygon_wkt));
	assert(sgl::ops::get_area(geom) == 0.0);

	// Test area of an empty multipolygon
	const auto empty_multipolygon_wkt = "MULTIPOLYGON EMPTY";
	assert(reader.try_parse(geom, empty_multipolygon_wkt));
	assert(sgl::ops::get_area(geom) == 0.0);

	// Test area of degenerate polygon
	const auto degenerate_polygon_wkt = "POLYGON((1 1, 1 1))";
	assert(reader.try_parse(geom, degenerate_polygon_wkt));
	assert(sgl::ops::get_area(geom) == 0.0);
}

void test_euclidean_perimeter() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);
	sgl::geometry geom;

	const auto point_wkt = "POINT(1 2)";
	assert(reader.try_parse(geom, point_wkt));
	assert(sgl::ops::get_perimeter(geom) == 0.0);

	const auto polygon_wkt = "POLYGON((1 1, 1 3, 3 3, 3 1, 1 1))";
	assert(reader.try_parse(geom, polygon_wkt));
	assert(sgl::ops::get_perimeter(geom) == 8.0);

	const auto polygon_wkt_with_hole = "POLYGON((1 1, 1 3, 3 3, 3 1, 1 1), (2 2, 2 2.5, 2.5 2.5, 2.5 2, 2 2))";
	assert(reader.try_parse(geom, polygon_wkt_with_hole));
	assert(sgl::ops::get_perimeter(geom) == 10.0);

	const auto multipolygon_wkt = "MULTIPOLYGON(((1 1, 1 3, 3 3, 3 1, 1 1)), ((4 4, 4 6, 6 6, 6 4, 4 4)))";
	assert(reader.try_parse(geom, multipolygon_wkt));
	assert(sgl::ops::get_perimeter(geom) == 16.0);

	const auto empty_polygon_wkt = "POLYGON EMPTY";
	assert(reader.try_parse(geom, empty_polygon_wkt));
	assert(sgl::ops::get_perimeter(geom) == 0.0);

	const auto empty_multipolygon_wkt = "MULTIPOLYGON EMPTY";
	assert(reader.try_parse(geom, empty_multipolygon_wkt));
	assert(sgl::ops::get_perimeter(geom) == 0.0);
}

void test_euclidean_centroid() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);
	sgl::geometry geom;
	sgl::vertex_xyzm centroid = {0, 0, 0, 0};

	// Invalid geometry
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_points(geom, centroid));
	assert(!sgl::ops::get_centroid_from_linestrings(geom, centroid));
	assert(!sgl::ops::get_centroid_from_polygons(geom, centroid));

	// Coverage: invalid non-empty geometry
	geom.set_vertex_array(nullptr, 10);
	assert(!sgl::ops::get_centroid(geom, centroid));

	geom.reset();

	// Point EMPTY
	geom.set_type(sgl::geometry_type::POINT);
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_points(geom, centroid));
	assert(centroid.x == 0.0 && centroid.y == 0.0 && centroid.z == 0.0 && centroid.m == 0.0);

	// Linestring EMPTY
	geom.set_type(sgl::geometry_type::LINESTRING);
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_linestrings(geom, centroid));
	assert(centroid.x == 0.0 && centroid.y == 0.0 && centroid.z == 0.0 && centroid.m == 0.0);

	// Polygon EMPTY
	geom.set_type(sgl::geometry_type::POLYGON);
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_polygons(geom, centroid));
	assert(centroid.x == 0.0 && centroid.y == 0.0 && centroid.z == 0.0 && centroid.m == 0.0);

	// MultiPoint EMPTY
	geom.set_type(sgl::geometry_type::MULTI_POINT);
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_points(geom, centroid));

	// MultiLineString EMPTY
	geom.set_type(sgl::geometry_type::MULTI_LINESTRING);
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_linestrings(geom, centroid));

	// MultiPolygon EMPTY
	geom.set_type(sgl::geometry_type::MULTI_POLYGON);
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_polygons(geom, centroid));

	// GeometryCollection EMPTY
	geom.set_type(sgl::geometry_type::GEOMETRY_COLLECTION);
	assert(!sgl::ops::get_centroid(geom, centroid));
	assert(!sgl::ops::get_centroid_from_polygons(geom, centroid));

	const auto point_wkt = "POINT(1 2)";
	assert(reader.try_parse(geom, point_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 1.0 && centroid.y == 2.0 && centroid.z == 0 && centroid.m == 0);

	const auto line_wkt = "LINESTRING(1 1, 3 3)";
	assert(reader.try_parse(geom, line_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 2.0 && centroid.y == 2.0 && centroid.z == 0.0 && centroid.m == 0.0);

	const auto polygon_wkt = "POLYGON((1 1, 1 3, 3 3, 3 1, 1 1))";
	assert(reader.try_parse(geom, polygon_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 2.0 && centroid.y == 2.0 && centroid.z == 0.0 && centroid.m == 0.0);

	const auto multi_point_wkt = "MULTIPOINT(1 2, 3 4)";
	assert(reader.try_parse(geom, multi_point_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 2.0 && centroid.y == 3.0 && centroid.z == 0.0 && centroid.m == 0.0);

	const auto multi_line_wkt = "MULTILINESTRING((1 1, 1 3), (3 3, 3 1))";
	assert(reader.try_parse(geom, multi_line_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 2.0 && centroid.y == 2.0 && centroid.z == 0.0 && centroid.m == 0.0);

	const auto multi_polygon_wkt = "MULTIPOLYGON(((1 1, 1 3, 3 3, 3 1, 1 1)), ((4 4, 4 6, 6 6, 6 4, 4 4)))";
	assert(reader.try_parse(geom, multi_polygon_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 3.5 && centroid.y == 3.5 && centroid.z == 0.0 && centroid.m == 0.0);

	const auto geometry_collection_point_wkt = "GEOMETRYCOLLECTION(POINT(1 2))";
	assert(reader.try_parse(geom, geometry_collection_point_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 1.0 && centroid.y == 2.0 && centroid.z == 0.0 && centroid.m == 0.0);

	const auto geometry_collection_line_wkt = "GEOMETRYCOLLECTION(POINT(1 2), LINESTRING(3 4, 5 6))";
	assert(reader.try_parse(geom, geometry_collection_line_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 4 && centroid.y == 5 && centroid.z == 0.0 && centroid.m == 0.0);

	const auto geometry_collection_polygon_wkt =
	    "GEOMETRYCOLLECTION(POINT(1 2), LINESTRING(3 4, 5 6), POLYGON((1 1, 1 3, 3 3, 3 1, 1 1)));";
	assert(reader.try_parse(geom, geometry_collection_polygon_wkt));
	centroid = {0, 0, 0, 0};
	assert(sgl::ops::get_centroid(geom, centroid));
	assert(centroid.x == 2.0 && centroid.y == 2.0 && centroid.z == 0.0 && centroid.m == 0.0);
}

void test_extent_xy() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);

	sgl::geometry geom;
	sgl::extent_xy extent_xy = sgl::extent_xy::smallest();

	// Test extent of an invalid geometry
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 0);

	// Test extent of a point
	extent_xy = sgl::extent_xy::smallest();
	const auto point_wkt = "POINT(1 2)";
	assert(reader.try_parse(geom, point_wkt));

	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 1);
	assert(extent_xy.min.x == 1.0 && extent_xy.min.y == 2.0);
	assert(extent_xy.max.x == 1.0 && extent_xy.max.y == 2.0);

	// Test extent of a linestring
	extent_xy = sgl::extent_xy::smallest();
	const auto line_wkt = "LINESTRING(1 1, 1 3, 3 3)";
	assert(reader.try_parse(geom, line_wkt));
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 3);
	assert(extent_xy.min.x == 1.0 && extent_xy.min.y == 1.0);
	assert(extent_xy.max.x == 3.0 && extent_xy.max.y == 3.0);

	// Test extent of a polygon
	extent_xy = sgl::extent_xy::smallest();
	const auto polygon_wkt = "POLYGON((1 1, 1 3, 3 3, 3 1, 1 1))";
	assert(reader.try_parse(geom, polygon_wkt));
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 5);
	assert(extent_xy.min.x == 1.0 && extent_xy.min.y == 1.0);
	assert(extent_xy.max.x == 3.0 && extent_xy.max.y == 3.0);

	// Test extent of a multipoint
	extent_xy = sgl::extent_xy::smallest();
	const auto multi_point_wkt = "MULTIPOINT(1 2, 3 4)";
	assert(reader.try_parse(geom, multi_point_wkt));
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 2);
	assert(extent_xy.min.x == 1.0 && extent_xy.min.y == 2.0);
	assert(extent_xy.max.x == 3.0 && extent_xy.max.y == 4.0);

	// Test extent of a multilinestring
	extent_xy = sgl::extent_xy::smallest();
	const auto multi_line_wkt = "MULTILINESTRING((1 1, 1 3), (3 3, 3 1))";
	assert(reader.try_parse(geom, multi_line_wkt));
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 4);
	assert(extent_xy.min.x == 1.0 && extent_xy.min.y == 1.0);
	assert(extent_xy.max.x == 3.0 && extent_xy.max.y == 3.0);

	// Test extent of a multipolygon
	extent_xy = sgl::extent_xy::smallest();
	const auto multi_polygon_wkt = "MULTIPOLYGON(((1 1, 1 3, 3 3, 3 1, 1 1)), ((4 4, 4 6, 6 6, 6 4, 4 4)))";
	assert(reader.try_parse(geom, multi_polygon_wkt));
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 10);
	assert(extent_xy.min.x == 1.0 && extent_xy.min.y == 1.0);
	assert(extent_xy.max.x == 6.0 && extent_xy.max.y == 6.0);

	// Test extent of a geometrycollection
	extent_xy = sgl::extent_xy::smallest();
	const auto geometry_collection_wkt = "GEOMETRYCOLLECTION(POINT(1 2), LINESTRING(3 4, 5 6))";
	assert(reader.try_parse(geom, geometry_collection_wkt));
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 3);
	assert(extent_xy.min.x == 1.0 && extent_xy.min.y == 2.0);
	assert(extent_xy.max.x == 5.0 && extent_xy.max.y == 6.0);

	// Test extent of an empty geometry
	extent_xy = sgl::extent_xy::smallest();
	const auto empty_wkt = "GEOMETRYCOLLECTION EMPTY";
	assert(reader.try_parse(geom, empty_wkt));
	assert(sgl::ops::get_total_extent_xy(geom, extent_xy) == 0);
}

void test_extent_xyzm() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);
	sgl::geometry geom;
	sgl::extent_xyzm extent = sgl::extent_xyzm::smallest();

	// Test extent of an invalid geometry
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 0);

	// Test extent of a point
	assert(reader.try_parse(geom, "POINT ZM (1 2 3 4)"));
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 1);
	assert(extent.min.x == 1.0 && extent.min.y == 2.0 && extent.min.z == 3.0 && extent.min.m == 4.0);
	assert(extent.max.x == 1.0 && extent.max.y == 2.0 && extent.max.z == 3.0 && extent.max.m == 4.0);

	// Test extent of a linestring
	assert(reader.try_parse(geom, "LINESTRING ZM (1 1 1 1, 1 3 3 3, 3 3 3 3)"));
	extent = sgl::extent_xyzm::smallest();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 3);
	assert(extent.min.x == 1.0 && extent.min.y == 1.0 && extent.min.z == 1.0 && extent.min.m == 1.0);
	assert(extent.max.x == 3.0 && extent.max.y == 3.0 && extent.max.z == 3.0 && extent.max.m == 3.0);

	// Test extent of a polygon
	assert(reader.try_parse(geom, "POLYGON ZM ((1 1 1 1, 1 3 3 3, 3 3 3 3, 3 1 1 1, 1 1 1 1))"));
	extent = sgl::extent_xyzm::smallest();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 5);
	assert(extent.min.x == 1.0 && extent.min.y == 1.0 && extent.min.z == 1.0 && extent.min.m == 1.0);
	assert(extent.max.x == 3.0 && extent.max.y == 3.0 && extent.max.z == 3.0 && extent.max.m == 3.0);

	// Test extent of a multipoint
	assert(reader.try_parse(geom, "MULTIPOINT ZM (1 2 1 1, 3 4 3 3)"));
	extent = sgl::extent_xyzm::smallest();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 2);
	assert(extent.min.x == 1.0 && extent.min.y == 2.0 && extent.min.z == 1.0 && extent.min.m == 1.0);
	assert(extent.max.x == 3.0 && extent.max.y == 4.0 && extent.max.z == 3.0 && extent.max.m == 3.0);

	// Test extent of a multilinestring
	assert(reader.try_parse(geom, "MULTILINESTRING ZM ((1 1 1 1, 1 3 3 3), (3 3 3 3, 3 1 1 1))"));
	extent = sgl::extent_xyzm::smallest();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 4);
	assert(extent.min.x == 1.0 && extent.min.y == 1.0 && extent.min.z == 1.0 && extent.min.m == 1.0);
	assert(extent.max.x == 3.0 && extent.max.y == 3.0 && extent.max.z == 3.0 && extent.max.m == 3.0);

	// Test extent of a multipolygon
	assert(reader.try_parse(geom, "MULTIPOLYGON ZM (((1 1 1 1, 1 3 3 3, 3 3 3 3, 3 1 1 1, 1 1 1 1)), ((4 4 4 4, 4 6 6 "
	                              "6, 6 6 6 6, 6 4 4 4, 4 4 4 4)))"));
	extent = sgl::extent_xyzm::smallest();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 10);
	assert(extent.min.x == 1.0 && extent.min.y == 1.0 && extent.min.z == 1.0 && extent.min.m == 1.0);
	assert(extent.max.x == 6.0 && extent.max.y == 6.0 && extent.max.z == 6.0 && extent.max.m == 6.0);

	// Test extent of a geometrycollection
	assert(reader.try_parse(geom, "GEOMETRYCOLLECTION ZM (POINT ZM (1 2 1 1), LINESTRING ZM (3 4 3 3, 5 6 5 6))"));
	extent = sgl::extent_xyzm::smallest();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 3);
	assert(extent.min.x == 1.0 && extent.min.y == 2.0 && extent.min.z == 1.0 && extent.min.m == 1.0);
	assert(extent.max.x == 5.0 && extent.max.y == 6.0 && extent.max.z == 5.0 && extent.max.m == 6.0);

	// Test extent of an empty geometry
	assert(reader.try_parse(geom, "GEOMETRYCOLLECTION EMPTY"));
	extent = sgl::extent_xyzm::zero();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 0);
	assert(extent.min.x == 0.0 && extent.min.y == 0.0 && extent.min.z == 0.0 && extent.min.m == 0.0);
	assert(extent.max.x == 0.0 && extent.max.y == 0.0 && extent.max.z == 0.0 && extent.max.m == 0.0);

	// Test extent of an empty geometry with Z and M
	assert(reader.try_parse(geom, "GEOMETRYCOLLECTION ZM EMPTY"));
	extent = sgl::extent_xyzm::zero();
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 0);
	assert(extent.min.x == 0.0 && extent.min.y == 0.0 && extent.min.z == 0.0 && extent.min.m == 0.0);
	assert(extent.max.x == 0.0 && extent.max.y == 0.0 && extent.max.z == 0.0 && extent.max.m == 0.0);

	// Now do the same, except with Z and no M
	assert(reader.try_parse(geom, "POINT Z (1 2 3)"));
	extent = sgl::extent_xyzm::smallest();
	extent.min.m = 0;
	extent.max.m = 0;
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 1);
	assert(extent.min.x == 1.0 && extent.min.y == 2.0 && extent.min.z == 3.0 && extent.min.m == 0.0);
	assert(extent.max.x == 1.0 && extent.max.y == 2.0 && extent.max.z == 3.0 && extent.max.m == 0.0);

	// And with M and no Z
	assert(reader.try_parse(geom, "POINT M (1 2 3)"));
	extent = sgl::extent_xyzm::smallest();
	extent.min.m = 0;
	extent.max.m = 0;
	assert(sgl::ops::get_total_extent_xyzm(geom, extent) == 1);
	assert(extent.min.x == 1.0 && extent.min.y == 2.0 && extent.min.z == 3.0 && extent.min.m == 0);
	assert(extent.max.x == 1.0 && extent.max.y == 2.0 && extent.max.z == 3.0 && extent.max.m == 0);
}

void test_vertex_count() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);
	sgl::geometry geom;

	// Test vertex count of invalid geometry
	assert(sgl::ops::get_total_vertex_count(geom) == 0);

	const auto point_wkt = "POINT(1 2)";
	assert(reader.try_parse(geom, point_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 1);

	const auto line_wkt = "LINESTRING(1 2, 3 4)";
	assert(reader.try_parse(geom, line_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 2);

	const auto polygon_wkt = "POLYGON((1 2, 3 4, 5 6, 1 2))";
	assert(reader.try_parse(geom, polygon_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 4);

	const auto multi_point_wkt = "MULTIPOINT(1 2, 3 4)";
	assert(reader.try_parse(geom, multi_point_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 2);

	const auto multi_line_wkt = "MULTILINESTRING((1 2, 3 4), (5 6, 7 8))";
	assert(reader.try_parse(geom, multi_line_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 4);

	const auto multi_polygon_wkt = "MULTIPOLYGON(((1 2, 3 4, 5 6, 1 2)), ((7 8, 9 10, 11 12, 7 8)))";
	assert(reader.try_parse(geom, multi_polygon_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 8);

	const auto geometry_collection_wkt = "GEOMETRYCOLLECTION(POINT(1 2), LINESTRING(3 4, 5 6))";
	assert(reader.try_parse(geom, geometry_collection_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 3);

	const auto empty_wkt = "GEOMETRYCOLLECTION EMPTY";
	assert(reader.try_parse(geom, empty_wkt));
	assert(sgl::ops::get_total_vertex_count(geom) == 0);
}

void test_euclidean_distance() {

	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);

	sgl::geometry rhs;
	sgl::geometry lhs;

	double result;

	sgl::ops::get_euclidean_distance(lhs, rhs, result); // Coverage

	assert(reader.try_parse(lhs, "POINT(1 2)"));
	assert(reader.try_parse(rhs, "POINT(1 2)"));
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);

	reader.try_parse(rhs, "POINT(1 4)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 2.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 2.0);

	reader.try_parse(rhs, "LINESTRING(1 2, 1 4)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	reader.try_parse(rhs, "LINESTRING(1 6, 1 8)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 4.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 4.0);

	reader.try_parse(rhs, "LINESTRING (1 6, 1 6)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 4.0); // Degenerate case
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 4.0);

	reader.try_parse(rhs, "LINESTRING (1 6)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 4.0); // Degenerate case
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 4.0);

	reader.try_parse(rhs, "POLYGON((2 2, 2 4, 4 4, 4 2, 2 2))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 1.0);
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 1.0);

	reader.try_parse(rhs, "POLYGON((-2 -2, -2 -4, -4 -4, -4 -2, -2 -2))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 5.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 5.0);

	// Inside polygon
	reader.try_parse(rhs, "POLYGON((0 0, 0 4, 4 4, 4 0, 0 0))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	// On the border of polygon
	reader.try_parse(rhs, "POLYGON((1 2, 1 4, 4 4, 4 2, 1 2))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	reader.try_parse(rhs, "POLYGON((0 2, 4 2, 4 0, 0 0, 0 2))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	reader.try_parse(rhs, "POLYGON((0 2, 1 2, 1 0, 0 0, 0 2))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	// Degenerate polygon (less than 3 points)
	reader.try_parse(rhs, "POLYGON((0 0, 0 0))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	// On the border of polygon with holes
	reader.try_parse(rhs, "POLYGON((0 0, 0 4, 4 4, 4 0, 0 0), (1 1, 1 3, 3 3, 3 1, 1 1))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	// Inside hole
	reader.try_parse(lhs, "POINT(1 1)");
	reader.try_parse(rhs, "POLYGON((-1 -1, -1 4, 4 4, 4 -1, -1 -1), (0 0, 0 3, 3 3, 3 0, 0 0))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 1.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 1.0);

	// Now test linestrings
	reader.try_parse(lhs, "LINESTRING(0 0, 0 2, 0 4, 4 4)");

	// Crossing linestrings
	reader.try_parse(rhs, "LINESTRING(0 2, 2 2, 2 4, 2 6)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);

	reader.try_parse(rhs, "LINESTRING(0 3, 3 3)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);

	// Non-crossing linestrings
	reader.try_parse(rhs, "LINESTRING(3 0, 3 3)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 1.0);

	// Degenerate (collapsed linestring)
	reader.try_parse(rhs, "LINESTRING(0 0, 0 0)");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 0.0);

	// Linestring<->Polygon
	// This is the line we will test against
	reader.try_parse(lhs, "LINESTRING(0 0, 2 0, 4 0)");

	reader.try_parse(rhs, "POLYGON((0 0, 0 4, 4 4, 4 0, 0 0))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);

	reader.try_parse(rhs, "POLYGON((0 0, 0 4, 4 4, 4 0, 0 0), (1 1, 1 3, 3 3, 3 1, 1 1))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);

	reader.try_parse(rhs, "POLYGON((3 -1, 3 4, 4 4, 4 -1, 3 -1))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0);

	// Completely inside hole
	reader.try_parse(rhs, "POLYGON((-10 -10, -10 10, 10 10, 10 -10, -10 -10), (-5 -5, -5 5, 5 5, 5 -5, -5 -5))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 1.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 1.0);

	// Polygon<->Polygon

	// Overlapping
	reader.try_parse(lhs, "POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))");
	reader.try_parse(rhs, "POLYGON((0 0, 0 4, 4 4, 4 0, 0 0))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 0.0);

	// Non overlapping
	reader.try_parse(rhs, "POLYGON((0 3, 0 4, 4 4, 4 3, 0 3))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 2.0);

	// Completely inside a hole
	reader.try_parse(rhs, "POLYGON((-10 -10, -10 10, 10 10, 10 -10, -10 -10), (6 6, 6 7, 7 7, 7 6, 6 6), (-5 -5, -5 5, "
	                      "5 5, 5 -5, -5 -5))");
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 4.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 4.0);

	// Multipoints
	assert(reader.try_parse(lhs, "MULTIPOINT(5 5, 10 8)"));
	assert(reader.try_parse(rhs, "MULTIPOINT(8 8, 15 15)"));
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 2.0);

	// Empty
	assert(reader.try_parse(lhs, "POINT EMPTY"));
	assert(reader.try_parse(rhs, "MULTIPOINT EMPTY"));
	sgl::ops::get_euclidean_distance(lhs, rhs, result); // Coverage
	sgl::ops::get_euclidean_distance(rhs, lhs, result); // Coverage

	assert(reader.try_parse(lhs, "POINT EMPTY"));
	assert(reader.try_parse(rhs, "POINT (1 1)"));
	sgl::ops::get_euclidean_distance(lhs, rhs, result); // Coverage
	sgl::ops::get_euclidean_distance(rhs, lhs, result);

	const sgl::geometry invalid;

	assert(reader.try_parse(lhs, "POLYGON EMPTY"));
	assert(!sgl::ops::get_euclidean_distance(lhs, lhs, result)); // Should return false
	assert(!sgl::ops::get_euclidean_distance(rhs, lhs, result)); // Should return false

	assert(reader.try_parse(rhs, "LINESTRING EMPTY"));
	assert(!sgl::ops::get_euclidean_distance(lhs, rhs, result)); // Should return false
	assert(!sgl::ops::get_euclidean_distance(rhs, rhs, result)); // Should return false

	assert(reader.try_parse(lhs, "POINT EMPTY"));
	assert(!sgl::ops::get_euclidean_distance(lhs, rhs, result)); // Should return false

	assert(!sgl::ops::get_euclidean_distance(lhs, invalid, result));     // Should return false
	assert(!sgl::ops::get_euclidean_distance(invalid, rhs, result));     // Should return false
	assert(!sgl::ops::get_euclidean_distance(invalid, invalid, result)); // Should return false

	// Degenerate linestring cases
	assert(reader.try_parse(lhs, "LINESTRING(1 1)")); // Degenerate linestring
	assert(reader.try_parse(rhs, "LINESTRING(1 3)")); // Degenerate linestring

	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 2.0); // Distance between two points

	assert(reader.try_parse(lhs, "LINESTRING(1 1, 1 2)"));
	assert(sgl::ops::get_euclidean_distance(lhs, rhs, result) && result == 1.0);
	assert(sgl::ops::get_euclidean_distance(rhs, lhs, result) && result == 1.0);
}

void test_prepared_geometry() {
	sgl::arena_allocator alloc;

	sgl::wkt_reader reader(alloc);

	// Pase two geometries, and compare that their distance is the same when using prepared geometries
	auto parse_and_compare = [&](const char *lhs_wkt, const char *rhs_wkt, bool expect_found, double expect_dist) {
		sgl::geometry lhs_base;
		sgl::geometry rhs_base;

		double base_dist = 0.0;

		assert(reader.try_parse(lhs_base, lhs_wkt));
		assert(reader.try_parse(rhs_base, rhs_wkt));

		const auto base_found = sgl::ops::get_euclidean_distance(lhs_base, rhs_base, base_dist);
		assert(base_found == expect_found);
		if (base_found) {
			assert(base_dist == expect_dist);
		}

		// Now prepare the geometries and compare again
		sgl::prepared_geometry lhs_prep;
		sgl::prepared_geometry rhs_prep;
		double prep_dist = 0.0;

		sgl::prepared_geometry::make(alloc, lhs_base, lhs_prep);
		sgl::prepared_geometry::make(alloc, rhs_base, rhs_prep);

		const auto prep_found = sgl::ops::get_euclidean_distance(lhs_prep, rhs_prep, prep_dist);
		assert(prep_found == expect_found);
		if (prep_found) {
			assert(prep_dist == expect_dist);
		}
	};

	static constexpr auto big_donut =
	    "POLYGON("
	    "(0 0, 0 2, 0 4, 0 6, 0 8, 0 10, 2 10, 4 10, 6 10, 8 10, 10 10, 10 8, 10 6, 10 4, 10 2, 10 0, 8 0, 6 0, 4 0, 2 "
	    "0, 0 0),"
	    "(1 1, 1 3, 1 5, 1 7, 1 9, 3 9, 5 9, 7 9, 9 9, 9 7, 9 5, 9 3, 9 1, 7 1, 5 1, 3 1, 1 1))";

	static constexpr auto big_donut_reversed =
	    "POLYGON("
	    "(0 0, 0 2, 0 4, 0 6, 0 8, 0 10, 2 10, 4 10, 6 10, 8 10, 10 10, 10 8, 10 6, 10 4, 10 2, 10 0, 8 0, 6 0, 4 0, 2 "
	    "0, 0 0),"
	    "(1 1, 3 1, 5 1, 7 1, 9 1, 9 3, 9 5, 9 7, 9 9, 7 9, 5 9, 3 9, 1 9, 1 7, 1 5, 1 3, 1 1))";

	// Point in polygon surface
	parse_and_compare(big_donut, "POINT(0.5 0.5)", true, 0.0);
	parse_and_compare("POINT(0.5 0.5)", big_donut, true, 0.0);
	parse_and_compare(big_donut_reversed, "POINT(0.5 0.5)", true, 0.0);
	parse_and_compare("POINT(0.5 0.5)", big_donut_reversed, true, 0.0);

	// Point outside polygon
	parse_and_compare("POINT(15 0)", big_donut, true, 5.0);
	parse_and_compare(big_donut, "POINT(15 0)", true, 5.0);
	parse_and_compare("POINT(15 0)", big_donut_reversed, true, 5.0);
	parse_and_compare(big_donut_reversed, "POINT(15 0)", true, 5.0);

	// Point in polygon hole
	parse_and_compare("POINT(5 5)", big_donut, true, 4.0);
	parse_and_compare(big_donut, "POINT(5 5)", true, 4.0);
	parse_and_compare("POINT(5 5)", big_donut_reversed, true, 4.0);
	parse_and_compare(big_donut_reversed, "POINT(5 5)", true, 4.0);

	// Point on polygon border
	parse_and_compare("POINT(2 10)", big_donut, true, 0.0);
	parse_and_compare(big_donut, "POINT(2 10)", true, 0.0);
	parse_and_compare("POINT(2 10)", big_donut_reversed, true, 0.0);
	parse_and_compare(big_donut_reversed, "POINT(2 10)", true, 0.0);

	// Point on polygon hole border
	parse_and_compare("POINT(9 5)", big_donut, true, 0.0);
	parse_and_compare(big_donut, "POINT(9 5)", true, 0.0);
	parse_and_compare("POINT(9 5)", big_donut_reversed, true, 0.0);
	parse_and_compare(big_donut_reversed, "POINT(9 5)", true, 0.0);

	// Crossing linestrings (distance should be 0)
	static constexpr auto line_a = "LINESTRING(0 0, 0 10, 10 10, 10 0)";
	static constexpr auto line_b = "LINESTRING(0 5, 5 5, 5 10, 10 10)";
	parse_and_compare(line_a, line_b, true, 0.0);
	parse_and_compare(line_b, line_a, true, 0.0);

	// Non crossing linestrings
	static constexpr auto line_c = "LINESTRING(0 0, 0 5, 0 10)";
	static constexpr auto line_d = "LINESTRING(5 0, 5 10)";
	parse_and_compare(line_c, line_d, true, 5.0);
	parse_and_compare(line_d, line_c, true, 5.0);

	static constexpr auto geom_col = "GEOMETRYCOLLECTION("
	                                 "POINT(0 0), "
	                                 "LINESTRING(0 0, 0 10, 10 10, 10 0), "
	                                 "POLYGON((0 0, 0 2, 2 2, 2 0, 0 0)), "
	                                 "MULTIPOINT(5 5, 6 6))";

	// Now compare a geometry collection with a point
	parse_and_compare(geom_col, "POINT(0 0)", true, 0.0);
	parse_and_compare("POINT(0 0)", geom_col, true, 0.0);

	parse_and_compare("POINT(5 5)", geom_col, true, 0.0);
	parse_and_compare(geom_col, "POINT(5 5)", true, 0.0);
}

void test_misc_coverage() {
	// Misc tests just to get code coverage up
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);

	sgl::geometry geom;
	const auto wkt = "POINT(1 2)";
	reader.try_parse(geom, wkt, strlen(wkt));
	assert(geom.get_vertex_array() != nullptr);
}

void test_linear_referencing() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);

	// 1. linestring::interpolate on zero-length line
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(5 5, 5 5)"));
		sgl::vertex_xyzm pt = {};
		assert(sgl::linestring::interpolate(geom, 0.5, pt));
		assert(pt.x == 5.0 && pt.y == 5.0);
		assert(!std::isnan(pt.x) && !std::isnan(pt.y));
	}

	// 2. linestring::interpolate_points on zero-length line (must not hang / infinite loop)
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(5 5, 5 5)"));
		sgl::geometry result;
		sgl::linestring::interpolate_points(alloc, geom, 0.2, result);
		assert(result.get_type() == sgl::geometry_type::POINT);
		assert(result.get_vertex_count() == 1);
		auto pt = result.get_vertex_xy(0);
		assert(pt.x == 5.0 && pt.y == 5.0);
		assert(!std::isnan(pt.x) && !std::isnan(pt.y));
	}

	// 3. linestring::substring on leading duplicate vertices
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(1 1, 1 1, 2 2)"));
		sgl::geometry result;
		sgl::linestring::substring(alloc, geom, 0.0, 0.5, result);
		assert(result.get_type() == sgl::geometry_type::LINESTRING);
		assert(result.get_vertex_count() == 3);
		for (size_t i = 0; i < result.get_vertex_count(); i++) {
			auto pt = result.get_vertex_xy(i);
			assert(!std::isnan(pt.x) && !std::isnan(pt.y));
		}
		auto pt0 = result.get_vertex_xy(0);
		auto pt1 = result.get_vertex_xy(1);
		auto pt2 = result.get_vertex_xy(2);
		assert(pt0.x == 1.0 && pt0.y == 1.0);
		assert(pt1.x == 1.0 && pt1.y == 1.0);
		assert(pt2.x == 1.5 && pt2.y == 1.5);
	}

	// 4. linestring::substring on collapsed zero-length line (beg_frac != end_frac)
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(5 5, 5 5)"));
		sgl::geometry result;
		sgl::linestring::substring(alloc, geom, 0.2, 0.8, result);
		assert(result.get_type() == sgl::geometry_type::LINESTRING);
		assert(result.get_vertex_count() == 2);
		auto pt0 = result.get_vertex_xy(0);
		auto pt1 = result.get_vertex_xy(1);
		assert(pt0.x == 5.0 && pt0.y == 5.0);
		assert(pt1.x == 5.0 && pt1.y == 5.0);
		assert(!std::isnan(pt0.x) && !std::isnan(pt0.y));
		assert(!std::isnan(pt1.x) && !std::isnan(pt1.y));
	}

	// 5. linestring::substring on collapsed zero-length line (beg_frac == end_frac)
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(5 5, 5 5)"));
		sgl::geometry result;
		sgl::linestring::substring(alloc, geom, 0.5, 0.5, result);
		assert(result.get_type() == sgl::geometry_type::POINT);
		assert(result.get_vertex_count() == 1);
		auto pt0 = result.get_vertex_xy(0);
		assert(pt0.x == 5.0 && pt0.y == 5.0);
		assert(!std::isnan(pt0.x) && !std::isnan(pt0.y));
	}

	// 6. linestring::substring on single-vertex linestring (beg_frac != end_frac)
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(0 0)"));
		sgl::geometry result;
		sgl::linestring::substring(alloc, geom, 0.25, 0.75, result);
		assert(result.get_type() == sgl::geometry_type::LINESTRING);
		assert(result.get_vertex_count() == 2);
		auto pt0 = result.get_vertex_xy(0);
		auto pt1 = result.get_vertex_xy(1);
		assert(pt0.x == 0.0 && pt0.y == 0.0);
		assert(pt1.x == 0.0 && pt1.y == 0.0);
		assert(!std::isnan(pt0.x) && !std::isnan(pt0.y));
		assert(!std::isnan(pt1.x) && !std::isnan(pt1.y));
	}

	// 7. linestring::substring on single-vertex linestring (0.0, 1.0)
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(0 0)"));
		sgl::geometry result;
		sgl::linestring::substring(alloc, geom, 0.0, 1.0, result);
		assert(result.get_type() == sgl::geometry_type::LINESTRING);
		assert(result.get_vertex_count() == 2);
		auto pt0 = result.get_vertex_xy(0);
		auto pt1 = result.get_vertex_xy(1);
		assert(pt0.x == 0.0 && pt0.y == 0.0);
		assert(pt1.x == 0.0 && pt1.y == 0.0);
		assert(!std::isnan(pt0.x) && !std::isnan(pt0.y));
		assert(!std::isnan(pt1.x) && !std::isnan(pt1.y));
	}
}

void test_linear_referencing_adversarial() {
	sgl::arena_allocator alloc;
	sgl::wkt_reader reader(alloc);

	const double qnan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();

	// =========================================================================
	// Category 1: Coordinate dimensions (Z, M, ZM)
	// =========================================================================
	{
		// LINESTRING Z with leading zero-length segment
		sgl::geometry geom_z;
		assert(reader.try_parse(geom_z, "LINESTRING Z (0 0 10, 0 0 10, 10 0 20)"));
		assert(geom_z.has_z());
		assert(!geom_z.has_m());

		// interpolate at 0.0, 0.5, 1.0
		sgl::vertex_xyzm pt = {};
		assert(sgl::linestring::interpolate(geom_z, 0.0, pt));
		assert(pt.x == 0.0 && pt.y == 0.0 && pt.z == 10.0);

		assert(sgl::linestring::interpolate(geom_z, 0.5, pt));
		assert(pt.x == 5.0 && pt.y == 0.0 && pt.z == 15.0);

		assert(sgl::linestring::interpolate(geom_z, 1.0, pt));
		assert(pt.x == 10.0 && pt.y == 0.0 && pt.z == 20.0);

		// interpolate_points
		sgl::geometry pts_z;
		sgl::linestring::interpolate_points(alloc, geom_z, 0.5, pts_z);
		assert(pts_z.get_type() == sgl::geometry_type::MULTI_POINT);
		assert(pts_z.has_z());
		assert(pts_z.get_part_count() == 2);
		const auto *part1 = pts_z.get_first_part();
		auto v1 = part1->get_vertex_xyzm(0);
		assert(v1.x == 5.0 && v1.y == 0.0 && v1.z == 15.0);
		const auto *part2 = part1->get_next();
		auto v2 = part2->get_vertex_xyzm(0);
		assert(v2.x == 10.0 && v2.y == 0.0 && v2.z == 20.0);

		// substring
		sgl::geometry sub_z;
		sgl::linestring::substring(alloc, geom_z, 0.0, 0.5, sub_z);
		assert(sub_z.get_type() == sgl::geometry_type::LINESTRING);
		assert(sub_z.has_z());
		assert(sub_z.get_vertex_count() == 3);
		auto sv0 = sub_z.get_vertex_xyzm(0);
		auto sv1 = sub_z.get_vertex_xyzm(1);
		auto sv2 = sub_z.get_vertex_xyzm(2);
		assert(sv0.x == 0.0 && sv0.y == 0.0 && sv0.z == 10.0);
		assert(sv1.x == 0.0 && sv1.y == 0.0 && sv1.z == 10.0);
		assert(sv2.x == 5.0 && sv2.y == 0.0 && sv2.z == 15.0);
	}

	{
		// LINESTRING ZM with middle zero-length segment
		sgl::geometry geom_zm;
		assert(reader.try_parse(geom_zm, "LINESTRING ZM (0 0 10 100, 10 0 20 200, 10 0 20 200, 20 0 30 300)"));
		assert(geom_zm.has_z());
		assert(geom_zm.has_m());

		sgl::vertex_xyzm pt = {};
		assert(sgl::linestring::interpolate(geom_zm, 0.25, pt));
		assert(pt.x == 5.0 && pt.y == 0.0 && pt.z == 15.0 && pt.m == 150.0);

		assert(sgl::linestring::interpolate(geom_zm, 0.5, pt));
		assert(pt.x == 10.0 && pt.y == 0.0 && pt.z == 20.0 && pt.m == 200.0);

		sgl::geometry sub_zm;
		sgl::linestring::substring(alloc, geom_zm, 0.25, 0.75, sub_zm);
		assert(sub_zm.get_type() == sgl::geometry_type::LINESTRING);
		assert(sub_zm.has_z());
		assert(sub_zm.has_m());
		auto v_beg = sub_zm.get_vertex_xyzm(0);
		assert(v_beg.x == 5.0 && v_beg.y == 0.0 && v_beg.z == 15.0 && v_beg.m == 150.0);
		auto v_end = sub_zm.get_vertex_xyzm(sub_zm.get_vertex_count() - 1);
		assert(v_end.x == 15.0 && v_end.y == 0.0 && v_end.z == 25.0 && v_end.m == 250.0);
	}

	{
		// LINESTRING M
		sgl::geometry geom_m;
		assert(reader.try_parse(geom_m, "LINESTRING M (0 0 100, 0 0 100, 10 0 200)"));
		assert(!geom_m.has_z());
		assert(geom_m.has_m());

		sgl::vertex_xyzm pt = {};
		assert(sgl::linestring::interpolate(geom_m, 0.5, pt));
		// In SGL layout for M without Z, vertex_width is 24 bytes (x, y, m).
		// When copied into vertex_xyzm, m sits at pt.z offset.
		assert(pt.x == 5.0 && pt.y == 0.0 && pt.z == 150.0);

		sgl::geometry sub_m;
		sgl::linestring::substring(alloc, geom_m, 0.0, 0.5, sub_m);
		assert(sub_m.get_type() == sgl::geometry_type::LINESTRING);
		assert(sub_m.has_m());
		assert(!sub_m.has_z());
		auto mv0 = sub_m.get_vertex_xyzm(0);
		auto mv1 = sub_m.get_vertex_xyzm(1);
		auto mv2 = sub_m.get_vertex_xyzm(2);
		assert(mv0.x == 0.0 && mv0.z == 100.0);
		assert(mv1.x == 0.0 && mv1.z == 100.0);
		assert(mv2.x == 5.0 && mv2.z == 150.0);
	}

	// =========================================================================
	// Category 2: Extreme inputs (NaN, Inf, negatives, order, precision)
	// =========================================================================
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(10 10, 20 20)"));

		// beg_frac > end_frac
		{
			sgl::geometry result;
			sgl::linestring::substring(alloc, geom, 0.8, 0.2, result);
			assert(result.is_empty());
		}

		// NaN inputs in substring must return empty, not corrupted geometry with (0, 0)
		{
			sgl::geometry r_nan_beg;
			sgl::linestring::substring(alloc, geom, qnan, 0.5, r_nan_beg);
			assert(r_nan_beg.is_empty());

			sgl::geometry r_nan_end;
			sgl::linestring::substring(alloc, geom, 0.5, qnan, r_nan_end);
			assert(r_nan_end.is_empty());

			sgl::geometry r_nan_both;
			sgl::linestring::substring(alloc, geom, qnan, qnan, r_nan_both);
			assert(r_nan_both.is_empty());
		}

		// NaN in interpolate must return false
		{
			sgl::vertex_xyzm pt = {};
			bool ret = sgl::linestring::interpolate(geom, qnan, pt);
			assert(!ret);
		}

		// NaN in interpolate_points must return empty POINT
		{
			sgl::geometry pts_res;
			sgl::linestring::interpolate_points(alloc, geom, qnan, pts_res);
			assert(pts_res.get_type() == sgl::geometry_type::POINT);
			assert(pts_res.is_empty());
		}

		// Very small frac / actual_length underflow test in interpolate_points (must not hang)
		{
			sgl::geometry tiny_geom;
			assert(reader.try_parse(tiny_geom, "LINESTRING(0 0, 1e-200 0)"));
			sgl::geometry result;
			sgl::linestring::interpolate_points(alloc, tiny_geom, 1e-200, result);
			assert(result.get_type() == sgl::geometry_type::POINT);
		}

		// -0.0
		{
			sgl::vertex_xyzm pt = {};
			assert(sgl::linestring::interpolate(geom, -0.0, pt));
			assert(pt.x == 10.0 && pt.y == 10.0);

			sgl::geometry result;
			sgl::linestring::substring(alloc, geom, -0.0, 0.5, result);
			assert(result.get_type() == sgl::geometry_type::LINESTRING);
			assert(result.get_vertex_count() == 2);
			auto p0 = result.get_vertex_xy(0);
			assert(p0.x == 10.0 && p0.y == 10.0);
		}

		// negative numbers clamp to 0
		{
			sgl::vertex_xyzm pt = {};
			assert(sgl::linestring::interpolate(geom, -5.0, pt));
			assert(pt.x == 10.0 && pt.y == 10.0);

			sgl::geometry result;
			sgl::linestring::substring(alloc, geom, -2.0, 0.5, result);
			assert(result.get_type() == sgl::geometry_type::LINESTRING);
			auto p0 = result.get_vertex_xy(0);
			assert(p0.x == 10.0 && p0.y == 10.0);
		}

		// numbers > 1.0 clamp to 1
		{
			sgl::vertex_xyzm pt = {};
			assert(sgl::linestring::interpolate(geom, 2.5, pt));
			assert(pt.x == 20.0 && pt.y == 20.0);

			sgl::geometry result;
			sgl::linestring::substring(alloc, geom, 0.5, 3.0, result);
			assert(result.get_type() == sgl::geometry_type::LINESTRING);
			auto p1 = result.get_vertex_xy(result.get_vertex_count() - 1);
			assert(p1.x == 20.0 && p1.y == 20.0);
		}

		// Inf and -Inf
		{
			sgl::vertex_xyzm pt = {};
			assert(sgl::linestring::interpolate(geom, inf, pt));
			assert(pt.x == 20.0 && pt.y == 20.0);

			assert(sgl::linestring::interpolate(geom, -inf, pt));
			assert(pt.x == 10.0 && pt.y == 10.0);

			sgl::geometry result;
			sgl::linestring::substring(alloc, geom, -inf, inf, result);
			assert(result.get_type() == sgl::geometry_type::LINESTRING);
			assert(result.get_vertex_count() == 2);
		}

		// beg_frac == end_frac at 0.0, 0.5, 1.0
		{
			sgl::geometry r0, r5, r1;
			sgl::linestring::substring(alloc, geom, 0.0, 0.0, r0);
			assert(r0.get_type() == sgl::geometry_type::POINT);
			assert(r0.get_vertex_xy(0).x == 10.0);

			sgl::linestring::substring(alloc, geom, 0.5, 0.5, r5);
			assert(r5.get_type() == sgl::geometry_type::POINT);
			assert(r5.get_vertex_xy(0).x == 15.0);

			sgl::linestring::substring(alloc, geom, 1.0, 1.0, r1);
			assert(r1.get_type() == sgl::geometry_type::POINT);
			assert(r1.get_vertex_xy(0).x == 20.0);
		}

		// Floating point precision very close to 1.0 (e.g. 0.999999999999999)
		{
			sgl::geometry result;
			sgl::linestring::substring(alloc, geom, 0.0, 0.999999999999999, result);
			assert(result.get_type() == sgl::geometry_type::LINESTRING);
			assert(result.get_vertex_count() == 2);
			auto pend = result.get_vertex_xy(1);
			assert(pend.x > 19.999 && pend.x <= 20.0);
		}
	}

	// =========================================================================
	// Category 3: Fully degenerate linestrings (2, 3, 10 vertices)
	// =========================================================================
	{
		sgl::geometry geom2, geom3, geom10;
		assert(reader.try_parse(geom2, "LINESTRING(0 0, 0 0)"));
		assert(reader.try_parse(geom3, "LINESTRING(0 0, 0 0, 0 0)"));
		assert(reader.try_parse(geom10, "LINESTRING(0 0, 0 0, 0 0, 0 0, 0 0, 0 0, 0 0, 0 0, 0 0, 0 0)"));

		sgl::vertex_xyzm pt = {};
		assert(sgl::linestring::interpolate(geom10, 0.5, pt));
		assert(pt.x == 0.0 && pt.y == 0.0);

		sgl::geometry pts_res;
		sgl::linestring::interpolate_points(alloc, geom10, 0.3, pts_res);
		assert(pts_res.get_type() == sgl::geometry_type::POINT);
		assert(pts_res.get_vertex_count() == 1);

		sgl::geometry sub_pt;
		sgl::linestring::substring(alloc, geom10, 0.5, 0.5, sub_pt);
		assert(sub_pt.get_type() == sgl::geometry_type::POINT);
		assert(sub_pt.get_vertex_count() == 1);

		sgl::geometry sub_line;
		sgl::linestring::substring(alloc, geom10, 0.2, 0.8, sub_line);
		assert(sub_line.get_type() == sgl::geometry_type::LINESTRING);
		assert(sub_line.get_vertex_count() == 10);
	}

	// =========================================================================
	// Category 4: Mixed duplicate vertices & spanning degenerate middle
	// =========================================================================
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(0 0, 0 0, 5 5, 5 5, 10 10, 10 10)"));
		assert(geom.get_vertex_count() == 6);

		sgl::geometry r_mid;
		// spanning exactly across the degenerate middle segment
		sgl::linestring::substring(alloc, geom, 0.49, 0.51, r_mid);
		assert(r_mid.get_type() == sgl::geometry_type::LINESTRING);
		assert(r_mid.get_vertex_count() >= 2);
		for (size_t i = 0; i < r_mid.get_vertex_count(); i++) {
			auto p = r_mid.get_vertex_xy(i);
			assert(!std::isnan(p.x) && !std::isnan(p.y));
		}

		sgl::geometry r_half;
		sgl::linestring::substring(alloc, geom, 0.5, 1.0, r_half);
		assert(r_half.get_type() == sgl::geometry_type::LINESTRING);
		assert(r_half.get_vertex_count() >= 2);
	}

	// =========================================================================
	// Category 5: Multi-point looping on interpolate_points
	// =========================================================================
	{
		sgl::geometry geom;
		assert(reader.try_parse(geom, "LINESTRING(0 0, 10 0)"));
		sgl::geometry mp;
		// Fraction 0.1 -> 10 points along the segment
		sgl::linestring::interpolate_points(alloc, geom, 0.1, mp);
		assert(mp.get_type() == sgl::geometry_type::MULTI_POINT);
		assert(mp.get_part_count() == 10);
	}
}

void test_wkb_parsing() {
	sgl::arena_allocator alloc;
	sgl::wkb_reader reader(alloc);

	const char point_wkb[] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	char concatenated_wkb[sizeof(point_wkb) * 2];
	memcpy(concatenated_wkb, point_wkb, sizeof(point_wkb));
	memcpy(concatenated_wkb + sizeof(point_wkb), point_wkb, sizeof(point_wkb));

	sgl::geometry geom;
	assert(reader.try_parse(geom, point_wkb, sizeof(point_wkb)));
	assert(!reader.try_parse(geom, concatenated_wkb, sizeof(concatenated_wkb)));
	assert(reader.get_error() == sgl::wkb_reader_error::TRAILING_DATA);
	assert(strcmp(reader.get_error_message(), "Unexpected trailing data after WKB geometry") == 0);
	assert(reader.try_parse(geom, point_wkb, sizeof(point_wkb)));

	sgl::extent_xy extent;
	size_t vertex_count;
	assert(reader.try_parse_stats(extent, vertex_count, point_wkb, sizeof(point_wkb)));
	assert(!reader.try_parse_stats(extent, vertex_count, concatenated_wkb, sizeof(concatenated_wkb)));
	assert(reader.get_error() == sgl::wkb_reader_error::TRAILING_DATA);
	assert(reader.try_parse_stats(extent, vertex_count, point_wkb, sizeof(point_wkb)));
}

int main() {

	test_allocator();
	test_wkt_parsing();
	test_euclidean_length();
	test_euclidean_area();
	test_euclidean_perimeter();
	test_euclidean_centroid();
	test_euclidean_distance();
	test_extent_xy();
	test_extent_xyzm();
	test_vertex_count();

	test_prepared_geometry();

	test_misc_coverage();
	test_linear_referencing();
	test_linear_referencing_adversarial();
	test_wkb_parsing();

	printf("All tests passed!\n");
	return 0;
}
