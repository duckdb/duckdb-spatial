#include "sgl.hpp"

#include <cassert>
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

void test_wkb_reader_dimension_state() {
	sgl::arena_allocator alloc;
	sgl::wkb_reader reader(alloc);
	reader.set_allow_mixed_zm(true);

	const unsigned char mixed_wkb[] = {
	    0x01, 0x07, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0xE9, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0xF0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x08, 0x40, 0x01, 0xD1, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x40,
	};
	const char point_wkb[] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	sgl::geometry geom;
	assert(reader.try_parse(geom, reinterpret_cast<const char *>(mixed_wkb), sizeof(mixed_wkb)));
	assert(reader.parsed_mixed_zm());
	assert(reader.parsed_any_z());
	assert(reader.parsed_any_m());

	assert(reader.try_parse(geom, point_wkb, sizeof(point_wkb)));
	assert(!reader.parsed_mixed_zm());
	assert(!reader.parsed_any_z());
	assert(!reader.parsed_any_m());

	sgl::extent_xy extent;
	size_t vertex_count;
	assert(reader.try_parse_stats(extent, vertex_count, point_wkb, sizeof(point_wkb)));
	assert(!reader.parsed_mixed_zm());
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
	test_wkb_reader_dimension_state();

	printf("All tests passed!\n");
	return 0;
}
