#include "spatial/modules/main/spatial_functions.hpp"
#include "spatial/geometry/bbox.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/index/flat_rtree.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/function_builder.hpp"
#include "spatial/util/math.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/window/window_boundaries_state.hpp"
#include "duckdb/function/window/window_collection.hpp"
#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_window_function_info.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace duckdb {

namespace {

//======================================================================================================================
// ST_ClusterDBSCAN
//======================================================================================================================
// DBSCAN over each window partition, using an R-Tree for the neighbour searches and a concurrent union-find to merge
// the core geometries into clusters.
//
// A hash group (which can contain many partitions) is clustered in the window FINALIZE stage:
//  - Each partition is read sequentially from the (buffer-managed) window collection into an in-memory R-Tree.
//    Points are stored inline in the tree, other geometries are deserialized into arenas owned by the partition, with
//    their polygon rings indexed to speed up the distance computations. Partitions that only contain points (which is
//    checked by first scanning the geometry headers) use a smaller, point-only item type.
//  - Small partitions are clustered by a single thread each. Large partitions are clustered by all threads together,
//    one partition at a time.
//  - All memory used to cluster a partition is released as soon as its rows have been labeled, so that only the
//    labels (one per row) are kept until the results have been emitted.
//
// Clusters are numbered in the order of their first core geometry, and border geometries are assigned to the cluster
// of their first core neighbour, so the result is deterministic for a given partition order.

// #define DUCKDB_SPATIAL_DEBUG_DBSCAN

struct ST_ClusterDBSCAN {

	static constexpr auto NAME = "ST_ClusterDBSCAN";

	//! The number of rows per task when clustering a large partition
	static constexpr idx_t MORSEL_SIZE = STANDARD_VECTOR_SIZE;
	//! Partitions with more rows than this are clustered by all threads together
	static constexpr idx_t PARALLEL_THRESHOLD = 8 * MORSEL_SIZE;

	using Box = Box2D<float>;

	//------------------------------------------------------------------------------------------------------------------
	// Items
	//------------------------------------------------------------------------------------------------------------------
	//! Compute the (conservative) R-Tree box of a geometry blob.
	//! Returns false if the geometry is empty or has non-finite coordinates, in which case it is not clustered.
	static bool TryGetBox(const string_t &blob, GeometryExtent &extent, Box &box) {
		extent = GeometryExtent::Empty();
		if (Geometry::GetExtent(blob, extent) == 0) {
			// Empty geometries are not within any distance of anything
			return false;
		}
		if (!std::isfinite(extent.x_min) || !std::isfinite(extent.y_min) || !std::isfinite(extent.x_max) ||
		    !std::isfinite(extent.y_max)) {
			// Neither are geometries with non-finite coordinates. These would also corrupt the R-Tree bounds.
			return false;
		}

		box.min.x = MathUtil::DoubleToFloatDown(extent.x_min);
		box.min.y = MathUtil::DoubleToFloatDown(extent.y_min);
		box.max.x = MathUtil::DoubleToFloatUp(extent.x_max);
		box.max.y = MathUtil::DoubleToFloatUp(extent.y_max);
		return true;
	}

	//! The maximum neighbour distance
	struct Eps {
		explicit Eps(const double value_p) : value(value_p), max_squared(GetMaxSquared(value_p)) {
		}

		//! Check if a distance, given as its square, is within eps. Equivalent to `std::sqrt(squared) <= value`.
		bool Contains(const double squared) const {
			return squared <= max_squared;
		}

		//! The distance
		const double value;
		//! The largest squared distance whose square root is within the distance
		const double max_squared;

	private:
		static double GetMaxSquared(const double eps) {
			if (!std::isfinite(eps)) {
				return eps;
			}
			// The square root is monotonic, so this is at most a few steps away from eps * eps
			auto result = eps * eps;
			while (std::sqrt(result) > eps) {
				result = std::nextafter(result, 0.0);
			}
			while (std::sqrt(std::nextafter(result, NumericLimits<double>::Maximum())) <= eps) {
				result = std::nextafter(result, NumericLimits<double>::Maximum());
			}
			return result;
		}
	};

	//! The squared distance between two points. Used for all point distances, so they are computed the same way.
	static double GetSquaredDistance(const double dx, const double dy) {
		return dx * dx + dy * dy;
	}

	//! The box to search for neighbours within eps distance of an item with the given (R-Tree) box
	static Box GetSearchBox(const Box &box, const Eps &eps) {
		Box search_box;
		search_box.min.x = MathUtil::DoubleToFloatDown(static_cast<double>(box.min.x) - eps.value);
		search_box.min.y = MathUtil::DoubleToFloatDown(static_cast<double>(box.min.y) - eps.value);
		search_box.max.x = MathUtil::DoubleToFloatUp(static_cast<double>(box.max.x) + eps.value);
		search_box.max.y = MathUtil::DoubleToFloatUp(static_cast<double>(box.max.y) + eps.value);
		return search_box;
	}

