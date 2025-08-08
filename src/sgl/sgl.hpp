#pragma once

//======================================================================================================================
// Includes
//======================================================================================================================

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef DUCKDB_SPATIAL_EXTENSION
#include "duckdb/common/assert.hpp"
#define SGL_ASSERT(x) D_ASSERT(x)
#else
#include <cassert>
#define SGL_ASSERT(x) assert(x)
#endif

//======================================================================================================================
// Allocator
//======================================================================================================================

namespace sgl {

class allocator {
public:
	virtual void *alloc(size_t size) = 0;
	virtual void dealloc(void *ptr, size_t size) = 0;
	virtual void *realloc(void *ptr, size_t old_size, size_t new_size) = 0;
	virtual ~allocator() = default;

	template <class T, class... ARGS>
	T *make(ARGS &&... args) {
		auto ptr = static_cast<T *>(alloc(sizeof(T)));
		if (!ptr) {
			return nullptr;
		}
		new (ptr) T(static_cast<ARGS &&>(args)...);
		return ptr;
	}
};

} // namespace sgl

//======================================================================================================================
// Math
//======================================================================================================================
namespace sgl {
namespace math {
// Avoid including <algorithm> in the header to keep dependencies minimal
template <class T>
const T &max(const T &a, const T &b) {
	return (a < b) ? b : a;
}
template <class T>
const T &min(const T &a, const T &b) {
	return !(b < a) ? a : b;
}

template <class T>
T clamp(const T &value, const T &min_value, const T &max_value) {
	return (value < min_value) ? min_value : (value > max_value ? max_value : value);
}
} // namespace math
} // namespace sgl
//======================================================================================================================
// Vertex
//======================================================================================================================

namespace sgl {

enum class vertex_type : uint8_t {
	XY = 0,
	XYZ = 1,
	XYM = 2,
	XYZM = 3,
};

struct vertex_xy {
	double x;
	double y;

	vertex_xy operator-(const vertex_xy &other) const {
		return {x - other.x, y - other.y};
	}
	vertex_xy operator+(const vertex_xy &other) const {
		return {x + other.x, y + other.y};
	}
	vertex_xy operator*(const double scalar) const {
		return {x * scalar, y * scalar};
	}
	vertex_xy operator/(const double scalar) const {
		return {x / scalar, y / scalar};
	}

	double dot(const vertex_xy &other) const {
		return x * other.x + y * other.y;
	}

	double norm_sq() const {
		return x * x + y * y;
	}
};

struct vertex_xyzm {
	double x;
	double y;
	double z;
	double m;

	vertex_xyzm operator-(const vertex_xyzm &other) const {
		return {x - other.x, y - other.y, z - other.z, m - other.m};
	}
	vertex_xyzm operator+(const vertex_xyzm &other) const {
		return {x + other.x, y + other.y, z + other.z, m + other.m};
	}
	vertex_xyzm operator*(const double scalar) const {
		return {x * scalar, y * scalar, z * scalar, m * scalar};
	}
	vertex_xyzm operator/(const double scalar) const {
		return {x / scalar, y / scalar, z / scalar, m / scalar};
	}

	double &operator[](const size_t index) {
		switch (index) {
		case 0:
			return x;
		case 1:
			return y;
		case 2:
			return z;
		case 3:
			return m;
		default:
			SGL_ASSERT(false && "Index out of bounds");
			static double dummy = 0; // To avoid returning a reference to an invalid location
			return dummy;
		}
	}
};

} // namespace sgl

//======================================================================================================================
// Extent
//======================================================================================================================

namespace sgl {

struct extent_xy {
	vertex_xy min;
	vertex_xy max;

	static extent_xy smallest() {
		return {{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()},
		        {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()}};
	}

	bool contains(const vertex_xy &other) const {
		return (min.x <= other.x && max.x >= other.x && min.y <= other.y && max.y >= other.y);
	}

	bool intersects(const extent_xy &other) const {
		return !(min.x > other.max.x || max.x < other.min.x || min.y > other.max.y || max.y < other.min.y);
	}

