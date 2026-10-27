#pragma once

#include "spatial/geometry/bbox.hpp"
#include "spatial/geometry/sgl.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//======================================================================================================================
// Flat RTree
//======================================================================================================================
// A static, packed R-Tree over 2D float boxes. Each leaf ("slot") carries a payload of type T.
//
// Usage:
//  1. Construct the tree with a capacity, either owning its memory or over caller-provided memory.
//  2. Fill slots [0, count) with Set(). Set() may be called concurrently, as long as each thread sets different slots.
//     Slots with an empty box (Box2D<float>()) are kept, but never match any search.
//  3. Call Build(count, scratch) to Hilbert-sort the leaves and pack the internal nodes.
//  4. Search with InitScan() + Lookup(), or iterate over the leaves in tree order with ScanLeaves().

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

class FlatRTreeScanState {
	template <class T>
	friend class FlatRTree;

public:
	bool IsExhausted() const {
		return exhausted;
	}

private:
	//! The internal nodes left to search, in breadth-first order. Reused across searches to avoid reallocating.
	vector<idx_t> search_queue;
	idx_t search_queue_head = 0;
	Box2D<float> search_box;
	idx_t entry_beg = 0;
	idx_t entry_pos = 0;
	bool exhausted = true;
};

template <class T>
class FlatRTree {
public:
	using Box = Box2D<float>;

	static_assert(alignof(T) <= alignof(idx_t), "FlatRTree payload alignment must not exceed alignof(idx_t)");

	//! The number of bytes of memory needed for a tree with the given capacity
	static idx_t GetMemorySize(const idx_t capacity, const idx_t node_size) {
		vector<idx_t> layer_bounds;
		ComputeLayerBounds(capacity, node_size, layer_bounds);
		const auto nodes = layer_bounds.back();
		return GetIndexArraySize(nodes) + GetValueArraySize(capacity) + nodes * sizeof(Box);
	}

	//! The number of bytes of scratch memory needed by Build() for the given item count (may be 0)
	static idx_t GetBuildScratchSize(const idx_t count, const idx_t node_size) {
		return count <= node_size ? 0 : count * sizeof(uint32_t);
	}

	//! Construct a tree that owns its memory
	FlatRTree(Allocator &allocator, const idx_t capacity_p, const idx_t node_size_p)
	    : capacity(capacity_p), node_size(node_size_p) {
		const auto size = GetMemorySize(capacity, node_size);
		if (size > 0) {
			owned_memory = allocator.Allocate(size);
		}
		Initialize(owned_memory.get());
	}

	//! Construct a tree over caller-provided memory of at least GetMemorySize(capacity, node_size) bytes.
	//! The memory must be aligned to alignof(idx_t), and must outlive the tree.
	FlatRTree(data_ptr_t memory, const idx_t capacity_p, const idx_t node_size_p)
	    : capacity(capacity_p), node_size(node_size_p) {
		Initialize(memory);
	}

	//! The number of items in the tree (only valid after Build())
	idx_t Count() const {
		return item_count;
	}

	//! The bounding box covering all items in the tree (only valid after Build())
	const Box &Bounds() const {
		return tree_box;
	}

	//! Set the box and payload of a leaf slot.
	void Set(const idx_t slot, const Box &box, const T &value) {
		D_ASSERT(slot < capacity);
		box_array[slot] = box;
		value_array[slot] = value;
	}

	//! Get the payload of a leaf slot
	const T &Get(const idx_t slot) const {
		D_ASSERT(slot < capacity);
		return value_array[slot];
	}