	//! A point to cluster, used when all the geometries in a partition are points
	struct PointItem {
		//! The R-Tree node size. Points are small and cheap to compare, so smaller nodes make the searches faster.
		static constexpr idx_t NODE_SIZE = 8;
		//! Whether checking the distance is cheaper than looking up the core flag and cluster of a neighbour
		static constexpr bool CHEAP_DISTANCE = true;

		//! The point coordinates. NaN if this is invalid (NULL or empty), and is not clustered.
		double x;
		double y;

		static PointItem Invalid() {
			return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
		}

		bool IsValid() const {
			return !std::isnan(x);
		}

		//! Load a (point) geometry blob into an item and compute its R-Tree box
		template <class GET_ARENA>
		static void Load(const string_t &blob, GET_ARENA &&get_arena, PointItem &item, Box &box) {
			auto extent = GeometryExtent::Empty();
			if (TryGetBox(blob, extent, box)) {
				item = {extent.x_min, extent.y_min};
			}
		}

		//! Check if two points are within eps distance
		static bool IsWithinDistance(const PointItem &lhs, const PointItem &rhs, const Eps &eps) {
			return eps.Contains(GetSquaredDistance(lhs.x - rhs.x, lhs.y - rhs.y));
		}

		//! Check if a point is within eps distance of another point, given its R-Tree box. The box contains the point,
		//! and all the operations are monotonic, so if the whole box is within (or not within) eps, then so is the
		//! point. Only if the box straddles the boundary, the other point itself has to be read.
		static bool IsWithinDistance(const PointItem &lhs, const Box &rhs_box, const PointItem &rhs, const Eps &eps) {
			double near_dx, far_dx, near_dy, far_dy;
			GetAxisDistances(lhs.x, rhs_box.min.x, rhs_box.max.x, near_dx, far_dx);
			GetAxisDistances(lhs.y, rhs_box.min.y, rhs_box.max.y, near_dy, far_dy);
			if (!eps.Contains(GetSquaredDistance(near_dx, near_dy))) {
				return false;
			}
			if (eps.Contains(GetSquaredDistance(far_dx, far_dy))) {
				return true;
			}
			return IsWithinDistance(lhs, rhs, eps);
		}

	private:
		//! The smallest and largest (absolute) distance between a coordinate and any coordinate in [lower, upper]
		static void GetAxisDistances(const double value, const float lower, const float upper, double &near_distance,
		                             double &far_distance) {
			const auto to_min = value - static_cast<double>(lower);
			const auto to_max = value - static_cast<double>(upper);
			far_distance = MaxValue(std::abs(to_min), std::abs(to_max));
			near_distance = (to_max <= 0 && to_min >= 0) ? 0 : MinValue(std::abs(to_min), std::abs(to_max));
		}
	};

	//! Any geometry to cluster
	struct GeometryItem {
		//! The R-Tree node size
		static constexpr idx_t NODE_SIZE = 32;
		//! Whether checking the distance is cheaper than looking up the core flag and cluster of a neighbour
		static constexpr bool CHEAP_DISTANCE = false;

		//! The deserialized geometry, or nullptr if this is a point (or invalid).
		//! Its polygon rings are indexed, which the distance functions make use of.
		const sgl::prepared_geometry *geom;
		//! The point coordinates, if this is a point. NaN if this is invalid (NULL or empty), and is not clustered.
		double x;
		double y;

