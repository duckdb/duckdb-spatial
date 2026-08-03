#include "sgl.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>
#include <queue>
#include <string> // TODO: Remove this, it is only used for error messages in WKT reader

//======================================================================================================================
// Helpers
//======================================================================================================================
namespace sgl {

namespace {

// TODO: Make robust
// Returns the orientation of the triplet (p, q, r)
// 0 if collinear, >0 if clockwise, <0 if counter-clockwise
int orient2d_fast(const vertex_xy &p, const vertex_xy &q, const vertex_xy &r) {
	const auto det_l = (p.x - r.x) * (q.y - r.y);
	const auto det_r = (p.y - r.y) * (q.x - r.x);
	const auto det = det_l - det_r;
	return (det > 0) - (det < 0);
}

enum class raycast_result { NONE = 0, CROSS, BOUNDARY };

// TODO: Make robust
raycast_result raycast_fast(const vertex_xy &prev, const vertex_xy &next, const vertex_xy &vert) {
	if (prev.x < vert.x && next.x < vert.x) {
		// The segment is to the left of the point
		return raycast_result::NONE;
	}

	if (next.x == vert.x && next.y == vert.y) {
		// The point is on the segment, they share a vertex
		return raycast_result::BOUNDARY;
	}

	if (prev.y == vert.y && next.y == vert.y) {
		// The segment is horizontal, check if the point is within the min/max x
		double minx = prev.x;
		double maxx = next.x;

		if (minx > maxx) {
			minx = next.x;
			maxx = prev.x;
		}

		if (vert.x >= minx && vert.x <= maxx) {
			// if its inside, then its on the boundary
			return raycast_result::BOUNDARY;
		}

		// otherwise it has no impact on the result
		return raycast_result::NONE;
	}

	if ((prev.y > vert.y && next.y <= vert.y) || (next.y > vert.y && prev.y <= vert.y)) {
		int sign = orient2d_fast(prev, next, vert);
		if (sign == 0) {
			return raycast_result::BOUNDARY;
		}

		if (next.y < prev.y) {
			sign = -sign;
		}

		if (sign > 0) {
			return raycast_result::CROSS;
		}
	}

	return raycast_result::NONE;
}

// TODO: Make robust
point_in_polygon_result vertex_in_ring(const vertex_xy &vert, const geometry &ring) {
	SGL_ASSERT(ring.get_type() == geometry_type::LINESTRING);

	if (ring.get_vertex_count() < 3) {
		// Degenerate case, should not happen
		// TODO: Return something better?
		return point_in_polygon_result::INVALID;
	}

	if (ring.is_prepared()) {
		auto &prep = static_cast<const prepared_geometry &>(ring);
		return prep.contains(vert);
	}

	const auto vertex_array = ring.get_vertex_array();
	const auto vertex_width = ring.get_vertex_width();
	const auto vertex_count = ring.get_vertex_count();

	uint32_t crossings = 0;

	vertex_xy prev;
	memcpy(&prev, vertex_array, sizeof(vertex_xy));
	for (uint32_t i = 1; i < vertex_count; i++) {

		vertex_xy next;
		memcpy(&next, vertex_array + i * vertex_width, sizeof(vertex_xy));

		switch (raycast_fast(prev, next, vert)) {
		case raycast_result::NONE:
			// No intersection
			break;
		case raycast_result::CROSS:
			// The ray crosses the segment, so we count it
			crossings++;
			break;
		case raycast_result::BOUNDARY:
			// The point is on the boundary, so we return BOUNDARY
			return point_in_polygon_result::BOUNDARY;
		}

		prev = next;
	}

	// Even number of crossings means the point is outside the polygon
	return crossings % 2 == 0 ? point_in_polygon_result::EXTERIOR : point_in_polygon_result::INTERIOR;
}

double vertex_distance_squared(const vertex_xy &lhs, const vertex_xy &rhs) {
	return std::pow(lhs.x - rhs.x, 2) + std::pow(lhs.y - rhs.y, 2);
}

double vertex_distance(const vertex_xy &lhs, const vertex_xy &rhs) {
	const auto dx = lhs.x - rhs.x;
	const auto dy = lhs.y - rhs.y;
	return std::sqrt(dx * dx + dy * dy);
}

double vertex_segment_distance(const vertex_xy &p, const vertex_xy &v, const vertex_xy &w) {
	const auto l2 = vertex_distance_squared(v, w);
	if (l2 == 0) {
		// is not better to just compare if w == v?
		return vertex_distance(p, v);
	}

	const auto t = ((p.x - v.x) * (w.x - v.x) + (p.y - v.y) * (w.y - v.y)) / l2;
	const auto t_clamped = math::max(0.0, math::min(1.0, t));
	const auto x = v.x + t_clamped * (w.x - v.x);
	const auto y = v.y + t_clamped * (w.y - v.y);

	const vertex_xy intersection {x, y};

	return vertex_distance(p, intersection);
}

// TODO: Make robust
double segment_segment_distance(const vertex_xy &a, const vertex_xy &b, const vertex_xy &c, const vertex_xy &d) {
	// Degenerate cases

	// TODO: Robust comparisons
	if (a.x == b.x && a.y == b.y) {
		return vertex_segment_distance(a, c, d);
	}
	if (c.x == d.x && c.y == d.y) {
		return vertex_segment_distance(c, a, b);
	}

	const auto denominator = ((b.x - a.x) * (d.y - c.y)) - ((b.y - a.y) * (d.x - c.x));
	if (denominator == 0) {
		// The segments are parallel, return the distance between the closest endpoints
		const auto dist_a = vertex_segment_distance(a, c, d);
		const auto dist_b = vertex_segment_distance(b, c, d);
		const auto dist_c = vertex_segment_distance(c, a, b);
		const auto dist_d = vertex_segment_distance(d, a, b);
		return math::min(math::min(dist_a, dist_b), math::min(dist_c, dist_d));
	}

	const auto r = ((a.y - c.y) * (d.x - c.x)) - ((a.x - c.x) * (d.y - c.y));
	const auto s = ((a.y - c.y) * (b.x - a.x)) - ((a.x - c.x) * (b.y - a.y));

	const auto r_norm = r / denominator;
	const auto s_norm = s / denominator;

	if (r_norm < 0 || r_norm > 1 || s_norm < 0 || s_norm > 1) {
		// The segments do not intersect, return the distance between the closest endpoints
		const auto dist_a = vertex_segment_distance(a, c, d);
		const auto dist_b = vertex_segment_distance(b, c, d);
		const auto dist_c = vertex_segment_distance(c, a, b);
		const auto dist_d = vertex_segment_distance(d, a, b);
		return math::min(math::min(dist_a, dist_b), math::min(dist_c, dist_d));
	}

	// Intersection, so no distance!
	return 0.0;
}

} // namespace

} // namespace sgl

//======================================================================================================================
// Internal Algorithms
//======================================================================================================================

