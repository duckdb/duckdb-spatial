#pragma once

#include "spatial/geometry/spatial_index_interface.hpp"
#include <vector>
#include <algorithm>
#include <cmath>
#include <queue>
#include <cstdint>
#include <limits>

namespace duckdb {
namespace spatial {

// Hilbert curve encoding (16-bit coordinates, fast integer bit twiddling)
inline uint32_t HilbertEncode(uint32_t x, uint32_t y) {
	uint32_t d = 0;
	for (uint32_t s = 1 << 15; s > 0; s /= 2) {
		uint32_t rx = (x & s) > 0 ? 1 : 0;
		uint32_t ry = (y & s) > 0 ? 1 : 0;
		d += s * s * ((3 * rx) ^ ry);
		if (ry == 0) {
			if (rx == 1) {
				x = (1 << 16) - 1 - x;
				y = (1 << 16) - 1 - y;
			}
			std::swap(x, y);
		}
	}
	return d;
}

// Bounding box using single-precision floats for high cache locality
struct BBox2Df {
	float min_x;
	float min_y;
	float max_x;
	float max_y;

	BBox2Df()
	    : min_x(std::numeric_limits<float>::max()),
	      min_y(std::numeric_limits<float>::max()),
	      max_x(std::numeric_limits<float>::lowest()),
	      max_y(std::numeric_limits<float>::lowest()) {}

	BBox2Df(float min_x_p, float min_y_p, float max_x_p, float max_y_p)
	    : min_x(min_x_p), min_y(min_y_p), max_x(max_x_p), max_y(max_y_p) {}

	bool Intersects(const BBox2Df &other) const {
		return !(min_x > other.max_x || max_x < other.min_x ||
		         min_y > other.max_y || max_y < other.min_y);
	}

	void Union(const BBox2Df &other) {
		min_x = std::min(min_x, other.min_x);
		min_y = std::min(min_y, other.min_y);
		max_x = std::max(max_x, other.max_x);
		max_y = std::max(max_y, other.max_y);
	}
};

// In-Memory Packed Static R-Tree (Hilbert-curve sorted bulk load)
class FlatRTree2D : public SpatialIndex2D {
public:
	explicit FlatRTree2D(uint32_t node_size = 64) : node_size_(node_size), item_count_(0) {}

	void Build(const ArrayView<Point2D> &points) override {
		points_ = points;
		item_count_ = static_cast<uint32_t>(points.size());

		if (item_count_ == 0) {
			boxes_.clear();
			indices_.clear();
			layer_bounds_.clear();
			return;
		}

		ComputeLayerBounds();
		const size_t total_nodes = layer_bounds_.back();

		boxes_.resize(total_nodes);
		indices_.resize(total_nodes);

		// Compute data bounds
		tree_box_ = BBox2Df();
		for (uint32_t i = 0; i < item_count_; ++i) {
			const float x = static_cast<float>(points[i].x);
			const float y = static_cast<float>(points[i].y);
			boxes_[i] = BBox2Df(x, y, x, y);
			indices_[i] = i;
			tree_box_.Union(boxes_[i]);
		}

		if (item_count_ <= node_size_) {
			return;
		}

		// Calculate 16-bit Hilbert curve projection
		constexpr double max_hilbert = 65535.0;
		const double width = std::max(static_cast<double>(tree_box_.max_x - tree_box_.min_x), 1e-9);
		const double height = std::max(static_cast<double>(tree_box_.max_y - tree_box_.min_y), 1e-9);

		std::vector<uint32_t> curve(item_count_);
		for (uint32_t i = 0; i < item_count_; ++i) {
			const double norm_x = (points[i].x - tree_box_.min_x) / width;
			const double norm_y = (points[i].y - tree_box_.min_y) / height;
			const uint32_t hx = static_cast<uint32_t>(std::max(0.0, std::min(max_hilbert, norm_x * max_hilbert)));
			const uint32_t hy = static_cast<uint32_t>(std::max(0.0, std::min(max_hilbert, norm_y * max_hilbert)));
			curve[i] = HilbertEncode(hx, hy);
		}

		// Sort leaves by Hilbert curve value
		QuickSort(curve, 0, item_count_ - 1);

		// Build internal R-Tree layers bottom-up
		size_t current_pos = item_count_;
		size_t layer_idx = 0;
		size_t entry_idx = 0;

		while (layer_idx < layer_bounds_.size() - 1) {
			const size_t entry_end = layer_bounds_[layer_idx];

			while (entry_idx < entry_end) {
				const size_t node_start = entry_idx;
				BBox2Df node_box = boxes_[entry_idx];

				size_t child_count = 0;
				while (child_count < node_size_ && entry_idx < entry_end) {
					node_box.Union(boxes_[entry_idx]);
					child_count++;
					entry_idx++;
				}

				indices_[current_pos] = static_cast<uint32_t>(node_start);
				boxes_[current_pos] = node_box;
				current_pos++;
			}

			layer_idx++;
		}
	}

