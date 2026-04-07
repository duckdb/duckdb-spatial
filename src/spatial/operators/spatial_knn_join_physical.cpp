#include "spatial/operators/spatial_knn_join_physical.hpp"
#include "spatial/operators/spatial_knn_join_logical.hpp"
#include "spatial/operators/flat_rtree.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/geometry/bbox.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/spatial_settings.hpp"
#include "spatial/spatial_types.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include "spatial/util/math.hpp"

namespace duckdb {

//======================================================================================================================
// Helpers
//======================================================================================================================

static unique_ptr<Expression> GetKNNBBOXExpression(ClientContext &context, const LogicalType &geom_type) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, DEFAULT_SCHEMA, "ST_Extent_Approx");
	auto func = entry.functions.GetFunctionByArguments(context, {geom_type});

	auto child_expr = make_uniq<BoundReferenceExpression>(geom_type, 0);
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child_expr));

	auto bbox_expr = make_uniq<BoundFunctionExpression>(GeoTypes::BOX_2DF(), func, std::move(children), nullptr);
	return std::move(bbox_expr);
}

//======================================================================================================================
// Constructor
//======================================================================================================================

PhysicalSpatialKNNJoin::PhysicalSpatialKNNJoin(PhysicalPlan &physical_plan, LogicalOperator &op,
                                               PhysicalOperator &left, PhysicalOperator &right,
                                               unique_ptr<Expression> condition_p, JoinType join_type,
                                               idx_t estimated_cardinality, int32_t k_p)
    : PhysicalJoin(physical_plan, op, PhysicalOperatorType::EXTENSION, join_type, estimated_cardinality),
      condition(std::move(condition_p)), k(k_p) {

	children.emplace_back(left);
	children.emplace_back(right);

	auto &func = condition->Cast<BoundFunctionExpression>();

	D_ASSERT(join_type == JoinType::INNER || join_type == JoinType::LEFT);

	// After optimizer child swap, arg0 always references children[0] (probe)
	// and arg1 always references children[1] (build/sink).
	probe_side_key = func.children[0].get();
	build_side_key = func.children[1].get();

	const auto &lop = op.Cast<LogicalSpatialKNNJoin>();

	// Probe-side output columns
	const auto &probe_side_input_types = children[0].get().types;
	probe_side_output_columns = lop.left_projection_map;
	if (probe_side_output_columns.empty()) {
		probe_side_output_columns.reserve(probe_side_input_types.size());
		for (idx_t i = 0; i < probe_side_input_types.size(); i++) {
			probe_side_output_columns.emplace_back(i);
		}
	}
	for (const auto &probe_col_idx : probe_side_output_columns) {
		probe_side_output_types.push_back(probe_side_input_types[probe_col_idx]);
	}

	// Build-side layout: key columns first, then payload columns
	unordered_map<idx_t, idx_t> conditions_in_layout;
	if (build_side_key->GetExpressionClass() == ExpressionClass::BOUND_REF) {
		conditions_in_layout.emplace(build_side_key->Cast<BoundReferenceExpression>().index, 0);
	}
	build_side_key_types.push_back(build_side_key->return_type);

	const auto &build_side_input_types = children[1].get().types;
	auto right_projection_map_copy = lop.right_projection_map;
	if (right_projection_map_copy.empty()) {
		right_projection_map_copy.reserve(build_side_input_types.size());
		for (idx_t i = 0; i < build_side_input_types.size(); i++) {
			right_projection_map_copy.emplace_back(i);
		}
	}

	for (auto &rhs_col : right_projection_map_copy) {
		auto &rhs_type = build_side_input_types[rhs_col];
		auto it = conditions_in_layout.find(rhs_col);
		if (it == conditions_in_layout.end()) {
			build_side_output_columns.push_back(build_side_key_types.size() + build_side_payload_types.size());
			build_side_payload_types.push_back(rhs_type);
			build_side_payload_columns.push_back(rhs_col);
		} else {
			build_side_output_columns.push_back(it->second);
		}
		build_side_output_types.push_back(rhs_type);
	}

	vector<LogicalType> layout_types;
	layout_types.insert(layout_types.end(), build_side_key_types.begin(), build_side_key_types.end());
	layout_types.insert(layout_types.end(), build_side_payload_types.begin(), build_side_payload_types.end());

	layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize(std::move(layout_types), TupleDataValidityType::CAN_HAVE_NULL_VALUES);
}