		static GeometryItem Invalid() {
			return {nullptr, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
		}

		bool IsValid() const {
			return geom || !std::isnan(x);
		}

		//! Load a geometry blob into an item and compute its R-Tree box.
		//! Non-point geometries are copied and deserialized into the arena of the loading thread, since the blob does
		//! not outlive the scan.
		template <class GET_ARENA>
		static void Load(const string_t &blob, GET_ARENA &&get_arena, GeometryItem &item, Box &box) {
			auto extent = GeometryExtent::Empty();
			if (!TryGetBox(blob, extent, box)) {
				return;
			}

			if (Geometry::GetType(blob).first == GeometryType::POINT) {
				item.x = extent.x_min;
				item.y = extent.y_min;
				return;
			}

			ArenaAllocator &arena = get_arena();
			const auto blob_size = blob.GetSize();
			const auto blob_copy = arena.Allocate(blob_size);
			memcpy(blob_copy, blob.GetData(), blob_size);

			const auto geom = new (arena.AllocateAligned(sizeof(sgl::prepared_geometry))) sgl::prepared_geometry();
			Serde::DeserializePrepared(*geom, arena, const_char_ptr_cast(blob_copy), blob_size);

			item.geom = geom;
		}

		//! Check if a geometry is within eps distance of another geometry (with the given R-Tree box)
		static bool IsWithinDistance(const GeometryItem &lhs, const Box &rhs_box, const GeometryItem &rhs,
		                             const Eps &eps) {
			if (!lhs.geom && !rhs.geom) {
				return PointItem::IsWithinDistance({lhs.x, lhs.y}, rhs_box, {rhs.x, rhs.y}, eps);
			}

			// Wrap the points (if any) in geometries
			const sgl::vertex_xy lhs_vertex = {lhs.x, lhs.y};
			const sgl::vertex_xy rhs_vertex = {rhs.x, rhs.y};
			sgl::geometry lhs_point(sgl::geometry_type::POINT, false, false);
			sgl::geometry rhs_point(sgl::geometry_type::POINT, false, false);
			lhs_point.set_vertex_array(&lhs_vertex, 1);
			rhs_point.set_vertex_array(&rhs_vertex, 1);

			const sgl::geometry *lhs_geom = lhs.geom ? lhs.geom : &lhs_point;
			const sgl::geometry *rhs_geom = rhs.geom ? rhs.geom : &rhs_point;

			double distance = 0;
			return sgl::ops::get_euclidean_distance(*lhs_geom, *rhs_geom, distance) && distance <= eps.value;
		}
	};

	//------------------------------------------------------------------------------------------------------------------
	// Scanning
	//------------------------------------------------------------------------------------------------------------------
	//! Call `callback(row, blob)` for the hash group rows [beg_row, end_row) of the window collection, where blob is
	//! nullptr for NULL geometries. Stops early if the callback returns false.
	template <class CALLBACK>
	static void ScanRows(WindowCursor &cursor, const idx_t beg_row, const idx_t end_row, CALLBACK &&callback) {
		for (idx_t row = beg_row; row < end_row;) {
			cursor.Seek(row);

			// Process the rest of the scanned chunk
			const auto chunk_beg = cursor.state.current_row_index;
			const auto chunk_end = MinValue(end_row, cursor.state.next_row_index);
			auto &geom_vec = cursor.chunk.data[0];
			const auto geom_data = FlatVector::GetData<string_t>(geom_vec);

			for (; row < chunk_end; row++) {
				const auto chunk_idx = row - chunk_beg;
				const auto blob = FlatVector::IsNull(geom_vec, chunk_idx) ? nullptr : &geom_data[chunk_idx];
				if (!callback(row, blob)) {
					return;
				}
			}
		}
	}

