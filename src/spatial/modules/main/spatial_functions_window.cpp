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
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/stl_allocator.hpp"
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
// Clustering Window Functions
//======================================================================================================================
// Window functions that cluster all the geometries in each window partition, and return a cluster id for each row.
//
// A hash group (which can contain many partitions) is clustered in the window FINALIZE stage. Every thread of the hash
// group calls it, and none of them may return before the whole hash group has been clustered:
//  - The geometries are materialized into the (buffer-managed) window collection, and are read from there.
//  - Partitions are clustered in stages that are defined by the clustering algorithm. Small partitions are clustered by
//    a single thread each, which runs all the stages. Large partitions are clustered by all threads together, one
//    partition at a time, with the tasks of each stage executed concurrently.
//  - All memory used to cluster a partition is released as soon as its rows have been labeled, so that only the
//    labels (one per row) are kept until the results have been emitted.
//
// A clustering ALGORITHM provides:
//  - `NAME`, the `BindData` with the (constant) clustering parameters, and `Bind()`.
//  - The `ThreadState` that its tasks need for each thread.
//  - The `StagedPartition`, which clusters a partition in stages, with:
//    - `StagedPartition(allocator, bind_data, begin, end)`
//    - `bool NextStage(idx_t &task_count)`, which moves on to the next stage, and returns false once the partition has
//      been labeled. This is called by a single thread, once all the tasks of the previous stage have been executed.
//    - `void ExecuteTask(context, task_idx)`, which executes a task of the current stage (concurrently).
//    - `const char *GetStageName()`, for debugging.

// #define DUCKDB_SPATIAL_DEBUG_CLUSTERING

//! A vector whose memory is allocated (and tracked) by a DuckDB allocator. Used for everything that can be as large
//! as the number of rows, so that it counts towards the memory limit.
template <class T>
using TrackedVector = vector<T, true, stl_allocator<T>>;

//! Everything that a clustering task has access to
template <class BIND_DATA, class THREAD_STATE>
struct ClusteringContext {
	Allocator &allocator;
	//! The clustering parameters
	const BIND_DATA &bind_data;
	//! The cluster label of each row in the hash group (INVALID_INDEX for noise)
	idx_t *labels;
	//! Reads the geometries of the hash group
	WindowCursor &cursor;
	//! Memory of the thread executing the task, which is released once the partition has been clustered. Until then,
	//! anything allocated in it can be used by all threads.
	ArenaAllocator &arena;
	//! The algorithm specific state of the thread
	THREAD_STATE &thread_state;

	//! Call `callback(row, blob)` for the hash group rows [beg_row, end_row), where blob is nullptr for NULL
	//! geometries. Stops early if the callback returns false.
	template <class CALLBACK>
	void ScanGeometries(const idx_t beg_row, const idx_t end_row, CALLBACK &&callback) {
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
};

template <class ALGORITHM>
struct ClusteringWindowFunction {
	using BindData = typename ALGORITHM::BindData;
	using ThreadState = typename ALGORITHM::ThreadState;
	using StagedPartition = typename ALGORITHM::StagedPartition;
	using Context = ClusteringContext<BindData, ThreadState>;

	//! Partitions with more rows than this are clustered by all threads together
	static constexpr idx_t PARALLEL_THRESHOLD = 8 * STANDARD_VECTOR_SIZE;

	//------------------------------------------------------------------------------------------------------------------
	// Global State
	//------------------------------------------------------------------------------------------------------------------
	enum class Stage : uint8_t { SMALL, LARGE, DONE };

	struct Task {
		Stage stage = Stage::DONE;
		//! The small partition (SMALL), or the task of the current stage of the large partition (LARGE)
		idx_t index = 0;
	};