	//! Build the tree over slots [0, count). `scratch` must point to GetBuildScratchSize(count, node_size) bytes,
	//! aligned to alignof(uint32_t), and is only used for the duration of the call.
	void Build(const idx_t count, data_ptr_t scratch) {
		D_ASSERT(count <= capacity);

		item_count = count;
		ComputeLayerBounds(item_count, node_size, layer_bounds);

		tree_box = Box();
		for (idx_t i = 0; i < item_count; i++) {
			idx_array[i] = i;
			tree_box.Union(box_array[i]);
		}

		if (item_count == 0) {
			// Nothing to build, scans will be exhausted immediately
			return;
		}

		if (item_count <= node_size) {
			// A single root node
			idx_array[item_count] = 0;
			box_array[item_count] = tree_box;
			return;
		}

		// Generate hilbert curve values
		// TODO: Parallelize this with tasks when the number of items is large?
		const auto curve = reinterpret_cast<uint32_t *>(scratch);

		constexpr auto max_hilbert = static_cast<float>(NumericLimits<uint16_t>::Maximum());
		const auto tree_w = tree_box.max.x - tree_box.min.x;
		const auto tree_h = tree_box.max.y - tree_box.min.y;
		// Guard against degenerate trees, e.g. where all items have the same x or y
		const auto hw = tree_w > 0 ? max_hilbert / tree_w : 0.0f;
		const auto hh = tree_h > 0 ? max_hilbert / tree_h : 0.0f;

		for (idx_t i = 0; i < item_count; i++) {
			const auto &box = box_array[i];
			if (box.min.x > box.max.x || box.min.y > box.max.y) {
				// Empty box, sort these last so that they end up in their own nodes
				curve[i] = NumericLimits<uint32_t>::Maximum();
				continue;
			}

			const auto cx = (box.min.x + box.max.x) / 2 - tree_box.min.x;
			const auto cy = (box.min.y + box.max.y) / 2 - tree_box.min.y;
			const auto hx = static_cast<uint32_t>(MinValue(MaxValue(hw * cx, 0.0f), max_hilbert));
			const auto hy = static_cast<uint32_t>(MinValue(MaxValue(hh * cy, 0.0f), max_hilbert));

			curve[i] = sgl::math::hilbert_encode(16, hx, hy);
		}

		// Now, sort the indices based on their curve value
		Sort(curve, 0, item_count - 1);

		// Pack the internal nodes, layer by layer
		idx_t position = item_count;
		idx_t entry_idx = 0;

		for (idx_t layer_idx = 0; layer_idx + 1 < layer_bounds.size(); layer_idx++) {
			const auto entry_end = layer_bounds[layer_idx];

			while (entry_idx < entry_end) {
				const auto node_idx = entry_idx;
				auto node_box = Box();

				for (idx_t child_idx = 0; child_idx < node_size && entry_idx < entry_end; child_idx++, entry_idx++) {
					node_box.Union(box_array[entry_idx]);
				}

				// Add a new parent node
				idx_array[position] = node_idx;
				box_array[position] = node_box;
				position++;
			}
		}
	}

	//! Call `callback(slot, box, value)` for the leaves at positions [beg, end) in the tree (only valid after Build()).
	//! The leaves are sorted along the hilbert curve, so consecutive leaves are usually close to each other.
	template <class CALLBACK>
	void ScanLeaves(const idx_t beg, const idx_t end, CALLBACK &&callback) const {
		D_ASSERT(end <= item_count);
		for (idx_t pos = beg; pos < end; pos++) {
			const auto slot = idx_array[pos];
			callback(slot, box_array[pos], value_array[slot]);
		}
	}

	void InitScan(FlatRTreeScanState &state, const Box &box) const {
		state.search_queue.clear();
		state.search_queue_head = 0;
		state.search_box = box;

		if (item_count == 0) {
			state.exhausted = true;
			return;
		}

		// The root node is the last entry of the top layer.
		// Note that this may be less than the allocated node count when the tree was built below its capacity.
		state.entry_beg = layer_bounds.back() - 1;
		state.entry_pos = state.entry_beg;
		state.exhausted = false;
	}