	//! Check if all the (non-NULL) geometries in the hash group rows [beg_row, end_row) are points
	static bool HasOnlyPoints(WindowCursor &cursor, const idx_t beg_row, const idx_t end_row) {
		auto only_points = true;
		ScanRows(cursor, beg_row, end_row, [&](const idx_t, const string_t *blob) {
			// This only reads the header of the geometry
			only_points = !blob || Geometry::GetType(*blob).first == GeometryType::POINT;
			return only_points;
		});
		return only_points;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Partition
	//------------------------------------------------------------------------------------------------------------------
	//! Everything needed to cluster a single partition. Destroyed as soon as the partition has been labeled.
	template <class ITEM>
	class Partition {
	public:
		Partition(Allocator &allocator_p, const idx_t begin_p, const idx_t end_p, const idx_t thread_count)
		    : allocator(allocator_p), begin(begin_p), end(end_p), tree(allocator_p, end_p - begin_p, ITEM::NODE_SIZE),
		      arenas(thread_count) {
			D_ASSERT(begin < end);
			parent_data = allocator.Allocate(Count() * sizeof(atomic<idx_t>));
			core_data = allocator.Allocate(Count() * sizeof(bool));
			parent = reinterpret_cast<atomic<idx_t> *>(parent_data.get());
			core = reinterpret_cast<bool *>(core_data.get());
		}

		//! The number of rows in the partition
		idx_t Count() const {
			return end - begin;
		}

		//! Load the partition rows [beg_idx, end_idx) into the R-Tree, using the arena of the given thread (if needed)
		void Load(WindowCursor &cursor, const idx_t beg_idx, const idx_t end_idx, const idx_t thread_idx) {
			const auto get_arena = [&]() -> ArenaAllocator & {
				return GetArena(thread_idx);
			};
			ScanRows(cursor, begin + beg_idx, begin + end_idx, [&](const idx_t row, const string_t *blob) {
				const auto i = row - begin;
				auto item = ITEM::Invalid();
				auto box = Box();
				if (blob) {
					ITEM::Load(*blob, get_arena, item, box);
				}
				tree.Set(i, box, item);
				new (&parent[i]) atomic<idx_t>(i);
				core[i] = false;
				return true;
			});
		}

		//! Build the R-Tree, once all the rows have been loaded
		void Build() {
			const auto count = Count();
			const auto scratch_size = FlatRTree<ITEM>::GetBuildScratchSize(count, ITEM::NODE_SIZE);
			auto scratch = scratch_size ? allocator.Allocate(scratch_size) : AllocatedData();
			tree.Build(count, scratch.get());
		}

		//! Find the core rows among the R-Tree leaves [beg_idx, end_idx)
		void FindCore(const Eps &eps, const idx_t min_points, FlatRTreeScanState &scan, const idx_t beg_idx,
		              const idx_t end_idx) {
			tree.ScanLeaves(beg_idx, end_idx, [&](const idx_t i, const Box &box, const ITEM &item) {
				if (!item.IsValid()) {
					return;
				}

				// Count the neighbours (including itself), until we have enough
				idx_t neighbours = 0;
				tree.InitScan(scan, GetSearchBox(box, eps));
				tree.Lookup(scan, [&](const idx_t slot, const Box &other_box, const ITEM &other) {
					if (slot == i || ITEM::IsWithinDistance(item, other_box, other, eps)) {
						neighbours++;
					}
					return neighbours >= min_points;
				});

				core[i] = neighbours >= min_points;
			});
		}

		//! Merge the core rows among the R-Tree leaves [beg_idx, end_idx) with their core neighbours, and attach the
		//! border rows to their first core neighbour
		void FindClusters(const Eps &eps, FlatRTreeScanState &scan, const idx_t beg_idx, const idx_t end_idx) {
			tree.ScanLeaves(beg_idx, end_idx, [&](const idx_t i, const Box &box, const ITEM &item) {
				if (!item.IsValid()) {
					return;
				}

				tree.InitScan(scan, GetSearchBox(box, eps));

				if (core[i]) {
					tree.Lookup(scan, [&](const idx_t slot, const Box &other_box, const ITEM &other) {
						// Every pair of core neighbours is found from both sides, so only look at the lower rows.
						// Do the cheapest checks first: skip the distance check if the rows are in the same cluster,
						// unless the distance check is the cheaper one.
						if (slot >= i || (ITEM::CHEAP_DISTANCE && !ITEM::IsWithinDistance(item, other_box, other, eps))) {
							return false;
						}
						if (!core[slot] || Find(i) == Find(slot)) {
							return false;
						}
						if (!ITEM::CHEAP_DISTANCE && !ITEM::IsWithinDistance(item, other_box, other, eps)) {
							return false;
						}
						Union(i, slot);
						return false;
					});
				} else {
					auto first_core = i;
					tree.Lookup(scan, [&](const idx_t slot, const Box &other_box, const ITEM &other) {
						if (first_core != i && slot >= first_core) {
							return false;
						}
						if (ITEM::CHEAP_DISTANCE && !ITEM::IsWithinDistance(item, other_box, other, eps)) {
							return false;
						}
						if (!core[slot]) {
							return false;
						}
						if (!ITEM::CHEAP_DISTANCE && !ITEM::IsWithinDistance(item, other_box, other, eps)) {
							return false;
						}
						first_core = slot;
						return false;
					});
					parent[i].store(first_core);
				}
			});
		}

		//! Assign the cluster labels of the partition rows (in the labels of the whole hash group)
		void Label(idx_t *hash_group_labels) const {
			const auto count = Count();
			const auto labels = hash_group_labels + begin;

			// Number the clusters in order of their root, which is their lowest core row
			idx_t next_label = 0;
			for (idx_t i = 0; i < count; i++) {
				if (core[i] && parent[i].load() == i) {
					labels[i] = next_label++;
				}
			}

			for (idx_t i = 0; i < count; i++) {
				if (core[i]) {
					labels[i] = labels[Find(i)];
					continue;
				}
				const auto first_core = parent[i].load();
				if (first_core != i) {
					// Border row
					labels[i] = labels[Find(first_core)];
				}
			}
		}

	private:
		//! Find the root of a cluster. Only valid for core rows.
		idx_t Find(idx_t row) const {
			// Parents always have a lower row number than their children, so the root is the lowest row in the
			// cluster. This also means that "halving" the path (pointing each row to its grandparent) is always safe.
			while (true) {
				auto row_parent = parent[row].load();
				if (row_parent == row) {
					return row;
				}
				const auto row_grandparent = parent[row_parent].load();
				if (row_parent != row_grandparent) {
					parent[row].compare_exchange_weak(row_parent, row_grandparent);
				}
				row = row_grandparent;
			}
		}

		//! Merge the clusters of two core rows
		void Union(idx_t lhs, idx_t rhs) const {
			while (true) {
				lhs = Find(lhs);
				rhs = Find(rhs);
				if (lhs == rhs) {
					return;
				}
				// Link the higher root under the lower one
				if (lhs < rhs) {
					std::swap(lhs, rhs);
				}
				auto expected = lhs;
				if (parent[lhs].compare_exchange_strong(expected, rhs)) {
					return;
				}
				// Someone else linked "lhs" in the meantime, try again
			}
		}

		//! Get the arena of a loading thread
		ArenaAllocator &GetArena(const idx_t thread_idx) {
			D_ASSERT(thread_idx < arenas.size());
			auto &arena = arenas[thread_idx];
			if (!arena) {
				arena = make_uniq<ArenaAllocator>(allocator);
			}
			return *arena;
		}

	private:
		Allocator &allocator;

		//! The rows of the partition within the hash group
		const idx_t begin;
		const idx_t end;

		//! The R-Tree, with one slot per row in the partition
		FlatRTree<ITEM> tree;

		//! The union-find forest over the core rows. For non-core rows, this holds the first core neighbour instead
		//! (or the row itself if there is none), which is never traversed by Find().
		AllocatedData parent_data;
		atomic<idx_t> *parent;

		//! Whether a row is a core row
		AllocatedData core_data;
		bool *core;

		//! The arenas holding deserialized geometries (if the items need them), one per loading thread
		vector<unique_ptr<ArenaAllocator>> arenas;
	};

	//------------------------------------------------------------------------------------------------------------------
	// Bind Data
	//------------------------------------------------------------------------------------------------------------------
	// There is no serialization for this, which is fine: the function is just bound again when deserializing.
	class BindData final : public FunctionData {
	public:
		BindData(const double eps_p, const idx_t min_points_p) : eps(eps_p), min_points(min_points_p) {
		}

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindData>(eps, min_points);
		}

		bool Equals(const FunctionData &other_p) const override {
			const auto &other = other_p.Cast<BindData>();
			return eps == other.eps && min_points == other.min_points;
		}

		//! The maximum neighbour distance
		const double eps;
		//! The minimum number of neighbours (including itself) for a geometry to be a core geometry
		const idx_t min_points;
	};

