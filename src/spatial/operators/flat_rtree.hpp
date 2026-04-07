#pragma once

#include "spatial/geometry/bbox.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/util/math.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/vector.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

namespace duckdb {

//======================================================================================================================
// Flat RTree
//======================================================================================================================

template <class T>
class typed_view {
public:
	size_t size() const {
		return len;
	}
	T *data() {
		return ptr;
	}
	const T *data() const {
		return ptr;
	}
	T &operator[](size_t idx) {
		return data()[idx];
	}
	const T &operator[](size_t idx) const {
		return data()[idx];
	}

	void set(T *ptr_p, const size_t len_p) {
		ptr = ptr_p;
		len = len_p;
	}

private:
	T *ptr = nullptr;
	size_t len = 0;
};

//----------------------------------------------------------------------------------------------------------------------
// Intersection scan state
//----------------------------------------------------------------------------------------------------------------------
class FlatRTree;

class FlatRTreeScanState {
	friend class FlatRTree;
	using Box = Box2D<float>;

public:
	explicit FlatRTreeScanState() : matches(LogicalType::POINTER) {
	}

public:
	Vector matches;
	idx_t matches_count = 0;
	idx_t matches_idx = 0;

private:
	queue<size_t> search_queue;
	Box search_box;
	size_t entry_beg = 0;
	size_t entry_pos = 0;
	bool exhausted = true;
};

//----------------------------------------------------------------------------------------------------------------------
// KNN scan state
//----------------------------------------------------------------------------------------------------------------------

class FlatRTreeKNNState {
	friend class FlatRTree;
	using Box = Box2D<float>;

public:
	struct HeapEntry {
		float min_dist_sq;
		uint32_t node_idx;
		bool is_leaf;
		data_ptr_t row_ptr;

		bool operator>(const HeapEntry &other) const {
			return min_dist_sq > other.min_dist_sq;
		}
	};

	vector<data_ptr_t> results;
	vector<float> result_distances_sq;
	idx_t result_idx = 0;

private:
	std::priority_queue<HeapEntry, vector<HeapEntry>, std::greater<HeapEntry>> pq;
};

//----------------------------------------------------------------------------------------------------------------------
// FlatRTree
//----------------------------------------------------------------------------------------------------------------------

class FlatRTree {
public:
	using Box = Box2D<float>;

	FlatRTree(Allocator &alloc, uint32_t item_count_p, uint32_t node_size_p)
	    : item_count(item_count_p), node_size(node_size_p) {

		uint32_t count = item_count;
		uint32_t nodes = item_count;

		layer_bounds.push_back(nodes);

		if (item_count_p == 0) {
			return;
		}

		do {
			count = (count + node_size - 1) / node_size;
			nodes += count;
			layer_bounds.push_back(nodes);
		} while (count > 1);

		total_nodes = nodes;

		box_array_mem = alloc.Allocate(sizeof(Box) * nodes);
		idx_array_mem = alloc.Allocate(sizeof(uint32_t) * nodes);
		row_array_mem = alloc.Allocate(sizeof(data_ptr_t) * item_count);

		box_array.set(reinterpret_cast<Box *>(box_array_mem.get()), nodes);
		idx_array.set(reinterpret_cast<uint32_t *>(idx_array_mem.get()), nodes);
		row_array.set(reinterpret_cast<data_ptr_t *>(row_array_mem.get()), item_count);

		for (size_t i = 0; i < nodes; i++) {
			box_array[i] = Box();
			idx_array[i] = 0;
		}
		for (size_t i = 0; i < item_count; i++) {
			row_array[i] = nullptr;
		}
	}

	// Returns the bounding box of the entire R-tree.
	const Box &GetBounds() const {
		return tree_box;
	}

	// Returns the number of leaf items in the R-tree.
	uint32_t Count() const {
		return item_count;
	}