	double distance_to(const vertex_xy &other) const {
		if (contains(other)) {
			return 0.0;
		}

		const auto dx = math::max(min.x - other.x, other.x - max.x);
		const auto dy = math::max(min.y - other.y, other.y - max.y);
		return std::sqrt(dx * dx + dy * dy);
	}

	double distance_to_sq(const extent_xy &other) const {
		const auto dx = math::max(0.0, math::max(min.x - other.max.x, other.min.x - max.x));
		const auto dy = math::max(0.0, math::max(min.y - other.max.y, other.min.y - max.y));
		return dx * dx + dy * dy;
	}

	double distance_to(const extent_xy &other) const {
		return std::sqrt(distance_to_sq(other));
	}

	double get_area() const {
		if (min.x >= max.x || min.y >= max.y) {
			return 0.0;
		}
		return (max.x - min.x) * (max.y - min.y);
	}
};

struct extent_xyzm {
	vertex_xyzm min;
	vertex_xyzm max;

	static extent_xyzm smallest() {
		return {{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
		         std::numeric_limits<double>::max(), std::numeric_limits<double>::max()},
		        {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
		         std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()}};
	}

	static extent_xyzm zero() {
		return {{0, 0, 0, 0}, {0, 0, 0, 0}};
	}
};

} // namespace sgl

//======================================================================================================================
// Affine Matrix
//======================================================================================================================
namespace sgl {

struct affine_matrix {

	// *------------*
	// | a b c xoff |
	// | d e f yoff |
	// | g h i zoff |
	// | 0 0 0 1	|
	// *------------*
	double v[16] = {};

	static affine_matrix identity() {
		affine_matrix result;
		result.v[0] = 1;
		result.v[5] = 1;
		result.v[10] = 1;
		return result;
	}

	static affine_matrix translate(const double x, const double y, const double z = 0) {
		affine_matrix result = identity();
		result.v[3] = x;
		result.v[7] = y;
		result.v[11] = z;
		return result;
	}

	static affine_matrix scale(const double x, const double y, const double z = 1) {
		affine_matrix result = identity();
		result.v[0] = x;
		result.v[5] = y;
		result.v[10] = z;
		return result;
	}

	static affine_matrix rotate_x(const double angle) {
		affine_matrix result = identity();
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		result.v[5] = c;
		result.v[6] = -s;
		result.v[9] = s;
		result.v[10] = c;
		return result;
	}

	static affine_matrix rotate_y(const double angle) {
		affine_matrix result = identity();
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		result.v[0] = c;
		result.v[2] = s;
		result.v[8] = -s;
		result.v[10] = c;
		return result;
	}

	static affine_matrix rotate_z(const double angle) {
		affine_matrix result = identity();
		const auto c = std::cos(angle);
		const auto s = std::sin(angle);
		result.v[0] = c;
		result.v[1] = -s;
		result.v[4] = s;
		result.v[5] = c;
		return result;
	}

	static affine_matrix translate_scale(const double x, const double y, const double z, const double sx,
	                                     const double sy, const double sz) {
		affine_matrix result = identity();
		result.v[0] = sx;
		result.v[5] = sy;
		result.v[10] = sz;
		result.v[3] = x;
		result.v[7] = y;
		result.v[11] = z;
		return result;
	}

	vertex_xy apply_xy(const vertex_xy &vertex) const {

		// x = a * x + b * y + xoff;
		// y = d * x + e * y + yoff;

		vertex_xy result = {0, 0};
		result.x = v[0] * vertex.x + v[1] * vertex.y + v[3];
		result.y = v[4] * vertex.x + v[5] * vertex.y + v[7];
		return result;
	}

	vertex_xyzm apply_xyz(const vertex_xyzm &vertex) const {

		// x = a * x + b * y + c * z + xoff;
		// y = d * x + e * y + f * z + yoff;
		// z = g * x + h * y + i * z + zoff;

		vertex_xyzm result = {0, 0, 0, 0};
		result.x = v[0] * vertex.x + v[1] * vertex.y + v[2] * vertex.z + v[3];
		result.y = v[4] * vertex.x + v[5] * vertex.y + v[6] * vertex.z + v[7];
		result.z = v[8] * vertex.x + v[9] * vertex.y + v[10] * vertex.z + v[11];
		return result;
	}
};

} // namespace sgl
//======================================================================================================================
// Geometry
//======================================================================================================================