	//------------------------------------------------------------------------------------------------------------------
	// Global State
	//------------------------------------------------------------------------------------------------------------------
	enum class Stage : uint8_t { SMALL, SCAN, LOAD, BUILD, CORE, UNION, LABEL, DONE };

	struct Task {
		Stage stage = Stage::DONE;
		//! The small partition index (SMALL), the partition rows (SCAN, LOAD) or the tree leaves (CORE, UNION) of the
		//! morsel
		idx_t begin = 0;
		idx_t end = 0;
	};

	class GlobalState final : public WindowExecutorGlobalState {
	public:
		GlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
		            const ValidityMask &partition_mask, const ValidityMask &order_mask)
		    : WindowExecutorGlobalState(client, executor, payload_count, partition_mask, order_mask),
		      allocator(BufferAllocator::Get(client)), geom_idx(executor.child_idx[0]),
		      bind_data(executor.wexpr.BindInfo()->Cast<BindData>()) {

			if (payload_count > 0) {
				labels_data = allocator.Allocate(payload_count * sizeof(idx_t));
				labels = reinterpret_cast<idx_t *>(labels_data.get());
				std::fill_n(labels, payload_count, DConstants::INVALID_INDEX);
			}

			// Split the hash group into its partitions (the partition mask has been computed at this point)
			for (idx_t beg = 0; beg < payload_count;) {
				idx_t n = 1;
				const auto end = WindowBoundariesState::FindNextStart(partition_mask, beg + 1, payload_count, n);
				if (end - beg > PARALLEL_THRESHOLD) {
					large_partitions.emplace_back(beg, end);
				} else {
					small_partitions.emplace_back(beg, end);
				}
				beg = end;
			}

			// Start by clustering all the small partitions
			stage = Stage::SMALL;
			total_tasks = small_partitions.size();
		}

		//! Get the next task to execute. Returns false if there is none available right now.
		bool TryGetTask(Task &task) {
			lock_guard<mutex> guard(lock);
			while (stage != Stage::DONE) {
				if (next_task < total_tasks) {
					const auto task_idx = next_task++;
					task.stage = stage;
					if (stage == Stage::SMALL) {
						task.begin = task_idx;
						task.end = task_idx + 1;
					} else {
						task.begin = task_idx * MORSEL_SIZE;
						task.end = MinValue(task.begin + MORSEL_SIZE, GetLargePartitionCount());
					}
					return true;
				}
				if (completed_tasks < total_tasks) {
					// Wait for the other threads to finish this stage
					return false;
				}
				NextStage();
			}
			return false;
		}