	uint32_t Push(const Box &box, data_ptr_t row) {
		idx_array[current_position] = current_position;
		box_array[current_position] = box;
		tree_box.Union(box);
		row_array[current_position] = row;
		return current_position++;
	}

	static void Sort(vector<uint32_t> &curve, typed_view<Box> &box_array, typed_view<uint32_t> &idx_array) {
		Sort(curve, box_array, idx_array, 0, curve.size() - 1);
	}

	static void Sort(vector<uint32_t> &curve, typed_view<Box> &box_array, typed_view<uint32_t> &idx_array, size_t l_idx,
	                 size_t r_idx) {
		if (l_idx < r_idx) {
			const auto pivot = curve[(l_idx + r_idx) >> 1];
			auto pivot_l = l_idx - 1;
			auto pivot_r = r_idx + 1;

			while (true) {
				do {
					++pivot_l;
				} while (curve[pivot_l] < pivot);
				do {
					--pivot_r;
				} while (curve[pivot_r] > pivot);

				if (pivot_l >= pivot_r) {
					break;
				}

				std::swap(curve[pivot_l], curve[pivot_r]);
				std::swap(box_array[pivot_l], box_array[pivot_r]);
				std::swap(idx_array[pivot_l], idx_array[pivot_r]);
			}

			Sort(curve, box_array, idx_array, l_idx, pivot_r);
			Sort(curve, box_array, idx_array, pivot_r + 1, r_idx);
		}
	}

	void Build() {
		D_ASSERT(item_count == current_position);

		if (item_count <= node_size) {
			box_array[current_position++] = tree_box;
			return;
		}

		constexpr auto max_hilbert = std::numeric_limits<uint16_t>::max();
		const auto hw = max_hilbert / (tree_box.max.x - tree_box.min.x);
		const auto hh = max_hilbert / (tree_box.max.y - tree_box.min.y);

		vector<uint32_t> curve(item_count);
		for (idx_t i = 0; i < item_count; i++) {
			const auto &node_box = box_array[i];

			const auto hx = static_cast<uint32_t>(hw * ((node_box.min.x + node_box.max.x) / 2 - tree_box.min.x));
			const auto hy = static_cast<uint32_t>(hh * ((node_box.min.y + node_box.max.y) / 2 - tree_box.min.y));

			curve[i] = sgl::math::hilbert_encode(16, hx, hy);
		}

		Sort(curve, box_array, idx_array);

		size_t layer_idx = 0;
		size_t entry_idx = 0;

		while (layer_idx < layer_bounds.size() - 1) {
			const auto entry_end = layer_bounds[layer_idx];

			while (entry_idx < entry_end) {
				auto node_idx = entry_idx;
				auto node_box = box_array[entry_idx];

				size_t child_idx = 0;
				while (child_idx < node_size && entry_idx < entry_end) {
					node_box.Union(box_array[entry_idx]);
					child_idx++;
					entry_idx++;
				}

				idx_array[current_position] = node_idx;
				box_array[current_position] = node_box;
				current_position++;
			}

			layer_idx++;
		}
	}