	//! Call `callback(slot, box, value)` for each leaf whose box intersects the search box.
	//! If the callback returns true, the lookup yields, and can be resumed by calling Lookup() again.
	template <class CALLBACK>
	void Lookup(FlatRTreeScanState &state, CALLBACK &&callback) const {
		if (state.exhausted) {
			return;
		}

		while (true) {
			const auto entry_end = MinValue(state.entry_beg + node_size, UpperBound(state.entry_beg));

			while (state.entry_pos < entry_end) {
				if (!state.search_box.Intersects(box_array[state.entry_pos])) {
					state.entry_pos++;
					continue;
				}

				auto yield = false;

				if (state.entry_beg >= item_count) {
					// Internal node
					state.search_queue.push_back(idx_array[state.entry_pos]);
				} else {
					// Leaf node
					const auto slot = idx_array[state.entry_pos];
					yield = callback(slot, box_array[state.entry_pos], value_array[slot]);
				}

				state.entry_pos++;

				if (yield) {
					// Yield!, there might be more matches
					return;
				}
			}

			if (state.search_queue_head == state.search_queue.size()) {
				// There are no more nodes to search
				state.exhausted = true;
				return;
			}

			state.entry_beg = state.search_queue[state.search_queue_head++];
			state.entry_pos = state.entry_beg;
		}
	}

private:
	static idx_t GetIndexArraySize(const idx_t nodes) {
		return nodes * sizeof(idx_t);
	}

	static idx_t GetValueArraySize(const idx_t items) {
		return AlignValue<idx_t, sizeof(idx_t)>(items * sizeof(T));
	}

	//! Compute the cumulative per-layer node counts for the given item count
	static void ComputeLayerBounds(const idx_t item_count, const idx_t node_size, vector<idx_t> &layer_bounds) {
		D_ASSERT(node_size > 1);
		layer_bounds.clear();
		idx_t count = item_count;
		idx_t nodes = item_count;
		layer_bounds.push_back(nodes);
		if (item_count == 0) {
			return;
		}
		do {
			count = (count + node_size - 1) / node_size;
			nodes += count;
			layer_bounds.push_back(nodes);
		} while (count > 1);
	}

	void Initialize(data_ptr_t memory) {
		ComputeLayerBounds(capacity, node_size, layer_bounds);
		const auto nodes = layer_bounds.back();

		// Lay out the arrays in order of decreasing alignment
		auto ptr = memory;
		idx_array.set(reinterpret_cast<idx_t *>(ptr), nodes);
		ptr += GetIndexArraySize(nodes);
		value_array.set(reinterpret_cast<T *>(ptr), capacity);
		ptr += GetValueArraySize(capacity);
		box_array.set(reinterpret_cast<Box *>(ptr), nodes);
	}

	void Sort(uint32_t *curve, idx_t l_idx, idx_t r_idx) {
		while (l_idx < r_idx) {
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

				// Reorder the curve, boxes and indices
				std::swap(curve[pivot_l], curve[pivot_r]);
				std::swap(box_array[pivot_l], box_array[pivot_r]);
				std::swap(idx_array[pivot_l], idx_array[pivot_r]);
			}

			// Recurse into the smaller half, and loop on the larger one to bound the stack depth
			if (pivot_r - l_idx < r_idx - pivot_r) {
				Sort(curve, l_idx, pivot_r);
				l_idx = pivot_r + 1;
			} else {
				Sort(curve, pivot_r + 1, r_idx);
				r_idx = pivot_r;
			}
		}
	}

	idx_t UpperBound(const idx_t node_idx) const {
		const auto it = std::upper_bound(layer_bounds.begin(), layer_bounds.end(), node_idx);
		if (it == layer_bounds.end()) {
			return layer_bounds.back();
		}
		return *it;
	}

private:
	idx_t capacity = 0;
	idx_t node_size = 0;
	idx_t item_count = 0;

	vector<idx_t> layer_bounds;

	AllocatedData owned_memory;

	typed_view<idx_t> idx_array;
	typed_view<T> value_array;
	typed_view<Box> box_array;

	Box tree_box;
};

} // namespace duckdb