		//! Stop handing out tasks (e.g. after an error)
		void Abort() {
			lock_guard<mutex> guard(lock);
			stage = Stage::DONE;
		}

	private:
		void NextStage() {
			next_task = 0;
			completed_tasks = 0;

#ifdef DUCKDB_SPATIAL_DEBUG_DBSCAN
			// TODO: Move this into proper profiling metrics later
			static constexpr const char *STAGE_NAMES[] = {"SMALL", "SCAN", "LOAD", "BUILD", "CORE", "UNION", "LABEL"};
			const auto now = std::chrono::steady_clock::now();
			if (stage != Stage::DONE) {
				Printer::PrintF("ST_ClusterDBSCAN %s: %.3fs\n", STAGE_NAMES[static_cast<uint8_t>(stage.load())],
				                std::chrono::duration<double>(now - stage_start).count());
			}
			stage_start = now;
#endif

			switch (stage.load()) {
			case Stage::LABEL:
				// Done with this large partition, release its memory
				large_point_partition.reset();
				large_geometry_partition.reset();
				large_partition_idx++;
				DUCKDB_EXPLICIT_FALLTHROUGH;
			case Stage::SMALL:
				if (large_partition_idx < large_partitions.size()) {
					// Start clustering the next large partition, by checking whether it only contains points
					has_non_points = false;
					stage = Stage::SCAN;
					total_tasks = GetMorselCount();
				} else {
					stage = Stage::DONE;
					total_tasks = 0;
				}
				break;
			case Stage::SCAN: {
				const auto &range = large_partitions[large_partition_idx];
				if (has_non_points) {
					large_geometry_partition = make_uniq<Partition<GeometryItem>>(allocator, range.first, range.second,
					                                                              thread_count.load());
				} else {
					large_point_partition =
					    make_uniq<Partition<PointItem>>(allocator, range.first, range.second, thread_count.load());
				}
				stage = Stage::LOAD;
				total_tasks = GetMorselCount();
			} break;
			case Stage::LOAD:
				stage = Stage::BUILD;
				total_tasks = 1;
				break;
			case Stage::BUILD:
				stage = Stage::CORE;
				total_tasks = GetMorselCount();
				break;
			case Stage::CORE:
				stage = Stage::UNION;
				total_tasks = GetMorselCount();
				break;
			case Stage::UNION:
				stage = Stage::LABEL;
				total_tasks = 1;
				break;
			case Stage::DONE:
				break;
			}
		}

		idx_t GetLargePartitionCount() const {
			const auto &range = large_partitions[large_partition_idx];
			return range.second - range.first;
		}

		idx_t GetMorselCount() const {
			return (GetLargePartitionCount() + MORSEL_SIZE - 1) / MORSEL_SIZE;
		}

	public:
		Allocator &allocator;

		//! The geometry column in the window collection
		const column_t geom_idx;
		//! The clustering parameters
		const BindData &bind_data;

		//! The cluster label of each row in the hash group (INVALID_INDEX for noise)
		AllocatedData labels_data;
		idx_t *labels = nullptr;

		//! The [begin, end) rows of the partitions in the hash group
		vector<pair<idx_t, idx_t>> small_partitions;
		vector<pair<idx_t, idx_t>> large_partitions;

		//! The number of threads working on this hash group (one per local state)
		mutable atomic<idx_t> thread_count {0};

		//! The current processing stage
		atomic<Stage> stage {Stage::DONE};
		//! The tasks of the current stage
		idx_t total_tasks = 0;
		idx_t next_task = 0;
		atomic<idx_t> completed_tasks {0};

		//! The large partition currently being clustered (either with points only, or with any geometries)
		idx_t large_partition_idx = 0;
		unique_ptr<Partition<PointItem>> large_point_partition;
		unique_ptr<Partition<GeometryItem>> large_geometry_partition;
		//! Whether the large partition contains geometries that are not points (determined in the SCAN stage)
		atomic<bool> has_non_points {false};

#ifdef DUCKDB_SPATIAL_DEBUG_DBSCAN
		std::chrono::steady_clock::time_point stage_start = std::chrono::steady_clock::now();
#endif

	private:
		mutex lock;
	};

	//------------------------------------------------------------------------------------------------------------------
	// Local State
	//------------------------------------------------------------------------------------------------------------------
	class LocalState final : public WindowExecutorLocalState {
	public:
		LocalState(ExecutionContext &context, const GlobalState &gstate)
		    : WindowExecutorLocalState(context, gstate), thread_idx(gstate.thread_count++) {
		}

