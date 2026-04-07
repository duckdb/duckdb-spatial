#pragma once

#include "spatial/geometry/vertex.hpp"
#include "duckdb/common/limits.hpp"

namespace duckdb {

template <class V>
struct Box {
	using VERTEX_TYPE = V;
	using VALUE_TYPE = typename V::VALUE_TYPE;

	V min;
	V max;

	Box() : min(NumericLimits<VALUE_TYPE>::Maximum()), max(NumericLimits<VALUE_TYPE>::Minimum()) {
	}

	Box(const V &min_p, const V &max_p) : min(min_p), max(max_p) {
	}

	bool IsUnbounded() const {
		return *this == Box();
	}

	// Only does a 2D intersection check
	bool Intersects(const Box &other) const {
		return !(min.x > other.max.x || max.x < other.min.x || min.y > other.max.y || max.y < other.min.y);
	}

	// Only does a 2D containment check
	bool Contains(const Box &other) const {
		return min.x <= other.min.x && min.y <= other.min.y && max.x >= other.max.x && max.y >= other.max.y;
	}

	bool Contains(const V &vertex) const {
		return min.x <= vertex.x && min.y <= vertex.y && max.x >= vertex.x && max.y >= vertex.y;
	}

	void Stretch(const V &vertex) {
		for (idx_t i = 0; i < V::SIZE; i++) {
			min[i] = MinValue(min[i], vertex[i]);
			max[i] = MaxValue(max[i], vertex[i]);
		}
	}

	void Union(const Box &other) {
		for (idx_t i = 0; i < V::SIZE; i++) {
			min[i] = MinValue(min[i], other.min[i]);
			max[i] = MaxValue(max[i], other.max[i]);
		}
	}

	static Box Union(const Box &left, const Box &right) {
		Box result = left;
		result.Union(right);
		return result;
	}

	VALUE_TYPE OverlapArea(const Box &other) const {
		VALUE_TYPE x_overlap =
		    MaxValue(static_cast<VALUE_TYPE>(0), MinValue(max.x, other.max.x) - MaxValue(min.x, other.min.x));
		VALUE_TYPE y_overlap =
		    MaxValue(static_cast<VALUE_TYPE>(0), MinValue(max.y, other.max.y) - MaxValue(min.y, other.min.y));
		return x_overlap * y_overlap;
	}

	// 2D area
	VALUE_TYPE Area() const {
		return (max.x - min.x) * (max.y - min.y);
	}

	// 2D Perimeter
	VALUE_TYPE Perimeter() const {
		return 2 * (max.x - min.x + max.y - min.y);
	}

	VERTEX_TYPE Center() const {
		return (min + max) / 2;
	}

	// Minimum squared distance from a point to the nearest edge of this box.
	// Returns 0 if the point is inside the box.
	VALUE_TYPE MinDistanceSquared(const V &point) const {
		VALUE_TYPE dx = 0, dy = 0;
		if (point.x < min.x) {
			dx = min.x - point.x;
		} else if (point.x > max.x) {
			dx = point.x - max.x;
		}
		if (point.y < min.y) {
			dy = min.y - point.y;
		} else if (point.y > max.y) {
			dy = point.y - max.y;
		}
		return dx * dx + dy * dy;
	}

	// Minimum squared distance between two boxes.
	// Returns 0 if the boxes overlap.
	VALUE_TYPE MinDistanceSquared(const Box &other) const {
		VALUE_TYPE dx = 0, dy = 0;
		if (other.max.x < min.x) {
			dx = min.x - other.max.x;
		} else if (other.min.x > max.x) {
			dx = other.min.x - max.x;
		}
		if (other.max.y < min.y) {
			dy = min.y - other.max.y;
		} else if (other.min.y > max.y) {
			dy = other.min.y - max.y;
		}
		return dx * dx + dy * dy;
	}

	bool operator==(const Box &other) const {
		return min == other.min && max == other.max;
	}

	bool operator!=(const Box &other) const {
		return !(*this == other);
	}
};

template <class T>
using Box2D = Box<PointXY<T>>;

} // namespace duckdb