#pragma once

#include <cstddef>
#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>

namespace duckdb {
namespace spatial {

// Lightweight non-owning view over contiguous memory (C++11 compatible alternative to std::span)
template <class T>
class ArrayView {
public:
	ArrayView() : ptr_(nullptr), size_(0) {}
	ArrayView(const T *ptr, size_t size) : ptr_(ptr), size_(size) {}
	ArrayView(const std::vector<T> &vec) : ptr_(vec.data()), size_(vec.size()) {}

	const T *data() const { return ptr_; }
	size_t size() const { return size_; }
	bool empty() const { return size_ == 0; }

	const T &operator[](size_t idx) const { return ptr_[idx]; }
	const T *begin() const { return ptr_; }
	const T *end() const { return ptr_ + size_; }

private:
	const T *ptr_;
	size_t size_;
};

// 2D Point structure with double precision
struct Point2D {
	double x;
	double y;

	Point2D() : x(0.0), y(0.0) {}
	Point2D(double x_p, double y_p) : x(x_p), y(y_p) {}

	double DistanceSquared(const Point2D &other) const {
		double dx = x - other.x;
		double dy = y - other.y;
		return dx * dx + dy * dy;
	}

	double Distance(const Point2D &other) const {
		return std::sqrt(DistanceSquared(other));
	}

	bool operator==(const Point2D &other) const {
		return x == other.x && y == other.y;
	}
};

// 3D Point structure with double precision
struct Point3D {
	double x;
	double y;
	double z;

	Point3D() : x(0.0), y(0.0), z(0.0) {}
	Point3D(double x_p, double y_p, double z_p) : x(x_p), y(y_p), z(z_p) {}

	double DistanceSquared(const Point3D &other) const {
		double dx = x - other.x;
		double dy = y - other.y;
		double dz = z - other.z;
		return dx * dx + dy * dy + dz * dz;
	}

	double Distance(const Point3D &other) const {
		return std::sqrt(DistanceSquared(other));
	}

	bool operator==(const Point3D &other) const {
		return x == other.x && y == other.y && z == other.z;
	}
};

// 2D Bounding Box
struct BoundingBox2D {
	double min_x;
	double min_y;
	double max_x;
	double max_y;

	BoundingBox2D()
	    : min_x(std::numeric_limits<double>::max()),
	      min_y(std::numeric_limits<double>::max()),
	      max_x(std::numeric_limits<double>::lowest()),
	      max_y(std::numeric_limits<double>::lowest()) {}

	BoundingBox2D(double min_x_p, double min_y_p, double max_x_p, double max_y_p)
	    : min_x(min_x_p), min_y(min_y_p), max_x(max_x_p), max_y(max_y_p) {}

	static BoundingBox2D FromPointWithRadius(const Point2D &center, double radius) {
		return BoundingBox2D(center.x - radius, center.y - radius, center.x + radius, center.y + radius);
	}

	bool Intersects(const BoundingBox2D &other) const {
		return !(min_x > other.max_x || max_x < other.min_x ||
		         min_y > other.max_y || max_y < other.min_y);
	}

	bool Contains(const Point2D &pt) const {
		return pt.x >= min_x && pt.x <= max_x && pt.y >= min_y && pt.y <= max_y;
	}

	void ExpandToInclude(const Point2D &pt) {
		min_x = std::min(min_x, pt.x);
		min_y = std::min(min_y, pt.y);
		max_x = std::max(max_x, pt.x);
		max_y = std::max(max_y, pt.y);
	}
};

// Abstract Base Class for spatial indexing of 2D points
class SpatialIndex2D {
public:
	virtual ~SpatialIndex2D() = default;

	// Build or initialize index over a set of points
	virtual void Build(const ArrayView<Point2D> &points) = 0;

	// Radius search: populates matches with indices of points within distance eps of center
	virtual void RadiusSearch(const Point2D &center, double eps, std::vector<size_t> &matches) const = 0;

	// Total number of indexed points
	virtual size_t Count() const = 0;
};

// Abstract Base Class for spatial indexing of 3D points
class SpatialIndex3D {
public:
	virtual ~SpatialIndex3D() = default;

	virtual void Build(const ArrayView<Point3D> &points) = 0;
	virtual void RadiusSearch(const Point3D &center, double eps, std::vector<size_t> &matches) const = 0;
	virtual size_t Count() const = 0;
};

} // namespace spatial
} // namespace duckdb