		//! The index of this thread within the hash group
		const idx_t thread_idx;
		//! Reads the geometries from the window collection
		unique_ptr<WindowCursor> cursor;
		//! Searches the R-Tree
		FlatRTreeScanState scan;
	};

	//------------------------------------------------------------------------------------------------------------------
	// Clustering
	//------------------------------------------------------------------------------------------------------------------
	//! Cluster a whole (small) partition on this thread
	template <class ITEM>
	static void ClusterPartition(GlobalState &gstate, LocalState &lstate, const idx_t begin, const idx_t end) {
		const auto &bind_data = gstate.bind_data;
		const Eps eps(bind_data.eps);
		Partition<ITEM> partition(gstate.allocator, begin, end, 1);
		const auto count = partition.Count();
		partition.Load(*lstate.cursor, 0, count, 0);
		partition.Build();
		partition.FindCore(eps, bind_data.min_points, lstate.scan, 0, count);
		partition.FindClusters(eps, lstate.scan, 0, count);
		partition.Label(gstate.labels);
	}

	//! Execute a stage task of the large partition that is currently being clustered
	template <class ITEM>
	static void ExecuteStage(GlobalState &gstate, LocalState &lstate, Partition<ITEM> &partition, const Task &task) {
		const auto &bind_data = gstate.bind_data;
		const Eps eps(bind_data.eps);
		switch (task.stage) {
		case Stage::LOAD:
			partition.Load(*lstate.cursor, task.begin, task.end, lstate.thread_idx);
			break;
		case Stage::BUILD:
			partition.Build();
			break;
		case Stage::CORE:
			partition.FindCore(eps, bind_data.min_points, lstate.scan, task.begin, task.end);
			break;
		case Stage::UNION:
			partition.FindClusters(eps, lstate.scan, task.begin, task.end);
			break;
		case Stage::LABEL:
			partition.Label(gstate.labels);
			break;
		default:
			throw InternalException("%s: unexpected stage for a large partition", NAME);
		}
	}

	static void ExecuteTask(GlobalState &gstate, LocalState &lstate, const Task &task) {
		auto &cursor = *lstate.cursor;

		switch (task.stage) {
		case Stage::SMALL: {
			const auto &range = gstate.small_partitions[task.begin];
			if (HasOnlyPoints(cursor, range.first, range.second)) {
				ClusterPartition<PointItem>(gstate, lstate, range.first, range.second);
			} else {
				ClusterPartition<GeometryItem>(gstate, lstate, range.first, range.second);
			}
		} break;
		case Stage::SCAN: {
			if (gstate.has_non_points) {
				// Another thread already found a geometry that is not a point
				break;
			}
			const auto begin = gstate.large_partitions[gstate.large_partition_idx].first;
			if (!HasOnlyPoints(cursor, begin + task.begin, begin + task.end)) {
				gstate.has_non_points = true;
			}
		} break;
		case Stage::DONE:
			break;
		default:
			if (gstate.large_point_partition) {
				ExecuteStage(gstate, lstate, *gstate.large_point_partition, task);
			} else {
				ExecuteStage(gstate, lstate, *gstate.large_geometry_partition, task);
			}
			break;
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Window Function Callbacks
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<FunctionData> Bind(BindWindowFunctionInput &input) {
		// The parameters must be constant
		const auto eps = input.GetNonNullConstant(1).GetValue<double>();
		const auto min_points = input.GetNonNullConstant(2).GetValue<int64_t>();
		if (std::isnan(eps) || eps < 0) {
			throw BinderException("%s: eps must be a non-negative number, got %f", NAME, eps);
		}
		if (min_points < 0) {
			throw BinderException("%s: minpoints must be a non-negative number, got %lld", NAME, min_points);
		}
		return make_uniq<BindData>(eps, UnsafeNumericCast<idx_t>(min_points));
	}

	static void GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared) {
		// Only materialize the geometries, the (constant) parameters are in the bind data
		const auto &wexpr = executor.wexpr;
		executor.child_idx.emplace_back(shared.RegisterCollection(wexpr.GetChildren()[0], false));
	}

	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask) {
		return make_uniq<GlobalState>(client, executor, payload_count, partition_mask, order_mask);
	}

	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
		return make_uniq<LocalState>(context, gstate.Cast<GlobalState>());
	}

	static void Finalize(ExecutionContext &context, optional_ptr<WindowCollection> collection,
	                     OperatorSinkInput &sink) {
		auto &gstate = sink.global_state.Cast<GlobalState>();
		auto &lstate = sink.local_state.Cast<LocalState>();

		D_ASSERT(collection);
		lstate.cursor = make_uniq<WindowCursor>(*collection, gstate.geom_idx);

		// Every thread of the hash group calls this, and none may return before all the clustering is done
		try {
			Task task;
			while (gstate.stage.load() != Stage::DONE) {
				if (!gstate.TryGetTask(task)) {
					std::this_thread::yield();
					continue;
				}
				ExecuteTask(gstate, lstate, task);
				++gstate.completed_tasks;
			}
		} catch (...) {
			// Make sure the other threads stop waiting for us
			gstate.Abort();
			throw;
		}

		// Unpin the collection
		lstate.cursor.reset();
	}

