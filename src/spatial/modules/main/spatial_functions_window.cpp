#include "spatial/geometry/spatial_index_interface.hpp"
#include "spatial/geometry/cluster_types.hpp"
#include "spatial/geometry/flat_rtree.hpp"
#include "spatial/geometry/dbscan_engine.hpp"
#include "spatial/modules/main/spatial_functions.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/function_builder.hpp"

#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

#include <atomic>

namespace duckdb {
namespace {

struct DBSCANWindowState {
	std::vector<int32_t> cluster_ids;
	std::vector<bool> is_null;
	mutable std::atomic<idx_t> next_row;

	DBSCANWindowState() : next_row(0) {
	}
};

struct ST_ClusterDBSCAN_Point2D {
	static idx_t StateSize(const AggregateFunction &) {
		return sizeof(DBSCANWindowState);
	}

	static void StateInitialize(const AggregateFunction &, data_ptr_t state) {
		new (state) DBSCANWindowState();
	}

	static void StateDestructor(Vector &state, AggregateInputData &, idx_t count) {
		UnifiedVectorFormat sdata;
		state.ToUnifiedFormat(count, sdata);
		auto states = UnifiedVectorFormat::GetData<DBSCANWindowState *>(sdata);
		for (idx_t i = 0; i < count; i++) {
			auto idx = sdata.sel->get_index(i);
			if (sdata.validity.RowIsValid(idx)) {
				states[idx]->~DBSCANWindowState();
			}
		}
	}

	static void WindowInit(AggregateInputData &, const WindowPartitionInput &partition, data_ptr_t g_state) {
		auto &wstate = *reinterpret_cast<DBSCANWindowState *>(g_state);
		const idx_t row_count = partition.count;
		wstate.cluster_ids.assign(row_count, -1);
		wstate.is_null.assign(row_count, false);
		wstate.next_row = 0;

		if (row_count == 0 || !partition.inputs || partition.column_ids.size() < 3) {
			return;
		}

		std::vector<spatial::Point2D> valid_points;
		std::vector<size_t> row_mapping; // maps valid_points index -> partition row index
		valid_points.reserve(row_count);
		row_mapping.reserve(row_count);

		double eps = 0.0;
		int64_t min_points = 1;
		bool params_read = false;

		idx_t current_row = 0;
		for (auto &chunk : partition.inputs->Chunks(partition.column_ids)) {
			const idx_t chunk_size = chunk.size();
			if (chunk_size == 0) {
				continue;
			}

			auto &pt_vec = chunk.data[0];
			auto &eps_vec = chunk.data[1];
			auto &min_pts_vec = chunk.data[2];

			if (!params_read) {
				UnifiedVectorFormat eps_format, min_pts_format;
				eps_vec.ToUnifiedFormat(chunk_size, eps_format);
				min_pts_vec.ToUnifiedFormat(chunk_size, min_pts_format);

				auto eps_idx = eps_format.sel->get_index(0);
				auto min_pts_idx = min_pts_format.sel->get_index(0);
				if (eps_format.validity.RowIsValid(eps_idx)) {
					eps = UnifiedVectorFormat::GetData<double>(eps_format)[eps_idx];
				}
				if (min_pts_format.validity.RowIsValid(min_pts_idx)) {
					min_points = UnifiedVectorFormat::GetData<int64_t>(min_pts_format)[min_pts_idx];
				}
				params_read = true;
			}

			UnifiedVectorFormat pt_format;
			pt_vec.ToUnifiedFormat(chunk_size, pt_format);

			auto &entries = StructVector::GetEntries(pt_vec);
			UnifiedVectorFormat x_format, y_format;
			entries[0]->ToUnifiedFormat(chunk_size, x_format);
			entries[1]->ToUnifiedFormat(chunk_size, y_format);

			auto x_data = UnifiedVectorFormat::GetData<double>(x_format);
			auto y_data = UnifiedVectorFormat::GetData<double>(y_format);

			for (idx_t i = 0; i < chunk_size; ++i) {
				const idx_t global_row = current_row + i;
				auto pt_idx = pt_format.sel->get_index(i);

				if (!pt_format.validity.RowIsValid(pt_idx)) {
					wstate.is_null[global_row] = true;
					continue;
				}

				auto x_idx = x_format.sel->get_index(pt_idx);
				auto y_idx = y_format.sel->get_index(pt_idx);

				if (!x_format.validity.RowIsValid(x_idx) || !y_format.validity.RowIsValid(y_idx)) {
					wstate.is_null[global_row] = true;
					continue;
				}

				valid_points.emplace_back(x_data[x_idx], y_data[y_idx]);
				row_mapping.push_back(global_row);
			}

			current_row += chunk_size;
		}

		if (valid_points.empty() || eps <= 0.0 || min_points <= 0) {
			return;
		}

		spatial::FlatRTree2D rtree(32);
		rtree.Build(spatial::ArrayView<spatial::Point2D>(valid_points));

		spatial::DBSCANParams params(eps, min_points);
		auto result = spatial::DBSCANEngine::Cluster2D(
		    spatial::ArrayView<spatial::Point2D>(valid_points), rtree, params);

		for (size_t i = 0; i < valid_points.size(); ++i) {
			const size_t orig_row = row_mapping[i];
			wstate.cluster_ids[orig_row] = result.GetClusterId(i);
		}
	}

	static void Window(AggregateInputData &, const WindowPartitionInput &,
	                   const_data_ptr_t g_state, data_ptr_t, const SubFrames &,
	                   Vector &result, idx_t rid) {
		auto &wstate = *reinterpret_cast<const DBSCANWindowState *>(g_state);
		idx_t global_row = wstate.next_row++;
		if (global_row >= wstate.cluster_ids.size() || wstate.is_null[global_row] || wstate.cluster_ids[global_row] < 0) {
			FlatVector::SetNull(result, rid, true);
		} else {
			FlatVector::GetData<int32_t>(result)[rid] = wstate.cluster_ids[global_row];
		}
	}
};

} // namespace

void RegisterSpatialWindowFunctions(ExtensionLoader &loader) {
	// Register ST_ClusterDBSCAN for POINT_2D
	AggregateFunction cluster_point2d(
	    "ST_ClusterDBSCAN",
	    {GeoTypes::POINT_2D(), LogicalType::DOUBLE, LogicalType::BIGINT},
	    LogicalType::INTEGER,
	    ST_ClusterDBSCAN_Point2D::StateSize,
	    ST_ClusterDBSCAN_Point2D::StateInitialize,
	    nullptr, // update (null for window-only aggregate)
	    nullptr, // combine
	    nullptr, // finalize
	    nullptr  // simple_update
	);

	cluster_point2d.destructor = ST_ClusterDBSCAN_Point2D::StateDestructor;
	cluster_point2d.window_init = ST_ClusterDBSCAN_Point2D::WindowInit;
	cluster_point2d.window = ST_ClusterDBSCAN_Point2D::Window;

	FunctionBuilder::RegisterAggregate(loader, "ST_ClusterDBSCAN", [&](AggregateFunctionBuilder &func) {
		func.SetFunction(cluster_point2d);
		func.SetDescription("Performs DBSCAN density-based clustering over 2D points using an inbuilt R-Tree.");
		func.SetExample("SELECT id, ST_ClusterDBSCAN(pt, 0.5, 5) OVER () AS cid FROM points;");
		func.CanThrowErrors();
		func.SetTag("ext", "spatial");
		func.SetTag("category", "clustering");
	});
}

} // namespace duckdb