	size_t UpperBound(size_t node_idx) const {
		const auto it = std::upper_bound(layer_bounds.begin(), layer_bounds.end(), node_idx);
		if (it == layer_bounds.end()) {
			return layer_bounds.back();
		}
		return *it;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Intersection scan
	//------------------------------------------------------------------------------------------------------------------

	void InitScan(FlatRTreeScanState &state, const Box &box) const {
		while (!state.search_queue.empty()) {
			state.search_queue.pop();
		}
		state.search_box = box;
		state.entry_beg = box_array.size() - 1;
		state.entry_pos = state.entry_beg;

		state.exhausted = false;
		state.matches_idx = 0;
		state.matches_count = 0;
	}

	bool Scan(FlatRTreeScanState &state) const {
		if (state.exhausted) {
			return false;
		}

		idx_t count = 0;
		const auto ptr = FlatVector::GetData<data_ptr_t>(state.matches);
		Lookup(state, [&](const data_ptr_t &row) {
			ptr[count++] = row;
			return count == STANDARD_VECTOR_SIZE;
		});
		state.matches_count = count;
		state.matches_idx = 0;

		return count > 0;
	}

	template <class CALLBACK>
	void Lookup(FlatRTreeScanState &state, CALLBACK &&callback) const {

		while (true) {

			const auto entry_end = std::min(state.entry_beg + node_size, UpperBound(state.entry_beg));

			while (state.entry_pos < entry_end) {
				if (!state.search_box.Intersects(box_array[state.entry_pos])) {
					state.entry_pos++;
					continue;
				}

				auto yield = false;

				if (state.entry_beg >= item_count) {
					state.search_queue.push(idx_array[state.entry_pos]);
				} else {
					yield = callback(row_array[idx_array[state.entry_pos]]);
				}

				state.entry_pos++;

				if (yield) {
					return;
				}
			}

			if (state.search_queue.empty()) {
				state.exhausted = true;
				return;
			}

			state.entry_beg = state.search_queue.front();
			state.entry_pos = state.entry_beg;
			state.search_queue.pop();
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// KNN search (Hjaltason-Samet best-first traversal)
	//------------------------------------------------------------------------------------------------------------------

	// K-nearest neighbor search using Hjaltason-Samet priority queue traversal.
	void KNNSearch(FlatRTreeKNNState &state, const Box &query, uint32_t k) const {
		KNNSearchImpl(state, query, k, box_array, idx_array, row_array);
	}

private:
	// Core KNN implementation — takes explicit array views for thread safety.
	// Internal KNN search implementation.
	void KNNSearchImpl(FlatRTreeKNNState &state, const Box &query, uint32_t k,
	                   const typed_view<Box> &boxes, const typed_view<uint32_t> &indices,
	                   const typed_view<data_ptr_t> &rows) const {
		while (!state.pq.empty()) {
			state.pq.pop();
		}
		state.results.clear();
		state.result_distances_sq.clear();
		state.result_idx = 0;

		if (item_count == 0 || k == 0) {
			return;
		}

		auto root_idx = static_cast<uint32_t>(boxes.size() - 1);
		float root_dist = query.MinDistanceSquared(boxes[root_idx]);
		state.pq.push({root_dist, root_idx, false, nullptr});

		while (!state.pq.empty() && state.results.size() < k) {
			auto top = state.pq.top();
			state.pq.pop();

			if (top.is_leaf) {
				state.results.push_back(top.row_ptr);
				state.result_distances_sq.push_back(top.min_dist_sq);
				continue;
			}

			auto entry_beg = top.node_idx;
			auto entry_end = std::min(static_cast<size_t>(entry_beg) + node_size, UpperBound(entry_beg));

			for (size_t i = entry_beg; i < entry_end; i++) {
				float child_dist = query.MinDistanceSquared(boxes[i]);

				if (entry_beg >= item_count) {
					state.pq.push({child_dist, indices[i], false, nullptr});
				} else {
					state.pq.push({child_dist, indices[i], true, rows[indices[i]]});
				}
			}
		}
	}

private:
	vector<uint32_t> layer_bounds;

	AllocatedData box_array_mem;
	AllocatedData idx_array_mem;
	AllocatedData row_array_mem;

	// Buffer-managed storage (lazy-pin path)
	BufferHandle build_pin_box; // held during Push/Build, released after
	BufferHandle build_pin_idx;
	BufferHandle build_pin_row;

	typed_view<uint32_t> idx_array;
	typed_view<Box> box_array;
	typed_view<data_ptr_t> row_array;

	Box tree_box;

	uint32_t item_count = 0;
	uint32_t total_nodes = 0;
	uint32_t node_size = 0;
	uint32_t current_position = 0;
};

} // namespace duckdb