	static void Evaluate(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                     idx_t row_idx, OperatorSinkInput &sink) {
		auto &gstate = sink.global_state.Cast<GlobalState>();
		const auto count = eval_chunk.size();

		auto writer = FlatVector::Writer<int64_t>(result, count);
		for (idx_t i = 0; i < count; i++) {
			const auto label = gstate.labels[row_idx + i];
			if (label == DConstants::INVALID_INDEX) {
				writer.WriteNull();
			} else {
				writer.WriteValue(UnsafeNumericCast<int64_t>(label));
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DOC_DESCRIPTION = R"(
		Window function that returns a cluster id for each input geometry, using the 2D DBSCAN (Density-Based Spatial Clustering of Applications with Noise) algorithm.

		A geometry is added to a cluster if it is either:
		- A "core" geometry, that is within `eps` distance of at least `minpoints` geometries in the partition (including itself).
		- A "border" geometry, that is within `eps` distance of a core geometry.

		Clusters are numbered from 0 within each partition, in the order of their first core geometry. A border geometry within `eps` distance of core geometries from multiple clusters is assigned to the cluster of its first such core geometry, so the result is deterministic for a given row order (e.g. when using `ORDER BY` in the `OVER` clause).

		Geometries that are not part of any cluster ("noise"), as well as `NULL` and empty geometries, are assigned `NULL`.

		Distances are planar, in the units of the input coordinates. `eps` and `minpoints` must be constant. The window frame is ignored, all geometries in the partition are clustered.
	)";

	static constexpr auto DOC_EXAMPLE = R"(
		SELECT id, ST_ClusterDBSCAN(geom, 1.5, 2) OVER () AS cluster
		FROM (VALUES
			(1, ST_Point(0, 0)),
			(2, ST_Point(1, 0)),
			(3, ST_Point(5, 5)),
			(4, ST_Point(5, 6)),
			(5, ST_Point(10, 10))
		) AS t(id, geom)
		ORDER BY id;
		-- 1	0
		-- 2	0
		-- 3	1
		-- 4	1
		-- 5	NULL
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		WindowFunction function(NAME, {}, LogicalType::BIGINT, ExpressionType::WINDOW_FUNCTION, Bind, nullptr,
		                        GetSharing, GetGlobal, GetLocal, nullptr, Finalize, Evaluate);

		function.GetSignature()
            .AddParameter("geom", LogicalType::GEOMETRY())
            .AddParameter("eps", LogicalType::DOUBLE)
            .AddParameter("minpoints", LogicalType::INTEGER)
            .SetReturnType(LogicalType::BIGINT);
    
		function.SetCanOrderBy(false);
		function.SetCanIgnoreNulls(false);
		function.SetFallible();

		CreateWindowFunctionInfo info(std::move(function));

		FunctionDescription description;
		description.parameter_names = {"geom", "eps", "minpoints"};
		description.parameter_types = {LogicalType::GEOMETRY(), LogicalType::DOUBLE, LogicalType::INTEGER};
		description.description = FunctionBuilder::RemoveIndentAndTrailingWhitespace(DOC_DESCRIPTION);
		description.examples.push_back(FunctionBuilder::RemoveIndentAndTrailingWhitespace(DOC_EXAMPLE));
		info.descriptions.push_back(std::move(description));

		loader.RegisterFunction(std::move(info));

		// The tags are not copied from the create info, so set them on the catalog entry instead
		auto &db = loader.GetDatabaseInstance();
		auto &catalog = Catalog::GetSystemCatalog(db);
		auto transaction = CatalogTransaction::GetSystemTransaction(db);
		auto &schema = catalog.GetSchema(transaction, Identifier::DefaultSchema());
		auto entry = schema.GetEntry(transaction, CatalogType::WINDOW_FUNCTION_ENTRY, Identifier(NAME));
		if (!entry) {
			throw InternalException("Window function \"%s\" not found after registering it", NAME);
		}
		auto &func_entry = entry->Cast<FunctionEntry>();
		func_entry.tags.insert("ext", "spatial");
		func_entry.tags.insert("category", "relation");
	}
};

} // namespace

//======================================================================================================================
// Register
//======================================================================================================================
void RegisterSpatialWindowFunctions(ExtensionLoader &loader) {
	ST_ClusterDBSCAN::Register(loader);
}

} // namespace duckdb