InsertionOrderPreservingMap<string> PhysicalSpatialKNNJoin::ParamsToString() const {
	auto result = PhysicalOperator::ParamsToString();
	result["Join Type"] = EnumUtil::ToString(join_type);
	result["Conditions"] = condition->GetName();
	result["K"] = to_string(k);
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

string PhysicalSpatialKNNJoin::GetName() const {
	return "SPATIAL_KNN_JOIN";
}

//======================================================================================================================
// Sink Interface (identical to PhysicalSpatialJoin — builds the FlatRTree)
//======================================================================================================================

// When the build side exceeds this threshold, partition into multiple R-trees
// so individual trees stay cache-friendly and can be evicted independently.
class KNNJoinGlobalSinkState final : public GlobalSinkState {
public:
	unique_ptr<TupleDataCollection> collection;
	idx_t total_rtree_size = 0;

	// Single R-tree for small datasets
	unique_ptr<FlatRTree> rtree = nullptr;

	// Partitioned R-trees for large datasets

	mutex combine_lock;
};

unique_ptr<GlobalSinkState> PhysicalSpatialKNNJoin::GetGlobalSinkState(ClientContext &context) const {
	auto gstate = make_uniq<KNNJoinGlobalSinkState>();
	gstate->collection =
	    make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(context), layout, MemoryTag::EXTENSION);
	return std::move(gstate);
}

class KNNJoinLocalSinkState final : public LocalSinkState {
public:
	KNNJoinLocalSinkState(const PhysicalSpatialKNNJoin &op, ClientContext &context,
	                      const shared_ptr<TupleDataLayout> &layout)
	    : build_side_key_executor(context), build_side_filter_executor(context) {
		collection =
		    make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(context), layout, MemoryTag::EXTENSION);
		collection->InitializeAppend(append_state, TupleDataPinProperties::UNPIN_AFTER_DONE);

		build_side_key_executor.AddExpression(*op.build_side_key);
		build_side_key_chunk.Initialize(context, op.build_side_key_types);
		build_side_row_chunk.InitializeEmpty(layout->GetTypes());
		build_side_payload_chunk.InitializeEmpty(op.build_side_payload_types);

		auto &geom_type = op.build_side_key->return_type;
		auto &catalog = Catalog::GetSystemCatalog(context);
		auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, DEFAULT_SCHEMA, "ST_IsEmpty");
		auto func = entry.functions.GetFunctionByArguments(context, {geom_type});

		auto is_empty_expr = make_uniq<BoundFunctionExpression>(LogicalTypeId::BOOLEAN, func,
		                                                        vector<unique_ptr<Expression>> {}, nullptr);
		is_empty_expr->children.push_back(make_uniq_base<Expression, BoundReferenceExpression>(geom_type, 0));

		auto is_not_empty_expr =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalTypeId::BOOLEAN);
		is_not_empty_expr->children.push_back(std::move(is_empty_expr));

		auto is_not_null_expr =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalTypeId::BOOLEAN);
		is_not_null_expr->children.push_back(make_uniq_base<Expression, BoundReferenceExpression>(geom_type, 0));

		auto filter_expr = make_uniq_base<Expression, BoundConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, std::move(is_not_empty_expr), std::move(is_not_null_expr));

		build_side_filter_expr = std::move(filter_expr);
		build_side_filter_executor.AddExpression(*build_side_filter_expr);
		build_side_filter_sel.Initialize(STANDARD_VECTOR_SIZE);
	}

	TupleDataAppendState append_state;
	unique_ptr<TupleDataCollection> collection;
	DataChunk build_side_key_chunk;
	DataChunk build_side_payload_chunk;
	DataChunk build_side_row_chunk;
	ExpressionExecutor build_side_key_executor;
	unique_ptr<Expression> build_side_filter_expr;
	ExpressionExecutor build_side_filter_executor;
	SelectionVector build_side_filter_sel;
	idx_t build_side_non_null_non_empty_count = 0;
};

unique_ptr<LocalSinkState> PhysicalSpatialKNNJoin::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<KNNJoinLocalSinkState>(*this, context.client, layout);
}