namespace sgl {

enum class geometry_type : uint8_t {
	INVALID = 0,
	POINT = 1,
	LINESTRING = 2,
	POLYGON = 3,
	MULTI_POINT = 4,
	MULTI_LINESTRING = 5,
	MULTI_POLYGON = 6,
	GEOMETRY_COLLECTION = 7,
};

class geometry {
public:
	//------------------------------------------------------------------------------------------------------------------
	// Constructors
	//------------------------------------------------------------------------------------------------------------------

	geometry() : next(nullptr), prnt(nullptr), type(geometry_type::INVALID), flag(0), padd(0), size(0), data(nullptr) {
	}

	geometry(const geometry_type type, const bool has_z, const bool has_m)
	    : next(nullptr), prnt(nullptr), type(type), flag(0), padd(0), size(0), data(nullptr) {
		set_z(has_z);
		set_m(has_m);
	}

	geometry(const geometry &other) = delete;            // Not copyable
	geometry &operator=(const geometry &other) = delete; // Not copy assignable
	geometry(geometry &&other) = delete;                 // Not movable
	geometry &operator=(geometry &&other) = delete;      // Not move assignable

	//------------------------------------------------------------------------------------------------------------------
	// Property Getters and Setters
	//------------------------------------------------------------------------------------------------------------------

	geometry_type get_type() const {
		return type;
	}

	bool is_multi_part() const {
		return type >= geometry_type::POLYGON && type <= geometry_type::GEOMETRY_COLLECTION;
	}

	bool is_multi_geom() const {
		return type >= geometry_type::MULTI_POINT && type <= geometry_type::GEOMETRY_COLLECTION;
	}

	void set_type(const geometry_type type) {
		this->type = type;
	}

	bool has_z() const {
		return flag & 0x01;
	}

	bool has_m() const {
		return flag & 0x02;
	}

	bool is_prepared() const {
		return flag & 0x04;
	}

	void set_z(const bool value) {
		if (value) {
			flag |= 0x01;
		} else {
			flag &= ~0x01;
		}
	}

	void set_m(const bool value) {
		if (value) {
			flag |= 0x02;
		} else {
			flag &= ~0x02;
		}
	}

	void set_prepared(const bool value) {
		if (value) {
			flag |= 0x04;
		} else {
			flag &= ~0x04;
		}
	}

	bool is_empty() const {
		return size == 0;
	}

	uint16_t get_extra() const {
		return padd;
	}