namespace sgl {

namespace {

double vertex_array_length(const geometry &geom) {
	SGL_ASSERT(geom.get_type() == geometry_type::LINESTRING);

	const auto v_count = geom.get_vertex_count();
	const auto v_array = geom.get_vertex_array();
	const auto v_width = geom.get_vertex_width();

	if (v_count < 2) {
		return 0.0;
	}

	auto length = 0.0;

	vertex_xy prev = {0, 0};
	vertex_xy next = {0, 0};

	memcpy(&prev, v_array, sizeof(vertex_xy));
	for (uint32_t i = 1; i < v_count; i++) {
		memcpy(&next, v_array + i * v_width, sizeof(vertex_xy));

		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;
		length += std::sqrt(dx * dx + dy * dy);
		prev = next;
	}

	return length;
}

double vertex_array_signed_area(const geometry &geom) {
	SGL_ASSERT(geom.get_type() == geometry_type::LINESTRING);

	const auto v_count = geom.get_vertex_count();
	const auto v_array = geom.get_vertex_array();
	const auto v_width = geom.get_vertex_width();

	if (v_count < 3) {
		return 0.0;
	}

	auto area = 0.0;

	auto x0 = 0.0;
	auto x1 = 0.0;
	auto y1 = 0.0;
	auto y2 = 0.0;

	const auto x_data = v_array;
	const auto y_data = v_array + sizeof(double);

	memcpy(&x0, x_data, sizeof(double));

	for (uint32_t i = 1; i < v_count - 1; i++) {
		memcpy(&x1, x_data + (i + 0) * v_width, sizeof(double));
		memcpy(&y1, y_data + (i + 1) * v_width, sizeof(double));
		memcpy(&y2, y_data + (i - 1) * v_width, sizeof(double));

		area += (x1 - x0) * (y2 - y1);
	}

	return area * 0.5;
}

template <class CALLBACK>
void visit_polygons(const geometry &geom, CALLBACK &&callback) {
	const auto root = geom.get_parent();

	auto part = &geom;
	while (true) {
		switch (part->get_type()) {
		case geometry_type::POLYGON:
			callback(*part);
			break;
		case geometry_type::MULTI_POLYGON:
		case geometry_type::GEOMETRY_COLLECTION:
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		default:
			break;
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

template <class CALLBACK>
void visit_lines(const geometry &geom, CALLBACK &&callback) {
	const auto root = geom.get_parent();

	auto part = &geom;
	while (true) {
		switch (part->get_type()) {
		case geometry_type::LINESTRING:
			callback(*part);
			break;
		case geometry_type::MULTI_LINESTRING:
		case geometry_type::GEOMETRY_COLLECTION:
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		default:
			break;
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

template <class CALLBACK>
void visit_points(const geometry &geom, CALLBACK &&callback) {
	const auto root = geom.get_parent();

	auto part = &geom;
	while (true) {
		switch (part->get_type()) {
		case geometry_type::POINT:
			callback(*part);
			break;
		case geometry_type::MULTI_POINT:
		case geometry_type::GEOMETRY_COLLECTION:
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		default:
			break;
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

// Calls callback for each vertex geometry, that is:
// - POINT
// - LINESTRING
// - POLYGON RING
template <class CALLBACK>
void visit_vertex_arrays(const geometry &geom, CALLBACK &&callback) {
	const auto root = geom.get_parent();

	auto part = &geom;
	while (true) {
		switch (part->get_type()) {
		case geometry_type::POINT:
		case geometry_type::LINESTRING:
			callback(*part);
			break;
		case geometry_type::POLYGON:
		case geometry_type::MULTI_POINT:
		case geometry_type::MULTI_LINESTRING:
		case geometry_type::MULTI_POLYGON:
		case geometry_type::GEOMETRY_COLLECTION:
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		default:
			break;
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

template <class CALLBACK>
void visit_vertex_arrays_mutable(geometry &geom, CALLBACK &&callback) {
	const auto root = geom.get_parent();

	auto part = &geom;
	while (true) {
		switch (part->get_type()) {
		case geometry_type::POINT:
		case geometry_type::LINESTRING:
			callback(*part);
			break;
		case geometry_type::POLYGON:
		case geometry_type::MULTI_POINT:
		case geometry_type::MULTI_LINESTRING:
		case geometry_type::MULTI_POLYGON:
		case geometry_type::GEOMETRY_COLLECTION:
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		default:
			break;
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

// Visit all non-collection geometries
template <class CALLBACK>
void visit_leaf_geometries(const geometry &geom, CALLBACK &&callback) {
	const auto root = geom.get_parent();

	auto part = &geom;

	while (true) {
		switch (part->get_type()) {
		case geometry_type::POINT:
		case geometry_type::LINESTRING:
		case geometry_type::POLYGON:
			callback(*part);
			break;
		case geometry_type::MULTI_POINT:
		case geometry_type::MULTI_LINESTRING:
		case geometry_type::MULTI_POLYGON:
		case geometry_type::GEOMETRY_COLLECTION:
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		default:
			break;
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

template <class CALLBACK_ENTER, class CALLBACK_LEAVE>
void visit_all_parts(const geometry &geom, CALLBACK_ENTER &&on_enter, CALLBACK_LEAVE &&on_leave) {
	const auto root = geom.get_parent();

	auto part = &geom;

	while (true) {

		on_enter(*part);

		if (part->is_multi_part() && !part->is_empty()) {
			part = part->get_first_part();
			continue;
		}

		while (true) {

			on_leave(*part);

			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

template <class CALLBACK_ENTER, class CALLBACK_LEAVE>
void visit_all_parts_mutable(geometry &geom, CALLBACK_ENTER &&on_enter, CALLBACK_LEAVE &&on_leave) {
	const auto root = geom.get_parent();

	auto part = &geom;

	while (true) {

		on_enter(*part);

		if (part->is_multi_part() && !part->is_empty()) {
			part = part->get_first_part();
			continue;
		}

		while (true) {

			on_leave(*part);

			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

} // namespace

} // namespace sgl

//======================================================================================================================
// Algorithms
//======================================================================================================================
namespace sgl {

double ops::get_area(const geometry &geom) {
	auto area = 0.0;

	visit_polygons(geom, [&area](const geometry &part) {
		const auto tail = part.get_last_part();
		if (!tail) {
			return;
		}
		auto head = tail->get_next();

		area += std::abs(vertex_array_signed_area(*head));

		while (head != tail) {
			head = head->get_next();
			area -= std::abs(vertex_array_signed_area(*head));
		}
	});

	return area;
}

double ops::get_length(const geometry &geom) {
	auto length = 0.0;

	visit_lines(geom, [&length](const geometry &part) { length += vertex_array_length(part); });

	return length;
}

double ops::get_perimeter(const geometry &geom) {
	auto perimeter = 0.0;

	visit_polygons(geom, [&perimeter](const geometry &part) {
		const auto tail = part.get_last_part();
		if (!tail) {
			return;
		}
		auto head = tail;
		do {
			head = head->get_next();
			perimeter += vertex_array_length(*head);
		} while (head != tail);
	});

	return perimeter;
}

uint32_t ops::get_total_vertex_count(const geometry &geom) {
	uint32_t count = 0;

	visit_vertex_arrays(geom, [&count](const geometry &part) { count += part.get_vertex_count(); });

	return count;
}

uint32_t ops::get_total_extent_xy(const geometry &geom, extent_xy &ext) {
	uint32_t count = 0;

	visit_vertex_arrays(geom, [&count, &ext](const geometry &part) {
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_array = part.get_vertex_array();
		const auto vertex_width = part.get_vertex_width();

		vertex_xy vertex;
		for (uint32_t i = 0; i < vertex_count; i++) {
			memcpy(&vertex, vertex_array + i * vertex_width, sizeof(vertex_xy));

			ext.min.x = math::min(ext.min.x, vertex.x);
			ext.min.y = math::min(ext.min.y, vertex.y);
			ext.max.x = math::max(ext.max.x, vertex.x);
			ext.max.y = math::max(ext.max.y, vertex.y);
		}

		count += vertex_count;
	});

	return count;
}

uint32_t ops::get_total_extent_xyzm(const geometry &geom, extent_xyzm &ext) {
	uint32_t count = 0;

	visit_vertex_arrays(geom, [&count, &ext](const geometry &part) {
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_array = part.get_vertex_array();
		const auto vertex_width = part.get_vertex_width();

		vertex_xyzm vertex = {0, 0, 0, 0};
		for (uint32_t i = 0; i < vertex_count; i++) {
			memcpy(&vertex, vertex_array + i * vertex_width, vertex_width);

			ext.min.x = math::min(ext.min.x, vertex.x);
			ext.min.y = math::min(ext.min.y, vertex.y);
			ext.min.z = math::min(ext.min.z, vertex.z);
			ext.min.m = math::min(ext.min.m, vertex.m);
			ext.max.x = math::max(ext.max.x, vertex.x);
			ext.max.y = math::max(ext.max.y, vertex.y);
			ext.max.z = math::max(ext.max.z, vertex.z);
			ext.max.m = math::max(ext.max.m, vertex.m);
		}

		count += vertex_count;
	});

	return count;
}

int32_t ops::get_max_surface_dimension(const geometry &geom, const bool ignore_empty) {
	const auto root = geom.get_parent();

	int32_t max_dim = -1;

	auto part = &geom;
	while (true) {
		if (!(part->is_empty() && ignore_empty)) {
			switch (part->get_type()) {
			case geometry_type::POINT:
			case geometry_type::MULTI_POINT:
				max_dim = math::max(max_dim, 0);
				break;
			case geometry_type::LINESTRING:
			case geometry_type::MULTI_LINESTRING:
				max_dim = math::max(max_dim, 1);
				break;
			case geometry_type::POLYGON:
			case geometry_type::MULTI_POLYGON:
				max_dim = math::max(max_dim, 2);
				break;
			case geometry_type::GEOMETRY_COLLECTION:
				if (part->is_empty()) {
					break;
				}
				part = part->get_first_part();
				continue;
			default:
				break;
			}
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return max_dim;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

void ops::visit_point_geometries(const geometry &geom, void *state,
                                 void (*callback)(void *state, const geometry &part)) {
	visit_points(geom, [state, callback](const geometry &part) {
		SGL_ASSERT(part.get_type() == geometry_type::POINT);
		callback(state, part);
	});
}

void ops::visit_linestring_geometries(const geometry &geom, void *state,
                                      void (*callback)(void *state, const geometry &part)) {
	visit_lines(geom, [state, callback](const geometry &part) {
		SGL_ASSERT(part.get_type() == geometry_type::LINESTRING);
		callback(state, part);
	});
}

void ops::visit_polygon_geometries(const geometry &geom, void *state,
                                   void (*callback)(void *state, const geometry &part)) {
	visit_polygons(geom, [state, callback](const geometry &part) {
		SGL_ASSERT(part.get_type() == geometry_type::POLYGON);
		callback(state, part);
	});
}

} // namespace sgl

//======================================================================================================================
// Linestring Operations
//======================================================================================================================

namespace sgl {

bool linestring::is_closed(const geometry &geom) {
	SGL_ASSERT(geom.get_type() == geometry_type::LINESTRING);

	if (geom.get_vertex_count() < 2) {
		return false;
	}

	const auto first = geom.get_vertex_xyzm(0);
	const auto last = geom.get_vertex_xyzm(geom.get_vertex_count() - 1);

	// TODO: Make this robust
	return first.x == last.x && first.y == last.y && first.z == last.z && first.m == last.m;
}

bool multi_linestring::is_closed(const geometry &geom) {
	SGL_ASSERT(geom.get_type() == geometry_type::MULTI_LINESTRING);

	const auto tail = geom.get_last_part();
	if (!tail) {
		return false;
	}

	auto part = tail;
	do {
		part = part->get_next();
		if (!linestring::is_closed(*part)) {
			return false;
		}
	} while (part != tail);

	return true;
}

bool linestring::interpolate(const geometry &geom, double frac, vertex_xyzm &out) {
	if (geom.get_type() != geometry_type::LINESTRING) {
		return false;
	}
	if (geom.is_empty()) {
		return false;
	}

	const auto vertex_width = geom.get_vertex_width();
	const auto vertex_array = geom.get_vertex_array();
	const auto vertex_count = geom.get_vertex_count();

	if (geom.get_vertex_count() == 1) {
		memcpy(&out, vertex_array, vertex_width);
		return true;
	}

	// Clamp the fraction to [0, 1]
	frac = math::min(math::max(frac, 0.0), 1.0);

	// Special cases
	if (frac == 0) {
		memcpy(&out, vertex_array, vertex_width);
		return true;
	}

	if (frac == 1) {
		memcpy(&out, vertex_array + (vertex_count - 1) * vertex_width, vertex_width);
		return true;
	}

	const auto actual_length = ops::get_length(geom);
	const auto target_length = actual_length * frac;

	// Compute the length of each segment, stop when we reach the target length
	double length = 0.0;

	vertex_xyzm prev = {0, 0, 0, 0};
	vertex_xyzm next = {0, 0, 0, 0};

	memcpy(&prev, vertex_array, vertex_width);

	for (size_t i = 1; i < vertex_count; i++) {
		memcpy(&next, vertex_array + i * vertex_width, vertex_width);
		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;

		const auto segment_length = std::sqrt(dx * dx + dy * dy);

		if (length + segment_length >= target_length) {
			const auto remaining = target_length - length;
			const auto sfrac = remaining / segment_length;
			out.x = prev.x + sfrac * (next.x - prev.x);
			out.y = prev.y + sfrac * (next.y - prev.y);
			out.z = prev.z + sfrac * (next.z - prev.z);
			out.m = prev.m + sfrac * (next.m - prev.m);
			return true;
		}
		length += segment_length;
		prev = next;
	}

	return false;
}

void linestring::interpolate_points(allocator &alloc, const geometry &geom, double frac, geometry &result) {

	result.set_z(geom.has_z());
	result.set_m(geom.has_m());

	if (geom.get_type() != geometry_type::LINESTRING) {
		result.set_type(geometry_type::POINT);
		return;
	}
	if (geom.is_empty()) {
		result.set_type(geometry_type::POINT);
		return;
	}

	if (geom.get_vertex_count() == 1) {
		result.set_type(geometry_type::POINT);
		result.set_vertex_array(geom.get_vertex_array(), 1);
		return;
	}

	// Clamp the fraction to [0, 1]
	frac = math::min(math::max(frac, 0.0), 1.0);

	const auto vertex_width = geom.get_vertex_width();
	const auto vertex_array = geom.get_vertex_array();
	const auto vertex_count = geom.get_vertex_count();

	// Special cases
	if (frac == 0) {
		result.set_type(geometry_type::POINT);
		result.set_vertex_array(geom.get_vertex_array(), 1);
		return;
	}

	if (frac == 1) {
		result.set_type(geometry_type::POINT);
		result.set_vertex_array(vertex_array + (vertex_count - 1) * vertex_width, 1);
		return;
	}

	// Make a multi-point
	result.set_type(geometry_type::MULTI_POINT);

	const auto actual_length = ops::get_length(geom); // TODO: use linstring::length
	double total_length = 0.0;
	double next_target = frac * actual_length;

	vertex_xyzm prev = {0, 0, 0, 0};
	vertex_xyzm next = {0, 0, 0, 0};

	memcpy(&prev, vertex_array, vertex_width);

	// Each target length, we add a point
	for (size_t i = 1; i < vertex_count; i++) {
		memcpy(&next, vertex_array + i * vertex_width, vertex_width);
		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;

		const auto segment_length = std::sqrt(dx * dx + dy * dy);

		// There can be multiple points on the same segment, so we need to loop here
		while (total_length + segment_length >= next_target) {
			const auto remaining = next_target - total_length;
			const auto sfrac = remaining / segment_length;

			vertex_xyzm point = {};
			point.x = prev.x + sfrac * (next.x - prev.x);
			point.y = prev.y + sfrac * (next.y - prev.y);
			point.z = prev.z + sfrac * (next.z - prev.z);
			point.m = prev.m + sfrac * (next.m - prev.m);

			// Allocate memory for the point vertex
			const auto data_mem = static_cast<char *>(alloc.alloc(vertex_width));
			memcpy(data_mem, &point, vertex_width);

			// Allocate memory for the point geometry
			auto point_mem = static_cast<char *>(alloc.alloc(sizeof(sgl::geometry)));
			auto point_ptr = new (point_mem) sgl::geometry(geometry_type::POINT, geom.has_z(), geom.has_m());

			// Set the vertex data and append it to the multi-point
			point_ptr->set_vertex_array(data_mem, 1);
			result.append_part(point_ptr);

			// Update the target length
			next_target += frac * actual_length;
		}
		total_length += segment_length;
		prev = next;
	}
}

// Returns an interpolated "m" value for at the closest location from the line to the point
bool linestring::interpolate_point(const geometry &linear_geom, const geometry &point_geom, double &out_measure) {
	if (linear_geom.get_type() != geometry_type::LINESTRING || point_geom.get_type() != geometry_type::POINT) {
		return false;
	}
	if (linear_geom.is_empty() || point_geom.is_empty()) {
		return false;
	}
	if (!linear_geom.has_m()) {
		return false; // No "m" values to interpolate
	}

	const auto vertex_width = linear_geom.get_vertex_width();
	const auto vertex_array = linear_geom.get_vertex_array();
	const auto vertex_count = linear_geom.get_vertex_count();

	if (vertex_count < 2) {
		return false; // Not enough points to interpolate
	}

	const auto m_offset = linear_geom.has_z() ? 3 * sizeof(double) : 2 * sizeof(double);

	vertex_xy point = point_geom.get_vertex_xy(0);

	double min_distance = std::numeric_limits<double>::max();

	vertex_xy prev = {0, 0};
	double prev_m = 0.0;
	vertex_xy next = {0, 0};
	double next_m = 0.0;

	memcpy(&prev, vertex_array, sizeof(vertex_xy));
	memcpy(&prev_m, vertex_array + m_offset, sizeof(double));

	for (size_t i = 1; i < vertex_count; i++) {
		memcpy(&next, vertex_array + i * vertex_width, sizeof(vertex_xy));
		memcpy(&next_m, vertex_array + i * vertex_width + m_offset, sizeof(double));

		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;

		const auto segment_length = std::sqrt(dx * dx + dy * dy);
		if (segment_length == 0) {
			prev = next;
			continue; // Skip zero-length segments
		}

		const double t = ((point.x - prev.x) * dx + (point.y - prev.y) * dy) / (segment_length * segment_length);
		const double clamped_t = math::clamp(t, 0.0, 1.0);

		const double closest_x = prev.x + clamped_t * dx;
		const double closest_y = prev.y + clamped_t * dy;

		const double distance_squared =
		    (closest_x - point.x) * (closest_x - point.x) + (closest_y - point.y) * (closest_y - point.y);

		if (distance_squared < min_distance) {
			min_distance = distance_squared;
			out_measure = prev_m + clamped_t * (next_m - prev_m);
		}

		prev = next;
		prev_m = next_m;
	}

	return true;
}

void linestring::locate_along(allocator &alloc, const geometry &linear_geom, double measure, double offset,
                              geometry &out_geom) {
	if (linear_geom.get_type() != geometry_type::LINESTRING) {
		return;
	}
	if (linear_geom.is_empty()) {
		return;
	}
	if (!linear_geom.has_m()) {
		return; // No "m" values to locate along
	}

	const auto vertex_width = linear_geom.get_vertex_width();
	const auto vertex_array = linear_geom.get_vertex_array();
	const auto vertex_count = linear_geom.get_vertex_count();

	if (vertex_count < 2) {
		return; // Not enough points to locate along
	}

	const auto has_z = linear_geom.has_z();
	const auto has_m = linear_geom.has_m();

	const auto m_offset = has_z ? 3 : 2;
	const auto z_offset = has_z ? 2 : 3;

	vertex_xyzm prev = {0, 0, 0, 0};
	vertex_xyzm next = {0, 0, 0, 0};

	memcpy(&prev, vertex_array, vertex_width);

	// Loop over the segments of the line
	for (uint32_t i = 1; i < vertex_count; i++) {
		memcpy(&next, vertex_array + i * vertex_width, vertex_width);

		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;

		const auto segment_length = std::sqrt(dx * dx + dy * dy);
		if (segment_length == 0) {
			prev = next;
			continue; // Skip zero-length segments
		}

		const auto prev_m = prev[m_offset];
		const auto next_m = next[m_offset];

		if (measure == prev_m) {
			// If the measure matches the previous point, just use it
			vertex_xyzm point = prev;
			if (offset != 0.0) {
				const double offset_x = offset * dy / segment_length;  // Perpendicular offset
				const double offset_y = -offset * dx / segment_length; // Perpendicular offset
				point.x += offset_x;
				point.y += offset_y;
			}

			// Allocate memory for the point vertex
			const auto mem = static_cast<char *>(alloc.alloc(vertex_width));
			memcpy(mem, &point, vertex_width);

			// Allocate memory for the point geometry
			const auto point_mem = alloc.alloc(sizeof(sgl::geometry));
			const auto point_ptr = new (point_mem) sgl::geometry(geometry_type::POINT, has_z, has_m);

			point_ptr->set_vertex_array(mem, 1);

			// Append the point to the output geometry
			out_geom.append_part(point_ptr);

			prev = next; // Move to the next segment
			continue;
		}

		// If the measure is on the segment, interpolate the point
		if (prev_m < measure && next_m > measure) {
			const auto t = (measure - prev_m) / (next_m - prev_m);

			vertex_xyzm point = {};
			point.x = prev.x + t * dx;
			point.y = prev.y + t * dy;
			point[m_offset] = measure; // Set the "m" value to the measure

			// If the geometry has Z values, interpolate them as well
			if (has_z) {
				point[z_offset] = prev[z_offset] + t * (next[z_offset] - prev[z_offset]);
			}

			if (offset != 0.0) {
				const double offset_x = offset * dy / segment_length;  // Perpendicular offset
				const double offset_y = -offset * dx / segment_length; // Perpendicular offset
				point.x += offset_x;
				point.y += offset_y;
			}

			// Allocate memory for the point vertex
			const auto mem = static_cast<char *>(alloc.alloc(vertex_width));
			memcpy(mem, &point, vertex_width);

			// Allocate memory for the point geometry
			const auto point_mem = alloc.alloc(sizeof(sgl::geometry));
			const auto point_ptr = new (point_mem) sgl::geometry(geometry_type::POINT, has_z, has_m);

			point_ptr->set_vertex_array(mem, 1);

			// Append the point to the output geometry
			out_geom.append_part(point_ptr);

			prev = next; // Move to the next segment
			continue;
		}

		// Else, if this is the last segment and the next point has the same "m" value, include it too
		if (i == vertex_count - 1 && next_m == measure) {
			vertex_xyzm point = next;
			if (offset != 0.0) {
				const double offset_x = offset * dy / segment_length;  // Perpendicular offset
				const double offset_y = -offset * dx / segment_length; // Perpendicular offset
				point.x += offset_x;
				point.y += offset_y;
			}
			// Allocate memory for the point vertex
			const auto mem = static_cast<char *>(alloc.alloc(vertex_width));
			memcpy(mem, &point, vertex_width);

			// Allocate memory for the point geometry
			const auto point_mem = alloc.alloc(sizeof(sgl::geometry));
			const auto point_ptr = new (point_mem) sgl::geometry(geometry_type::POINT, has_z, has_m);

			point_ptr->set_vertex_array(mem, 1);

			// Append the point to the output geometry
			out_geom.append_part(point_ptr);
		}
	}
}

class vertex_vec {
public:
	vertex_vec(allocator &alloc, uint32_t vertex_width)
	    : alloc(alloc), vertex_width(vertex_width), vertex_count(0), vertex_total(0), vertex_array(nullptr) {
	}

	void push_back(const vertex_xyzm &v) {
		reserve(vertex_count + 1);
		memcpy(vertex_array + vertex_count * vertex_width, &v, vertex_width);
		vertex_count++;
	}

	void push_back(const vertex_xy &v) {
		vertex_xyzm v_xyzm = {v.x, v.y, 0.0, 0.0};
		push_back(v_xyzm);
	}

	void reserve(uint32_t new_size) {
		if (new_size > vertex_total) {
			if (vertex_array == nullptr) {
				const auto new_total = std::max(new_size, static_cast<uint32_t>(4));
				vertex_array = static_cast<char *>(alloc.alloc(new_total * vertex_width));
				vertex_total = new_total;
			} else {
				const auto new_total = std::max(new_size, vertex_total * 2);
				vertex_array = static_cast<char *>(
				    alloc.realloc(vertex_array, vertex_total * vertex_width, new_total * vertex_width));
				vertex_total = new_total;
			}
		}
	}

	uint32_t size() const {
		return vertex_count;
	}

	void assign_and_give_ownership(geometry &geom) {
		SGL_ASSERT(geom.get_type() == geometry_type::LINESTRING || geom.get_type() == geometry_type::POINT);
		SGL_ASSERT(geom.get_vertex_width() == vertex_width);

		geom.set_vertex_array(vertex_array, vertex_count);
		vertex_array = nullptr; // Transfer ownership to the geometry
		vertex_count = 0;       // Reset the count since the geometry now owns the data
		vertex_total = 0;       // Reset the total size since the geometry now owns the data
	}

private:
	allocator &alloc;
	uint32_t vertex_width;
	uint32_t vertex_count;
	uint32_t vertex_total;
	char *vertex_array;
};

void linestring::locate_between(allocator &alloc, const geometry &linear_geom, double measure_lower,
                                double measure_upper, double offset, geometry &out_geom) {
	if (linear_geom.get_type() != geometry_type::LINESTRING) {
		return;
	}
	if (linear_geom.is_empty()) {
		return;
	}
	if (!linear_geom.has_m()) {
		return; // No "m" values to locate along
	}
	if (measure_lower > measure_upper) {
		return; // Invalid range
	}

	const auto vertex_width = linear_geom.get_vertex_width();
	const auto vertex_array = linear_geom.get_vertex_array();
	const auto vertex_count = linear_geom.get_vertex_count();

	if (vertex_count < 2) {
		return; // Not enough points to locate along
	}

	const auto has_z = linear_geom.has_z();
	const auto has_m = linear_geom.has_m();

	const auto m_offset = has_z ? 3 : 2;
	const auto z_offset = has_z ? 2 : 3;

	vertex_xyzm prev = {0, 0, 0, 0};
	vertex_xyzm next = {0, 0, 0, 0};

	// The goal: collect all the vertices that are between the measures. Interpolate them if necessary.
	vertex_vec filtered_vertices(alloc, vertex_width);

	memcpy(&prev, vertex_array, vertex_width);

	// Loop over the segments of the line
	for (uint32_t i = 1; i < vertex_count; i++) {
		memcpy(&next, vertex_array + i * vertex_width, vertex_width);

		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;

		const auto segment_length = std::sqrt(dx * dx + dy * dy);
		if (segment_length == 0) {
			prev = next;
			continue; // Skip zero-length segments
		}

		const auto prev_m = prev[m_offset];
		const auto next_m = next[m_offset];

		// TODO: Check that they cant be equal
		if (prev_m < measure_lower && next_m > measure_lower) {
			// Segment intersects with lower bound
			// This is the start of a new line, so we need to add the start point
			const double t_beg = (measure_lower - prev_m) / (next_m - prev_m);
			vertex_xyzm beg_point = {};
			beg_point.x = prev.x + t_beg * dx;
			beg_point.y = prev.y + t_beg * dy;
			beg_point[m_offset] = measure_lower;
			if (has_z) {
				beg_point[z_offset] = prev[z_offset] + t_beg * (next[z_offset] - prev[z_offset]);
			}

			if (offset != 0.0) {
				const double offset_x = offset * dy / segment_length;  // Perpendicular offset
				const double offset_y = -offset * dx / segment_length; // Perpendicular offset
				beg_point.x += offset_x;
				beg_point.y += offset_y;
			}

			// Add the start point to the line
			filtered_vertices.push_back(beg_point);
		}

		if (prev_m >= measure_lower && prev_m <= measure_upper) {
			// The previous vertex is inside the range

			if (offset != 0.0) {
				// Apply the offset to the previous vertex
				const double offset_x = offset * dy / segment_length;  // Perpendicular offset
				const double offset_y = -offset * dx / segment_length; // Perpendicular offset

				vertex_xyzm offset_vertex = prev;
				offset_vertex.x += offset_x;
				offset_vertex.y += offset_y;
				filtered_vertices.push_back(offset_vertex);

			} else {
				filtered_vertices.push_back(prev);
			}
		}

		if (prev_m < measure_upper && next_m > measure_upper) {
			// Segment intersects with upper bound
			// This is the end of the current line, so we need to add the end point

			const double t_end = (measure_upper - prev_m) / (next_m - prev_m);
			vertex_xyzm end_point = {};
			end_point.x = prev.x + t_end * dx;
			end_point.y = prev.y + t_end * dy;
			end_point[m_offset] = measure_upper;
			if (has_z) {
				end_point[z_offset] = prev[z_offset] + t_end * (next[z_offset] - prev[z_offset]);
			}

			if (offset != 0.0) {
				const double offset_x = offset * dy / segment_length;  // Perpendicular offset
				const double offset_y = -offset * dx / segment_length; // Perpendicular offset
				end_point.x += offset_x;
				end_point.y += offset_y;
			}

			// Add the end point to the line
			filtered_vertices.push_back(end_point);

			// Now we can create a new geometry from the collected points
			const auto part_type = filtered_vertices.size() == 1 ? geometry_type::POINT : geometry_type::LINESTRING;
			const auto part_mem = static_cast<char *>(alloc.alloc(sizeof(geometry)));
			const auto part_ptr = new (part_mem) geometry(part_type, has_z, has_m);

			// Assign the collected vertices to the part
			filtered_vertices.assign_and_give_ownership(*part_ptr);

			// Append the part to the output geometry
			out_geom.append_part(part_ptr);
		} else if (i == vertex_count - 1) {
			// If we are at the last vertex and it is inside the range, we need to add it
			if (next_m >= measure_lower && next_m <= measure_upper) {
				if (offset != 0.0) {
					// Apply the offset to the last vertex
					const double offset_x = offset * dy / segment_length;  // Perpendicular offset
					const double offset_y = -offset * dx / segment_length; // Perpendicular offset

					vertex_xyzm offset_vertex = next;
					offset_vertex.x += offset_x;
					offset_vertex.y += offset_y;
					filtered_vertices.push_back(offset_vertex);
				} else {
					filtered_vertices.push_back(next);
				}
			}
		}

		prev = next;
	}

	// Push whatever is left in the last segment
	if (filtered_vertices.size() > 0) {

		const auto part_type = filtered_vertices.size() == 1 ? geometry_type::POINT : geometry_type::LINESTRING;
		const auto part_mem = static_cast<char *>(alloc.alloc(sizeof(geometry)));
		const auto part_ptr = new (part_mem) geometry(part_type, has_z, has_m);

		// Assign the collected vertices to the part
		filtered_vertices.assign_and_give_ownership(*part_ptr);

		// Append the part to the output geometry
		out_geom.append_part(part_ptr);
	}
}

// TODO: Make use of prepared geometry to accelerate this operation
double linestring::line_locate_point(const geometry &line_geom, const geometry &point_geom) {
	SGL_ASSERT(line_geom.get_type() == geometry_type::LINESTRING);
	SGL_ASSERT(point_geom.get_type() == geometry_type::POINT);
	SGL_ASSERT(!line_geom.is_empty() && !point_geom.is_empty());

	const auto point = point_geom.get_vertex_xy(0);

	const auto vertex_width = line_geom.get_vertex_width();
	const auto vertex_array = line_geom.get_vertex_array();
	const auto vertex_count = line_geom.get_vertex_count();

	vertex_xy prev = {0, 0};
	vertex_xy next = {0, 0};

	memcpy(&prev, vertex_array, vertex_width);

	double length = 0.0;
	double closest_sqdist = std::numeric_limits<double>::max();
	double closest_length = 0.0;

	for (uint32_t i = 1; i < vertex_count; i++) {
		memcpy(&next, vertex_array + i * vertex_width, vertex_width);

		const auto segment_length_squared = vertex_distance_squared(prev, next);
		if (segment_length_squared == 0) {
			// Compute the distance to the previous point
			// This is a zero-length segment, so we can just check the distance to the previous point
			const auto sqdist = vertex_distance_squared(prev, point);
			if (sqdist < closest_sqdist) {
				closest_length = length;
				closest_sqdist = sqdist;
			}
			prev = next;
			continue;
		}

		// Otherwise, compute the projection of the point onto the segment
		const double t =
		    ((point.x - prev.x) * (next.x - prev.x) + (point.y - prev.y) * (next.y - prev.y)) / segment_length_squared;
		const double clamped_t = math::clamp(t, 0.0, 1.0);
		const double closest_x = prev.x + clamped_t * (next.x - prev.x);
		const double closest_y = prev.y + clamped_t * (next.y - prev.y);
		const double sqdist =
		    (closest_x - point.x) * (closest_x - point.x) + (closest_y - point.y) * (closest_y - point.y);

		const auto segment_length = std::sqrt(segment_length_squared);
		if (sqdist < closest_sqdist) {
			closest_sqdist = sqdist;
			closest_length = length + clamped_t * segment_length;
		}

		length += segment_length;
		prev = next;
	}

	if (closest_length == 0 || length == 0) {
		return 0.0; // The point is at the start of the line
	}
	return closest_length / length;
}

void linestring::substring(allocator &alloc, const geometry &geom, double beg_frac, double end_frac, geometry &result) {
	result.set_type(geometry_type::LINESTRING);
	result.set_z(geom.has_z());
	result.set_m(geom.has_m());

	if (geom.get_type() != sgl::geometry_type::LINESTRING) {
		return;
	}
	if (geom.is_empty()) {
		if (beg_frac == end_frac) {
			result.set_type(geometry_type::POINT);
		}
		return;
	}

	if (beg_frac > end_frac) {
		return;
	}

	beg_frac = math::min(math::max(beg_frac, 0.0), 1.0);
	end_frac = math::min(math::max(end_frac, 0.0), 1.0);

	const auto vertex_width = geom.get_vertex_width();
	const auto vertex_array = geom.get_vertex_array();
	const auto vertex_count = geom.get_vertex_count();

	// Reference the whole line
	if (beg_frac == 0 && end_frac == 1) {
		result.set_vertex_array(vertex_array, vertex_count);
		return;
	}

	if (beg_frac == end_frac) {
		// Just interpolate once
		vertex_xyzm point = {};

		// Make it a point instead!
		result.set_type(geometry_type::POINT);

		if (interpolate(geom, beg_frac, point)) {
			const auto mem = static_cast<char *>(alloc.alloc(vertex_width));
			memcpy(mem, &point, vertex_width);
			result.set_vertex_array(mem, 1);
		}

		return;
	}

	vertex_xyzm beg = {};
	size_t beg_idx = 0;
	vertex_xyzm end = {};
	size_t end_idx = 0;

	const double total_length = ops::get_length(geom); // TODO: use linstring::length
	const double beg_length = total_length * beg_frac;
	const double end_length = total_length * end_frac;
	double length = 0.0;

	vertex_xyzm prev = {0, 0, 0, 0};
	vertex_xyzm next = {0, 0, 0, 0};

	memcpy(&prev, vertex_array, vertex_width);

	// First look for the beg point
	size_t vertex_idx = 1;

	for (; vertex_idx < vertex_count; vertex_idx++) {
		memcpy(&next, vertex_array + vertex_idx * vertex_width, vertex_width);
		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;
		const auto segment_length = std::sqrt(dx * dx + dy * dy);

		if (length + segment_length >= beg_length) {
			const auto remaining = beg_length - length;
			const auto sfrac = remaining / segment_length;
			beg.x = prev.x + sfrac * (next.x - prev.x);
			beg.y = prev.y + sfrac * (next.y - prev.y);
			beg.z = prev.z + sfrac * (next.z - prev.z);
			beg.m = prev.m + sfrac * (next.m - prev.m);

			beg_idx = vertex_idx - 1;
			break;
		}
		length += segment_length;
		prev = next;
	}

	// Now look for the end point
	for (; vertex_idx < vertex_count; vertex_idx++) {
		memcpy(&next, vertex_array + vertex_idx * vertex_width, vertex_width);
		const auto dx = next.x - prev.x;
		const auto dy = next.y - prev.y;
		const auto segment_length = std::sqrt(dx * dx + dy * dy);

		if (length + segment_length >= end_length) {
			const auto remaining = end_length - length;
			const auto sfrac = remaining / segment_length;
			end.x = prev.x + sfrac * (next.x - prev.x);
			end.y = prev.y + sfrac * (next.y - prev.y);
			end.z = prev.z + sfrac * (next.z - prev.z);
			end.m = prev.m + sfrac * (next.m - prev.m);

			end_idx = vertex_idx - 1;
			break;
		}
		length += segment_length;
		prev = next;
	}

	// Now create a new line containing beg, all the points in between, and end
	const auto new_vertex_count = end_idx - beg_idx + 2;
	const auto new_vertex_size = new_vertex_count * vertex_width;
	const auto new_vertex_data = static_cast<char *>(alloc.alloc(new_vertex_size));

	// Copy the beg point
	memcpy(new_vertex_data, &beg, vertex_width);
	// Copy the points in between
	memcpy(new_vertex_data + vertex_width, vertex_array + (beg_idx + 1) * vertex_width,
	       (new_vertex_count - 2) * vertex_width);
	// Copy the end point
	memcpy(new_vertex_data + (new_vertex_count - 1) * vertex_width, &end, vertex_width);

	result.set_vertex_array(new_vertex_data, new_vertex_count);
}

void polygon::init_from_bbox(allocator &alloc, double min_x, double min_y, double max_x, double max_y,
                             geometry &result) {
	result.set_type(geometry_type::POLYGON);
	result.set_z(false);
	result.set_m(false);

	const auto ring_mem = alloc.alloc(sizeof(geometry));
	const auto ring_ptr = new (ring_mem) geometry(geometry_type::LINESTRING, false, false);

	const auto data_mem = alloc.alloc(2 * sizeof(double) * 5);
	const auto data_ptr = static_cast<double *>(data_mem);

	data_ptr[0] = min_x;
	data_ptr[1] = min_y;

	data_ptr[2] = min_x;
	data_ptr[3] = max_y;

	data_ptr[4] = max_x;
	data_ptr[5] = max_y;

	data_ptr[6] = max_x;
	data_ptr[7] = min_y;

	data_ptr[8] = min_x;
	data_ptr[9] = min_y;

	ring_ptr->set_vertex_array(data_mem, 5);
	result.append_part(ring_ptr);
}

} // namespace sgl
//======================================================================================================================
// Centroid
//======================================================================================================================

namespace sgl {

void ops::locate_along(allocator &alloc, const geometry &geom, double measure, double offset, geometry &out_geom) {

	if (!geom.has_m()) {
		return; // No "m" values to locate along
	}

	const auto has_z = geom.has_z();

	visit_leaf_geometries(geom, [&](const geometry &part) {
		if (part.is_empty()) {
			return; // Skip empty geometries
		}
		switch (part.get_type()) {
		case geometry_type::POINT: {
			const auto vertex = part.get_vertex_xyzm(0);
			if ((has_z && vertex.m == measure) || (!has_z && vertex.z == measure)) {
				// If the point's "m" value matches the measure, include it in the output

				// Make a new point geometry
				const auto point_mem = alloc.alloc(sizeof(sgl::geometry));
				const auto point_ptr = new (point_mem) sgl::geometry(geometry_type::POINT, has_z, part.has_m());

				// Make the point reference the same vertex
				point_ptr->set_vertex_array(part.get_vertex_array(), 1);

				// Push the point to the output geometry
				out_geom.append_part(point_ptr);
			}
		} break;
		case geometry_type::LINESTRING: {
			linestring::locate_along(alloc, part, measure, offset, out_geom);
		} break;
		case geometry_type::POLYGON: {
			// Only consider the outer ring of the polygon
			const auto shell = part.get_first_part();
			linestring::locate_along(alloc, *shell, measure, offset, out_geom);
		} break;
		default:
			// Unsupported geometry type
			SGL_ASSERT(false);
			return;
		}
	});
}

void ops::locate_between(allocator &alloc, const geometry &geom, double measure_lower, double measure_upper,
                         double offset, geometry &out_geom) {
	if (!geom.has_m()) {
		return; // No "m" values to locate along
	}

	const auto has_z = geom.has_z();

	visit_leaf_geometries(geom, [&](const geometry &part) {
		if (part.is_empty()) {
			return; // Skip empty geometries
		}
		switch (part.get_type()) {
		case geometry_type::POINT: {
			const auto vertex = part.get_vertex_xyzm(0);
			if ((has_z && vertex.m >= measure_lower && vertex.m <= measure_upper) ||
			    (!has_z && vertex.z >= measure_lower && vertex.z <= measure_upper)) {

				// Make a new point geometry
				const auto point_mem = alloc.alloc(sizeof(sgl::geometry));
				const auto point_ptr = new (point_mem) sgl::geometry(geometry_type::POINT, has_z, part.has_m());

				// Make the point reference the same vertex
				point_ptr->set_vertex_array(part.get_vertex_array(), 1);

				// Push the point to the output geometry
				out_geom.append_part(point_ptr);
			}
		} break;
		case geometry_type::LINESTRING: {
			linestring::locate_between(alloc, part, measure_lower, measure_upper, offset, out_geom);
		} break;
		case geometry_type::POLYGON: {
			// Only consider the outer ring of the polygon
			const auto shell = part.get_first_part();
			linestring::locate_between(alloc, *shell, measure_lower, measure_upper, offset, out_geom);
		} break;
		default:
			// Unsupported geometry type
			SGL_ASSERT(false);
			return;
		}
	});
}

bool ops::get_centroid_from_points(const geometry &geom, vertex_xyzm &out) {

	uint32_t total_count = 0;
	vertex_xyzm centroid = {0, 0, 0, 0};

	visit_points(geom, [&centroid, &total_count](const geometry &part) {
		if (part.is_empty()) {
			return;
		}

		const auto v_array = part.get_vertex_array();
		const auto v_width = part.get_vertex_width();

		vertex_xyzm vertex = {0, 0, 0, 0};
		memcpy(&vertex, v_array, v_width);

		centroid.x += vertex.x;
		centroid.y += vertex.y;
		centroid.z += vertex.z;
		centroid.m += vertex.m;

		total_count++;
	});

	if (total_count > 0) {
		out.x = centroid.x / total_count;
		out.y = centroid.y / total_count;
		out.z = centroid.z / total_count;
		out.m = centroid.m / total_count;
		return true;
	}
	return false;
}

bool ops::get_centroid_from_linestrings(const geometry &geom, vertex_xyzm &out) {

	double total_length = 0;
	vertex_xyzm centroid = {0, 0, 0, 0};

	visit_lines(geom, [&centroid, &total_length](const geometry &part) {
		if (part.is_empty()) {
			return;
		}

		const auto v_array = part.get_vertex_array();
		const auto v_count = part.get_vertex_count();
		const auto v_width = part.get_vertex_width();

		vertex_xyzm prev = {0, 0, 0, 0};
		vertex_xyzm next = {0, 0, 0, 0};

		memcpy(&prev, v_array, v_width);
		for (uint32_t i = 1; i < v_count; i++) {
			memcpy(&next, v_array + i * v_width, v_width);

			const auto dx = next.x - prev.x;
			const auto dy = next.y - prev.y;

			const auto segment_length = std::sqrt(dx * dx + dy * dy);

			centroid.x += (next.x + prev.x) * segment_length;
			centroid.y += (next.y + prev.y) * segment_length;
			centroid.z += (next.z + prev.z) * segment_length;
			centroid.m += (next.m + prev.m) * segment_length;

			total_length += segment_length;
			prev = next;
		}
	});

	if (total_length != 0) {
		out.x = centroid.x / 2.0 / total_length;
		out.y = centroid.y / 2.0 / total_length;
		out.z = centroid.z / 2.0 / total_length;
		out.m = centroid.m / 2.0 / total_length;
		return true;
	}
	return false;
}

bool ops::get_centroid_from_polygons(const geometry &geom, vertex_xyzm &out) {

	double total_area2 = 0;
	vertex_xyzm centroid = {0, 0, 0, 0};

	visit_polygons(geom, [&centroid, &total_area2](const geometry &part) {
		const auto tail = part.get_last_part();
		if (!tail) {
			return;
		}

		vertex_xyzm base = {0, 0, 0, 0};

		auto head = tail;
		do {
			head = head->get_next();
			if (head->is_empty()) {
				continue;
			}

			const auto v_array = head->get_vertex_array();
			const auto v_count = head->get_vertex_count();
			const auto v_width = head->get_vertex_width();

			const auto is_shell = head == tail->get_next();
			const auto is_clock = vertex_array_signed_area(*head) >= 0;

			if (is_shell) {
				memcpy(&base, v_array, v_width);
			}

			vertex_xyzm prev = {0, 0, 0, 0};
			vertex_xyzm next = {0, 0, 0, 0};

			const auto sign = is_shell != is_clock ? -1.0 : 1.0;

			memcpy(&prev, v_array, v_width);
			for (uint32_t i = 1; i < v_count; i++) {
				memcpy(&next, v_array + i * v_width, v_width);

				const auto area2 = (prev.x - base.x) * (next.y - base.y) - (next.x - base.x) * (prev.y - base.y);

				centroid.x += sign * area2 * (base.x + next.x + prev.x);
				centroid.y += sign * area2 * (base.y + next.y + prev.y);
				centroid.z += sign * area2 * (base.z + next.z + prev.z);
				centroid.m += sign * area2 * (base.m + next.m + prev.m);
				total_area2 += sign * area2;

				prev = next;
			}
		} while (head != tail);
	});

	if (total_area2 != 0) {

		out.x = centroid.x / 3.0 / total_area2;
		out.y = centroid.y / 3.0 / total_area2;
		out.z = centroid.z / 3.0 / total_area2;
		out.m = centroid.m / 3.0 / total_area2;

		return true;
	}
	return false;
}

bool ops::get_centroid(const geometry &geom, vertex_xyzm &centroid) {
	if (geom.is_empty()) {
		return false;
	}
	const auto dim = get_max_surface_dimension(geom, true);
	switch (dim) {
	case 0:
		return get_centroid_from_points(geom, centroid);
	case 1:
		return get_centroid_from_linestrings(geom, centroid);
	case 2:
		return get_centroid_from_polygons(geom, centroid);
	default:
		return false;
	}
}

} // namespace sgl

//======================================================================================================================
// Extraction
//======================================================================================================================
namespace sgl {

void geometry::filter_parts(void *state, bool (*select_callback)(void *state, const geometry *part),
                            void (*handle_callback)(void *state, geometry *part)) {

	auto tail = get_last_part();

	if (!tail) {
		return;
	}

	auto prev = tail;
	bool shrank = true;

	while (size > 0 && (prev != tail || shrank)) {
		shrank = false;
		auto curr = prev->next;
		auto next = curr->next;

		if (select_callback(state, curr)) {

			// Unlink the current part
			prev->next = next;
			size--;
			shrank = true;

			if (curr == tail) {
				// We removed the tail, update the tail pointer
				tail = prev;
				data = tail;
			}

			// Before passing this to the handle function,
			// null the relationship pointers
			curr->prnt = nullptr;
			curr->next = nullptr;

			// Pass on to the handle callback
			handle_callback(state, curr);

		} else {
			prev = curr;
		}
	}

	if (size == 0) {
		// We extracted everything. Reset the data pointer
		data = nullptr;
	}
}

static bool select_points(void *, const geometry *geom) {
	switch (geom->get_type()) {
	case geometry_type::POINT:
	case geometry_type::MULTI_POINT:
	case geometry_type::GEOMETRY_COLLECTION:
		return true;
	default:
		return false;
	}
}

static void handle_points(void *state, geometry *geom) {
	auto &points = *static_cast<geometry *>(state);

	switch (geom->get_type()) {
	case geometry_type::POINT:
		points.append_part(geom);
		break;
	case geometry_type::MULTI_POINT:
	case geometry_type::GEOMETRY_COLLECTION:
		geom->filter_parts(state, select_points, handle_points);
		break;
	default:
		SGL_ASSERT(false);
		break;
	}
}

static bool select_lines(void *state, const geometry *geom) {
	switch (geom->get_type()) {
	case geometry_type::LINESTRING:
	case geometry_type::MULTI_LINESTRING:
	case geometry_type::GEOMETRY_COLLECTION:
		return true;
	default:
		return false;
	}
}

static void handle_lines(void *state, geometry *geom) {
	auto &lines = *static_cast<geometry *>(state);

	switch (geom->get_type()) {
	case geometry_type::LINESTRING:
		lines.append_part(geom);
		break;
	case geometry_type::MULTI_LINESTRING:
	case geometry_type::GEOMETRY_COLLECTION:
		geom->filter_parts(state, select_lines, handle_lines);
		break;
	default:
		SGL_ASSERT(false);
		break;
	}
}

static bool select_polygons(void *state, const geometry *geom) {
	switch (geom->get_type()) {
	case geometry_type::POLYGON:
	case geometry_type::MULTI_POLYGON:
	case geometry_type::GEOMETRY_COLLECTION:
		return true;
	default:
		return false;
	}
}

static void handle_polygons(void *state, geometry *geom) {
	auto &polygons = *static_cast<geometry *>(state);

	switch (geom->get_type()) {
	case geometry_type::POLYGON:
		polygons.append_part(geom);
		break;
	case geometry_type::MULTI_POLYGON:
	case geometry_type::GEOMETRY_COLLECTION:
		geom->filter_parts(state, select_polygons, handle_polygons);
		break;
	default:
		SGL_ASSERT(false);
		break;
	}
}

// TODO: Make these non-recursive
void ops::extract_points(geometry &geom, geometry &result) {
	result.set_type(geometry_type::MULTI_POINT);
	result.set_z(geom.has_z());
	result.set_m(geom.has_m());

	geom.filter_parts(&result, select_points, handle_points);
}

void ops::extract_linestrings(geometry &geom, geometry &result) {
	result.set_type(geometry_type::MULTI_LINESTRING);
	result.set_z(geom.has_z());
	result.set_m(geom.has_m());

	geom.filter_parts(&result, select_lines, handle_lines);
}

void ops::extract_polygons(geometry &geom, geometry &result) {
	result.set_type(geometry_type::MULTI_POLYGON);
	result.set_z(geom.has_z());
	result.set_m(geom.has_m());

	geom.filter_parts(&result, select_polygons, handle_polygons);
}

} // namespace sgl
//======================================================================================================================
// Distance
//=======================================================================================================================

namespace sgl {

namespace {

//----------------------------------------------------------------------------------------------------------------------
// Distance Cases
//----------------------------------------------------------------------------------------------------------------------

struct distance_result {
	double distance;

	explicit distance_result(const double start) : distance(start) {
	}

	void set(const double &dist) {
		distance = math::min(distance, dist);
	}
};

bool distance_point_point(const geometry &lhs, const geometry &rhs, distance_result &result) {
	SGL_ASSERT(lhs.get_type() == geometry_type::POINT);
	SGL_ASSERT(rhs.get_type() == geometry_type::POINT);

	if (lhs.is_empty() || rhs.is_empty()) {
		// If either point is empty, return false;
		return false;
	}

	const auto lhs_vertex_array = lhs.get_vertex_array();
	const auto rhs_vertex_array = rhs.get_vertex_array();

	vertex_xy lhs_vertex;
	vertex_xy rhs_vertex;

	memcpy(&lhs_vertex, lhs_vertex_array, sizeof(vertex_xy));
	memcpy(&rhs_vertex, rhs_vertex_array, sizeof(vertex_xy));

	result.set(vertex_distance(lhs_vertex, rhs_vertex));

	return true;
}

bool distance_point_lines(const geometry &lhs, const geometry &rhs, distance_result &result) {
	SGL_ASSERT(lhs.get_type() == geometry_type::POINT);
	SGL_ASSERT(rhs.get_type() == geometry_type::LINESTRING);

	if (lhs.is_empty() || rhs.is_empty()) {
		return false;
	}

	const auto lhs_vertex_array = lhs.get_vertex_array();
	const auto rhs_vertex_array = rhs.get_vertex_array();
	const auto rhs_vertex_count = rhs.get_vertex_count();

	vertex_xy lhs_vertex;
	vertex_xy rhs_vertex;

	memcpy(&lhs_vertex, lhs_vertex_array, sizeof(vertex_xy));
	memcpy(&rhs_vertex, rhs_vertex_array, sizeof(vertex_xy));

	if (rhs_vertex_count == 1) {
		// Degenerate case, should not happen
		result.set(vertex_distance(lhs_vertex, rhs_vertex));
		return true;
	}

	// Special case: prepared
	if (rhs.is_prepared()) {
		auto &prep = static_cast<const prepared_geometry &>(rhs);
		double dist = 0;
		if (prep.try_get_distance(lhs_vertex, dist)) {
			result.set(dist);
			return true;
		}
		return false;
	}

	const auto rhs_vertex_width = rhs.get_vertex_width();

	SGL_ASSERT(rhs_vertex_count >= 2);

	for (size_t v_idx = 1; v_idx < rhs_vertex_count; v_idx++) {
		vertex_xy rhs_vertex_next;
		memcpy(&rhs_vertex_next, rhs_vertex_array + v_idx * rhs_vertex_width, sizeof(vertex_xy));

		result.set(vertex_segment_distance(lhs_vertex, rhs_vertex, rhs_vertex_next));

		rhs_vertex = rhs_vertex_next;
	}

	return true;
}

bool distance_point_polyg(const geometry &lhs, const geometry &rhs, distance_result &result) {
	SGL_ASSERT(lhs.get_type() == geometry_type::POINT);
	SGL_ASSERT(rhs.get_type() == geometry_type::POLYGON);

	if (lhs.is_empty() || rhs.is_empty()) {
		// If either point or polygon is empty, return false;
		return false;
	}

	const auto lhs_vertex_array = lhs.get_vertex_array();
	vertex_xy lhs_vertex;
	memcpy(&lhs_vertex, lhs_vertex_array, sizeof(vertex_xy));

	const auto shell = rhs.get_first_part();
	switch (vertex_in_ring(lhs_vertex, *shell)) {
	case point_in_polygon_result::EXTERIOR:
		return distance_point_lines(lhs, *shell, result);
	case point_in_polygon_result::INTERIOR:
		// We need to check the holes
		for (auto ring = shell->get_next(); ring != shell; ring = ring->get_next()) {
			if (vertex_in_ring(lhs_vertex, *ring) != point_in_polygon_result::EXTERIOR) {
				// A point can only be inside a single hole, so we can stop here
				// (holes cant overlap if the polygon is valid)
				return distance_point_lines(lhs, *ring, result);
			}
		}
		// The point is inside the polygon and not inside any holes
		// fall-through
	case point_in_polygon_result::BOUNDARY:
	default:
		result.set(0.0);
		return true;
	}
}

bool distance_lines_lines(const geometry &lhs, const geometry &rhs, distance_result &result) {
	SGL_ASSERT(lhs.get_type() == geometry_type::LINESTRING);
	SGL_ASSERT(rhs.get_type() == geometry_type::LINESTRING);

	if (lhs.is_empty() || rhs.is_empty()) {
		// If either linestring is empty, return false;
		return false;
	}

	if (lhs.is_prepared() && rhs.is_prepared()) {
		// Both linestrings are prepared, so we can use the prepared distance
		auto &lhs_prep = static_cast<const prepared_geometry &>(lhs);
		auto &rhs_prep = static_cast<const prepared_geometry &>(rhs);
		double dist = 0;
		if (lhs_prep.try_get_distance(rhs_prep, dist)) {
			result.set(dist);
			return true;
		}
		return false;
	}

	const auto lhs_vertex_array = lhs.get_vertex_array();
	const auto lhs_vertex_count = lhs.get_vertex_count();
	const auto lhs_vertex_width = lhs.get_vertex_width();

	const auto rhs_vertex_array = rhs.get_vertex_array();
	const auto rhs_vertex_count = rhs.get_vertex_count();
	const auto rhs_vertex_width = rhs.get_vertex_width();

	vertex_xy lhs_prev;
	vertex_xy lhs_next;
	vertex_xy rhs_prev;
	vertex_xy rhs_next;

	if (lhs_vertex_count == 1 && rhs_vertex_count == 1) {
		// Degenerate case, should not happen
		memcpy(&lhs_prev, lhs_vertex_array, sizeof(vertex_xy));
		memcpy(&rhs_prev, rhs_vertex_array, sizeof(vertex_xy));
		result.set(vertex_distance(lhs_prev, rhs_prev));
		return true;
	}

	if (lhs_vertex_count == 1) {
		// LHS is a point, so we can just compute the distance to each segment in RHS
		memcpy(&lhs_prev, lhs_vertex_array, sizeof(vertex_xy));
		memcpy(&rhs_prev, rhs_vertex_array, sizeof(vertex_xy));
		for (uint32_t i = 1; i < rhs_vertex_count; i++) {
			memcpy(&rhs_next, rhs_vertex_array + i * rhs_vertex_width, sizeof(vertex_xy));
			result.set(vertex_segment_distance(lhs_prev, rhs_prev, rhs_next));
			rhs_prev = rhs_next;
		}
		return true;
	}

	if (rhs_vertex_count == 1) {
		// RHS is a point, so we can just compute the distance to each segment in LHS
		memcpy(&rhs_prev, rhs_vertex_array, sizeof(vertex_xy));
		memcpy(&lhs_prev, lhs_vertex_array, sizeof(vertex_xy));
		for (uint32_t i = 1; i < lhs_vertex_count; i++) {
			memcpy(&lhs_next, lhs_vertex_array + i * lhs_vertex_width, sizeof(vertex_xy));
			result.set(vertex_segment_distance(rhs_prev, lhs_prev, lhs_next));
			lhs_prev = lhs_next;
		}
		return true;
	}

	SGL_ASSERT(lhs_vertex_count >= 2 && rhs_vertex_count >= 2);

	// Otherwise, we have two linestrings with at least 2 vertices each
	memcpy(&lhs_prev, lhs_vertex_array, sizeof(vertex_xy));
	for (uint32_t i = 1; i < lhs_vertex_count; i++) {
		memcpy(&lhs_next, lhs_vertex_array + i * lhs_vertex_width, sizeof(vertex_xy));

		memcpy(&rhs_prev, rhs_vertex_array, sizeof(vertex_xy));
		for (uint32_t j = 1; j < rhs_vertex_count; j++) {
			memcpy(&rhs_next, rhs_vertex_array + j * rhs_vertex_width, sizeof(vertex_xy));

			const auto dist = segment_segment_distance(lhs_prev, lhs_next, rhs_prev, rhs_next);
			result.set(dist);

			rhs_prev = rhs_next;
		}

		lhs_prev = lhs_next;
	}

	return true;
}

bool distance_lines_polyg(const geometry &lhs, const geometry &rhs, distance_result &result) {
	SGL_ASSERT(lhs.get_type() == geometry_type::LINESTRING);
	SGL_ASSERT(rhs.get_type() == geometry_type::POLYGON);

	if (lhs.is_empty() || rhs.is_empty()) {
		// If either linestring or polygon is empty, return false;
		return false;
	}

	const auto lhs_vertex_array = lhs.get_vertex_array();
	vertex_xy lhs_vertex;
	memcpy(&lhs_vertex, lhs_vertex_array, sizeof(vertex_xy));

	const auto shell = rhs.get_first_part();
	if (vertex_in_ring(lhs_vertex, *shell) == point_in_polygon_result::EXTERIOR) {
		// The linestring is either outside the polygon, or intersects somewhere on the boundary
		// - If the linestring were completely inside, then _all_ vertices would be inside
		return distance_lines_lines(lhs, *shell, result);
	}

	// Get the distance to each ring
	for (auto ring = shell->get_next(); ring != shell; ring = ring->get_next()) {
		if (!distance_lines_lines(lhs, *ring, result)) {
			return false;
		}
	}

	for (auto ring = shell->get_next(); ring != shell; ring = ring->get_next()) {
		// The linestring is either completely in the hole, or intersects somewhere on hole boundary
		// Regardless, we already have the distance to the hole
		if (vertex_in_ring(lhs_vertex, *ring) != point_in_polygon_result::EXTERIOR) {
			return true;
		}
	}

	// Otherwise, we must be partly inside the polygon, at which point the distance is 0
	result.set(0.0);
	return true;
}

bool distance_polyg_polyg(const geometry &lhs, const geometry &rhs, distance_result &result) {
	SGL_ASSERT(lhs.get_type() == geometry_type::POLYGON);
	SGL_ASSERT(rhs.get_type() == geometry_type::POLYGON);

	if (lhs.is_empty() || rhs.is_empty()) {
		// If either polygon is empty, return false;
		return false;
	}

	const auto lhs_shell = lhs.get_first_part();
	const auto rhs_shell = rhs.get_first_part();

	const auto lhs_vertex_array = lhs_shell->get_vertex_array();
	const auto rhs_vertex_array = rhs_shell->get_vertex_array();

	vertex_xy lhs_vert;
	vertex_xy rhs_vert;

	memcpy(&lhs_vert, lhs_vertex_array, sizeof(vertex_xy));
	memcpy(&rhs_vert, rhs_vertex_array, sizeof(vertex_xy));

	const auto lhs_loc = vertex_in_ring(lhs_vert, *rhs_shell);
	const auto rhs_loc = vertex_in_ring(rhs_vert, *lhs_shell);

	if (lhs_loc == point_in_polygon_result::EXTERIOR && rhs_loc == point_in_polygon_result::EXTERIOR) {
		// The polygons are completely disjoint, or intersect on the boundary
		return distance_lines_lines(*lhs_shell, *rhs_shell, result);
	}

	for (auto lhs_ring = lhs_shell->get_next(); lhs_ring != lhs_shell; lhs_ring = lhs_ring->get_next()) {
		if (vertex_in_ring(rhs_vert, *lhs_ring) != point_in_polygon_result::EXTERIOR) {
			// One polygon is inside the hole of the other, or they intersect on one of the holes.
			return distance_lines_lines(*lhs_ring, *rhs_shell, result);
		}
	}

	for (auto rhs_ring = rhs_shell->get_next(); rhs_ring != rhs_shell; rhs_ring = rhs_ring->get_next()) {
		if (vertex_in_ring(lhs_vert, *rhs_ring) != point_in_polygon_result::EXTERIOR) {
			// One polygon is inside the hole of the other, or they intersect on one of the holes.
			return distance_lines_lines(*lhs_shell, *rhs_ring, result);
		}
	}

	// Otherwise, the polygons must have some intersection, so the distance is 0
	result.set(0.0);
	return true;
}

bool distance_dispatch(const geometry &lhs, const geometry &rhs, distance_result &result) {
	SGL_ASSERT(!lhs.is_multi_geom());
	SGL_ASSERT(!rhs.is_multi_geom());

	switch (lhs.get_type()) {
	case geometry_type::POINT:
		switch (rhs.get_type()) {
		case geometry_type::POINT:
			return distance_point_point(lhs, rhs, result);
		case geometry_type::LINESTRING:
			return distance_point_lines(lhs, rhs, result);
		case geometry_type::POLYGON:
			return distance_point_polyg(lhs, rhs, result);
		default:
			return false;
		}
	case geometry_type::LINESTRING:
		switch (rhs.get_type()) {
		case geometry_type::POINT:
			return distance_point_lines(rhs, lhs, result);
		case geometry_type::LINESTRING:
			return distance_lines_lines(lhs, rhs, result);
		case geometry_type::POLYGON:
			return distance_lines_polyg(lhs, rhs, result);
		default:
			return false;
		}
	case geometry_type::POLYGON:
		switch (rhs.get_type()) {
		case geometry_type::POINT:
			return distance_point_polyg(rhs, lhs, result);
		case geometry_type::LINESTRING:
			return distance_lines_polyg(rhs, lhs, result);
		case geometry_type::POLYGON:
			return distance_polyg_polyg(lhs, rhs, result);
		default:
			return false;
		}
	default:
		return false;
	}
}

} // namespace

bool ops::get_euclidean_distance(const geometry &lhs_geom, const geometry &rhs_geom, double &result) {

	// Start out with infinite distance
	distance_result distance_result(std::numeric_limits<double>::infinity());

	bool found = false;

	visit_leaf_geometries(lhs_geom, [&](const geometry &lhs) {
		visit_leaf_geometries(rhs_geom, [&](const geometry &rhs) {
			if (distance_dispatch(lhs, rhs, distance_result)) {
				found = true;
			}
		});
	});

	result = distance_result.distance;

	return found;
}

// Visit all non-collection geometries
template <class CALLBACK>
void visit_leaf_pairs(const geometry &lhs, const geometry &rhs, CALLBACK &&callback) {
	const auto lhs_root = lhs.get_parent();
	auto lhs_part = &lhs;

	while (true) {
		switch (lhs_part->get_type()) {
		case geometry_type::POINT:
		case geometry_type::LINESTRING:
		case geometry_type::POLYGON: {

			const auto rhs_root = rhs.get_parent();
			auto rhs_part = &rhs;

			while (true) {
				switch (rhs_part->get_type()) {
				case geometry_type::POINT:
				case geometry_type::LINESTRING:
				case geometry_type::POLYGON: {
					// Found a pair of leaf geometries
					// If the callback returns true, we stop visiting
					if (callback(*lhs_part, *rhs_part)) {
						return;
					}
				} break;
				case geometry_type::MULTI_POINT:
				case geometry_type::MULTI_LINESTRING:
				case geometry_type::MULTI_POLYGON:
				case geometry_type::GEOMETRY_COLLECTION:
					if (rhs_part->is_empty()) {
						break;
					}
					rhs_part = rhs_part->get_first_part();
					continue;
				default:
					break;
				}

				while (true) {
					const auto parent = rhs_part->get_parent();
					if (parent == rhs_root) {
						break;
					}
					if (rhs_part != parent->get_last_part()) {
						rhs_part = rhs_part->get_next();
						break;
					}
					rhs_part = parent;
				}
			}

		} break;
		case geometry_type::MULTI_POINT:
		case geometry_type::MULTI_LINESTRING:
		case geometry_type::MULTI_POLYGON:
		case geometry_type::GEOMETRY_COLLECTION:
			if (lhs_part->is_empty()) {
				break;
			}
			lhs_part = lhs_part->get_first_part();
			continue;
		default:
			break;
		}

		while (true) {
			const auto parent = lhs_part->get_parent();
			if (parent == lhs_root) {
				return;
			}
			if (lhs_part != parent->get_last_part()) {
				lhs_part = lhs_part->get_next();
				break;
			}
			lhs_part = parent;
		}
	}
}

} // namespace sgl

//======================================================================================================================
// Vertex Operations
//======================================================================================================================
namespace sgl {

namespace ops {

void visit_vertices_xyzm(const geometry &geom, void *state, void (*callback)(void *state, const vertex_xyzm &vertex)) {
	visit_vertex_arrays(geom, [&](const geometry &part) {
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_width = part.get_vertex_width();
		const auto vertex_array = part.get_vertex_array();

		vertex_xyzm vertex = {0, 0, 0, 0};
		for (uint32_t i = 0; i < vertex_count; i++) {
			memcpy(&vertex, vertex_array + i * vertex_width, vertex_width);
			callback(state, vertex);
		}
	});
}

void visit_vertices_xy(const geometry &geom, void *state, void (*callback)(void *state, const vertex_xy &vertex)) {
	visit_vertex_arrays(geom, [&](const geometry &part) {
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_width = part.get_vertex_width();
		const auto vertex_array = part.get_vertex_array();

		vertex_xy vertex;
		for (uint32_t i = 0; i < vertex_count; i++) {
			memcpy(&vertex, vertex_array + i * vertex_width, sizeof(vertex_xy));
			callback(state, vertex);
		}
	});
}

void transform_vertices(allocator &allocator, geometry &geom, void *state,
                        void (*callback)(void *state, vertex_xyzm &vertex)) {

	visit_vertex_arrays_mutable(geom, [&](geometry &part) {
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_width = part.get_vertex_width();

		const auto old_vertex_array = part.get_vertex_array();
		const auto new_vertex_array = static_cast<uint8_t *>(allocator.alloc(vertex_count * vertex_width));

		vertex_xyzm vertex = {0, 0, 0, 0};
		for (uint32_t i = 0; i < vertex_count; i++) {
			memcpy(&vertex, old_vertex_array + i * vertex_width, vertex_width);

			callback(state, vertex);

			memcpy(new_vertex_array + i * vertex_width, &vertex, vertex_width);
		}

		part.set_vertex_array(new_vertex_array, vertex_count);
	});
}

void flip_vertices(allocator &allocator, geometry &geom) {
	visit_vertex_arrays_mutable(geom, [&allocator](geometry &part) {
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_width = part.get_vertex_width();

		const auto old_vertex_array = part.get_vertex_array();
		const auto new_vertex_array = static_cast<uint8_t *>(allocator.alloc(vertex_count * vertex_width));

		vertex_xyzm vertex = {0, 0, 0, 0};
		for (uint32_t i = 0; i < vertex_count; i++) {
			memcpy(&vertex, old_vertex_array + i * vertex_width, vertex_width);
			// Flip x and y
			const auto tmp = vertex.x;
			vertex.x = vertex.y;
			vertex.y = tmp;
			memcpy(new_vertex_array + i * vertex_width, &vertex, vertex_width);
		}

		part.set_vertex_array(new_vertex_array, vertex_count);
	});
}

void affine_transform(allocator &allocator, geometry &geom, const affine_matrix &matrix) {
	visit_vertex_arrays_mutable(geom, [&allocator, &matrix](geometry &part) {
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_width = part.get_vertex_width();

		const auto old_vertex_array = part.get_vertex_array();
		const auto new_vertex_array = static_cast<uint8_t *>(allocator.alloc(vertex_count * vertex_width));

		vertex_xyzm vertex = {0, 0, 1, 1};
		for (uint32_t i = 0; i < vertex_count; i++) {
			memcpy(&vertex, old_vertex_array + i * vertex_width, vertex_width);

			// Apply affine transformation
			auto new_vertex = matrix.apply_xyz(vertex);

			memcpy(new_vertex_array + i * vertex_width, &new_vertex, vertex_width);
		}

		part.set_vertex_array(new_vertex_array, vertex_count);
	});
}

void collect_vertices(allocator &alloc, const geometry &geom, geometry &result) {

	// Collect all vertices from the geometry into a new geometry
	const auto has_z = geom.has_z();
	const auto has_m = geom.has_m();

	// Initialize the new result multipoint
	result.set_type(geometry_type::MULTI_POINT);
	result.set_z(has_z);
	result.set_m(has_m);

	visit_vertex_arrays(geom, [&](const geometry &part) {
		const auto vertex_array = part.get_vertex_array();
		const auto vertex_count = part.get_vertex_count();
		const auto vertex_width = part.get_vertex_width();

		for (uint32_t i = 0; i < vertex_count; i++) {
			const auto point_mem = alloc.alloc(sizeof(geometry));
			const auto point_ptr = new (point_mem) geometry(geometry_type::POINT, has_z, has_m);

			point_ptr->set_vertex_array(vertex_array + i * vertex_width, 1);

			result.append_part(point_ptr);
		}
	});
}

} // namespace ops

} // namespace sgl
//======================================================================================================================
// Force Z and M
//======================================================================================================================
namespace sgl {

namespace ops {

static char *resize_vertices(allocator &alloc, geometry &geom, bool set_z, bool set_m, double default_z,
                             double default_m) {

	const auto has_z = geom.has_z();
	const auto has_m = geom.has_m();

	const auto source_type = static_cast<vertex_type>(has_z + 2 * has_m);
	const auto target_type = static_cast<vertex_type>(set_z + 2 * set_m);

	const auto source_data = geom.get_vertex_array();
	const auto count = geom.get_vertex_count();

	if (source_type == target_type) {
		return source_data;
	}

	switch (source_type) {
	case vertex_type::XY: {
		constexpr auto source_size = sizeof(double) * 2;
		switch (target_type) {
		case vertex_type::XY: {
			// Do nothing
			return source_data;
		}
		case vertex_type::XYZ: {
			constexpr auto target_size = sizeof(double) * 3;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, source_size);
				memcpy(target_data + target_offset + source_size, &default_z, sizeof(double));
			}

			return target_data;
		}
		case vertex_type::XYM: {
			constexpr auto target_size = sizeof(double) * 3;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, source_size);
				memcpy(target_data + target_offset + source_size, &default_m, sizeof(double));
			}

			return target_data;
		}
		case vertex_type::XYZM: {
			constexpr auto target_size = sizeof(double) * 4;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, source_size);
				memcpy(target_data + target_offset + source_size, &default_z, sizeof(double));
				memcpy(target_data + target_offset + source_size + sizeof(double), &default_m, sizeof(double));
			}

			return target_data;
		}
		default:
			SGL_ASSERT(false);
			return nullptr;
		}
	}
	case vertex_type::XYZ: {
		constexpr auto source_size = sizeof(double) * 3;
		switch (target_type) {
		case vertex_type::XY: {
			constexpr auto target_size = sizeof(double) * 2;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, target_size);
			}

			return target_data;
		}
		case vertex_type::XYZ: {
			// Do nothing
			return source_data;
		}
		case vertex_type::XYM: {
			constexpr auto target_size = sizeof(double) * 3;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, target_size);
				memcpy(target_data + target_offset + sizeof(double) * 2, &default_m, sizeof(double));
			}

			return target_data;
		}
		case vertex_type::XYZM: {
			constexpr auto target_size = sizeof(double) * 4;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, target_size);
				memcpy(target_data + target_offset + sizeof(double) * 3, &default_m, sizeof(double));
			}

			return target_data;
		}
		default:
			SGL_ASSERT(false);
			return nullptr;
		}
	}
	case vertex_type::XYM: {
		constexpr auto source_size = sizeof(double) * 3;
		switch (target_type) {
		case vertex_type::XY: {
			constexpr auto target_size = sizeof(double) * 2;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, target_size);
			}

			return target_data;
		}
		case vertex_type::XYZ: {
			constexpr auto target_size = sizeof(double) * 3;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, sizeof(double) * 2);
				memcpy(target_data + target_offset + sizeof(double) * 2, &default_z, sizeof(double));
			}

			return target_data;
		}
		case vertex_type::XYM: {
			// Do nothing
			return source_data;
		}
		case vertex_type::XYZM: {
			constexpr auto target_size = sizeof(double) * 4;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, sizeof(double) * 2);
				memcpy(target_data + target_offset + sizeof(double) * 2, &default_z, sizeof(double));
				memcpy(target_data + target_offset + sizeof(double) * 3,
				       source_data + source_offset + sizeof(double) * 2, sizeof(double));
			}

			return target_data;
		}
		default:
			SGL_ASSERT(false);
			return nullptr;
		}
	}
	case vertex_type::XYZM: {
		constexpr auto source_size = sizeof(double) * 4;
		switch (target_type) {
		case vertex_type::XY: {
			constexpr auto target_size = sizeof(double) * 2;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, sizeof(double) * 2);
			}

			return target_data;
		}
		case vertex_type::XYZ: {
			constexpr auto target_size = sizeof(double) * 3;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, sizeof(double) * 3);
			}

			return target_data;
		}
		case vertex_type::XYM: {
			constexpr auto target_size = sizeof(double) * 3;
			const auto target_data = static_cast<char *>(alloc.alloc(count * target_size));

			for (size_t i = 0; i < count; i++) {
				const auto source_offset = i * source_size;
				const auto target_offset = i * target_size;
				memcpy(target_data + target_offset, source_data + source_offset, sizeof(double) * 2);
				memcpy(target_data + target_offset + sizeof(double) * 2,
				       source_data + source_offset + sizeof(double) * 3, sizeof(double));
			}

			return target_data;
		}
		case vertex_type::XYZM: {
			// Do nothing
			return source_data;
		}
		default:
			SGL_ASSERT(false);
			return nullptr;
		}
	}
	default:
		SGL_ASSERT(false);
		return nullptr;
	}
}

void force_zm(allocator &alloc, geometry &geom, const bool set_z, const bool set_m, const double default_z,
              const double default_m) {
	visit_all_parts_mutable(
	    geom,
	    [&](geometry &part) {
		    if (!part.is_multi_part() && !part.is_empty()) {
			    SGL_ASSERT(part.get_type() == geometry_type::LINESTRING || part.get_type() == geometry_type::POINT);

			    // Single part geometry, resize vertices
			    const auto target_data = resize_vertices(alloc, part, set_z, set_m, default_z, default_m);
			    part.set_vertex_array(target_data, part.get_vertex_count());
		    }
	    },
	    [&](geometry &part) {
		    part.set_z(set_z);
		    part.set_m(set_m);
	    });
}

} // namespace ops

} // namespace sgl
//======================================================================================================================
// Prepared Geometry
//======================================================================================================================
namespace sgl {

void prepared_geometry::make(allocator &allocator, const geometry &geom, prepared_geometry &result) {

	result.set_z(geom.has_z());
	result.set_m(geom.has_m());
	result.set_type(geom.get_type());

	if (!geom.is_multi_part()) {
		result.set_vertex_array(geom.get_vertex_array(), geom.get_vertex_count());
	} else {
		// Multi-part geometry
		const auto tail = geom.get_last_part();
		if (tail) {
			auto head = tail;
			do {
				head = head->get_next();
				const auto part_mem = allocator.alloc(sizeof(prepared_geometry));
				const auto part_ptr = new (part_mem) prepared_geometry();
				make(allocator, *head, *part_ptr);
				result.append_part(part_ptr);
			} while (head != tail);
		}
	}
	result.build(allocator);
}

void prepared_geometry::build(allocator &allocator) {
	const auto geom_type = get_type();

	if (geom_type != geometry_type::LINESTRING) {
		return; // Only LINESTRINGs are prepared for now
	}

	const auto vertex_array = get_vertex_array();
	const auto vertex_count = get_vertex_count();
	const auto vertex_width = get_vertex_width();

	if (vertex_count == 0) {
		return; // Nothing to prepare
	}

	// Use these constants
	constexpr auto MAX_DEPTH = prepared_index::MAX_DEPTH;
	constexpr auto NODE_SIZE = prepared_index::NODE_SIZE;

	uint32_t layer_bound[MAX_DEPTH] = {};
	uint32_t layer_count = 0;

	const uint32_t count = (vertex_count + NODE_SIZE - 1) / NODE_SIZE;
	while (true) {
		layer_bound[layer_count] = static_cast<uint32_t>(
		    std::ceil(static_cast<double>(count) / std::pow(static_cast<double>(NODE_SIZE), layer_count)));

		if (layer_bound[layer_count++] <= 1) {
			break; // We have reached the last layer
		}
	}

	std::reverse(layer_bound, layer_bound + layer_count);

	index.items_count = vertex_count;

	// Allocate the layers
	index.level_array =
	    static_cast<prepared_index::level *>(allocator.alloc(sizeof(prepared_index::level) * layer_count));
	index.level_count = layer_count;

	for (uint32_t i = 0; i < layer_count; i++) {
		index.level_array[i].entry_count = layer_bound[i];
		index.level_array[i].entry_array =
		    static_cast<extent_xy *>(allocator.alloc(sizeof(extent_xy) * layer_bound[i]));
	}

	// Fill lower layer
	const auto &last_entry = index.level_array[layer_count - 1];
	for (uint32_t i = 0; i < last_entry.entry_count; i++) {

		auto &box = last_entry.entry_array[i];
		box = extent_xy::smallest();

		const auto beg = i * NODE_SIZE;

		// We add +1 to the node size here, to get not just the start point of the segment, but also the end point,
		// which may be in the next node. This ensures there is no gaps between node bounding boxes.
		const auto end = math::min(beg + NODE_SIZE + 1, vertex_count);

		for (uint32_t j = beg; j < end; j++) {
			vertex_xy curr = {0, 0};
			memcpy(&curr, vertex_array + j * vertex_width, sizeof(vertex_xy));

			box.min.x = math::min(box.min.x, curr.x);
			box.min.y = math::min(box.min.y, curr.y);
			box.max.x = math::max(box.max.x, curr.x);
			box.max.y = math::max(box.max.y, curr.y);
		}
	}

	// Now fill the upper layers (if we got any)
	for (int64_t i = static_cast<int64_t>(index.level_count) - 2; i >= 0; i--) {
		const auto &prev = index.level_array[i + 1];
		const auto &curr = index.level_array[i];

		for (uint32_t j = 0; j < curr.entry_count; j++) {
			auto &box = curr.entry_array[j];
			box = extent_xy::smallest();

			const auto beg = j * NODE_SIZE;
			const auto end = math::min(beg + NODE_SIZE, prev.entry_count);

			for (uint32_t k = beg; k < end; k++) {
				auto &prev_box = prev.entry_array[k];

				box.min.x = math::min(box.min.x, prev_box.min.x);
				box.min.y = math::min(box.min.y, prev_box.min.y);
				box.max.x = math::max(box.max.x, prev_box.max.x);
				box.max.y = math::max(box.max.y, prev_box.max.y);
			}
		}
	}

	// Mark this geometry as prepared
	set_prepared(true);
}

point_in_polygon_result prepared_geometry::contains(const vertex_xy &vert) const {
	if (!is_prepared()) {
		// Not prepared!
		return point_in_polygon_result::INVALID;
	}

	constexpr auto NODE_SIZE = prepared_index::NODE_SIZE;
	constexpr auto MAX_DEPTH = prepared_index::MAX_DEPTH;

	const auto vertex_array = get_vertex_array();
	const auto vertex_width = get_vertex_width();

	uint32_t stack[MAX_DEPTH] = {0};
	uint32_t depth = 0;

	uint32_t crossings = 0;

	// Traverse the tree
	while (true) {
		const auto &level = index.level_array[depth];
		const auto entry = stack[depth];
		const auto &box = level.entry_array[entry];

		// Check if the vertex is in the box y-slice
		SGL_ASSERT(box.min.y <= box.max.y);
		if (box.min.y <= vert.y && box.max.y >= vert.y) {
			if (depth != index.level_count - 1) {
				// We are not at a leaf, so go downwards
				depth++;
				stack[depth] = entry * NODE_SIZE;
				continue;
			}

			// Now, we are at a leaf, so we need to check the segments
			const auto beg_idx = entry * NODE_SIZE;
			// We +1 to the node size here to get the end index, because the last segment spans across
			// the end of the node. And our indexes are always sequential.
			const auto end_idx = math::min(beg_idx + NODE_SIZE + 1, index.items_count);

			// Loop over the segments
			vertex_xy prev;
			memcpy(&prev, vertex_array + beg_idx * vertex_width, sizeof(vertex_xy));

			for (uint32_t i = beg_idx + 1; i < end_idx; i++) {
				vertex_xy next;
				memcpy(&next, vertex_array + i * vertex_width, sizeof(vertex_xy));

				switch (raycast_fast(prev, next, vert)) {
				case raycast_result::NONE:
					// No intersection
					break;
				case raycast_result::CROSS:
					// The ray crosses the segment, so we count it
					crossings++;
					break;
				case raycast_result::BOUNDARY:
					// The point is on the boundary, so we return BOUNDARY
					return point_in_polygon_result::BOUNDARY;
				}
				prev = next;
			}
		}

		while (true) {

			if (depth == 0) {
				// Even number of crossings means the point is outside the polygon
				return crossings % 2 == 0 ? point_in_polygon_result::EXTERIOR : point_in_polygon_result::INTERIOR;
			}

			// The end of this node is either the end of the current node, or the end of the level
			const auto node_end = ((stack[depth - 1] + 1) * NODE_SIZE) - 1;
			const auto levl_end = index.level_array[depth].entry_count - 1;

			const auto end = math::min(node_end, levl_end);

			if (stack[depth] != end) {
				// Go sideways!
				stack[depth]++;
				break;
			}

			// Go upwards!
			depth--;
		}
	}
}

// We use the "Branch and Bound" algorithm to find the distance.
// The reason for this (instead of e.g. Best-First-Search) is that we don't need to allocate or manipulate a  priority
// queue, which is more expensive than just the stack. The stack is basically shallowly bounded due to
// the large fan-out of the tree and doesn't require allocating any memory on the heap. In comparison, the worst-case
// in Best-First-Search requires us to keep track of all nodes in the priority queue.
//
// The trick of the branch-and-bound algorithm is to compute the worst case upper bound of the distance for all the
// child nodes in a node, and only expand a child node if its minimum distance is smaller than the worst-case upper
// bound
//
// Basically, the distance to a box is an optimistic lower bound for the distance to the closest element within it,
// but the "minimum maximum distance" is a pessimistic upper bound.
//
bool prepared_geometry::try_get_distance_recursive(uint32_t level, uint32_t entry, const vertex_xy &vertex,
                                                   double &distance) const {

	constexpr auto NODE_SIZE = prepared_index::NODE_SIZE;

	if (level == index.level_count - 1 || level == prepared_index::MAX_DEPTH) {
		// We are at the leaf level, so we need to check the segments
		const auto vertex_array = get_vertex_array();
		const auto vertex_width = get_vertex_width();

		const auto beg_idx = entry * NODE_SIZE;
		// We +1 to the node size here to get the end index, because the last segment spans across
		// the end of the node. And our indexes are always sequential.
		const auto end_idx = math::min(beg_idx + NODE_SIZE + 1, index.items_count);

		if (beg_idx >= end_idx) {
			return false; // No segments to check
		}

		vertex_xy prev;
		memcpy(&prev, vertex_array + beg_idx * vertex_width, sizeof(vertex_xy));
		for (uint32_t i = beg_idx + 1; i < end_idx; i++) {
			vertex_xy next;
			memcpy(&next, vertex_array + i * vertex_width, sizeof(vertex_xy));

			distance = math::min(distance, vertex_segment_distance(vertex, prev, next));

			prev = next;
		}

		return true; // We found a distance
	}

	// Find child nodes
	const auto beg_idx = entry * NODE_SIZE;
	const auto end_idx = math::min(beg_idx + NODE_SIZE, index.level_array[level + 1].entry_count);

	if (beg_idx >= end_idx) {
		return false; // No child nodes to check
	}

	const auto get_min_max_distance = [](const extent_xy &r, const vertex_xy &q) {
		const auto squared = [](const double &x) {
			return x * x;
		};

		const double qx = q.x, qy = q.y;
		const double min_x = r.min.x, max_x = r.max.x;
		const double min_y = r.min.y, max_y = r.max.y;

		// Case 1: k = x
		const double rmk_x = (qx <= (min_x + max_x) / 2.0) ? min_x : max_x;
		const double rMi_y = (qy <= (min_y + max_y) / 2.0) ? max_y : min_y;

		const double term1 = squared(rmk_x - qx) + squared(rMi_y - qy);

		// Case 2: k = y
		const double rmk_y = (qy <= (min_y + max_y) / 2.0) ? min_y : max_y;
		const double rMi_x = (qx <= (min_x + max_x) / 2.0) ? max_x : min_x;

		const double term2 = squared(rmk_y - qy) + squared(rMi_x - qx);

		return math::max(math::min(term1, term2), 0.0); // Ensure we don't have negative distances
	};

	const auto get_min_distance = [](const extent_xy &r, const vertex_xy &q) {
		const double dx = (q.x < r.min.x) ? r.min.x - q.x : (q.x > r.max.x) ? q.x - r.max.x : 0.0;
		const double dy = (q.y < r.min.y) ? r.min.y - q.y : (q.y > r.max.y) ? q.y - r.max.y : 0.0;
		return math::max((dx * dx + dy * dy), 0.0); // Ensure we don't have negative distances
	};

	// Compute the minimum maximum distance for this level
	auto min_max_dist = std::numeric_limits<double>::infinity();
	for (auto i = beg_idx; i < end_idx; i++) {
		// Get the box for this entry
		const auto &box = index.level_array[level + 1].entry_array[i];
		min_max_dist = math::min(min_max_dist, get_min_max_distance(box, vertex));
	}

	bool found_any = false;

	// Now we can check the child nodes
	for (auto i = beg_idx; i < end_idx; i++) {

		const auto &box = index.level_array[level + 1].entry_array[i];
		const auto min_dist = get_min_distance(box, vertex);

		// Use a small epsilon to avoid floating point issues
		// Because this is an optimization (a pessimistic upper bound), its ok if we are a bit too pessimistic
		// We will just check some more boxes than theoretically neccessary, but the result will still be correct
		if (min_dist > min_max_dist + 1e-6) {
			continue;
		}

		found_any |= try_get_distance_recursive(level + 1, i, vertex, distance);
	}

	return found_any;
}

bool prepared_geometry::try_get_distance(const vertex_xy &vertex, double &distance) const {
	if (!is_prepared()) {
		return false; // Not prepared
	}
	auto dist = std::numeric_limits<double>::infinity();
	if (try_get_distance_recursive(0, 0, vertex, dist)) {
		distance = dist;
		return true; // We found a distance
	}
	return false; // No distance found
}

static double point_segment_dist_sq(const vertex_xy &p, const vertex_xy &a, const vertex_xy &b) {
	const auto ab = b - a;
	const auto ap = p - a;

	const auto ab_len_sq = ab.norm_sq();

	if (ab_len_sq == 0.0) {
		// Segment is a point
		return ap.norm_sq();
	}

	const auto t = math::clamp(ap.dot(ab) / ab_len_sq, 0.0, 1.0);
	const auto proj = a + ab * t; // Closest point on the segment to the point p
	const auto diff = p - proj;
	return diff.norm_sq();
}

// Check if point P is on segment QR
static bool point_on_segment(const vertex_xy &p, const vertex_xy &q, const vertex_xy &r) {
	return q.x >= std::min(p.x, r.x) && q.x <= std::max(p.x, r.x) && q.y >= std::min(p.y, r.y) &&
	       q.y <= std::max(p.y, r.y);
}

static bool segment_intersects(const vertex_xy &a1, const vertex_xy &a2, const vertex_xy &b1, const vertex_xy &b2) {
	// Check if two segments intersect using the orientation method
	// Handle degenerate cases where a segment is actually a single point
	const bool a_is_point = (a1.x == a2.x && a1.y == a2.y);
	const bool b_is_point = (b1.x == b2.x && b1.y == b2.y);

	if (a_is_point && b_is_point) {
		// Both are points: intersect only if identical
		return (a1.x == b1.x && a1.y == b1.y);
	}
	if (a_is_point) {
		// A is a point: check if A lies on segment B
		return point_on_segment(a1, b1, b2);
	}
	if (b_is_point) {
		// B is a point: check if B lies on segment A
		return point_on_segment(b1, a1, a2);
	}

	const auto o1 = orient2d_fast(a1, a2, b1);
	const auto o2 = orient2d_fast(a1, a2, b2);
	const auto o3 = orient2d_fast(b1, b2, a1);
	const auto o4 = orient2d_fast(b1, b2, a2);

	if (o1 != o2 && o3 != o4) {
		return true; // Segments intersect
	}

	if (o1 == 0 && point_on_segment(a1, b1, b2)) {
		return true; // a1 is collinear with b1 and b2
	}
	if (o2 == 0 && point_on_segment(a2, b1, b2)) {
		return true; // a2 is collinear with b1 and b2
	}
	if (o3 == 0 && point_on_segment(b1, a1, a2)) {
		return true; // b1 is collinear with a1 and a2
	}
	if (o4 == 0 && point_on_segment(b2, a1, a2)) {
		return true; // b2 is collinear with a1 and a2
	}

	return false; // Segments do not intersect
}

static double segment_segment_dist_sq(const vertex_xy &a1, const vertex_xy &a2, const vertex_xy &b1,
                                      const vertex_xy &b2) {

	// Calculate the squared distance between two segments
	if (segment_intersects(a1, a2, b1, b2)) {
		return 0.0; // Segments intersect, distance is zero
	}

	return math::min(math::min(point_segment_dist_sq(a1, b1, b2), point_segment_dist_sq(a2, b1, b2)),
	                 math::min(point_segment_dist_sq(b1, a1, a2), point_segment_dist_sq(b2, a1, a2)));
}

static bool try_get_prepared_distance_lines(const prepared_geometry &lhs, const prepared_geometry &rhs,
                                            double &distance) {

	SGL_ASSERT(lhs.is_prepared() && rhs.is_prepared());

	if (lhs.is_empty() || rhs.is_empty()) {
		return false; // No distance to compute
	}

	struct entry {
		double distance;
		uint32_t lhs_level;
		uint32_t lhs_entry;
		uint32_t rhs_level;
		uint32_t rhs_entry;

		entry(double dist, uint32_t lhs_lvl, uint32_t lhs_ent, uint32_t rhs_lvl, uint32_t rhs_ent)
		    : distance(dist), lhs_level(lhs_lvl), lhs_entry(lhs_ent), rhs_level(rhs_lvl), rhs_entry(rhs_ent) {
		}

		bool operator<(const entry &other) const {
			return distance > other.distance; // We want a min-heap, so we reverse the comparison
		}
	};

	std::priority_queue<entry> pq;
	pq.emplace(0, 0, 0, 0, 0); // Start with the root node

	// We use the squared distance to avoid computing square roots, which is more expensive
	double min_dist = std::numeric_limits<double>::infinity();
	bool found_any = false;

	const auto lhs_vertex_array = lhs.get_vertex_array();
	const auto lhs_vertex_width = lhs.get_vertex_width();

	const auto rhs_vertex_array = rhs.get_vertex_array();
	const auto rhs_vertex_width = rhs.get_vertex_width();

	constexpr auto NODE_SIZE = prepared_geometry::prepared_index::NODE_SIZE;

	while (!pq.empty() && min_dist > 0) {
		const auto pair = pq.top();
		pq.pop();

		if (pair.distance >= min_dist && found_any) {
			// All other pairs in the queue will have a distance greater than or equal to min_dist
			break;
		}

		const auto lhs_is_leaf = pair.lhs_level == lhs.index.level_count - 1;
		const auto rhs_is_leaf = pair.rhs_level == rhs.index.level_count - 1;

		if (lhs_is_leaf && rhs_is_leaf) {

			const auto lhs_beg_idx = pair.lhs_entry * NODE_SIZE;
			const auto lhs_end_idx = math::min(lhs_beg_idx + NODE_SIZE + 1, lhs.index.items_count);

			const auto rhs_beg_idx = pair.rhs_entry * NODE_SIZE;
			const auto rhs_end_idx = math::min(rhs_beg_idx + NODE_SIZE + 1, rhs.index.items_count);

			if (lhs_beg_idx >= lhs_end_idx || rhs_beg_idx >= rhs_end_idx) {
				continue; // No segments to check
			}

			const auto rhs_box = rhs.index.level_array[pair.rhs_level].entry_array[pair.rhs_entry];

			vertex_xy lhs_prev;
			vertex_xy rhs_prev;
			vertex_xy lhs_next;
			vertex_xy rhs_next;

			memcpy(&lhs_prev, lhs_vertex_array + lhs_beg_idx * lhs_vertex_width, sizeof(vertex_xy));
			for (uint32_t i = lhs_beg_idx + 1; i < lhs_end_idx; i++) {
				memcpy(&lhs_next, lhs_vertex_array + i * lhs_vertex_width, sizeof(vertex_xy));

				// If this is a zero-length segment, skip it
				// LINESTRINGs must have at least two distinct vertices to be valid, so this is safe. Even if we skip
				// this vertex now, we must eventually reach a non-zero-length segment that includes this vertex as
				// its start point. It will therefore still contribute to the distance calculation once we process that
				// segment.
				if (lhs_prev.x == lhs_next.x && lhs_prev.y == lhs_next.y) {
					continue;
				}

				// Quick check: If the distance between the segment and the box (all the segments)
				// is greater than min_dist, we can skip the exact distance check

				extent_xy lhs_seg;
				lhs_seg.min.x = std::min(lhs_prev.x, lhs_next.x);
				lhs_seg.min.y = std::min(lhs_prev.y, lhs_next.y);
				lhs_seg.max.x = std::max(lhs_prev.x, lhs_next.x);
				lhs_seg.max.y = std::max(lhs_prev.y, lhs_next.y);

				if (lhs_seg.distance_to_sq(rhs_box) > min_dist) {
					lhs_prev = lhs_next;
					continue;
				}

				memcpy(&rhs_prev, rhs_vertex_array + rhs_beg_idx * rhs_vertex_width, sizeof(vertex_xy));
				for (uint32_t j = rhs_beg_idx + 1; j < rhs_end_idx; j++) {
					memcpy(&rhs_next, rhs_vertex_array + j * rhs_vertex_width, sizeof(vertex_xy));

					// If this is a zero-length segment, skip it
					// LINESTRINGs must have at least two distinct points to be valid, so this is safe.
					// (see comment above)
					if (rhs_prev.x == rhs_next.x && rhs_prev.y == rhs_next.y) {
						continue;
					}

					// Quick check: If the distance between the segment bounds are greater than min_dist,
					// we can skip the exact distance check
					extent_xy rhs_seg;
					rhs_seg.min.x = std::min(rhs_prev.x, rhs_next.x);
					rhs_seg.min.y = std::min(rhs_prev.y, rhs_next.y);
					rhs_seg.max.x = std::max(rhs_prev.x, rhs_next.x);
					rhs_seg.max.y = std::max(rhs_prev.y, rhs_next.y);

					if (rhs_seg.distance_to_sq(lhs_seg) > min_dist) {
						rhs_prev = rhs_next;
						continue;
					}

					const auto dist = segment_segment_dist_sq(lhs_prev, lhs_next, rhs_prev, rhs_next);
					if (dist < min_dist) {
						min_dist = dist;
						found_any = true;
					}

					rhs_prev = rhs_next;
				}

				lhs_prev = lhs_next;
			}
		} else if (lhs_is_leaf && !rhs_is_leaf) {
			const auto rhs_beg_idx = pair.rhs_entry * NODE_SIZE;
			const auto rhs_end_idx =
			    math::min(rhs_beg_idx + NODE_SIZE, rhs.index.level_array[pair.rhs_level + 1].entry_count);

			const auto lhs_box = lhs.index.level_array[pair.lhs_level].entry_array[pair.lhs_entry];

			for (auto i = rhs_beg_idx; i < rhs_end_idx; i++) {

				const auto &rhs_box = rhs.index.level_array[pair.rhs_level + 1].entry_array[i];
				const auto dist = lhs_box.distance_to_sq(rhs_box);

				if (dist < min_dist) {
					pq.emplace(dist, pair.lhs_level, pair.lhs_entry, pair.rhs_level + 1, i);
				}
			}
		} else if (!lhs_is_leaf && rhs_is_leaf) {
			const auto lhs_beg_idx = pair.lhs_entry * NODE_SIZE;
			const auto lhs_end_idx =
			    math::min(lhs_beg_idx + NODE_SIZE, lhs.index.level_array[pair.lhs_level + 1].entry_count);

			const auto &rhs_box = rhs.index.level_array[pair.rhs_level].entry_array[pair.rhs_entry];

			for (auto i = lhs_beg_idx; i < lhs_end_idx; i++) {
				const auto &lhs_box = lhs.index.level_array[pair.lhs_level + 1].entry_array[i];
				const auto dist = rhs_box.distance_to_sq(lhs_box);

				if (dist < min_dist) {
					pq.emplace(dist, pair.lhs_level + 1, i, pair.rhs_level, pair.rhs_entry);
				}
			}
		} else {
			SGL_ASSERT(!lhs_is_leaf && !rhs_is_leaf);

			const auto lhs_box = lhs.index.level_array[pair.lhs_level].entry_array[pair.lhs_entry];
			const auto rhs_box = rhs.index.level_array[pair.rhs_level].entry_array[pair.rhs_entry];

			// Decide which box to expand based on the area
			const auto lhs_area = lhs_box.get_area();
			const auto rhs_area = rhs_box.get_area();

			if (lhs_area > rhs_area) {

				const auto lhs_beg_idx = pair.lhs_entry * NODE_SIZE;
				const auto lhs_end_idx =
				    math::min(lhs_beg_idx + NODE_SIZE, lhs.index.level_array[pair.lhs_level + 1].entry_count);

				for (auto i = lhs_beg_idx; i < lhs_end_idx; i++) {
					const auto &lhs_child_box = lhs.index.level_array[pair.lhs_level + 1].entry_array[i];
					const auto dist = lhs_child_box.distance_to_sq(rhs_box);

					if (dist < min_dist) {
						pq.emplace(dist, pair.lhs_level + 1, i, pair.rhs_level, pair.rhs_entry);
					}
				}
			} else {

				const auto rhs_beg_idx = pair.rhs_entry * NODE_SIZE;
				const auto rhs_end_idx =
				    math::min(rhs_beg_idx + NODE_SIZE, rhs.index.level_array[pair.rhs_level + 1].entry_count);

				for (auto i = rhs_beg_idx; i < rhs_end_idx; i++) {
					const auto &rhs_child_box = rhs.index.level_array[pair.rhs_level + 1].entry_array[i];
					const auto dist = rhs_child_box.distance_to_sq(lhs_box);

					if (dist < min_dist) {
						pq.emplace(dist, pair.lhs_level, pair.lhs_entry, pair.rhs_level + 1, i);
					}
				}
			}
		}
	}

	if (found_any) {
		distance = std::sqrt(min_dist); // Convert squared distance to actual distance
		return true;                    // We found a distance
	}
	return false; // No distance found
}

bool prepared_geometry::try_get_distance(const prepared_geometry &other, double &distance) const {
	return try_get_prepared_distance_lines(*this, other, distance);
}

} // namespace sgl
//======================================================================================================================
// WKT Parsing
//======================================================================================================================

namespace sgl {

namespace {

class vertex_buffer {
public:
	explicit vertex_buffer(allocator &alloc, uint32_t vertex_width_p)
	    : alloc(alloc), vertex_array(nullptr), vertex_width(vertex_width_p), vertex_count(0), vertex_total(1) {
		vertex_array = static_cast<double *>(alloc.alloc(vertex_width * 1 * sizeof(double)));
	}

	void push_back(const double *vertex) {
		if (vertex_count >= vertex_total) {
			const auto new_total = math::max(vertex_total * 2, 8u);
			const auto old_total = vertex_total;

			const auto new_size = vertex_width * new_total * sizeof(double);
			const auto old_size = vertex_width * old_total * sizeof(double);

			vertex_array = static_cast<double *>(alloc.realloc(vertex_array, old_size, new_size));
			vertex_total = new_total;
		}

		memcpy(vertex_array + vertex_count * vertex_width, vertex, sizeof(double) * vertex_width);
		vertex_count++;
	}

	void assign_to(geometry &geom) {
		// Shrink to fit
		if (vertex_count < vertex_total) {
			const auto old_size = vertex_width * vertex_total * sizeof(double);
			const auto new_size = vertex_width * vertex_count * sizeof(double);

			vertex_array = static_cast<double *>(alloc.realloc(vertex_array, old_size, new_size));
		}
		geom.set_vertex_array(vertex_array, vertex_count);
	}

private:
	allocator &alloc;
	double *vertex_array;
	const size_t vertex_width;
	uint32_t vertex_count;
	uint32_t vertex_total;
};

} // namespace

void wkt_reader::match_ws() {
	while (pos < end && std::isspace(*pos)) {
		pos++;
	}
}

// case-insensitive match
bool wkt_reader::match_str(const char *str) {

	auto ptr = pos;
	while (ptr < end && *str != '\0' && std::tolower(*str) == std::tolower(*ptr)) {
		str++;
		ptr++;
	}
	if (*str != '\0') {
		return false;
	}
	pos = ptr;
	match_ws();
	return true;
}

bool wkt_reader::match_char(char c) {
	if (pos < end && std::tolower(*pos) == std::tolower(c)) {
		pos++;
		match_ws();
		return true;
	}
	return false;
}

bool wkt_reader::match_number(double &val) {
	// Because we care about the length, we cant just use std::strtod straight away without risking
	// out-of-bounds reads. Instead, we will manually parse the number and then use std::strtod to
	// convert the value.

	auto ptr = pos;

	// Match sign
	if (ptr < end && (*ptr == '+' || *ptr == '-')) {
		ptr++;
	}

	// Match number part
	while (ptr < end && std::isdigit(*ptr)) {
		ptr++;
	}
	// Match decimal part
	if (ptr < end && *ptr == '.') {
		ptr++;
		while (ptr < end && std::isdigit(*ptr)) {
			ptr++;
		}
	}
	// Match exponent part
	if (ptr < end && (*ptr == 'e' || *ptr == 'E')) {
		ptr++;
		if (ptr < end && (*ptr == '+' || *ptr == '-')) {
			ptr++;
		}
		while (ptr < end && std::isdigit(*ptr)) {
			ptr++;
		}
	}

	// Did we manage to parse anything?
	if (ptr == pos) {
		return false;
	}

	// If we got here, we know there is something resembling a number within the bounds of the buffer
	// We can now use std::strtod to actually parse the number
	char *end_ptr = nullptr;
	val = std::strtod(pos, &end_ptr);
	if (end_ptr == pos) {
		return false;
	}
	pos = end_ptr;
	match_ws();
	return true;
}

bool wkt_reader::try_parse(geometry &out, const char *buf_arg, const char *end_arg) {

	// Setup state
	buf = buf_arg;
	end = end_arg;
	pos = buf;
	error = nullptr;

	// These need to be set by the caller
	SGL_ASSERT(buf != nullptr);
	SGL_ASSERT(end != nullptr);

	geometry *root = &out;
	geometry *geom = root;

	// Reset the geometry
	geom->reset();

	// clang-format off
#define expect_char(C) do { if(!match_char(C)) { error = "Expected character: '" #C "'"; return false; } } while(0)
#define expect_number(RESULT) do { if(!match_number(RESULT)) { error = "Expected number"; return false; } } while(0)
	// clang-format on

	// Skip whitespace
	match_ws();

	// Skip leading SRID, we dont support it
	// TODO: Parse this and stuff it into the result
	if (match_str("SRID")) {
		while (pos < end && *pos != ';') {
			pos++;
		}
		expect_char(';');
	}

	// Main loop
	while (true) {
		// Now we should have a geometry type
		if (match_str("POINT")) {
			geom->set_type(geometry_type::POINT);
		} else if (match_str("LINESTRING")) {
			geom->set_type(geometry_type::LINESTRING);
		} else if (match_str("POLYGON")) {
			geom->set_type(geometry_type::POLYGON);
		} else if (match_str("MULTIPOINT")) {
			geom->set_type(geometry_type::MULTI_POINT);
		} else if (match_str("MULTILINESTRING")) {
			geom->set_type(geometry_type::MULTI_LINESTRING);
		} else if (match_str("MULTIPOLYGON")) {
			geom->set_type(geometry_type::MULTI_POLYGON);
		} else if (match_str("GEOMETRYCOLLECTION")) {
			geom->set_type(geometry_type::GEOMETRY_COLLECTION);
		} else if (match_str("INVALID")) {
			// For coverage
			geom->set_type(geometry_type::INVALID);
		} else {
			error = "Expected geometry type";
			return false;
		}

		// Match Z and M
		if (match_char('z')) {
			geom->set_z(true);
		}
		if (match_char('m')) {
			geom->set_m(true);
		}

		// TODO: make this check configurable
		if ((geom->has_m() != root->has_m()) || (geom->has_z() != root->has_z())) {
			error = "Mixed Z and M values are not supported";
			return false;
		}

		const size_t vertex_stride = 2 + geom->has_z() + geom->has_m();

		// Parse EMPTY
		if (!match_str("EMPTY")) {
			switch (geom->get_type()) {
			case geometry_type::POINT: {
				expect_char('(');

				vertex_buffer verts(alloc, vertex_stride);
				double vert[4] = {0, 0, 0, 0};
				for (size_t i = 0; i < vertex_stride; i++) {
					expect_number(vert[i]);
				}
				verts.push_back(vert);
				verts.assign_to(*geom);

				expect_char(')');
			} break;
			case geometry_type::LINESTRING: {
				expect_char('(');

				vertex_buffer verts(alloc, vertex_stride);
				do {
					double vert[4] = {0, 0, 0, 0};
					for (size_t i = 0; i < vertex_stride; i++) {
						expect_number(vert[i]);
					}
					verts.push_back(vert);
				} while (match_char(','));

				verts.assign_to(*geom);

				expect_char(')');
			} break;
			case geometry_type::POLYGON: {
				expect_char('(');
				do {
					auto ring = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
					new (ring) geometry(geometry_type::LINESTRING, geom->has_z(), geom->has_m());
					if (!match_str("EMPTY")) {
						expect_char('(');

						vertex_buffer verts(alloc, vertex_stride);
						do {
							double vert[4] = {0, 0, 0, 0};
							for (size_t i = 0; i < vertex_stride; i++) {
								expect_number(vert[i]);
							}
							verts.push_back(vert);
						} while (match_char(','));

						verts.assign_to(*ring);

						expect_char(')');
					}
					geom->append_part(ring);
				} while (match_char(','));
				expect_char(')');
			} break;
			case geometry_type::MULTI_POINT: {
				expect_char('(');
				// Multipoints are special in that parens around each point is optional.
				do {
					bool has_paren = false;
					if (match_char('(')) {
						has_paren = true;
					}
					auto point = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
					new (point) geometry(geometry_type::POINT, geom->has_z(), geom->has_m());
					if (!match_str("EMPTY")) {
						// TODO: Do we need to have optional parens to accept EMPTY?

						vertex_buffer verts(alloc, vertex_stride);
						double vert[4] = {0, 0, 0, 0};
						for (size_t i = 0; i < vertex_stride; i++) {
							expect_number(vert[i]);
						}
						verts.push_back(vert);
						verts.assign_to(*point);
					}
					if (has_paren) {
						expect_char(')');
					}
					geom->append_part(point);
				} while (match_char(','));
				expect_char(')');
			} break;
			case geometry_type::MULTI_LINESTRING: {
				expect_char('(');
				do {
					auto line = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
					new (line) geometry(geometry_type::LINESTRING, geom->has_z(), geom->has_m());
					if (!match_str("EMPTY")) {
						expect_char('(');

						vertex_buffer verts(alloc, vertex_stride);
						do {
							double vert[4] = {0, 0, 0, 0};
							for (size_t i = 0; i < vertex_stride; i++) {
								expect_number(vert[i]);
							}
							verts.push_back(vert);
						} while (match_char(','));

						verts.assign_to(*line);

						expect_char(')');
					}
					geom->append_part(line);
				} while (match_char(','));
				expect_char(')');
			} break;
			case geometry_type::MULTI_POLYGON: {
				expect_char('(');
				do {
					auto poly = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
					new (poly) geometry(geometry_type::POLYGON, geom->has_z(), geom->has_m());
					if (!match_str("EMPTY")) {
						expect_char('(');
						do {
							auto ring = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
							new (ring) geometry(geometry_type::LINESTRING, geom->has_z(), geom->has_m());
							if (!match_str("EMPTY")) {
								expect_char('(');

								vertex_buffer verts(alloc, vertex_stride);
								do {
									double vert[4] = {0, 0, 0, 0};
									for (size_t i = 0; i < vertex_stride; i++) {
										expect_number(vert[i]);
									}
									verts.push_back(vert);
								} while (match_char(','));

								verts.assign_to(*ring);

								expect_char(')');
							}
							poly->append_part(ring);
						} while (match_char(','));
						expect_char(')');
					}
					geom->append_part(poly);
				} while (match_char(','));
				expect_char(')');
			} break;
			case geometry_type::GEOMETRY_COLLECTION: {
				expect_char('(');

				// add another child
				auto new_geom = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
				new (new_geom) geometry(geometry_type::INVALID, false, false);

				geom->append_part(new_geom);
				geom = new_geom;
			}
				continue; // This 'continue' moves us to the next iteration
			default:
				error = "Unsupported geometry type";
				return false;
			}
		}

		while (true) {
			const auto parent = geom->get_parent();
			if (!parent) {
				// Done!
				return true;
			}

			SGL_ASSERT(parent->get_type() == geometry_type::GEOMETRY_COLLECTION);

			if (match_char(',')) {
				// The geometry collection is not done yet, add another sibling
				auto new_geom = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
				new (new_geom) geometry(geometry_type::INVALID, false, false);

				parent->append_part(new_geom);
				geom = new_geom;

				// goto begin;
				break;
			}

			expect_char(')');
			// The geometry collection is done, go up
			geom = parent;
		}
	}

#undef expect_char
#undef expect_number
}

const char *wkt_reader::get_error_message() const {
	if (!error) {
		return nullptr;
	}

	// Return a string of the current position in the input string
	constexpr auto len = 32;
	const auto range_beg = math::max(pos - len, buf);
	const auto range_end = math::min(pos + 1, end);
	auto range = std::string(range_beg, range_end);
	if (range_beg != buf) {
		range = "..." + range;
	}
	// Add an arrow to indicate the position
	const auto err = std::string(error);
	const auto pos = std::to_string(this->pos - buf);
	const auto msg = err + " at position '" + pos + "' near: '" + range + "'|<---";

	// Allocate a new string in the allocator and return it
	const auto str = static_cast<char *>(alloc.alloc(msg.size() + 1));
	memcpy(str, msg.c_str(), msg.size() + 1); // +1 for the null terminator

	// Return the pointer to the string
	return str;
}

} // namespace sgl

//======================================================================================================================
// WKB Parsing
//======================================================================================================================

namespace sgl {

bool wkb_reader::skip(const size_t size) {
	if (pos + size > end) {
		error = wkb_reader_error::OUT_OF_BOUNDS;
		return false;
	}
	pos += size;
	return true;
}

bool wkb_reader::read_u8(uint8_t &val) {
	if (pos + sizeof(uint8_t) > end) {
		error = wkb_reader_error::OUT_OF_BOUNDS;
		return false;
	}

	val = *pos;

	pos += sizeof(uint8_t);
	return true;
}

bool wkb_reader::read_u8(bool &val) {
	uint8_t tmp;
	if (!read_u8(tmp)) {
		return false;
	}
	val = tmp;
	return true;
}

bool wkb_reader::read_u32(uint32_t &val) {
	if (pos + sizeof(uint32_t) > end) {
		error = wkb_reader_error::OUT_OF_BOUNDS;
		return false;
	}

	if (le) {
		memcpy(&val, pos, sizeof(uint32_t));
	} else {
		char ibuf[sizeof(uint32_t)];
		char obuf[sizeof(uint32_t)];
		memcpy(ibuf, pos, sizeof(uint32_t));
		for (size_t i = 0; i < sizeof(uint32_t); i++) {
			obuf[i] = ibuf[sizeof(uint32_t) - i - 1];
		}
		memcpy(&val, obuf, sizeof(uint32_t));
	}

	pos += sizeof(uint32_t);

	return true;
}

bool wkb_reader::read_f64(double &val) {
	if (pos + sizeof(double) > end) {
		error = wkb_reader_error::OUT_OF_BOUNDS;
		return false;
	}
	if (le) {
		memcpy(&val, pos, sizeof(double));
	} else {
		char ibuf[sizeof(double)];
		char obuf[sizeof(double)];
		memcpy(ibuf, pos, sizeof(double));
		for (size_t i = 0; i < sizeof(double); i++) {
			obuf[i] = ibuf[sizeof(double) - i - 1];
		}
		memcpy(&val, obuf, sizeof(double));
	}

	pos += sizeof(double);

	return true;
}

bool wkb_reader::read_point(geometry *geom) {
	const size_t dims = 2 + geom->has_z() + geom->has_m();

	bool all_nan = true;
	double coords[4];

	const auto ptr = pos;
	for (size_t i = 0; i < dims; i++) {
		if (!read_f64(coords[i])) {
			return false;
		}
		if (!std::isnan(coords[i])) {
			all_nan = false;
		}
	}

	if (nan_as_empty && all_nan) {
		geom->set_vertex_array(nullptr, 0);
		return true;
	}
	if (le && !copy_vertices) {
		geom->set_vertex_array(ptr, 1);
		return true;
	}

	const auto data = static_cast<char *>(alloc.alloc(sizeof(double) * dims));
	memcpy(data, coords, sizeof(double) * dims);
	geom->set_vertex_array(data, 1);
	return true;
}

bool wkb_reader::read_line(geometry *geom) {
	uint32_t vertex_count;
	if (!read_u32(vertex_count)) {
		return false;
	}

	const auto vertex_width = geom->get_vertex_width();
	const auto byte_size = vertex_count * vertex_width;

	if (pos + byte_size > end) {
		error = wkb_reader_error::OUT_OF_BOUNDS;
		return false;
	}

	const auto ptr = pos;
	pos += byte_size;

	// If this is LE encoded, and we dont want to copy the vertices, we can just return the pointer
	if (le) {
		if (copy_vertices) {
			const auto mem = static_cast<char *>(alloc.alloc(byte_size));
			memcpy(mem, ptr, byte_size);
			geom->set_vertex_array(mem, vertex_count);
		} else {
			geom->set_vertex_array(ptr, vertex_count);
		}
	} else {
		// Otherwise, we need to allocate and swap the bytes
		const auto mem = static_cast<char *>(alloc.alloc(byte_size));
		for (size_t i = 0; i < vertex_count; i++) {
			const auto src = ptr + i * vertex_width;
			const auto dst = mem + i * vertex_width;

			// Swap doubles within the vertex
			for (size_t j = 0; j < vertex_width; j += sizeof(double)) {
				for (size_t k = 0; k < sizeof(double); k++) {
					dst[j + k] = src[j + sizeof(double) - k - 1];
				}
			}
		}

		geom->set_vertex_array(mem, vertex_count);
	}
	return true;
}

bool wkb_reader::try_parse(geometry &out, const char *buf_ptr, const char *end_ptr) {

	// Setup state
	buf = buf_ptr;
	pos = buf_ptr;
	end = end_ptr;

	error = wkb_reader_error::OK;
	stack_depth = 0;

	le = false;
	type_id = 0;
	has_any_m = false;
	has_any_z = false;

	geometry *geom = &out;

	while (true) {
		if (!read_u8(le)) {
			return false;
		}
		if (!read_u32(type_id)) {
			return false;
		}

		const auto type = static_cast<geometry_type>((type_id & 0xffff) % 1000);
		const auto flags = (type_id & 0xffff) / 1000;
		const auto has_z = (flags == 1) || (flags == 3) || ((type_id & 0x80000000) != 0);
		const auto has_m = (flags == 2) || (flags == 3) || ((type_id & 0x40000000) != 0);
		const auto has_srid = (type_id & 0x20000000) != 0;

		if (has_srid) {
			// Parse the SRID, but we don't use it
			if (!read_u32(srid)) {
				return false;
			}
		}

		geom->set_type(type);
		geom->set_z(has_z);
		geom->set_m(has_m);

		// Compare with root
		if (!has_mixed_zm && (out.has_m() != has_m || out.has_z() != has_z)) {
			has_any_z |= has_z;
			has_any_m |= has_m;
			has_mixed_zm = true;
			if (!allow_mixed_zm) {
				// Error out!
				error = wkb_reader_error::MIXED_ZM;
				return false;
			}
		}

		switch (geom->get_type()) {
		case geometry_type::POINT: {
			// Read the point data
			if (!read_point(geom)) {
				return false;
			}
		} break;
		case geometry_type::LINESTRING: {
			if (!read_line(geom)) {
				return false;
			}
		} break;
		case geometry_type::POLYGON: {
			// Read the ring count
			uint32_t ring_count;
			if (!read_u32(ring_count)) {
				return false;
			}

			// Read the point data;
			for (size_t i = 0; i < ring_count; i++) {
				const auto ring = static_cast<geometry *>(alloc.alloc(sizeof(geometry)));
				new (ring) geometry(geometry_type::LINESTRING, has_z, has_m);
				if (!read_line(ring)) {
					return false;
				}
				geom->append_part(ring);
			}
		} break;
		case geometry_type::MULTI_POINT:
		case geometry_type::MULTI_LINESTRING:
		case geometry_type::MULTI_POLYGON:
		case geometry_type::GEOMETRY_COLLECTION: {

			// Check stack depth
			if (stack_depth >= MAX_STACK_DEPTH) {
				error = wkb_reader_error::RECURSION_LIMIT;
				return false;
			}

			// Read the count
			uint32_t count;
			if (!read_u32(count)) {
				return false;
			}

			if (count == 0) {
				break;
			}

			stack_buf[stack_depth++] = count;

			// Make a new child
			const auto part_mem = alloc.alloc(sizeof(geometry));
			const auto part_ptr = new (part_mem) geometry(geometry_type::INVALID, has_z, has_m);

			geom->append_part(part_ptr);

			// Set the new child as the current geometry
			geom = part_ptr;

			// Continue to the next iteration in the outer loop
			continue;
		}
		default:
			error = wkb_reader_error::UNSUPPORTED_TYPE;
			return false;
		}

		// Inner loop
		while (true) {
			const auto parent = geom->get_parent();

			if (stack_depth == 0) {
				SGL_ASSERT(parent == nullptr);
				if (pos == end) {
					return true;
				}
				error = wkb_reader_error::TRAILING_DATA;
				return false;
			}

			SGL_ASSERT(parent != nullptr);

			// Check that we are of the right type
			const auto ptype = parent->get_type();
			const auto ctype = geom->get_type();

			if (ptype == geometry_type::MULTI_POINT && ctype != geometry_type::POINT) {
				error = wkb_reader_error::INVALID_CHILD_TYPE;
				return false;
			}
			if (ptype == geometry_type::MULTI_LINESTRING && ctype != geometry_type::LINESTRING) {
				error = wkb_reader_error::INVALID_CHILD_TYPE;
				return false;
			}
			if (ptype == geometry_type::MULTI_POLYGON && ctype != geometry_type::POLYGON) {
				error = wkb_reader_error::INVALID_CHILD_TYPE;
				return false;
			}

			// Check if we are done with the current part
			stack_buf[stack_depth - 1]--;

			if (stack_buf[stack_depth - 1] > 0) {
				// There are still more parts to read
				// Create a new part and append it to the parent
				const auto part_mem = alloc.alloc(sizeof(geometry));
				const auto part_ptr = new (part_mem) geometry(geometry_type::INVALID, has_z, has_m);

				parent->append_part(part_ptr);

				// Go "sideways" to the new part
				geom = part_ptr;
				break;
			}

			// Go upwards
			geom = parent;
			stack_depth--;
		}
	}
}

bool wkb_reader::try_parse_stats(extent_xy &out_extent, size_t &out_vertex_count, const char *buf_ptr,
                                 const char *end_ptr) {

	// Setup state
	buf = buf_ptr;
	pos = buf_ptr;
	end = end_ptr;

	error = wkb_reader_error::OK;
	stack_depth = 0;

	le = false;
	type_id = 0;
	has_any_m = false;
	has_any_z = false;

	uint32_t vertex_count = 0;
	extent_xy extent = extent_xy::smallest();

	while (true) {
		if (!read_u8(le)) {
			return false;
		}
		if (!read_u32(type_id)) {
			return false;
		}

		const auto type = static_cast<geometry_type>((type_id & 0xffff) % 1000);
		const auto flags = (type_id & 0xffff) / 1000;
		const auto has_z = (flags == 1) || (flags == 3) || ((type_id & 0x80000000) != 0);
		const auto has_m = (flags == 2) || (flags == 3) || ((type_id & 0x40000000) != 0);
		const auto has_srid = (type_id & 0x20000000) != 0;

		if (has_srid) {
			// Parse the SRID, but we don't use it
			if (!read_u32(srid)) {
				return false;
			}
		}

		switch (type) {
		case geometry_type::POINT: {
			bool all_nan = true;
			double x;
			double y;

			if (!read_f64(x)) {
				return false;
			}
			if (!read_f64(y)) {
				return false;
			}

			all_nan = std::isnan(x) && std::isnan(y);

			if (has_z) {
				double z;
				if (!read_f64(z)) {
					return false;
				}
				all_nan = all_nan && std::isnan(z);
			}
			if (has_m) {
				double m;
				if (!read_f64(m)) {
					return false;
				}
				all_nan = all_nan && std::isnan(m);
			}
			// For points, all NaN is usually interpreted as an empty point
			if (nan_as_empty && all_nan) {
				break;
			}
			extent.min.x = math::min(extent.min.x, x);
			extent.min.y = math::min(extent.min.y, y);
			extent.max.x = math::max(extent.max.x, x);
			extent.max.y = math::max(extent.max.y, y);
			vertex_count++;
		} break;
		case geometry_type::LINESTRING: {
			uint32_t num_points;
			if (!read_u32(num_points)) {
				return false;
			}
			for (uint32_t i = 0; i < num_points; i++) {
				double x;
				double y;
				if (!read_f64(x)) {
					return false;
				}
				if (!read_f64(y)) {
					return false;
				}
				if (has_z) {
					if (!skip(sizeof(double))) {
						return false;
					}
				}
				if (has_m) {
					if (!skip(sizeof(double))) {
						return false;
					}
				}
				extent.min.x = math::min(extent.min.x, x);
				extent.min.y = math::min(extent.min.y, y);
				extent.max.x = math::max(extent.max.x, x);
				extent.max.y = math::max(extent.max.y, y);
			}
			vertex_count += num_points;
		} break;
		case geometry_type::POLYGON: {
			uint32_t num_rings;
			if (!read_u32(num_rings)) {
				return false;
			}
			for (uint32_t i = 0; i < num_rings; i++) {
				uint32_t num_points;
				if (!read_u32(num_points)) {
					return false;
				}
				for (uint32_t j = 0; j < num_points; j++) {
					double x;
					double y;
					if (!read_f64(x)) {
						return false;
					}
					if (!read_f64(y)) {
						return false;
					}
					if (has_z) {
						if (!skip(sizeof(double))) {
							return false;
						}
					}
					if (has_m) {
						if (!skip(sizeof(double))) {
							return false;
						}
					}
					extent.min.x = math::min(extent.min.x, x);
					extent.min.y = math::min(extent.min.y, y);
					extent.max.x = math::max(extent.max.x, x);
					extent.max.y = math::max(extent.max.y, y);
				}
				vertex_count += num_points;
			}
		} break;
		case geometry_type::MULTI_POINT:
		case geometry_type::MULTI_LINESTRING:
		case geometry_type::MULTI_POLYGON:
		case geometry_type::GEOMETRY_COLLECTION: {
			// Check stack depth
			if (stack_depth >= MAX_STACK_DEPTH) {
				error = wkb_reader_error::RECURSION_LIMIT;
				return false;
			}

			// Read the count
			uint32_t count;
			if (!read_u32(count)) {
				return false;
			}
			if (count == 0) {
				break;
			}

			// Push the count to the stack, go downwards and continue
			stack_buf[stack_depth++] = count;
			continue;
		}
		default:
			error = wkb_reader_error::UNSUPPORTED_TYPE;
			return false;
		}

		while (true) {
			if (stack_depth == 0) {
				if (pos != end) {
					error = wkb_reader_error::TRAILING_DATA;
					return false;
				}
				out_vertex_count = vertex_count;
				out_extent = extent;
				return true;
			}

			// Decrement current remaining count
			stack_buf[stack_depth - 1]--;

			// Are there still more parts to read, then break out and continue
			if (stack_buf[stack_depth - 1] > 0) {
				break;
			}

			// Otherwise, go upwards
			stack_depth--;
		}
	}
}

const char *wkb_reader::get_error_message() const {
	if (error == wkb_reader_error::OK) {
		return nullptr;
	}
	switch (error) {
	case wkb_reader_error::OUT_OF_BOUNDS: {
		return "Out of bounds read (is the WKB corrupt?)";
	}
	case wkb_reader_error::MIXED_ZM: {
		return "Mixed Z and M values are not allowed";
	}
	case wkb_reader_error::TRAILING_DATA: {
		return "Unexpected trailing data after WKB geometry";
	}
	case wkb_reader_error::RECURSION_LIMIT: {
		const auto msg_fmt = "Recursion limit '%u' reached";
		const auto msg_len = std::snprintf(nullptr, 0, msg_fmt, MAX_STACK_DEPTH);

		auto msg_buf = static_cast<char *>(alloc.alloc(msg_len + 1));
		std::snprintf(msg_buf, msg_len + 1, msg_fmt, MAX_STACK_DEPTH);
		return msg_buf;
	}
	case wkb_reader_error::UNSUPPORTED_TYPE: {
		// Try to fish out the type anyway
		const auto type = ((type_id & 0xffff) % 1000);
		const auto flags = (type_id & 0xffff) / 1000;
		const auto has_z = (flags == 1) || (flags == 3) || ((type_id & 0x80000000) != 0);
		const auto has_m = (flags == 2) || (flags == 3) || ((type_id & 0x40000000) != 0);
		const auto has_srid = (type_id & 0x20000000) != 0;

		auto guessed_type = "UNKNOWN";
		switch (type) {
		case 1:
			guessed_type = "POINT";
			break;
		case 2:
			guessed_type = "LINESTRING";
			break;
		case 3:
			guessed_type = "POLYGON";
			break;
		case 4:
			guessed_type = "MULTIPOINT";
			break;
		case 5:
			guessed_type = "MULTILINESTRING";
			break;
		case 6:
			guessed_type = "MULTIPOLYGON";
			break;
		case 7:
			guessed_type = "GEOMETRYCOLLECTION";
			break;
		case 8:
			guessed_type = "CIRCULARSTRING";
			break;
		case 9:
			guessed_type = "COMPOUNDCURVE";
			break;
		case 10:
			guessed_type = "CURVEPOLYGON";
			break;
		case 11:
			guessed_type = "MULTICURVE";
			break;
		case 12:
			guessed_type = "MULTISURFACE";
			break;
		case 13:
			guessed_type = "CURVE";
			break;
		case 14:
			guessed_type = "SURFACE";
			break;
		case 15:
			guessed_type = "POLYHEDRALSURFACE";
			break;
		case 16:
			guessed_type = "TIN";
			break;
		case 17:
			guessed_type = "TRIANGLE";
			break;
		case 18:
			guessed_type = "CIRCLE";
			break;
		case 19:
			guessed_type = "GEODESICSTRING";
			break;
		case 20:
			guessed_type = "ELLIPTICALCURVE";
			break;
		case 21:
			guessed_type = "NURBSCURVE";
			break;
		case 22:
			guessed_type = "CLOTHOID";
			break;
		case 23:
			guessed_type = "SPIRALCURVE";
			break;
		case 24:
			guessed_type = "COMPOUNDSURFACE";
			break;
		case 25:
			guessed_type = "ORIENTABLESURFACE";
			break;
		case 102:
			guessed_type = "AFFINEPLACEMENT";
			break;
		default:
			break;
		}

		auto fmt_str = "WKB type '%s' is not supported! (type id: %u, SRID: %u)";
		if (has_z && !has_m) {
			fmt_str = "WKB type '%s Z' is not supported! (type id: %u, SRID: %u)";
		} else if (!has_z && has_m) {
			fmt_str = "WKB type '%s M' is not supported! (type id: %u, SRID: %u)";
		} else if (has_z && has_m) {
			fmt_str = "WKB type '%s ZM' is not supported! (type id: %u, SRID: %u)";
		}

		const auto msg_len = std::snprintf(nullptr, 0, fmt_str, guessed_type, type_id, has_srid ? srid : 0);
		const auto msg_buf = static_cast<char *>(alloc.alloc(msg_len + 1));
		std::snprintf(msg_buf, msg_len + 1, fmt_str, guessed_type, type_id, has_srid ? srid : 0);
		return msg_buf;
	}
	case wkb_reader_error::INVALID_CHILD_TYPE: {
		return "Invalid child type";
	}
	default: {
		return "Unknown error";
	}
	}
}

} // namespace sgl