	class GlobalState final : public WindowExecutorGlobalState {
	public:
		GlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
		            const ValidityMask &partition_mask, const ValidityMask &order_mask)
		    : WindowExecutorGlobalState(client, executor, payload_count, partition_mask, order_mask),
		      allocator(BufferAllocator::Get(client)), geom_idx(executor.child_idx[0]),
		      bind_data(executor.wexpr.BindInfo()->Cast<BindData>()),
		      labels(payload_count, DConstants::INVALID_INDEX, allocator), small_partitions(allocator) {

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
					task.stage = stage;
					task.index = next_task++;
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

#ifdef DUCKDB_SPATIAL_DEBUG_CLUSTERING
			// TODO: Move this into proper profiling metrics later
			const auto now = std::chrono::steady_clock::now();
			if (stage != Stage::DONE) {
				const auto stage_name = stage == Stage::SMALL ? "SMALL" : large_partition->GetStageName();
				Printer::PrintF("%s %s: %.3fs\n", ALGORITHM::NAME, stage_name,
				                std::chrono::duration<double>(now - stage_start).count());
			}
			stage_start = now;
#endif

			idx_t task_count = 0;
			if (stage == Stage::LARGE) {
				if (large_partition->NextStage(task_count)) {
					total_tasks = task_count;
					return;
				}
				// Done with this large partition, release its memory
				large_partition.reset();
				for (auto &arena : arenas) {
					arena.get().FreeAll();
				}
				large_partition_idx++;
			}

			// Start clustering the next large partition (if any)
			for (; large_partition_idx < large_partitions.size(); large_partition_idx++) {
				const auto &range = large_partitions[large_partition_idx];
				large_partition = make_uniq<StagedPartition>(allocator, bind_data, range.first, range.second);
				if (large_partition->NextStage(task_count)) {
					stage = Stage::LARGE;
					total_tasks = task_count;
					return;
				}
				large_partition.reset();
			}

			stage = Stage::DONE;
			total_tasks = 0;
		}

	public:
		Allocator &allocator;

		//! The geometry column in the window collection
		const column_t geom_idx;
		//! The clustering parameters
		const BindData &bind_data;

		//! The cluster label of each row in the hash group (INVALID_INDEX for noise)
		TrackedVector<idx_t> labels;

		//! The [begin, end) rows of the partitions in the hash group. There can be as many small partitions as rows.
		TrackedVector<pair<idx_t, idx_t>> small_partitions;
		vector<pair<idx_t, idx_t>> large_partitions;

		//! The arenas of the threads working on this hash group (one per local state)
		mutable vector<reference<ArenaAllocator>> arenas;
		mutable mutex arenas_lock;

		//! The current processing stage
		atomic<Stage> stage {Stage::DONE};
		//! The tasks of the current stage
		idx_t total_tasks = 0;
		idx_t next_task = 0;
		atomic<idx_t> completed_tasks {0};

		//! The large partition that is currently being clustered
		idx_t large_partition_idx = 0;
		unique_ptr<StagedPartition> large_partition;

#ifdef DUCKDB_SPATIAL_DEBUG_CLUSTERING
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
		    : WindowExecutorLocalState(context, gstate), arena(BufferAllocator::Get(context.client)) {
			lock_guard<mutex> guard(gstate.arenas_lock);
			gstate.arenas.push_back(arena);
		}

		//! Reads the geometries from the window collection
		unique_ptr<WindowCursor> cursor;
		//! The memory of this thread for clustering a partition
		ArenaAllocator arena;
		//! The algorithm specific state of this thread
		ThreadState thread_state;
	};