	void reset() {
		next = nullptr;
		prnt = nullptr;
		type = geometry_type::INVALID;
		flag = 0;
		padd = 0;
		size = 0;
		data = nullptr;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Relationship Getters and Setters
	//------------------------------------------------------------------------------------------------------------------

	geometry *get_parent() {
		return prnt;
	}

	const geometry *get_parent() const {
		return prnt;
	}

	geometry *get_next() {
		return next;
	}

	const geometry *get_next() const {
		return next;
	}

	geometry *get_first_part() {
		return get_last_part() ? get_last_part()->next : nullptr;
	}

	const geometry *get_first_part() const {
		return get_last_part() ? get_last_part()->next : nullptr;
	}

	geometry *get_last_part() {
		return static_cast<geometry *>(data);
	}

	const geometry *get_last_part() const {
		return static_cast<const geometry *>(data);
	}

	uint32_t get_part_count() const {
		return size;
	}

	void append_part(geometry *part) {
		SGL_ASSERT(is_multi_part() || type == geometry_type::INVALID);
		SGL_ASSERT(part != nullptr);

		const auto tail = get_last_part();
		if (tail == nullptr) {
			SGL_ASSERT(size == 0);
			part->next = part;
		} else {
			SGL_ASSERT(size != 0);
			const auto head = tail->next;
			tail->next = part;
			part->next = head;
		}

		part->prnt = this;
		data = part;
		size++;
	}

	void filter_parts(void *state, bool (*select_callback)(void *state, const geometry *part),
	                  void (*handle_callback)(void *state, geometry *part));

	//------------------------------------------------------------------------------------------------------------------
	// Vertex Getters and Setters
	//------------------------------------------------------------------------------------------------------------------

	vertex_type get_vertex_type() const {
		return static_cast<vertex_type>(has_z() + has_m() * 2);
	}

	size_t get_vertex_width() const {
		return sizeof(double) * (2 + has_z() + has_m());
	}

	uint32_t get_vertex_count() const {
		SGL_ASSERT(!is_multi_part() || type == geometry_type::INVALID);
		return size;
	}

	char *get_vertex_array() {
		SGL_ASSERT(!is_multi_part() || type == geometry_type::INVALID);
		return static_cast<char *>(data);
	}

	void set_vertex_array(const void *data, uint32_t size) {
		SGL_ASSERT(!is_multi_part() || type == geometry_type::INVALID);
		this->data = const_cast<void *>(data);
		this->size = size;
	}

	const char *get_vertex_array() const {
		SGL_ASSERT(!is_multi_part() || type == geometry_type::INVALID);
		return static_cast<char *>(data);
	}

	vertex_xy get_vertex_xy(const uint32_t index) const {
		SGL_ASSERT(index < size);
		const auto vertex_array = get_vertex_array();
		const auto vertex_width = get_vertex_width();
		vertex_xy result;
		memcpy(&result, vertex_array + index * vertex_width, sizeof(vertex_xy));
		return result;
	}

	vertex_xyzm get_vertex_xyzm(const uint32_t index) const {
		SGL_ASSERT(index < size);
		const auto vertex_array = get_vertex_array();
		const auto vertex_width = get_vertex_width();
		vertex_xyzm result = {0, 0, 0, 0};
		memcpy(&result, vertex_array + index * vertex_width, vertex_width);
		return result;
	}

private:
	//------------------------------------------------------------------------------------------------------------------
	// Private Members
	//------------------------------------------------------------------------------------------------------------------

	geometry *next;
	geometry *prnt;

	geometry_type type;
	uint8_t flag;
	uint16_t padd;
	uint32_t size;

	void *data;
};

} // namespace sgl
//======================================================================================================================
// Prepared Geometry
//======================================================================================================================
namespace sgl {

enum class point_in_polygon_result {
	INVALID = 0,
	INTERIOR,
	EXTERIOR,
	BOUNDARY,
};

class prepared_geometry : public geometry {
public:
	explicit prepared_geometry(geometry_type type = geometry_type::INVALID, bool has_z = false, bool has_m = false)
	    : geometry(type, has_z, has_m) {
	}

	// Construct the prepared geometry by indexing the vertex array
	void build(allocator &allocator);

	// Check if the prepared geometry "contains" a vertex
	point_in_polygon_result contains(const vertex_xy &vertex) const;

	// Get the distance to the nearest vertex in the prepared geometry
	bool try_get_distance(const vertex_xy &vertex, double &distance) const;

	// Get the distance between the nearest segments of this prepared geometry and another prepared geometry
	bool try_get_distance(const prepared_geometry &other, double &distance) const;

	// Construct from existing geometry
	static void make(allocator &allocator, const geometry &geom, prepared_geometry &result);

	bool try_get_extent(extent_xy &extent) const {
		if (index.items_count == 0) {
			return false;
		}
		extent = index.level_array[0].entry_array[0];
		return true;
	}

public:
	bool try_get_distance_recursive(uint32_t depth, uint32_t entry, const vertex_xy &vertex, double &distance) const;

	struct prepared_index {
		static constexpr uint32_t NODE_SIZE = 32;
		static constexpr uint32_t MAX_DEPTH = 8; // 16^8 >= max(uint32_t)

		struct level {
			extent_xy *entry_array;
			uint32_t entry_count;
		};
		level *level_array;
		uint32_t level_count;

		uint32_t items_count; // Total number of leaf items in the index
	};