SinkResultType PhysicalSpatialKNNJoin::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<KNNJoinLocalSinkState>();

	lstate.build_side_key_chunk.Reset();
	lstate.build_side_key_executor.Execute(chunk, lstate.build_side_key_chunk);

	lstate.build_side_non_null_non_empty_count +=
	    lstate.build_side_filter_executor.SelectExpression(lstate.build_side_key_chunk, lstate.build_side_filter_sel);

	if (build_side_payload_types.empty()) {
		lstate.build_side_payload_chunk.SetCardinality(chunk.size());
	} else {
		lstate.build_side_payload_chunk.ReferenceColumns(chunk, build_side_payload_columns);
	}

	idx_t layout_col_idx = 0;
	for (auto &key_col : lstate.build_side_key_chunk.data) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(key_col);
	}
	for (auto &payload_col : lstate.build_side_payload_chunk.data) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(payload_col);
	}

	lstate.build_side_row_chunk.SetCardinality(chunk.size());
	lstate.collection->Append(lstate.append_state, lstate.build_side_row_chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalSpatialKNNJoin::Combine(ExecutionContext &context,
                                                      OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<KNNJoinGlobalSinkState>();
	auto &lstate = input.local_state.Cast<KNNJoinLocalSinkState>();

	lstate.collection->FinalizePinState(lstate.append_state.pin_state);

	lock_guard<mutex> lock(gstate.combine_lock);
	gstate.collection->Combine(*lstate.collection);
	gstate.total_rtree_size += lstate.build_side_non_null_non_empty_count;

	return SinkCombineResultType::FINISHED;
}

// Entry collected during Finalize for R-tree building
struct RTreeBuildEntry {
	Box2D<float> bbox;
	data_ptr_t row_ptr;
};

SinkFinalizeType PhysicalSpatialKNNJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<KNNJoinGlobalSinkState>();

	if (gstate.collection->Count() == 0) {
		return IsLeftOuterJoin(join_type) ? SinkFinalizeType::READY : SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	// Phase 1: collect all bounding boxes and row pointers
	vector<RTreeBuildEntry> all_entries;
	all_entries.reserve(gstate.total_rtree_size);

	TupleDataChunkIterator iterator(*gstate.collection, TupleDataPinProperties::KEEP_EVERYTHING_PINNED, true);

	const auto rows_ptr = iterator.GetRowLocations();
	Vector row_pointer_vector(LogicalType::POINTER, reinterpret_cast<data_ptr_t>(rows_ptr));

	auto &sel = *FlatVector::IncrementalSelectionVector();

	DataChunk geom_chunk;
	geom_chunk.Initialize(context, {build_side_key_types[0]});
	auto &geom_vec = geom_chunk.data[0];

	DataChunk bbox_chunk;
	bbox_chunk.Initialize(context, {GeoTypes::BOX_2DF()});
	auto bbox_expr = GetKNNBBOXExpression(context, build_side_key_types[0]);
	ExpressionExecutor bbox_executor(context);
	bbox_executor.AddExpression(*bbox_expr);

	do {
		const auto row_count = iterator.GetCurrentChunkCount();

		geom_chunk.Reset();
		bbox_chunk.Reset();
		geom_chunk.SetCardinality(row_count);
		bbox_chunk.SetCardinality(row_count);

		constexpr auto build_side_key_col = 0;
		D_ASSERT(build_side_key_types.size() == 1);

		gstate.collection->Gather(row_pointer_vector, sel, row_count, build_side_key_col, geom_vec, sel, nullptr);

		bbox_chunk.Flatten();
		bbox_executor.Execute(geom_chunk, bbox_chunk);
		const auto &entries = StructVector::GetEntries(bbox_chunk.data[0]);
		const auto xmin_data = FlatVector::GetData<float>(*entries[0]);
		const auto ymin_data = FlatVector::GetData<float>(*entries[1]);
		const auto xmax_data = FlatVector::GetData<float>(*entries[2]);
		const auto ymax_data = FlatVector::GetData<float>(*entries[3]);

		auto &validity = FlatVector::Validity(bbox_chunk.data[0]);

		for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
			if (!validity.RowIsValid(row_idx)) {
				continue;
			}

			Box2D<float> bbox;
			bbox.min.x = xmin_data[row_idx];
			bbox.min.y = ymin_data[row_idx];
			bbox.max.x = xmax_data[row_idx];
			bbox.max.y = ymax_data[row_idx];

			all_entries.push_back({bbox, rows_ptr[row_idx]});
		}
	} while (iterator.Next());

	// All geometries were NULL or empty — no valid entries to index
	if (all_entries.empty()) {
		return IsLeftOuterJoin(join_type) ? SinkFinalizeType::READY : SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	// Phase 2: build single in-memory R-tree
	static constexpr auto RTREE_NODE_SIZE = 32;
	auto &allocator = Allocator::Get(context);

	gstate.rtree = make_uniq<FlatRTree>(allocator, all_entries.size(), RTREE_NODE_SIZE);
	for (auto &entry : all_entries) {
		gstate.rtree->Push(entry.bbox, entry.row_ptr);
	}
	gstate.rtree->Build();

	return SinkFinalizeType::READY;
}

//======================================================================================================================
// Operator Interface — KNN Probe
//======================================================================================================================

enum class KNNJoinState { START = 0, INIT, PROBE, EMIT, EMIT_LHS };

// Candidate with exact distance for refinement
struct KNNCandidate {
	data_ptr_t row_ptr;
	double exact_distance;
};

class KNNJoinLocalOperatorState final : public CachingOperatorState {
public:
	idx_t input_index = 0;
	KNNJoinState state = KNNJoinState::START;

	FlatRTreeKNNState knn_state;
	idx_t emit_idx = 0;

	// Refined results after exact distance computation
	vector<KNNCandidate> refined_results;

	DataChunk probe_side_row_chunk;
	DataChunk probe_side_key_chunk;
	DataChunk probe_side_box_chunk;

	ExpressionExecutor join_probe_executor;
	ExpressionExecutor bbox_probe_executor;

	UnifiedVectorFormat probe_side_key_vformat;
	UnifiedVectorFormat probe_side_box_vformat;
	UnifiedVectorFormat probe_side_box_xmin_vformat;
	UnifiedVectorFormat probe_side_box_ymin_vformat;
	UnifiedVectorFormat probe_side_box_xmax_vformat;
	UnifiedVectorFormat probe_side_box_ymax_vformat;

	unique_ptr<Expression> bound_expr;

	// Geometry deserialization for exact distance
	ArenaAllocator arena;

	SelectionVector probe_side_source_sel;
	SelectionVector build_side_source_sel;
	SelectionVector build_side_target_sel;
	SelectionVector lhs_match_sel;

	uint8_t left_outer_marker[STANDARD_VECTOR_SIZE] = {};

	explicit KNNJoinLocalOperatorState(ClientContext &context)
	    : join_probe_executor(context), bbox_probe_executor(context),
	      arena(BufferAllocator::Get(context)),
	      probe_side_source_sel(STANDARD_VECTOR_SIZE),
	      build_side_source_sel(STANDARD_VECTOR_SIZE), build_side_target_sel(STANDARD_VECTOR_SIZE),
	      lhs_match_sel(STANDARD_VECTOR_SIZE) {
	}
};

class KNNJoinGlobalOperatorState final : public GlobalOperatorState {
public:
	unique_ptr<FlatRTree> rtree;
	unique_ptr<TupleDataCollection> collection;
};

unique_ptr<OperatorState> PhysicalSpatialKNNJoin::GetOperatorState(ExecutionContext &context) const {
	auto lstate = make_uniq<KNNJoinLocalOperatorState>(context.client);

	lstate->join_probe_executor.AddExpression(*probe_side_key);

	lstate->bound_expr = GetKNNBBOXExpression(context.client, probe_side_key->return_type);
	lstate->bbox_probe_executor.AddExpression(*lstate->bound_expr);

	lstate->probe_side_row_chunk.Initialize(context.client, probe_side_output_types);
	lstate->probe_side_key_chunk.Initialize(context.client, {probe_side_key->return_type});
	lstate->probe_side_box_chunk.Initialize(context.client, {lstate->bound_expr->return_type});

	return std::move(lstate);
}

unique_ptr<GlobalOperatorState> PhysicalSpatialKNNJoin::GetGlobalOperatorState(ClientContext &context) const {
	auto &gstate = sink_state->Cast<KNNJoinGlobalSinkState>();
	auto result = make_uniq<KNNJoinGlobalOperatorState>();
	result->rtree = std::move(gstate.rtree);
	result->collection = std::move(gstate.collection);
	return std::move(result);
}

OperatorResultType PhysicalSpatialKNNJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input,
                                                           DataChunk &chunk, GlobalOperatorState &gstate_p,
                                                           OperatorState &lstate_p) const {
	auto &gstate = gstate_p.Cast<KNNJoinGlobalOperatorState>();
	auto &lstate = lstate_p.Cast<KNNJoinLocalOperatorState>();

	idx_t output_index = 0;
	idx_t output_count = chunk.GetCapacity();
	const auto effective_k = static_cast<uint32_t>(k);

	while (true) {
		switch (lstate.state) {
		//--------------------------------------------------------------------------------------------------------------
		// START
		//--------------------------------------------------------------------------------------------------------------
		case KNNJoinState::START: {
			bool has_rtree = (gstate.rtree != nullptr && gstate.rtree->Count() > 0);
			if (!has_rtree) {
				if (IsLeftOuterJoin(join_type)) {
					lstate.probe_side_row_chunk.ReferenceColumns(input, probe_side_output_columns);
					PhysicalComparisonJoin::ConstructEmptyJoinResult(join_type, false, lstate.probe_side_row_chunk, chunk);
					return OperatorResultType::NEED_MORE_INPUT;
				}
				return OperatorResultType::FINISHED;
			}
			lstate.state = KNNJoinState::INIT;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// INIT — compute probe keys and bboxes for the input chunk
		//--------------------------------------------------------------------------------------------------------------
		case KNNJoinState::INIT: {
			lstate.join_probe_executor.Execute(input, lstate.probe_side_key_chunk);

			lstate.bbox_probe_executor.Execute(lstate.probe_side_key_chunk, lstate.probe_side_box_chunk);
			lstate.probe_side_box_chunk.data[0].ToUnifiedFormat(input.size(), lstate.probe_side_box_vformat);

			const auto &entries = StructVector::GetEntries(lstate.probe_side_box_chunk.data[0]);
			entries[0]->ToUnifiedFormat(input.size(), lstate.probe_side_box_xmin_vformat);
			entries[1]->ToUnifiedFormat(input.size(), lstate.probe_side_box_ymin_vformat);
			entries[2]->ToUnifiedFormat(input.size(), lstate.probe_side_box_xmax_vformat);
			entries[3]->ToUnifiedFormat(input.size(), lstate.probe_side_box_ymax_vformat);

			lstate.probe_side_row_chunk.ReferenceColumns(input, probe_side_output_columns);

			memset(lstate.left_outer_marker, 0, sizeof(lstate.left_outer_marker));
			lstate.input_index = 0;
			lstate.state = KNNJoinState::PROBE;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// PROBE — run KNN search for the current probe row
		//--------------------------------------------------------------------------------------------------------------
		case KNNJoinState::PROBE: {
			if (lstate.input_index == input.size()) {
				// Flush any accumulated output before requesting more input
				if (output_index > 0) {
					chunk.Slice(lstate.probe_side_row_chunk, lstate.probe_side_source_sel, output_index);
					chunk.SetCardinality(output_index);
				}

				if (IsLeftOuterJoin(join_type)) {
					lstate.state = KNNJoinState::EMIT_LHS;
					if (output_index > 0) {
						return OperatorResultType::HAVE_MORE_OUTPUT;
					}
					continue;
				}

				lstate.state = KNNJoinState::INIT;
				return OperatorResultType::NEED_MORE_INPUT;
			}

			const auto geom_idx = lstate.probe_side_box_vformat.sel->get_index(lstate.input_index);
			if (!lstate.probe_side_box_vformat.validity.RowIsValid(geom_idx)) {
				lstate.input_index++;
				continue;
			}

			// Extract bounding box for this probe row
			const auto xmin_data = UnifiedVectorFormat::GetData<float>(lstate.probe_side_box_xmin_vformat);
			const auto ymin_data = UnifiedVectorFormat::GetData<float>(lstate.probe_side_box_ymin_vformat);
			const auto xmax_data = UnifiedVectorFormat::GetData<float>(lstate.probe_side_box_xmax_vformat);
			const auto ymax_data = UnifiedVectorFormat::GetData<float>(lstate.probe_side_box_ymax_vformat);

			const auto xmin_idx = lstate.probe_side_box_xmin_vformat.sel->get_index(geom_idx);
			const auto ymin_idx = lstate.probe_side_box_ymin_vformat.sel->get_index(geom_idx);
			const auto xmax_idx = lstate.probe_side_box_xmax_vformat.sel->get_index(geom_idx);
			const auto ymax_idx = lstate.probe_side_box_ymax_vformat.sel->get_index(geom_idx);

			Box2D<float> bbox;
			bbox.min.x = xmin_data[xmin_idx];
			bbox.min.y = ymin_data[ymin_idx];
			bbox.max.x = xmax_data[xmax_idx];
			bbox.max.y = ymax_data[ymax_idx];

			// Over-fetch candidates for exact distance refinement
			// Spheroid needs more candidates because planar bbox distance is a worse lower bound
			const uint32_t overfetch_factor = 2;

			{
				// Single in-memory R-tree: direct KNN search
				const auto fetch_count = MinValue(effective_k * overfetch_factor, gstate.rtree->Count());
				gstate.rtree->KNNSearch(lstate.knn_state, bbox, fetch_count);
			}

			// Compute exact distances and re-rank.
			// Uses adaptive overfetch: if the k-th exact distance exceeds the
			// last fetched bbox distance, we may have missed candidates and retry.
			lstate.refined_results.clear();
			lstate.arena.Reset();

			// Get probe geometry for distance computation
			auto &probe_key_data = lstate.probe_side_key_chunk.data[0];
			probe_key_data.ToUnifiedFormat(input.size(), lstate.probe_side_key_vformat);
			const auto probe_key_idx = lstate.probe_side_key_vformat.sel->get_index(lstate.input_index);
			const auto &probe_blob = UnifiedVectorFormat::GetData<string_t>(lstate.probe_side_key_vformat)[probe_key_idx];

			sgl::prepared_geometry probe_geom;
			Serde::DeserializePrepared(probe_geom, lstate.arena, probe_blob.GetDataUnsafe(), probe_blob.GetSize());

			auto compute_exact_distance = [&](const sgl::prepared_geometry &p_geom,
			                                  const sgl::prepared_geometry &b_geom,
			                                  float bbox_dist_sq) -> double {
				// Fast path: if the bbox lower bound is already 0 (query point inside
				// the candidate's bbox), the exact distance is at most bbox_dist_sq and
				// commonly 0 for point-vs-point cases. Still compute exact for correctness.
				(void)bbox_dist_sq;
				double dist = 0.0;
				if (!sgl::ops::get_euclidean_distance(p_geom, b_geom, dist)) {
					// One side is empty — treat as infinitely far so it ranks last.
					return std::numeric_limits<double>::infinity();
				}
				return dist;
			};

			for (idx_t i = 0; i < lstate.knn_state.results.size(); i++) {
				auto row_ptr = lstate.knn_state.results[i];
				auto geom_ptr = row_ptr + layout->GetOffsets()[0];
				auto geom_blob = Load<string_t>(geom_ptr);

				sgl::prepared_geometry build_geom;
				Serde::DeserializePrepared(build_geom, lstate.arena, geom_blob.GetDataUnsafe(), geom_blob.GetSize());

				double dist = compute_exact_distance(probe_geom, build_geom, lstate.knn_state.result_distances_sq[i]);
				lstate.refined_results.push_back({row_ptr, dist});
			}

			// Sort by exact distance and truncate to k
			std::stable_sort(lstate.refined_results.begin(), lstate.refined_results.end(),
			          [](const KNNCandidate &a, const KNNCandidate &b) { return a.exact_distance < b.exact_distance; });

			// Adaptive overfetch check: if the k-th exact distance is larger than the
			// last bbox distance we fetched, a true nearest neighbor may have been missed.
			// Retry with a much larger fetch count.
			if (lstate.refined_results.size() >= effective_k && !lstate.knn_state.result_distances_sq.empty()) {
				auto kth_exact = lstate.refined_results[effective_k - 1].exact_distance;
				auto last_bbox = std::sqrt(static_cast<double>(lstate.knn_state.result_distances_sq.back()));
				if (kth_exact > last_bbox * 1.001) {
					// The k-th exact distance exceeds what we fetched from the R-tree.
					// Re-fetch with a much larger candidate set.
					auto expanded_fetch = MinValue(effective_k * 8u, gstate.rtree->Count());
					if (expanded_fetch > lstate.knn_state.results.size()) {
						lstate.refined_results.clear();
						lstate.arena.Reset();
						Serde::DeserializePrepared(probe_geom, lstate.arena, probe_blob.GetDataUnsafe(), probe_blob.GetSize());

						gstate.rtree->KNNSearch(lstate.knn_state, bbox, expanded_fetch);

						for (idx_t j = 0; j < lstate.knn_state.results.size(); j++) {
							auto rp = lstate.knn_state.results[j];
							auto gp = rp + layout->GetOffsets()[0];
							auto gb = Load<string_t>(gp);
							sgl::prepared_geometry bg;
							Serde::DeserializePrepared(bg, lstate.arena, gb.GetDataUnsafe(), gb.GetSize());
							double d = compute_exact_distance(probe_geom, bg, lstate.knn_state.result_distances_sq[j]);
							lstate.refined_results.push_back({rp, d});
						}

						std::stable_sort(lstate.refined_results.begin(), lstate.refined_results.end(),
						          [](const KNNCandidate &a, const KNNCandidate &b) {
							          return a.exact_distance < b.exact_distance;
						          });
					}
				}
			}

			if (lstate.refined_results.size() > effective_k) {
				lstate.refined_results.resize(effective_k);
			}

			lstate.emit_idx = 0;
			lstate.state = KNNJoinState::EMIT;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// EMIT — write KNN results to the output chunk
		//--------------------------------------------------------------------------------------------------------------
		case KNNJoinState::EMIT: {
			const auto result_count = static_cast<idx_t>(lstate.refined_results.size());
			const auto remaining = result_count - lstate.emit_idx;

			if (remaining == 0) {
				lstate.input_index++;
				lstate.state = KNNJoinState::PROBE;
				continue;
			}

			// Mark this probe row as having matches (for LEFT join)
			if (IsLeftOuterJoin(join_type)) {
				lstate.left_outer_marker[lstate.input_index] = 1;
			}

			const auto space = output_count - output_index;
			const auto batch = MinValue(remaining, space);

			// Set up row pointers for gathering build-side columns
			Vector row_pointers(LogicalType::POINTER);
			auto ptr_data = FlatVector::GetData<data_ptr_t>(row_pointers);
			for (idx_t i = 0; i < batch; i++) {
				ptr_data[i] = lstate.refined_results[lstate.emit_idx + i].row_ptr;
				lstate.probe_side_source_sel.set_index(output_index + i, lstate.input_index);
				lstate.build_side_source_sel.set_index(i, i);
				lstate.build_side_target_sel.set_index(i, output_index + i);
			}

			// Gather build-side output columns
			for (idx_t i = 0; i < build_side_output_columns.size(); i++) {
				auto &target = chunk.data[probe_side_output_columns.size() + i];
				const auto build_side_col_idx = build_side_output_columns[i];
				gstate.collection->Gather(row_pointers, lstate.build_side_source_sel, batch, build_side_col_idx,
				                          target, lstate.build_side_target_sel, nullptr);
			}

			output_index += batch;
			lstate.emit_idx += batch;

			if (output_index >= output_count) {
				// Output chunk is full
				chunk.Slice(lstate.probe_side_row_chunk, lstate.probe_side_source_sel, output_index);
				chunk.SetCardinality(output_index);
				return OperatorResultType::HAVE_MORE_OUTPUT;
			}

			// Still have space — check if we're done with this probe row
			if (lstate.emit_idx >= result_count) {
				lstate.input_index++;
				lstate.state = KNNJoinState::PROBE;
			}
			continue;
		}
		//--------------------------------------------------------------------------------------------------------------
		// EMIT LEFT OUTER — emit unmatched probe rows with NULL build columns
		//--------------------------------------------------------------------------------------------------------------
		case KNNJoinState::EMIT_LHS: {
			idx_t remaining_count = 0;
			for (idx_t i = 0; i < input.size(); i++) {
				if (!lstate.left_outer_marker[i]) {
					lstate.lhs_match_sel.set_index(remaining_count++, i);
				}
			}

			if (remaining_count > 0) {
				chunk.Slice(lstate.probe_side_row_chunk, lstate.lhs_match_sel, remaining_count);

				// Null the build-side columns
				for (idx_t i = 0; i < build_side_output_columns.size(); i++) {
					auto &target = chunk.data[probe_side_output_columns.size() + i];
					target.SetVectorType(VectorType::CONSTANT_VECTOR);
					ConstantVector::SetNull(target, true);
				}
			}

			lstate.state = KNNJoinState::INIT;
			return OperatorResultType::NEED_MORE_INPUT;
		}
		default:
			D_ASSERT(false);
			break;
		}
	}
}

//======================================================================================================================
// Progress
//======================================================================================================================

ProgressData PhysicalSpatialKNNJoin::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	return ProgressData();
}

} // namespace duckdb