	void RadiusSearch(const Point2D &center, double eps, std::vector<size_t> &matches) const override {
		matches.clear();
		if (item_count_ == 0) {
			return;
		}

		const float search_min_x = static_cast<float>(center.x - eps);
		const float search_min_y = static_cast<float>(center.y - eps);
		const float search_max_x = static_cast<float>(center.x + eps);
		const float search_max_y = static_cast<float>(center.y + eps);
		const BBox2Df search_box(search_min_x, search_min_y, search_max_x, search_max_y);

		const double eps_sq = eps * eps;

		// Stack-based depth first search through tree layers
		std::vector<size_t> stack;
		stack.reserve(64);

		// Root is at the end of upper layer
		const size_t root_pos = layer_bounds_.back() - 1;
		stack.push_back(root_pos);

		while (!stack.empty()) {
			const size_t node_pos = stack.back();
			stack.pop_back();

			if (!search_box.Intersects(boxes_[node_pos])) {
				continue;
			}

			if (node_pos < item_count_) {
				// Leaf node: perform exact double-precision Euclidean distance check
				const size_t orig_idx = indices_[node_pos];
				if (center.DistanceSquared(points_[orig_idx]) <= eps_sq) {
					matches.push_back(orig_idx);
				}
			} else {
				// Internal node: push children
				const size_t child_start = indices_[node_pos];
				const size_t child_end = std::min(child_start + node_size_, UpperBound(child_start));

				for (size_t c = child_start; c < child_end; ++c) {
					if (search_box.Intersects(boxes_[c])) {
						stack.push_back(c);
					}
				}
			}
		}
	}

	size_t Count() const override {
		return item_count_;
	}

private:
	void ComputeLayerBounds() {
		layer_bounds_.clear();
		uint32_t count = item_count_;
		uint32_t total = item_count_;
		layer_bounds_.push_back(total);

		while (count > 1) {
			count = (count + node_size_ - 1) / node_size_;
			total += count;
			layer_bounds_.push_back(total);
		}
	}

	size_t UpperBound(size_t node_idx) const {
		for (size_t bound : layer_bounds_) {
			if (node_idx < bound) {
				return bound;
			}
		}
		return layer_bounds_.back();
	}

	void QuickSort(std::vector<uint32_t> &curve, int64_t left, int64_t right) {
		if (left >= right) {
			return;
		}

		uint32_t pivot = curve[(left + right) / 2];
		int64_t i = left - 1;
		int64_t j = right + 1;

		while (true) {
			do { i++; } while (curve[i] < pivot);
			do { j--; } while (curve[j] > pivot);
			if (i >= j) break;

			std::swap(curve[i], curve[j]);
			std::swap(boxes_[i], boxes_[j]);
			std::swap(indices_[i], indices_[j]);
		}

		QuickSort(curve, left, j);
		QuickSort(curve, j + 1, right);
	}

	uint32_t node_size_;
	uint32_t item_count_;
	BBox2Df tree_box_;
	ArrayView<Point2D> points_;
	std::vector<uint32_t> layer_bounds_;
	std::vector<BBox2Df> boxes_;
	std::vector<uint32_t> indices_;
};

} // namespace spatial
} // namespace duckdb