	prepared_index index = {};
};

} // namespace sgl

//======================================================================================================================
// Algorithms
//======================================================================================================================

namespace sgl {

namespace ops {

// Return the area of all polygonal parts of the geometry
double get_area(const geometry &geom);

// Return the length of all linestring parts of the geometry
double get_length(const geometry &geom);

// Return the perimeter of all polygonal parts of the geometry
double get_perimeter(const geometry &geom);

// Get the total number of vertices in all parts of the geometry
uint32_t get_total_vertex_count(const geometry &geom);

// Get the total extent of all parts of the geometry, and return the number of vertices
uint32_t get_total_extent_xy(const geometry &geom, extent_xy &ext);

// Get the total extent of all parts of the geometry, and return the number of vertices
uint32_t get_total_extent_xyzm(const geometry &geom, extent_xyzm &ext);

// Get the max surface dimension of the geometry, ignoring empty parts
// Empty geometries are not counted, and if the whole geometry is empty, -1 is returned
int32_t get_max_surface_dimension(const geometry &geom, bool ignore_empty);

bool get_centroid(const geometry &geom, vertex_xyzm &centroid);

bool get_centroid_from_polygons(const geometry &geom, vertex_xyzm &centroid);

bool get_centroid_from_points(const geometry &geom, vertex_xyzm &centroid);

bool get_centroid_from_linestrings(const geometry &geom, vertex_xyzm &centroid);

bool get_euclidean_distance(const geometry &lhs_geom, const geometry &rhs_geom, double &result);

// Visits all geometries in the geometry, calling the callback for each vertex
void visit_vertices_xy(const geometry &geom, void *state, void (*callback)(void *state, const vertex_xy &vertex));
void visit_vertices_xyzm(const geometry &geom, void *state, void (*callback)(void *state, const vertex_xyzm &vertex));
void transform_vertices(allocator &allocator, geometry &geom, void *state,
                        void (*callback)(void *state, vertex_xyzm &vertex));

void visit_point_geometries(const geometry &geom, void *state, void (*callback)(void *state, const geometry &part));
void visit_linestring_geometries(const geometry &geom, void *state,
                                 void (*callback)(void *state, const geometry &part));
void visit_polygon_geometries(const geometry &geom, void *state, void (*callback)(void *state, const geometry &part));

// Flips vertices, by replacing the vertex arrays in each geometry with a newly allocated array where
// the x and y coordinates are swapped.
void flip_vertices(allocator &allocator, geometry &geom);

// Transforms the vertices of the geometry using the affine matrix, by replacing all vertex arrays with a new array
// with the transformed vertices.
void affine_transform(allocator &allocator, geometry &geom, const affine_matrix &matrix);

// Force all vertices to have Z and M values as specified, by replacing the vertex arrays with new arrays
void force_zm(allocator &alloc, geometry &geom, bool set_z, bool set_m, double default_z, double default_m);

// Collects all vertices into a new multipoint. Initializes result as a multipoint geometry with the same
// vertex type as the input geometry.
void collect_vertices(allocator &alloc, const geometry &geom, geometry &result);

bool is_closed(const geometry &geom);

void extract_points(geometry &geom, geometry &result);
void extract_linestrings(geometry &geom, geometry &result);
void extract_polygons(geometry &geom, geometry &result);

void locate_along(allocator &alloc, const geometry &linear_geom, double measure, double offset, geometry &out_geom);
void locate_between(allocator &alloc, const geometry &linear_geom, double measure_beg, double measure_end,
                    double offset, geometry &out_geom);

void clip_point_to_box(const geometry &geom, const geometry &bbox, geometry &out_geom);
void clip_multipoint_to_box(allocator &alloc, const geometry &geom, const geometry &bbox, geometry &out_geom);

} // namespace ops

// TODO: Move these
namespace linestring {
bool is_closed(const geometry &geom);
bool interpolate(const geometry &geom, double frac, vertex_xyzm &out);

// returns a multipoint with interpolated points
void interpolate_points(allocator &alloc, const geometry &geom, double frac, geometry &result);
void substring(allocator &alloc, const geometry &geom, double beg_frac, double end_frac, geometry &result);

bool interpolate_point(const geometry &linear_geom, const geometry &point_geom, double &out_measure);
void locate_along(allocator &alloc, const geometry &linear_geom, double measure, double offset, geometry &out_geom);
void locate_between(allocator &alloc, const geometry &linear_geom, double measure_beg, double measure_end,
                    double offset, geometry &out_geom);

double line_locate_point(const geometry &line_geom, const geometry &point_geom);

} // namespace linestring

namespace multi_linestring {
bool is_closed(const geometry &geom);
}

namespace polygon {
void init_from_bbox(allocator &alloc, double min_x, double min_y, double max_x, double max_y, geometry &result);
}

} // namespace sgl

//======================================================================================================================
// Conversion
//======================================================================================================================

namespace sgl {

class wkt_reader {
public:
	explicit wkt_reader(allocator &alloc) : alloc(alloc), buf(nullptr), end(nullptr), pos(nullptr), error(nullptr) {
	}