	static void ExecuteTask(GlobalState &gstate, LocalState &lstate, const Task &task) {
		Context context {gstate.allocator, gstate.bind_data, gstate.labels.data(),
		                 *lstate.cursor,   lstate.arena,      lstate.thread_state};

		switch (task.stage) {
		case Stage::SMALL: {
			// Cluster the whole partition on this thread, by running all of its stages
			const auto &range = gstate.small_partitions[task.index];
			{
				StagedPartition partition(gstate.allocator, gstate.bind_data, range.first, range.second);
				idx_t task_count = 0;
				while (partition.NextStage(task_count)) {
					for (idx_t task_idx = 0; task_idx < task_count; task_idx++) {
						partition.ExecuteTask(context, task_idx);
					}
				}
			}
			// Release the memory of the partition
			lstate.arena.FreeAll();
		} break;
		case Stage::LARGE:
			gstate.large_partition->ExecuteTask(context, task.index);
			break;
		case Stage::DONE:
			break;
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Window Function Callbacks
	//------------------------------------------------------------------------------------------------------------------
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
				// The clustering can take a while, so check whether the query was interrupted between the tasks
				if (context.client.IsInterrupted()) {
					throw InterruptException();
				}
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
	// Function
	//------------------------------------------------------------------------------------------------------------------
	//! Get the window function, without its parameters
	static WindowFunction GetFunction() {
		WindowFunction function(ALGORITHM::NAME, {}, LogicalType::BIGINT, ExpressionType::WINDOW_FUNCTION,
		                        ALGORITHM::Bind, nullptr, GetSharing, GetGlobal, GetLocal, nullptr, Finalize, Evaluate);
		function.SetCanOrderBy(false);
		function.SetCanIgnoreNulls(false);
		function.SetFallible();
		return function;
	}
};

//======================================================================================================================
// ST_ClusterDBSCAN
//======================================================================================================================
// DBSCAN over each window partition, using an R-Tree for the neighbour searches and a concurrent union-find to merge
// the core geometries into clusters:
//  - Each partition is loaded into an in-memory R-Tree. Points are stored inline in the tree, other geometries are
//    deserialized into the arenas of the loading threads, with their polygon rings indexed to speed up the distance
//    computations. Partitions that only contain points (which is checked by first scanning the geometry headers) use
//    a smaller, point-only item type.
//  - The core geometries are found with a search for their neighbours, and then merged with their core neighbours,
//    while the other geometries are attached to their first core neighbour (if any).
//
// Clusters are numbered in the order of their first core geometry, and border geometries are assigned to the cluster
// of their first core neighbour, so the result is deterministic for a given partition order.

struct ST_ClusterDBSCAN {

	static constexpr auto NAME = "ST_ClusterDBSCAN";

	//! The number of rows (or R-Tree leaves) per task
	static constexpr idx_t MORSEL_SIZE = STANDARD_VECTOR_SIZE;

	using Box = Box2D<float>;

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

	//------------------------------------------------------------------------------------------------------------------
	// Thread State
	//------------------------------------------------------------------------------------------------------------------
	struct ThreadState {
		//! Searches the R-Tree
		FlatRTreeScanState scan;
	};

	using Context = ClusteringContext<BindData, ThreadState>;

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
		static void Load(const string_t &blob, ArenaAllocator &, PointItem &item, Box &box) {
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
		//! Non-point geometries are copied and deserialized into the arena, since the blob does not outlive the scan.
		static void Load(const string_t &blob, ArenaAllocator &arena, GeometryItem &item, Box &box) {
			auto extent = GeometryExtent::Empty();
			if (!TryGetBox(blob, extent, box)) {
				return;
			}

			if (Geometry::GetType(blob).first == GeometryType::POINT) {
				item.x = extent.x_min;
				item.y = extent.y_min;
				return;
			}

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

	//! Check if all the (non-NULL) geometries in the hash group rows [beg_row, end_row) are points
	static bool HasOnlyPoints(Context &context, const idx_t beg_row, const idx_t end_row) {
		auto only_points = true;
		context.ScanGeometries(beg_row, end_row, [&](const idx_t, const string_t *blob) {
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
		Partition(Allocator &allocator_p, const idx_t begin_p, const idx_t end_p)
		    : allocator(allocator_p), begin(begin_p), end(end_p), tree(allocator_p, end_p - begin_p, ITEM::NODE_SIZE),
		      parent(end_p - begin_p, allocator_p), core(end_p - begin_p, allocator_p) {
			D_ASSERT(begin < end);
		}

		//! The number of rows in the partition
		idx_t Count() const {
			return end - begin;
		}

		//! Load the partition rows [beg_idx, end_idx) into the R-Tree
		void Load(Context &context, const idx_t beg_idx, const idx_t end_idx) {
			context.ScanGeometries(begin + beg_idx, begin + end_idx, [&](const idx_t row, const string_t *blob) {
				const auto i = row - begin;
				auto item = ITEM::Invalid();
				auto box = Box();
				if (blob) {
					ITEM::Load(*blob, context.arena, item, box);
				}
				tree.Set(i, box, item);
				parent[i].store(i);
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
		void Label(idx_t *hash_group_labels) {
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
		idx_t Find(idx_t row) {
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
		void Union(idx_t lhs, idx_t rhs) {
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

	private:
		Allocator &allocator;

		//! The rows of the partition within the hash group
		const idx_t begin;
		const idx_t end;

		//! The R-Tree, with one slot per row in the partition
		FlatRTree<ITEM> tree;

		//! The union-find forest over the core rows. For non-core rows, this holds the first core neighbour instead
		//! (or the row itself if there is none), which is never traversed by Find().
		TrackedVector<atomic<idx_t>> parent;

		//! Whether a row is a core row (not a vector<bool>, since the rows are written concurrently)
		TrackedVector<uint8_t> core;
	};

	//------------------------------------------------------------------------------------------------------------------
	// Staged Partition
	//------------------------------------------------------------------------------------------------------------------
	//! A partition, clustered in stages. It is only known which kind of items to cluster after the SCAN stage, so this
	//! then creates the partition with the right items, and executes the other stages on it.
	class StagedPartition {
	public:
		StagedPartition(Allocator &allocator_p, const BindData &bind_data_p, const idx_t begin_p, const idx_t end_p)
		    : allocator(allocator_p), bind_data(bind_data_p), eps(bind_data_p.eps), begin(begin_p), end(end_p) {
		}

		bool NextStage(idx_t &task_count) {
			switch (stage) {
			case Stage::INIT:
				// Start by checking whether the partition only contains points
				stage = Stage::SCAN;
				task_count = GetMorselCount();
				return true;
			case Stage::SCAN:
				if (has_non_points) {
					geometry_partition = make_uniq<Partition<GeometryItem>>(allocator, begin, end);
				} else {
					point_partition = make_uniq<Partition<PointItem>>(allocator, begin, end);
				}
				stage = Stage::LOAD;
				task_count = GetMorselCount();
				return true;
			case Stage::LOAD:
				stage = Stage::BUILD;
				task_count = 1;
				return true;
			case Stage::BUILD:
				stage = Stage::CORE;
				task_count = GetMorselCount();
				return true;
			case Stage::CORE:
				stage = Stage::UNION;
				task_count = GetMorselCount();
				return true;
			case Stage::UNION:
				stage = Stage::LABEL;
				task_count = 1;
				return true;
			case Stage::LABEL:
			case Stage::DONE:
				stage = Stage::DONE;
				return false;
			}
			return false;
		}

		void ExecuteTask(Context &context, const idx_t task_idx) {
			const auto beg_idx = task_idx * MORSEL_SIZE;
			const auto end_idx = MinValue(beg_idx + MORSEL_SIZE, end - begin);

			if (stage == Stage::SCAN) {
				if (has_non_points) {
					// Another thread already found a geometry that is not a point
					return;
				}
				if (!HasOnlyPoints(context, begin + beg_idx, begin + end_idx)) {
					has_non_points = true;
				}
				return;
			}

			if (point_partition) {
				ExecuteTask(*point_partition, context, beg_idx, end_idx);
			} else {
				ExecuteTask(*geometry_partition, context, beg_idx, end_idx);
			}
		}

		const char *GetStageName() const {
			static constexpr const char *STAGE_NAMES[] = {"INIT", "SCAN",  "LOAD",  "BUILD",
			                                              "CORE", "UNION", "LABEL", "DONE"};
			return STAGE_NAMES[static_cast<uint8_t>(stage)];
		}

	private:
		enum class Stage : uint8_t { INIT, SCAN, LOAD, BUILD, CORE, UNION, LABEL, DONE };

		template <class ITEM>
		void ExecuteTask(Partition<ITEM> &partition, Context &context, const idx_t beg_idx, const idx_t end_idx) {
			switch (stage) {
			case Stage::LOAD:
				partition.Load(context, beg_idx, end_idx);
				break;
			case Stage::BUILD:
				partition.Build();
				break;
			case Stage::CORE:
				partition.FindCore(eps, bind_data.min_points, context.thread_state.scan, beg_idx, end_idx);
				break;
			case Stage::UNION:
				partition.FindClusters(eps, context.thread_state.scan, beg_idx, end_idx);
				break;
			case Stage::LABEL:
				partition.Label(context.labels);
				break;
			default:
				throw InternalException("%s: unexpected stage %s", NAME, GetStageName());
			}
		}

		idx_t GetMorselCount() const {
			return (end - begin + MORSEL_SIZE - 1) / MORSEL_SIZE;
		}

	private:
		Allocator &allocator;
		const BindData &bind_data;
		const Eps eps;

		//! The rows of the partition within the hash group
		const idx_t begin;
		const idx_t end;

		Stage stage = Stage::INIT;

		//! Whether the partition contains geometries that are not points (determined in the SCAN stage)
		atomic<bool> has_non_points {false};

		//! The partition, either with points only, or with any geometries
		unique_ptr<Partition<PointItem>> point_partition;
		unique_ptr<Partition<GeometryItem>> geometry_partition;
	};

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
		auto function = ClusteringWindowFunction<ST_ClusterDBSCAN>::GetFunction();
		function.GetSignature()
		    .AddParameter("geom", LogicalType::GEOMETRY())
		    .AddParameter("eps", LogicalType::DOUBLE)
		    .AddParameter("minpoints", LogicalType::INTEGER)
		    .SetReturnType(LogicalType::BIGINT);

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
		func_entry.tags.insert("category", "cluster");
	}
};

//======================================================================================================================
// ST_ClusterKMeans
//======================================================================================================================
// K-means over each window partition, with Lloyd's algorithm and k-means++ initialization:
//  - Each geometry is reduced to a weighted point: points are weighted by their M value (if any), and other geometries
//    are represented by their centroid.
//  - The initial cluster centers are chosen with k-means++, one at a time: each is a pseudo-random point, picked with a
//    probability proportional to its weight times its squared distance to the nearest center chosen so far.
//  - Then the points are assigned to their nearest center, and the centers are moved to the (weighted) mean of their
//    points, until no point changes its cluster anymore.
//
// The pseudo-random choices use a fixed seed, and all the sums are computed in the same order, regardless of how the
// tasks are scheduled. So the result is deterministic for a given partition order. Clusters are numbered in the order
// of their first point.

struct ST_ClusterKMeans {

	static constexpr auto NAME = "ST_ClusterKMeans";

	//! The number of rows per task when loading the points and choosing the initial centers
	static constexpr idx_t MORSEL_SIZE = STANDARD_VECTOR_SIZE;
	//! The minimum number of rows per cluster in a task that assigns the points to their nearest center. This keeps the
	//! memory for the partial sums of the clusters (per task) in proportion to the number of rows.
	static constexpr idx_t ASSIGN_ROWS_PER_CLUSTER = 64;
	//! The maximum number of iterations of assigning the points and moving the centers
	static constexpr idx_t MAX_ITERATIONS = 1000;
	//! The seed for choosing the initial centers
	static constexpr int64_t SEED = 42;

	//------------------------------------------------------------------------------------------------------------------
	// Bind Data
	//------------------------------------------------------------------------------------------------------------------
	// There is no serialization for this, which is fine: the function is just bound again when deserializing.
	class BindData final : public FunctionData {
	public:
		explicit BindData(const idx_t cluster_count_p) : cluster_count(cluster_count_p) {
		}

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindData>(cluster_count);
		}

		bool Equals(const FunctionData &other_p) const override {
			return cluster_count == other_p.Cast<BindData>().cluster_count;
		}

		//! The number of clusters
		const idx_t cluster_count;
	};

	static unique_ptr<FunctionData> Bind(BindWindowFunctionInput &input) {
		// The parameters must be constant
		const auto cluster_count = input.GetNonNullConstant(1).GetValue<int64_t>();
		if (cluster_count < 1) {
			throw BinderException("%s: k must be larger than 0, got %lld", NAME, cluster_count);
		}
		return make_uniq<BindData>(UnsafeNumericCast<idx_t>(cluster_count));
	}

	//------------------------------------------------------------------------------------------------------------------
	// Thread State
	//------------------------------------------------------------------------------------------------------------------
	struct ThreadState {};

	using Context = ClusteringContext<BindData, ThreadState>;

	//------------------------------------------------------------------------------------------------------------------
	// Points
	//------------------------------------------------------------------------------------------------------------------
	//! A weighted point to cluster. The weight is 0 if the row is not clustered (because it is NULL or empty).
	struct Point {
		double x;
		double y;
		double w;
	};

	//! A cluster center
	struct Center {
		double x;
		double y;
	};

	//! The (weighted) sum of the points in a cluster
	struct Sum {
		double x;
		double y;
		double w;
	};

	static double GetSquaredDistance(const Point &point, const Center &center) {
		const auto dx = point.x - center.x;
		const auto dy = point.y - center.y;
		return dx * dx + dy * dy;
	}

	//! Load the point to cluster for a geometry: the point itself (weighted by its M value, if any), or the centroid of
	//! other geometries. Returns false if the geometry is empty, or has non-finite coordinates.
	static bool TryLoadPoint(const string_t &blob, ArenaAllocator &arena, Point &point) {
		// Nothing else is kept in the arena, so it can be reused for every geometry
		arena.Reset();
		sgl::geometry geom;
		Serde::Deserialize(geom, arena, blob.GetData(), blob.GetSize());

		if (geom.get_type() == sgl::geometry_type::POINT) {
			if (geom.is_empty()) {
				return false;
			}
			const auto vertex = geom.get_vertex_xy(0);
			point = {vertex.x, vertex.y, 1.0};
			if (geom.has_m()) {
				// The M value comes after the X, Y and (optional) Z values
				memcpy(&point.w, geom.get_vertex_array() + (geom.has_z() ? 3 : 2) * sizeof(double), sizeof(double));
				if (!(point.w > 0)) {
					throw InvalidInputException("%s: the M value of a point is its weight, and must be larger than 0, "
					                            "got %f",
					                            NAME, point.w);
				}
			}
		} else {
			sgl::vertex_xyzm centroid = {0, 0, 0, 0};
			if (!sgl::ops::get_centroid(geom, centroid)) {
				return false;
			}
			point = {centroid.x, centroid.y, 1.0};
		}
		return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.w);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Partition
	//------------------------------------------------------------------------------------------------------------------
	//! A partition, clustered in stages
	class StagedPartition {
	public:
		StagedPartition(Allocator &allocator_p, const BindData &bind_data, const idx_t begin_p, const idx_t end_p)
		    : allocator(allocator_p), begin(begin_p), end(end_p), max_clusters(bind_data.cluster_count),
		      points(end_p - begin_p, allocator_p), clusters(end_p - begin_p, allocator_p), centers(allocator_p),
		      distances(allocator_p), rng(SEED), sums(allocator_p), totals(allocator_p) {
			D_ASSERT(begin < end);

			load_morsel_count = (Count() + MORSEL_SIZE - 1) / MORSEL_SIZE;
			morsel_counts.resize(load_morsel_count);
			morsel_masses.resize(load_morsel_count);
		}

		//! The number of rows in the partition
		idx_t Count() const {
			return end - begin;
		}

		bool NextStage(idx_t &task_count) {
			switch (stage) {
			case Stage::INIT:
				stage = Stage::LOAD;
				task_count = load_morsel_count;
				return true;
			case Stage::LOAD: {
				// There can not be more clusters than points
				idx_t point_count = 0;
				for (const auto count : morsel_counts) {
					point_count += count;
				}
				if (point_count == 0) {
					// Nothing to cluster, all rows are NULL (or empty)
					stage = Stage::DONE;
					return false;
				}
				max_clusters = MinValue(max_clusters, point_count);
				centers.reserve(max_clusters);
				if (max_clusters > 1) {
					distances.resize(Count());
				}
				// Pick the first center (with the masses being the weights of the points)
				stage = Stage::PICK;
				task_count = 1;
				return true;
			}
			case Stage::PICK:
				if (centers.size() < max_clusters) {
					stage = Stage::DISTANCES;
					task_count = load_morsel_count;
					return true;
				}
				// All the initial centers have been chosen, so the distances are no longer needed
				distances = TrackedVector<double>(allocator);
				StartAssigning();
				task_count = assign_morsel_count;
				return true;
			case Stage::DISTANCES:
				stage = Stage::PICK;
				task_count = 1;
				return true;
			case Stage::ASSIGN:
				stage = Stage::UPDATE;
				task_count = 1;
				return true;
			case Stage::UPDATE:
				if (has_changes && iteration < MAX_ITERATIONS) {
					stage = Stage::ASSIGN;
					task_count = assign_morsel_count;
					return true;
				}
				stage = Stage::LABEL;
				task_count = 1;
				return true;
			case Stage::LABEL:
			case Stage::DONE:
				stage = Stage::DONE;
				return false;
			}
			return false;
		}

		void ExecuteTask(Context &context, const idx_t task_idx) {
			switch (stage) {
			case Stage::LOAD:
				Load(context, task_idx);
				break;
			case Stage::PICK:
				Pick();
				break;
			case Stage::DISTANCES:
				UpdateDistances(task_idx);
				break;
			case Stage::ASSIGN:
				Assign(task_idx);
				break;
			case Stage::UPDATE:
				Update();
				break;
			case Stage::LABEL:
				Label(context.labels);
				break;
			default:
				throw InternalException("%s: unexpected stage %s", NAME, GetStageName());
			}
		}

		const char *GetStageName() const {
			static constexpr const char *STAGE_NAMES[] = {"INIT",   "LOAD",   "PICK",  "DISTANCES",
			                                              "ASSIGN", "UPDATE", "LABEL", "DONE"};
			return STAGE_NAMES[static_cast<uint8_t>(stage)];
		}

	private:
		enum class Stage : uint8_t { INIT, LOAD, PICK, DISTANCES, ASSIGN, UPDATE, LABEL, DONE };

		//! Load the points of a morsel
		void Load(Context &context, const idx_t morsel_idx) {
			const auto beg_idx = morsel_idx * MORSEL_SIZE;
			const auto end_idx = MinValue(beg_idx + MORSEL_SIZE, Count());

			idx_t count = 0;
			double mass = 0;
			context.ScanGeometries(begin + beg_idx, begin + end_idx, [&](const idx_t row, const string_t *blob) {
				const auto i = row - begin;
				auto &point = points[i];
				if (!blob || !TryLoadPoint(*blob, context.arena, point)) {
					point = {0, 0, 0};
				}
				clusters[i] = DConstants::INVALID_INDEX;
				if (point.w > 0) {
					count++;
					mass += point.w;
				}
				return true;
			});
			morsel_counts[morsel_idx] = count;
			morsel_masses[morsel_idx] = mass;
		}

		//! The mass of a point when choosing the next initial center
		double GetMass(const idx_t i) const {
			return centers.size() == 0 ? points[i].w : points[i].w * distances[i];
		}

		//! Choose the next initial center
		void Pick() {
			double total = 0;
			for (const auto mass : morsel_masses) {
				total += mass;
			}
			if (!(total > 0)) {
				// All the points coincide with the chosen centers, so there are no more distinct centers to choose
				max_clusters = centers.size();
				return;
			}

			// Find the morsel of the point to pick, and then the point within it
			const auto target = rng.NextRandom() * total;
			double cumulative = 0;
			auto morsel_idx = DConstants::INVALID_INDEX;
			auto reached = false;
			for (idx_t m = 0; m < load_morsel_count; m++) {
				if (!(morsel_masses[m] > 0)) {
					continue;
				}
				morsel_idx = m;
				if (cumulative + morsel_masses[m] > target) {
					reached = true;
					break;
				}
				cumulative += morsel_masses[m];
			}
			D_ASSERT(morsel_idx != DConstants::INVALID_INDEX);
			if (!reached) {
				// The target was not reached (due to rounding), so pick from the last morsel with any mass
				cumulative -= morsel_masses[morsel_idx];
			}

			const auto beg_idx = morsel_idx * MORSEL_SIZE;
			const auto end_idx = MinValue(beg_idx + MORSEL_SIZE, Count());
			auto picked = DConstants::INVALID_INDEX;
			for (idx_t i = beg_idx; i < end_idx; i++) {
				const auto mass = GetMass(i);
				if (!(mass > 0)) {
					continue;
				}
				picked = i;
				cumulative += mass;
				if (cumulative > target) {
					break;
				}
			}
			D_ASSERT(picked != DConstants::INVALID_INDEX);

			centers.push_back({points[picked].x, points[picked].y});
		}

		//! Update the squared distances of the points in a morsel to their nearest center, with the latest center
		void UpdateDistances(const idx_t morsel_idx) {
			const auto beg_idx = morsel_idx * MORSEL_SIZE;
			const auto end_idx = MinValue(beg_idx + MORSEL_SIZE, Count());
			const auto &center = centers[centers.size() - 1];

			double mass = 0;
			for (idx_t i = beg_idx; i < end_idx; i++) {
				const auto &point = points[i];
				if (!(point.w > 0)) {
					continue;
				}
				const auto distance = GetSquaredDistance(point, center);
				distances[i] = centers.size() == 1 ? distance : MinValue(distances[i], distance);
				mass += point.w * distances[i];
			}
			morsel_masses[morsel_idx] = mass;
		}

		void StartAssigning() {
			// Bound the number of tasks by the number of clusters, so the partial sums are bounded by the row count
			assign_morsel_size = MaxValue(MORSEL_SIZE, centers.size() * ASSIGN_ROWS_PER_CLUSTER);
			assign_morsel_count = (Count() + assign_morsel_size - 1) / assign_morsel_size;
			sums.resize(assign_morsel_count * centers.size());
			totals.resize(centers.size());
			morsel_changes.resize(assign_morsel_count);
			stage = Stage::ASSIGN;
		}

		//! Assign the points of a morsel to their nearest center, and sum up the points of each cluster
		void Assign(const idx_t morsel_idx) {
			const auto beg_idx = morsel_idx * assign_morsel_size;
			const auto end_idx = MinValue(beg_idx + assign_morsel_size, Count());
			const auto morsel_sums = sums.data() + morsel_idx * centers.size();
			std::fill_n(morsel_sums, centers.size(), Sum {0, 0, 0});

			idx_t changes = 0;
			for (idx_t i = beg_idx; i < end_idx; i++) {
				const auto &point = points[i];
				if (!(point.w > 0)) {
					continue;
				}

				// Find the nearest center (the first one, on ties)
				idx_t nearest = 0;
				auto nearest_distance = GetSquaredDistance(point, centers[0]);
				for (idx_t c = 1; c < centers.size(); c++) {
					const auto distance = GetSquaredDistance(point, centers[c]);
					if (distance < nearest_distance) {
						nearest = c;
						nearest_distance = distance;
					}
				}

				if (clusters[i] != nearest) {
					clusters[i] = nearest;
					changes++;
				}

				auto &sum = morsel_sums[nearest];
				sum.x += point.w * point.x;
				sum.y += point.w * point.y;
				sum.w += point.w;
			}
			morsel_changes[morsel_idx] = changes;
		}

		//! Move the centers to the (weighted) mean of their points
		void Update() {
			// Add up the partial sums in morsel order, so the result does not depend on the task scheduling
			std::fill(totals.begin(), totals.end(), Sum {0, 0, 0});
			for (idx_t m = 0; m < assign_morsel_count; m++) {
				const auto morsel_sums = sums.data() + m * centers.size();
				for (idx_t c = 0; c < centers.size(); c++) {
					totals[c].x += morsel_sums[c].x;
					totals[c].y += morsel_sums[c].y;
					totals[c].w += morsel_sums[c].w;
				}
			}
			for (idx_t c = 0; c < centers.size(); c++) {
				// Clusters without any points keep their center
				if (totals[c].w > 0) {
					centers[c] = {totals[c].x / totals[c].w, totals[c].y / totals[c].w};
				}
			}

			has_changes = false;
			for (const auto changes : morsel_changes) {
				has_changes = has_changes || changes > 0;
			}
			iteration++;
		}

		//! Assign the cluster labels of the partition rows (in the labels of the whole hash group), numbering the
		//! clusters in the order of their first point
		void Label(idx_t *hash_group_labels) {
			const auto labels = hash_group_labels + begin;

			// Reuse the partial sums for the cluster numbers
			auto numbers = reinterpret_cast<idx_t *>(totals.data());
			static_assert(sizeof(Sum) >= sizeof(idx_t), "the cluster numbers must fit in the partial sums");
			std::fill_n(numbers, centers.size(), DConstants::INVALID_INDEX);

			idx_t next_number = 0;
			for (idx_t i = 0; i < Count(); i++) {
				const auto cluster = clusters[i];
				if (cluster == DConstants::INVALID_INDEX) {
					continue;
				}
				if (numbers[cluster] == DConstants::INVALID_INDEX) {
					numbers[cluster] = next_number++;
				}
				labels[i] = numbers[cluster];
			}
		}

	private:
		Allocator &allocator;

		//! The rows of the partition within the hash group
		const idx_t begin;
		const idx_t end;

		//! The number of clusters to choose initial centers for, which is reduced if there are not enough (distinct) points
		idx_t max_clusters;

		Stage stage = Stage::INIT;

		//! The point of each row
		TrackedVector<Point> points;
		//! The cluster (center) of each row, or INVALID_INDEX if it is not clustered
		TrackedVector<idx_t> clusters;

		//! The number of points, and their mass, per (load) morsel
		idx_t load_morsel_count;
		vector<idx_t> morsel_counts;
		vector<double> morsel_masses;

		//! The cluster centers (as many as have been chosen so far)
		TrackedVector<Center> centers;

		//! The squared distance of each row to its nearest center, while choosing the initial centers
		TrackedVector<double> distances;
		//! Picks the initial centers
		RandomEngine rng;

		//! The partial sums of the clusters, per (assign) morsel, and their totals
		idx_t assign_morsel_size = 0;
		idx_t assign_morsel_count = 0;
		TrackedVector<Sum> sums;
		TrackedVector<Sum> totals;
		//! The number of points that changed their cluster, per (assign) morsel
		vector<idx_t> morsel_changes;
		bool has_changes = true;
		idx_t iteration = 0;
	};

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DOC_DESCRIPTION = R"(
		Window function that returns a cluster id for each input geometry, using the K-means algorithm to divide the geometries of the partition into `k` clusters.

		The geometries are clustered by their centroid. The M value of a point (if any) is used as its weight, and must be larger than 0.

		Clusters are numbered from 0 within each partition, in the order of their first geometry. There can be fewer clusters than requested, if there are fewer distinct geometries. `NULL` and empty geometries are assigned `NULL`.

		The initial cluster centers are chosen with k-means++, which is pseudo-random but uses a fixed seed, so the result is deterministic for a given row order (e.g. when using `ORDER BY` in the `OVER` clause).

		Distances are planar, in the units of the input coordinates. `k` must be constant. The window frame is ignored, all geometries in the partition are clustered.
	)";

	static constexpr auto DOC_EXAMPLE = R"(
		SELECT id, ST_ClusterKMeans(geom, 2) OVER () AS cluster
		FROM (VALUES
			(1, ST_Point(0, 0)),
			(2, ST_Point(1, 0)),
			(3, ST_Point(10, 10)),
			(4, ST_Point(11, 10))
		) AS t(id, geom)
		ORDER BY id;
		-- 1	0
		-- 2	0
		-- 3	1
		-- 4	1
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		auto function = ClusteringWindowFunction<ST_ClusterKMeans>::GetFunction();
		function.GetSignature()
		    .AddParameter("geom", LogicalType::GEOMETRY())
		    .AddParameter("k", LogicalType::INTEGER)
		    .SetReturnType(LogicalType::BIGINT);

		CreateWindowFunctionInfo info(std::move(function));

		FunctionDescription description;
		description.parameter_names = {"geom", "k"};
		description.parameter_types = {LogicalType::GEOMETRY(), LogicalType::INTEGER};
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
		func_entry.tags.insert("category", "cluster");
	}
};

} // namespace

//======================================================================================================================
// Register
//======================================================================================================================
void RegisterSpatialWindowFunctions(ExtensionLoader &loader) {
	ST_ClusterDBSCAN::Register(loader);
	ST_ClusterKMeans::Register(loader);
}

} // namespace duckdb