	wkt_reader(const wkt_reader &other) = delete;
	wkt_reader &operator=(const wkt_reader &other) = delete;
	wkt_reader(wkt_reader &&other) = delete;
	wkt_reader &operator=(wkt_reader &&other) = delete;

	bool try_parse(geometry &out, const char *buf, const size_t size) {
		return try_parse(out, buf, buf + size);
	}
	bool try_parse(geometry &out, const char *cstr) {
		return try_parse(out, cstr, cstr + strlen(cstr));
	}
	bool try_parse(geometry &out, const char *buf, const char *end);

	const char *get_error_message() const;

private:
	void match_ws();
	bool match_str(const char *str);
	bool match_char(char c);
	bool match_number(double &val);

	allocator &alloc;
	const char *buf;
	const char *end;
	const char *pos;
	const char *error;
};

enum class wkb_reader_error {
	OK = 0,
	UNSUPPORTED_TYPE = 1,
	OUT_OF_BOUNDS = 2,
	RECURSION_LIMIT = 3,
	MIXED_ZM = 4,
	INVALID_CHILD_TYPE = 5,
};

class wkb_reader {
public:
	explicit wkb_reader(allocator &alloc)
	    : alloc(alloc), buf(nullptr), end(nullptr), pos(nullptr), copy_vertices(false), allow_mixed_zm(false),
	      nan_as_empty(false), error(wkb_reader_error::OK), srid(0), type_id(0), le(false), has_mixed_zm(false),
	      has_any_z(false), has_any_m(false), stack_depth(0) {
	}

	bool try_parse(geometry &out, const char *buf, const size_t size) {
		return try_parse(out, buf, buf + size);
	}
	bool try_parse(geometry &out, const char *cstr) {
		return try_parse(out, cstr, cstr + strlen(cstr));
	}
	bool try_parse(geometry &out, const char *buf, const char *end);

	bool try_parse_stats(extent_xy &out_extent, size_t &out_vertex_count, const char *buf, const size_t size) {
		return try_parse_stats(out_extent, out_vertex_count, buf, buf + size);
	}
	bool try_parse_stats(extent_xy &out_extent, size_t &out_vertex_count, const char *cstr) {
		return try_parse_stats(out_extent, out_vertex_count, cstr, cstr + strlen(cstr));
	}
	bool try_parse_stats(extent_xy &out_extent, size_t &out_vertex_count, const char *buf, const char *end);

	// Options
	void set_copy_vertices(const bool value) {
		copy_vertices = value;
	}
	void set_allow_mixed_zm(const bool value) {
		allow_mixed_zm = value;
	}
	void set_nan_as_empty(const bool value) {
		nan_as_empty = value;
	}

	// Inspect
	wkb_reader_error get_error() const {
		return error;
	}

	bool parsed_mixed_zm() const {
		return has_mixed_zm;
	}
	bool parsed_any_z() const {
		return has_any_z;
	}
	bool parsed_any_m() const {
		return has_any_m;
	}

	// This might allocate memory using the allocator, or return a static string.
	// Returns nullptr if there is no error.
	const char *get_error_message() const;

private:
	static constexpr auto MAX_STACK_DEPTH = 32;

	bool skip(size_t size);
	bool read_u8(bool &val);
	bool read_u8(uint8_t &val);
	bool read_u32(uint32_t &val);
	bool read_f64(double &val);
	bool read_point(geometry *val);
	bool read_line(geometry *val);

	allocator &alloc;
	const char *buf;
	const char *end;
	const char *pos;

	bool copy_vertices;
	bool allow_mixed_zm;
	bool nan_as_empty;

	wkb_reader_error error;

	uint32_t srid;
	uint32_t type_id;
	bool le;
	bool has_mixed_zm;
	bool has_any_z;
	bool has_any_m;

	size_t stack_depth;
	uint32_t stack_buf[16] = {0};
};

} // namespace sgl

//======================================================================================================================
// Math
//======================================================================================================================
namespace sgl {

namespace math {

inline double haversine_distance(const double lat1_p, const double lon1_p, const double lat2_p, const double lon2_p) {
	// Radius of the earth in km
	constexpr auto R = 6371000.0;
	constexpr auto PI = 3.14159265358979323846;

	// Convert to radians
	const auto lat1 = lat1_p * PI / 180.0;
	const auto lon1 = lon1_p * PI / 180.0;
	const auto lat2 = lat2_p * PI / 180.0;
	const auto lon2 = lon2_p * PI / 180.0;

	const auto dlat = lat2 - lat1;
	const auto dlon = lon2 - lon1;

	const auto a =
	    std::pow(std::sin(dlat / 2.0), 2.0) + std::cos(lat1) * std::cos(lat2) * std::pow(std::sin(dlon / 2.0), 2.0);
	const auto c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));

	return R * c;
}

// Hilbert Curve Encoding, from (Public Domain): https://github.com/rawrunprotected/hilbert_curves
inline uint32_t hilbert_interleave(uint32_t x) {
	x = (x | (x << 8)) & 0x00FF00FF;
	x = (x | (x << 4)) & 0x0F0F0F0F;
	x = (x | (x << 2)) & 0x33333333;
	x = (x | (x << 1)) & 0x55555555;
	return x;
}

inline uint32_t hilbert_encode(uint32_t n, uint32_t x, uint32_t y) {
	x = x << (16 - n);
	y = y << (16 - n);

	// Initial prefix scan round, prime with x and y
	uint32_t a = x ^ y;
	uint32_t b = 0xFFFF ^ a;
	uint32_t c = 0xFFFF ^ (x | y);
	uint32_t d = x & (y ^ 0xFFFF);
	uint32_t A = a | (b >> 1);
	uint32_t B = (a >> 1) ^ a;
	uint32_t C = ((c >> 1) ^ (b & (d >> 1))) ^ c;
	uint32_t D = ((a & (c >> 1)) ^ (d >> 1)) ^ d;

	a = A;
	b = B;
	c = C;
	d = D;
	A = ((a & (a >> 2)) ^ (b & (b >> 2)));
	B = ((a & (b >> 2)) ^ (b & ((a ^ b) >> 2)));
	C ^= ((a & (c >> 2)) ^ (b & (d >> 2)));
	D ^= ((b & (c >> 2)) ^ ((a ^ b) & (d >> 2)));

	a = A;
	b = B;
	c = C;
	d = D;
	A = ((a & (a >> 4)) ^ (b & (b >> 4)));
	B = ((a & (b >> 4)) ^ (b & ((a ^ b) >> 4)));
	C ^= ((a & (c >> 4)) ^ (b & (d >> 4)));
	D ^= ((b & (c >> 4)) ^ ((a ^ b) & (d >> 4)));

	// Final round and projection
	a = A;
	b = B;
	c = C;
	d = D;
	C ^= ((a & (c >> 8)) ^ (b & (d >> 8)));
	D ^= ((b & (c >> 8)) ^ ((a ^ b) & (d >> 8)));

	// Undo transformation prefix scan
	a = C ^ (C >> 1);
	b = D ^ (D >> 1);

	// Recover index bits
	const uint32_t i0 = x ^ y;
	const uint32_t i1 = b | (0xFFFF ^ (i0 | a));

	return ((hilbert_interleave(i1) << 1) | hilbert_interleave(i0)) >> (32 - 2 * n);
}

inline uint32_t hilbert_f32_to_u32(float f) {
	if (std::isnan(f)) {
		return 0xFFFFFFFF;
	}
	uint32_t res;
	memcpy(&res, &f, sizeof(res));
	if ((res & 0x80000000) != 0) {
		res ^= 0xFFFFFFFF;
	} else {
		res |= 0x80000000;
	}
	return res;
}

} // namespace math

} // namespace sgl
