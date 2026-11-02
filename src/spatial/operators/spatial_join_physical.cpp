#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "spatial/operators/spatial_join_physical.hpp"
#include "spatial/operators/spatial_join_logical.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/geometry/bbox.hpp"
#include "spatial/index/flat_rtree.hpp"
#include "spatial/spatial_types.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include "spatial/util/math.hpp"
#include "spatial/spatial_settings.hpp"

#ifdef SPATIAL_USE_GEOS
#include "spatial/modules/geos/geos_geometry.hpp"
#include "spatial/modules/geos/geos_serde.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/common/list.hpp"
#endif

namespace duckdb {

//======================================================================================================================
// RTree Scan
//======================================================================================================================

// #define DUCKDB_SPATIAL_DEBUG_JOIN

namespace {

using JoinRTree = FlatRTree<data_ptr_t>;

// Scans the build-side rtree, buffering up to STANDARD_VECTOR_SIZE matching row pointers at a time
class JoinRTreeScanState {
public:
	JoinRTreeScanState() : matches(LogicalType::POINTER) {
	}

	void Init(const JoinRTree &tree, const Box2D<float> &box) {
		tree.InitScan(state, box);
		matches_count = 0;
		matches_idx = 0;
	}

	bool Scan(const JoinRTree &tree) {
		if (state.IsExhausted()) {
			return false;
		}

		idx_t count = 0;
		const auto ptr = FlatVector::GetDataMutable<data_ptr_t>(matches);
		tree.Lookup(state, [&](const idx_t, const Box2D<float> &, const data_ptr_t &row) {
			ptr[count++] = row;
			return count == STANDARD_VECTOR_SIZE;
		});
		// Set the count of the result vector
		matches_count = count;
		matches_idx = 0;

		return count > 0;
	}

public:
	Vector matches;
	idx_t matches_count = 0;
	idx_t matches_idx = 0;

private:
	FlatRTreeScanState state;
};

} // namespace

//======================================================================================================================
// Helpers
//======================================================================================================================

static unique_ptr<Expression> GetBBOXExpression(ClientContext &context, const LogicalType &geom_type) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "ST_Extent_Approx"));
	const auto &func = *entry.functions.GetFunctionByArguments(context, {geom_type});

	auto child_expr = make_uniq<BoundReferenceExpression>(geom_type, 0);
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child_expr));

	auto bbox_expr = func.Bind(context, std::move(children));

	return std::move(bbox_expr);
}

// Build a constant GEOMETRY value (a rectangular polygon) covering the given box by constant-folding ST_MakeEnvelope.
// Used to construct the bounding-box filter pushed into the probe side.
static Value MakeEnvelopeValue(ClientContext &context, const Box2D<float> &box) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "ST_MakeEnvelope"));
	const auto &func = *entry.functions.GetFunctionByArguments(
	    context, {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE});

	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(box.min.x)));
	children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(box.min.y)));
	children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(box.max.x)));
	children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(box.max.y)));

	auto envelope_expr = func.Bind(context, std::move(children));

	return ExpressionExecutor::EvaluateScalar(context, *envelope_expr);
}

// Build an `ST_Intersects_Extent(<column>, <const envelope>)` expression filter for the build-side bounding box.
// The column is referenced as BoundReference index 0
// - (the convention for expression filters, which are evaluated against the single filtered column).
static unique_ptr<TableFilter> MakeBoundingBoxFilter(ClientContext &context, const Value &envelope,
                                                     const LogicalType &column_type) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "ST_Intersects_Extent"));
	const auto &func =
	    *entry.functions.GetFunctionByArguments(context, {LogicalType::GEOMETRY(), LogicalType::GEOMETRY()});

	// The column reference must carry the column's exact type (e.g. GEOMETRY with a CRS), so binding propagates it
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(column_type, 0));
	children.push_back(make_uniq<BoundConstantExpression>(envelope));

	auto predicate = func.Bind(context, std::move(children));

	// The filter is redundant with the join itself (every probe row is checked exactly against the R-tree),
	// so wrap it as optional, mirroring what the hash join does with its pushed min/max filters.
	// This makes the filter still participate in stats pruning and can feed a deferred R-tree scan its bounds,
	// but it skips the per-row evaluation, which would deserialize geometries per probe row just to pre-check a bbox.
	// (which the r-tree in the spatial join already does)
	return make_uniq<ExpressionFilter>(CreateOptionalFilterExpression(std::move(predicate), column_type));
}

//======================================================================================================================
// Physical Spatial Join Operator
//======================================================================================================================

PhysicalSpatialJoin::PhysicalSpatialJoin(PhysicalPlan &physical_plan, LogicalOperator &op, PhysicalOperator &left,
                                         PhysicalOperator &right, unique_ptr<Expression> condition_p,
                                         JoinType join_type, idx_t estimated_cardinality, bool has_const_distance,
                                         double const_distance,
                                         vector<SpatialJoinPushdownTarget> filter_pushdown_targets)
    : PhysicalJoin(physical_plan, op, PhysicalOperatorType::EXTENSION, join_type, estimated_cardinality),
      condition(std::move(condition_p)), has_const_distance(has_const_distance), const_distance(const_distance),
      filter_pushdown_targets(std::move(filter_pushdown_targets)) {

	children.emplace_back(left);
	children.emplace_back(right);

	// Disable operator caching: our output usually references large geometry blobs (zero-copy string_t's into the
	// build-side collection), and the caching wrapper would deep-copy them into its cache chunk. With low match rates
	// (= small output chunks) every chunk gets cached, accumulating gigabytes of copied blobs per thread.
	caching_supported = false;

	auto &func = condition->Cast<BoundFunctionExpression>();

	// Extract the probe side and build side join keys
	probe_side_key = func.GetChildren()[0].get();
	build_side_key = func.GetChildren()[1].get();

	// Only simple join types are supported
	D_ASSERT(join_type == JoinType::INNER || join_type == JoinType::LEFT || join_type == JoinType::OUTER ||
	         join_type == JoinType::RIGHT);

	// Always make sure we have a consistent order of the output columns, regardless if we have projection maps or not

	const auto &lop = op.Cast<LogicalSpatialJoin>();

	// Probe-side
	const auto &probe_side_input_types = children[0].get().types;
	probe_side_output_columns = FillProjectionMap(children[0].get(), lop.left_projection_map);
	for (const auto &probe_col_idx : probe_side_output_columns) {
		const auto type = probe_side_input_types[probe_col_idx];
		probe_side_output_types.push_back(type);
	}

	// The build side is slightly different.
	// Here we reorder the layout so that join keys are first, and the payload columns.
	// For right outer, the match column is added at the end

	unordered_map<idx_t, idx_t> conditions_in_layout;
	// TODO: Loop over multiple conds
	if (build_side_key->GetExpressionClass() == ExpressionClass::BOUND_REF) {
		conditions_in_layout.emplace(build_side_key->Cast<BoundReferenceExpression>().Index(), 0); // TODO: i, not 0
	}
	// TODO Add rest too
	build_side_key_types.push_back(build_side_key->GetReturnType());

	const auto &build_side_input_types = children[1].get().types;
	auto right_projection_map_copy = FillProjectionMap(children[1].get(), lop.right_projection_map);
	for (auto &rhs_col : right_projection_map_copy) {
		auto &rhs_type = build_side_input_types[rhs_col];

		auto it = conditions_in_layout.find(rhs_col);
		if (it == conditions_in_layout.end()) {
			// This column is not a condition, but we want to include it in the output.
			// And thus need to add it to the layout
			build_side_output_columns.push_back(build_side_key_types.size() + build_side_payload_types.size());
			build_side_payload_types.push_back(rhs_type);
			build_side_payload_columns.push_back(rhs_col);
		} else {
			// This condition is part of the layout already, so just project it out
			// (conditions are added to the layout separately below)
			build_side_output_columns.push_back(it->second);
		}
		build_side_output_types.push_back(rhs_type);
	}

	vector<LogicalType> layout_types;
	// Insert all condition types
	layout_types.insert(layout_types.end(), build_side_key_types.begin(), build_side_key_types.end());
	layout_types.insert(layout_types.end(), build_side_payload_types.begin(), build_side_payload_types.end());
	if (PropagatesBuildSide(join_type)) {
		// Add it to the ned
		layout_types.push_back(LogicalType::BOOLEAN);
	}

	// Initialize the layout
	// TODO: Align?
	layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize(std::move(layout_types), TupleDataValidityType::CAN_HAVE_NULL_VALUES);

	// For right/outer joins, this is where the build side match column goes
	if (PropagatesBuildSide(join_type)) {
		const auto &offsets = layout->GetOffsets();
		build_side_match_offset = offsets[build_side_key_types.size() + build_side_payload_types.size()];
	}
}

InsertionOrderPreservingMap<string> PhysicalSpatialJoin::ParamsToString() const {
	// TODO: Add condition to the result (GetName is wrong)
	auto result = PhysicalOperator::ParamsToString();
	result["Join Type"] = EnumUtil::ToString(join_type);
	result["Conditions"] = condition->GetName().GetIdentifierName();
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

string PhysicalSpatialJoin::GetName() const {
	return "SPATIAL_JOIN";
}

//----------------------------------------------------------------------------------------------------------------------
// Sink Interface
//----------------------------------------------------------------------------------------------------------------------
class SpatialJoinGlobalState final : public GlobalSinkState {
public:
	unique_ptr<TupleDataCollection> collection;

	// The number of non-null and non-empty geometries on the build side
	idx_t total_rtree_size = 0;

	// This is initialized in the finalize state
	unique_ptr<JoinRTree> rtree = nullptr;

	mutex combine_lock;
};

unique_ptr<GlobalSinkState> PhysicalSpatialJoin::GetGlobalSinkState(ClientContext &context) const {

	// Clear any filters previously pushed by this operator. The same physical operator can be executed multiple times
	// (e.g. in a recursive CTE), and the build-side bounding box differs each time. Stale filters must be cleared.
	for (auto &target : filter_pushdown_targets) {
		target.dynamic_filters->ClearFilters(*this);
	}

	auto gstate = make_uniq<SpatialJoinGlobalState>();
	gstate->collection =
	    make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(context), layout, MemoryTag::EXTENSION);

	return std::move(gstate);
}

class SpatialJoinLocalState final : public LocalSinkState {
public:
	SpatialJoinLocalState(const PhysicalSpatialJoin &op, ClientContext &context,
	                      const shared_ptr<TupleDataLayout> &layout)
	    : build_side_key_executor(context), build_side_filter_executor(context) {
		// Dont keep the tuples in memory after appending.
		collection =
		    make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(context), layout, MemoryTag::EXTENSION);
		collection->InitializeAppend(append_state, TupleDataPinProperties::UNPIN_AFTER_DONE);

		// TODO: Add other join condition expressions here
		build_side_key_executor.AddExpression(*op.build_side_key);
		build_side_key_chunk.Initialize(context, op.build_side_key_types);

		build_side_row_chunk.InitializeEmpty(layout->GetTypes());

		build_side_payload_chunk.InitializeEmpty(op.build_side_payload_types);

		auto &geom_type = op.build_side_key->GetReturnType();

		auto &catalog = Catalog::GetSystemCatalog(context);
		auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
		    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "ST_IsEmpty"));
		const auto &func = *entry.functions.GetFunctionByArguments(context, {geom_type});

		vector<unique_ptr<Expression>> children;
		children.push_back(make_uniq_base<Expression, BoundReferenceExpression>(geom_type, 0));
		auto is_empty_expr = func.Bind(context, std::move(children));

		auto is_not_empty_expr =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalTypeId::BOOLEAN);
		is_not_empty_expr->GetChildrenMutable().push_back(std::move(is_empty_expr));

		auto is_not_null_expr =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalTypeId::BOOLEAN);
		is_not_null_expr->GetChildrenMutable().push_back(
		    make_uniq_base<Expression, BoundReferenceExpression>(geom_type, 0));

		auto filter_expr = make_uniq_base<Expression, BoundConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, std::move(is_not_empty_expr), std::move(is_not_null_expr));

		build_side_filter_expr = std::move(filter_expr);
		build_side_filter_executor.AddExpression(*build_side_filter_expr);
		build_side_filter_sel.Initialize(STANDARD_VECTOR_SIZE);
	}

	TupleDataAppendState append_state;
	unique_ptr<TupleDataCollection> collection;

	// Owning DataChunk containing the build side join-key
	DataChunk build_side_key_chunk;
	DataChunk build_side_payload_chunk;
	// Referencing DataChunk chunk, having the same layout as the collection
	DataChunk build_side_row_chunk;
	// Used to execute the build side join key expression
	ExpressionExecutor build_side_key_executor;

	// Used to count how many non-null and non-empty geometries we have on the build side
	// (so we can initialize the rtree to the correct size)
	unique_ptr<Expression> build_side_filter_expr;
	ExpressionExecutor build_side_filter_executor;
	SelectionVector build_side_filter_sel; // not used, but needed for the executor

	idx_t build_side_non_null_non_empty_count = 0;
};

unique_ptr<LocalSinkState> PhysicalSpatialJoin::GetLocalSinkState(ExecutionContext &context) const {
	// auto &gstate = sink_state->Cast<SpatialJoinGlobalState>();
	auto lstate = make_uniq<SpatialJoinLocalState>(*this, context.client, layout);
	return std::move(lstate);
}

SinkResultType PhysicalSpatialJoin::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {

	auto &lstate = input.local_state.Cast<SpatialJoinLocalState>();

	lstate.build_side_key_chunk.Reset();
	lstate.build_side_key_executor.Execute(chunk, lstate.build_side_key_chunk);

	// Count how many non-null and non-empty geometries we have on the build side
	lstate.build_side_non_null_non_empty_count +=
	    lstate.build_side_filter_executor.SelectExpression(lstate.build_side_key_chunk, lstate.build_side_filter_sel);

	if (build_side_payload_types.empty()) {
		// There are only keys. Make the payload chunk empty
		lstate.build_side_payload_chunk.SetCardinality(chunk.size());
	} else {
		lstate.build_side_payload_chunk.ReferenceColumns(chunk, build_side_payload_columns);
	}

	// Now reference the key chunk, the payload chunk, and the build side match column (if needed)
	idx_t layout_col_idx = 0;
	for (auto &key_col : lstate.build_side_key_chunk.data) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(key_col);
	}

	for (auto &payload_col : lstate.build_side_payload_chunk.data) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(payload_col);
	}

	if (PropagatesBuildSide(join_type)) {
		lstate.build_side_row_chunk.data[layout_col_idx++].Reference(Value::BOOLEAN(false), count_t(chunk.size()));
	}

	// Set the cardinality to match the input
	lstate.build_side_row_chunk.SetCardinality(chunk.size());

	// Sink the build side chunk
	lstate.collection->Append(lstate.append_state, lstate.build_side_row_chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalSpatialJoin::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<SpatialJoinGlobalState>();
	auto &lstate = input.local_state.Cast<SpatialJoinLocalState>();

	// Flush the append state
	lstate.collection->FinalizePinState(lstate.append_state.pin_state);

	// Append the local collection to the global collection
	lock_guard<mutex> lock(gstate.combine_lock);
	gstate.collection->Combine(*lstate.collection);

	// Merge the non-null and non-empty count
	gstate.total_rtree_size += lstate.build_side_non_null_non_empty_count;

	return SinkCombineResultType::FINISHED;
}

// This is where we would build the rtree, by iterating through the tupledata collection we've created
SinkFinalizeType PhysicalSpatialJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                               OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<SpatialJoinGlobalState>();

	if (gstate.collection->Count() == 0) {
		return EmptyResultIfRHSIsEmpty() ? SinkFinalizeType::NO_OUTPUT_POSSIBLE : SinkFinalizeType::READY;
	}

	// Initialize the flat R-Tree
	static constexpr auto RTREE_NODE_SIZE = 32;
	gstate.rtree = make_uniq<JoinRTree>(BufferAllocator::Get(context), gstate.total_rtree_size, RTREE_NODE_SIZE);
	idx_t rtree_count = 0;

	// Now, this is where we build the rtree, by iterating over the tuples in the collection.
	// We need to keep everything pinned so that we can probe the pointers later
	TupleDataChunkIterator iterator(*gstate.collection, TupleDataPinProperties::KEEP_EVERYTHING_PINNED, true);

	auto &row_pointer_vector = iterator.GetChunkState().row_locations;
	auto rows_ptr = FlatVector::GetData<data_ptr_t>(row_pointer_vector);

	auto &sel = *FlatVector::IncrementalSelectionVector();

	DataChunk geom_chunk;
	geom_chunk.Initialize(context, {build_side_key_types[0]});
	auto &geom_vec = geom_chunk.data[0];

	// Setup the bounding box
	DataChunk bbox_chunk;
	bbox_chunk.Initialize(context, {GeoTypes::BOX_2DF()});
	auto bbox_expr = GetBBOXExpression(context, build_side_key_types[0]);
	ExpressionExecutor bbox_executor(context);
	bbox_executor.AddExpression(*bbox_expr);

	do {

		const auto row_count = iterator.GetCurrentChunkCount();

		geom_chunk.Reset();
		bbox_chunk.Reset();
		geom_chunk.SetCardinality(row_count);
		bbox_chunk.SetCardinality(row_count);

		// We only need to fetch the build-side key column to build the rtree.
		// The key column is always the first column in the layout.
		constexpr auto build_side_key_col = 0;      // TODO: layout_key_col_idx
		D_ASSERT(build_side_key_types.size() == 1); // TODO: remove this

		gstate.collection->Gather(row_pointer_vector, sel, row_count, build_side_key_col, geom_vec, sel, nullptr);
		FlatVector::SetSize(geom_vec, count_t(row_count));

		// This should be flat to begin with, but just to be sure
		bbox_chunk.Flatten();

		// Execute the bbox expression
		bbox_executor.Execute(geom_chunk, bbox_chunk);
		auto &entries = StructVector::GetEntries(bbox_chunk.data[0]);
		const auto xmin_data = FlatVector::GetData<float>(entries[0]);
		const auto ymin_data = FlatVector::GetData<float>(entries[1]);
		const auto xmax_data = FlatVector::GetData<float>(entries[2]);
		const auto ymax_data = FlatVector::GetData<float>(entries[3]);

		// Push the bounding boxes into the R-Tree
		auto &validity = FlatVector::Validity(bbox_chunk.data[0]);

		for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
			if (!validity.RowIsValid(row_idx)) {
				// Skip null geometries
				continue;
			}

			Box2D<float> bbox;
			bbox.min.x = xmin_data[row_idx];
			bbox.min.y = ymin_data[row_idx];
			bbox.max.x = xmax_data[row_idx];
			bbox.max.y = ymax_data[row_idx];

			if (std::isnan(bbox.min.x) || std::isnan(bbox.min.y) || std::isnan(bbox.max.x) || std::isnan(bbox.max.y)) {
				// Skip geometries with NaN bounds: they can never satisfy a spatial predicate.
				// A NaN box would corrupt every union it participates in, silently dropping matches of other rows.
				continue;
			}

			if (has_const_distance) {
				// If this is a ST_DWithin join, we need to expand the bounding box by the constant distance
				const auto f_dist = MathUtil::DoubleToFloatUp(const_distance);
				bbox.min.x -= f_dist;
				bbox.min.y -= f_dist;
				bbox.max.x += f_dist;
				bbox.max.y += f_dist;
			}

			// Push the bounding box into the R-Tree
			gstate.rtree->Set(rtree_count++, bbox, rows_ptr[row_idx]);
		}
	} while (iterator.Next());

	// Build the R-Tree once we've gathered everything
	const auto scratch_size = JoinRTree::GetBuildScratchSize(rtree_count, RTREE_NODE_SIZE);
	auto scratch = scratch_size ? BufferAllocator::Get(context).Allocate(scratch_size) : AllocatedData();
	gstate.rtree->Build(rtree_count, scratch.get());

	// If we have probe-side targets, push down a bounding-box filter derived from the build-side R-tree.
	// Every match requires the probe geometry's bbox to intersect some build box, which is contained in the R-tree's
	// root box, so probe rows outside it can be pruned. For ST_DWithin the distance is already baked into the root box.
	// (the per-item boxes were expanded before insertion).
	if (!filter_pushdown_targets.empty() && gstate.rtree->Count() > 0) {
		const auto envelope = MakeEnvelopeValue(context, gstate.rtree->Bounds());
		for (auto &target : filter_pushdown_targets) {
			target.dynamic_filters->PushFilter(*this, target.probe_column_index,
			                                   MakeBoundingBoxFilter(context, envelope, target.column_type));
		}
	}

	return SinkFinalizeType::READY;
}

//----------------------------------------------------------------------------------------------------------------------
// Prepared Build Side
//----------------------------------------------------------------------------------------------------------------------
// Evaluating the join predicate as a regular expression deserializes both geometries of every candidate pair and runs
// the predicate unprepared: the constant-argument fast path of the GEOS predicates never applies, because the join
// passes the build side as a dictionary vector. A large build-side polygon matched by many probe rows (e.g. a 1M+
// vertex polygon over thousands of points) is then decoded again, and all its edges re-scanned, for every candidate
// pair. Instead, for GEOS predicates, each probe thread keeps the build-side geometries it touches deserialized and
// prepared, keyed by their (pinned) row pointer, in a small LRU cache, and evaluates the predicate against the prepared
// geometry directly. Prepared geometries build their indexes lazily and are not thread-safe, hence a cache per thread
// rather than one shared cache. A geometry is only prepared on its second use, and one larger than the whole cache is
// not cached, so that a cache too small for the working set does not re-prepare or evict on every candidate.

namespace {

// The prepared operation to run as `prepared_build.op(probe)` so that it equals `predicate(probe, build)`
enum class PreparedJoinPredicate : uint8_t {
	NONE = 0,
	INTERSECTS,
	CONTAINS,
	CONTAINS_PROPERLY,
	WITHIN,
	COVERS,
	COVERED_BY,
	TOUCHES,
	CROSSES,
	OVERLAPS,
};

#ifdef SPATIAL_USE_GEOS

PreparedJoinPredicate GetPreparedJoinPredicate(const Expression &condition) {
	auto &func = condition.Cast<BoundFunctionExpression>();
	if (func.GetChildren().size() != 2) {
		return PreparedJoinPredicate::NONE;
	}
	for (auto &child : func.GetChildren()) {
		if (child->GetReturnType().id() != LogicalTypeId::GEOMETRY) {
			return PreparedJoinPredicate::NONE;
		}
	}

	// The probe side is the first argument, the build side the second
	const auto name = func.Function().GetName().GetIdentifierName();
	if (name == "ST_Intersects") {
		return PreparedJoinPredicate::INTERSECTS;
	}
	if (name == "ST_Contains") {
		return PreparedJoinPredicate::WITHIN;
	}
	if (name == "ST_Within") {
		return PreparedJoinPredicate::CONTAINS;
	}
	if (name == "ST_WithinProperly") {
		return PreparedJoinPredicate::CONTAINS_PROPERLY;
	}
	if (name == "ST_Covers") {
		return PreparedJoinPredicate::COVERED_BY;
	}
	if (name == "ST_CoveredBy") {
		return PreparedJoinPredicate::COVERS;
	}
	if (name == "ST_Touches") {
		return PreparedJoinPredicate::TOUCHES;
	}
	if (name == "ST_Crosses") {
		return PreparedJoinPredicate::CROSSES;
	}
	if (name == "ST_Overlaps") {
		return PreparedJoinPredicate::OVERLAPS;
	}
	// ST_ContainsProperly has no prepared inverse, and ST_Equals, ST_DWithin and ST_Intersects_Extent are not
	// (prepared) GEOS predicates, so these keep the regular expression path.
	return PreparedJoinPredicate::NONE;
}

bool EvaluateJoinPredicate(PreparedJoinPredicate predicate, const GeosGeometry &build, const GeosGeometry &probe) {
	switch (predicate) {
	case PreparedJoinPredicate::INTERSECTS:
		return build.intersects(probe);
	case PreparedJoinPredicate::CONTAINS:
		return build.contains(probe);
	case PreparedJoinPredicate::WITHIN:
		return build.within(probe);
	case PreparedJoinPredicate::COVERS:
		return build.covers(probe);
	case PreparedJoinPredicate::COVERED_BY:
		return build.covered_by(probe);
	case PreparedJoinPredicate::TOUCHES:
		return build.touches(probe);
	case PreparedJoinPredicate::CROSSES:
		return build.crosses(probe);
	case PreparedJoinPredicate::OVERLAPS:
		return build.overlaps(probe);
	default:
		throw InternalException("Unexpected spatial join predicate");
	}
}

bool EvaluateJoinPredicate(PreparedJoinPredicate predicate, const PreparedGeosGeometry &build,
                           const GeosGeometry &probe) {
	switch (predicate) {
	case PreparedJoinPredicate::INTERSECTS:
		return build.intersects(probe);
	case PreparedJoinPredicate::CONTAINS:
		return build.contains(probe);
	case PreparedJoinPredicate::CONTAINS_PROPERLY:
		return build.contains_properly(probe);
	case PreparedJoinPredicate::WITHIN:
		return build.within(probe);
	case PreparedJoinPredicate::COVERS:
		return build.covers(probe);
	case PreparedJoinPredicate::COVERED_BY:
		return build.covered_by(probe);
	case PreparedJoinPredicate::TOUCHES:
		return build.touches(probe);
	case PreparedJoinPredicate::CROSSES:
		return build.crosses(probe);
	case PreparedJoinPredicate::OVERLAPS:
		return build.overlaps(probe);
	default:
		throw InternalException("Unexpected spatial join predicate");
	}
}

class PreparedBuildCache {
public:
	PreparedBuildCache(ClientContext &context, idx_t capacity_p)
	    : arena(BufferAllocator::Get(context)), capacity(capacity_p) {
		ctx = GEOS_init_r();
		GEOSContext_setErrorMessageHandler_r(
		    ctx, [](const char *message, void *) { throw InvalidInputException(message); }, nullptr);
	}

	~PreparedBuildCache() {
		// The geometries must be destroyed before the context they were created in
		probe.reset();
		lookup.clear();
		entries.clear();
		GEOS_finish_r(ctx);
	}

	//! Evaluates the predicate for the build-side geometry stored at the given (pinned) row
	bool Evaluate(PreparedJoinPredicate predicate, data_ptr_t row, const string_t &blob, const GeosGeometry &probe) {
		if (blob.GetSize() > capacity) {
			// Too large to cache: evaluate it like the unprepared path would, rather than evicting everything else
			const auto geom = Deserialize(blob);
			if (predicate == PreparedJoinPredicate::CONTAINS_PROPERLY) {
				return EvaluateJoinPredicate(predicate, geom.get_prepared(), probe);
			}
			return EvaluateJoinPredicate(predicate, geom, probe);
		}

		auto &entry = GetBuild(row, blob);
		if (!entry.prepared && (entry.uses++ > 0 || predicate == PreparedJoinPredicate::CONTAINS_PROPERLY)) {
			// GEOS has no unprepared ContainsProperly
			entry.prepared = make_uniq<PreparedGeosGeometry>(entry.geom.get_prepared());
		}
		if (entry.prepared) {
			return EvaluateJoinPredicate(predicate, *entry.prepared, probe);
		}
		return EvaluateJoinPredicate(predicate, entry.geom, probe);
	}

	//! The probe-side geometry of the given input row. Consecutive candidate pairs share the probe row, so the last
	//! one is kept until the row (or the input chunk, see ResetProbe) changes.
	const GeosGeometry &GetProbe(idx_t input_idx, const string_t &blob) {
		if (!probe || probe_idx != input_idx) {
			probe.reset();
			probe = make_uniq<GeosGeometry>(Deserialize(blob));
			probe_idx = input_idx;
		}
		return *probe;
	}

	void ResetProbe() {
		probe.reset();
	}

private:
	struct Entry {
		Entry(data_ptr_t row_p, idx_t size_p, GeosGeometry geom_p) : row(row_p), size(size_p), geom(std::move(geom_p)) {
		}
		data_ptr_t row;
		idx_t size;
		idx_t uses = 0;
		// Declared before the prepared geometry, so that it is destroyed after it
		GeosGeometry geom;
		unique_ptr<PreparedGeosGeometry> prepared;
	};

	//! The cache entry of the build-side geometry stored at the given (pinned) row.
	//! The reference stays valid until the next call, which may evict it.
	Entry &GetBuild(data_ptr_t row, const string_t &blob) {
		const auto it = lookup.find(row);
		if (it != lookup.end()) {
			entries.splice(entries.begin(), entries, it->second);
			return entries.front();
		}

		entries.emplace_front(row, blob.GetSize(), Deserialize(blob));
		lookup[row] = entries.begin();
		size += blob.GetSize();

		// Evict the least recently used geometries (the one we just added fits by itself, see Evaluate)
		while (size > capacity && entries.size() > 1) {
			auto &last = entries.back();
			size -= last.size;
			lookup.erase(last.row);
			entries.pop_back();
		}
		return entries.front();
	}

	GeosGeometry Deserialize(const string_t &blob) {
		// GEOS copies the coordinates out of the arena, so the arena only ever holds one geometry's staging buffers
		arena.Reset();
		const auto geom = GeosSerde::Deserialize(ctx, arena, blob.GetData(), blob.GetSize());
		arena.Reset();
		if (geom == nullptr) {
			throw InvalidInputException("Could not deserialize geometry");
		}
		return GeosGeometry(ctx, geom);
	}

	GEOSContextHandle_t ctx;
	ArenaAllocator arena;

	idx_t capacity;
	idx_t size = 0;
	list<Entry> entries;
	unordered_map<data_ptr_t, list<Entry>::iterator> lookup;

	unique_ptr<GeosGeometry> probe;
	idx_t probe_idx = 0;
};

#else

PreparedJoinPredicate GetPreparedJoinPredicate(const Expression &) {
	return PreparedJoinPredicate::NONE;
}

class PreparedBuildCache {};

#endif

} // namespace

//----------------------------------------------------------------------------------------------------------------------
// Operator Interface
//----------------------------------------------------------------------------------------------------------------------
// This is where we do the probing of the rtree.

enum class SpatialJoinState { START = 0, INIT, PROBE, SCAN, EMIT, EMIT_LHS };

class SpatialJoinLocalOperatorState final : public CachingOperatorState {
public:
	bool is_initialized = false;

	idx_t input_index = 0;
	SpatialJoinState state = SpatialJoinState::START;

	DataChunk overflow_matches;

	JoinRTreeScanState scan;

	DataChunk probe_side_row_chunk; // holds the projected lhs columns
	DataChunk probe_side_key_chunk; // holds the lhs probe key
	DataChunk probe_side_box_chunk; // holds the lhs probe key as a box
	DataChunk build_side_key_chunk; // holds the rhs build key
	DataChunk match_pred_arg_chunk; // references the lhs probe key and the rhs build key, used to compute the predicate

	ExpressionExecutor join_probe_executor; // used to compute the probe key
	ExpressionExecutor join_match_executor; // used to compute the predicate
	ExpressionExecutor bbox_probe_executor; // used to compute the bounding box for the probe key

	UnifiedVectorFormat probe_side_key_vformat; // used to access the probe side key, after its been computed
	UnifiedVectorFormat probe_side_box_vformat; // used to access the probe side bounding box, after its been computed
	UnifiedVectorFormat probe_side_box_xmin_vformat;
	UnifiedVectorFormat probe_side_box_ymin_vformat;
	UnifiedVectorFormat probe_side_box_xmax_vformat;
	UnifiedVectorFormat probe_side_box_ymax_vformat;

	unique_ptr<Expression> match_expr; // to evaluate the predicate
	unique_ptr<Expression> bound_expr; // to compute the bounding box of the probe key

	SelectionVector probe_side_source_sel; // maps what output rows correspond to which input lhs rows
	SelectionVector build_side_source_sel; // used when gathering the build side
	SelectionVector build_side_target_sel; // used when gathering the build side
	SelectionVector match_sel;             // used to select the output rows, by executing the predicate

	SelectionVector lhs_match_sel; // this is only used when emitting the lhs side of the join

	uint8_t left_outer_marker[STANDARD_VECTOR_SIZE] = {};

	idx_t build_side_match_offset = 0;
	unsafe_unique_array<data_ptr_t> build_side_pointers = nullptr;

	// Set when the predicate is evaluated against prepared build-side geometries (see PreparedBuildCache)
	PreparedJoinPredicate prepared_predicate = PreparedJoinPredicate::NONE;
	unique_ptr<PreparedBuildCache> prepared_build;

	explicit SpatialJoinLocalOperatorState(ClientContext &context)
	    : join_probe_executor(context), join_match_executor(context), bbox_probe_executor(context),
	      probe_side_source_sel(STANDARD_VECTOR_SIZE), build_side_source_sel(STANDARD_VECTOR_SIZE),
	      build_side_target_sel(STANDARD_VECTOR_SIZE), match_sel(STANDARD_VECTOR_SIZE),
	      lhs_match_sel(STANDARD_VECTOR_SIZE) {

		build_side_pointers = make_unsafe_uniq_array<data_ptr_t>(STANDARD_VECTOR_SIZE);
	}
};

class SpatialJoinGlobalOperatorState final : public GlobalOperatorState {
public:
	unique_ptr<JoinRTree> rtree;
	unique_ptr<TupleDataCollection> collection;

#ifdef DUCKDB_SPATIAL_DEBUG_JOIN
	// TODO: Move this into proper profiling metrics later
	// Statistics
	atomic<idx_t> total_rtree_probes = {0};
	atomic<idx_t> total_rtree_successfull_probes = {0};
	atomic<idx_t> total_rtree_candidates = {0};
	atomic<idx_t> max_candidates = {0};
	atomic<idx_t> min_candidates = {std::numeric_limits<idx_t>::max()};

	~SpatialJoinGlobalOperatorState() override {
		Printer::PrintF("Spatial Join RTree Probes: %llu\n", total_rtree_probes.load());
		Printer::PrintF("Spatial Join RTree Successful Probes: %llu\n", total_rtree_successfull_probes.load());
		Printer::PrintF("Spatial Join RTree Candidates: %llu\n", total_rtree_candidates.load());
		Printer::PrintF("Spatial Join RTree Max Candidates per Probe: %llu\n", max_candidates.load());
		Printer::PrintF("Spatial Join RTree Min Candidates per Probe: %llu\n", min_candidates.load());
	}
#endif
};

unique_ptr<OperatorState> PhysicalSpatialJoin::GetOperatorState(ExecutionContext &context) const {
	auto lstate = make_uniq<SpatialJoinLocalOperatorState>(context.client);

	// Create a match expression using the condition, that will be used to filter the results
	lstate->match_expr = condition->Copy();
	auto &func_expr = lstate->match_expr->Cast<BoundFunctionExpression>();
	func_expr.GetChildrenMutable()[0] = make_uniq<BoundReferenceExpression>(probe_side_key->GetReturnType(), 0);
	func_expr.GetChildrenMutable()[1] = make_uniq<BoundReferenceExpression>(build_side_key->GetReturnType(), 1);

	lstate->join_match_executor.AddExpression(*lstate->match_expr);

#ifdef SPATIAL_USE_GEOS
	// Evaluate GEOS predicates against prepared build-side geometries, unless disabled
	const auto prepared_cache_size = SpatialSettings::JoinPreparedCacheSize(context.client);
	if (prepared_cache_size > 0) {
		lstate->prepared_predicate = GetPreparedJoinPredicate(*condition);
		if (lstate->prepared_predicate != PreparedJoinPredicate::NONE) {
			lstate->prepared_build = make_uniq<PreparedBuildCache>(context.client, prepared_cache_size);
		}
	}
#endif

	// Add the probe side join key expression
	lstate->join_probe_executor.AddExpression(*probe_side_key);

	// Make bbox expression for probe side
	lstate->bound_expr = GetBBOXExpression(context.client, probe_side_key->GetReturnType());
	lstate->bbox_probe_executor.AddExpression(*lstate->bound_expr);

	// The chunks we need for the join
	lstate->probe_side_row_chunk.Initialize(context.client, probe_side_output_types);
	lstate->probe_side_key_chunk.Initialize(context.client, {probe_side_key->GetReturnType()});
	lstate->probe_side_box_chunk.Initialize(context.client, {lstate->bound_expr->GetReturnType()});
	lstate->build_side_key_chunk.Initialize(context.client, {build_side_key->GetReturnType()});
	lstate->match_pred_arg_chunk.Initialize(context.client,
	                                        {probe_side_key->GetReturnType(), build_side_key->GetReturnType()});

	return std::move(lstate);
}

unique_ptr<GlobalOperatorState> PhysicalSpatialJoin::GetGlobalOperatorState(ClientContext &context) const {
	auto &gstate = sink_state->Cast<SpatialJoinGlobalState>();

	auto result = make_uniq<SpatialJoinGlobalOperatorState>();
	// Steal the built rtree from the sink state.
	result->rtree = std::move(gstate.rtree);
	// Steal the tuple data collection
	result->collection = std::move(gstate.collection);

	return std::move(result);
}

OperatorResultType PhysicalSpatialJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                        GlobalOperatorState &gstate_p, OperatorState &lstate_p) const {
	auto &gstate = gstate_p.Cast<SpatialJoinGlobalOperatorState>();
	auto &lstate = lstate_p.Cast<SpatialJoinLocalOperatorState>();

	idx_t output_index = 0;
	idx_t output_count = STANDARD_VECTOR_SIZE;

	while (true) {
		switch (lstate.state) {
		//--------------------------------------------------------------------------------------------------------------
		// START
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::START: {
			// Check if the build side is empty
			if (gstate.rtree == nullptr || gstate.rtree->Count() == 0) {
				if (EmptyResultIfRHSIsEmpty()) {
					return OperatorResultType::FINISHED;
				}

				// Slice the input chunk to what we need
				lstate.probe_side_row_chunk.ReferenceColumns(input, probe_side_output_columns);

				// Construct empty join result
				PhysicalComparisonJoin::ConstructEmptyJoinResult(join_type, false, lstate.probe_side_row_chunk, chunk);

				return OperatorResultType::NEED_MORE_INPUT;
			}

			lstate.state = SpatialJoinState::INIT;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// INIT
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::INIT: {
			// We have a new fresh input chunk
			lstate.probe_side_key_chunk.Reset();
			lstate.probe_side_box_chunk.Reset();

			// Compute the probe side join key
			lstate.join_probe_executor.Execute(input, lstate.probe_side_key_chunk);
			lstate.probe_side_key_chunk.data[0].ToUnifiedFormat(lstate.probe_side_key_vformat);

			// Setup bounding box
			lstate.bbox_probe_executor.Execute(lstate.probe_side_key_chunk, lstate.probe_side_box_chunk);
			lstate.probe_side_box_chunk.data[0].ToUnifiedFormat(lstate.probe_side_box_vformat);

			auto &entries = StructVector::GetEntries(lstate.probe_side_box_chunk.data[0]);
			entries[0].ToUnifiedFormat(lstate.probe_side_box_xmin_vformat);
			entries[1].ToUnifiedFormat(lstate.probe_side_box_ymin_vformat);
			entries[2].ToUnifiedFormat(lstate.probe_side_box_xmax_vformat);
			entries[3].ToUnifiedFormat(lstate.probe_side_box_ymax_vformat);

			// Reference the columns that we actually care about
			lstate.probe_side_row_chunk.ReferenceColumns(input, probe_side_output_columns);

			// zero miss vector
			memset(lstate.left_outer_marker, 0, sizeof(lstate.left_outer_marker));

#ifdef SPATIAL_USE_GEOS
			if (lstate.prepared_build) {
				lstate.prepared_build->ResetProbe();
			}
#endif

			// Reset the input index and move on to the next state
			lstate.input_index = 0;
			lstate.state = SpatialJoinState::PROBE;

		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// PROBE
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::PROBE: {
			if (lstate.input_index == input.size()) {
				lstate.state = SpatialJoinState::EMIT;
				continue;
			}

			// Loop over the bounding boxes
			const auto geom_idx = lstate.probe_side_box_vformat.sel->get_index(lstate.input_index);
			if (!lstate.probe_side_box_vformat.validity.RowIsValid(geom_idx)) {
				lstate.input_index++;
				continue;
			}

			// Extract the bounding box
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

			lstate.scan.Init(*gstate.rtree, bbox);

#ifdef DUCKDB_SPATIAL_DEBUG_JOIN
			gstate.total_rtree_probes += 1;
#endif

			if (!lstate.scan.Scan(*gstate.rtree)) {
				lstate.input_index++;
				continue;
			}

#ifdef DUCKDB_SPATIAL_DEBUG_JOIN
			gstate.total_rtree_successfull_probes += 1;
			gstate.total_rtree_candidates += lstate.scan.matches_count;
			gstate.max_candidates = MaxValue(gstate.max_candidates.load(), lstate.scan.matches_count);
			gstate.min_candidates = MinValue(gstate.min_candidates.load(), lstate.scan.matches_count);
#endif

			lstate.state = SpatialJoinState::SCAN;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// SCAN
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::SCAN: {
			const auto matches_remaining = lstate.scan.matches_count - lstate.scan.matches_idx;
			if (matches_remaining == 0) {
				// We are out of matches. Try to get the next probe
				if (lstate.scan.Scan(*gstate.rtree)) {
					continue;
				}
				// Otherwise, we are done with this probe
				lstate.input_index++;
				lstate.state = SpatialJoinState::PROBE;
				continue;
			}

			const auto output_remaining = output_count - output_index;
			const auto scan_count = MinValue(output_remaining, matches_remaining);

			D_ASSERT(scan_count != 0);

			for (idx_t i = 0; i < scan_count; i++) {
				// These control what we gather from the build side
				lstate.build_side_target_sel.set_index(i, output_index + i);
				lstate.build_side_source_sel.set_index(i, lstate.scan.matches_idx + i);

				// This stores which probe side row we used to gather the build side
				lstate.probe_side_source_sel.set_index(output_index + i, lstate.input_index);
			}

			// Fetch each column from the build side
			D_ASSERT(build_side_key_types.size() == 1);
			// TODO: Add more key conditions here
			constexpr auto build_side_key_col = 0;

			auto &row_pointers = lstate.scan.matches;

			// Collect the build side join key(s)
			// TODO: Multiple join keys
			gstate.collection->Gather(row_pointers, lstate.build_side_source_sel, scan_count, build_side_key_col,
			                          lstate.build_side_key_chunk.data[0], lstate.build_side_target_sel, nullptr);

			// Now, lets collect the rest of the build side columns
			for (idx_t i = 0; i < build_side_output_columns.size(); i++) {
				auto &target = chunk.data[probe_side_output_columns.size() + i];
				const auto build_side_col_idx = build_side_output_columns[i];
				D_ASSERT(target.GetType() == build_side_output_types[i]);

				// TODO: We should use cached cast vectors here to improve performance of nested array payloads
				gstate.collection->Gather(row_pointers, lstate.build_side_source_sel, scan_count, build_side_col_idx,
				                          target, lstate.build_side_target_sel, nullptr);
			}

			// Also collect the build side row pointers (if we have a match column, or cache prepared build rows)
			// Note: we must offset by matches_idx here, just like build_side_source_sel above. A single probe's
			// candidate batch can be split across multiple output chunks, and in that case we resume in the middle of
			// the batch. Reading from the start of the batch would mark the wrong build rows as matched, dropping them
			// from the right-outer output while emitting the actual matches twice.
			if (IsRightOuterJoin(join_type) || lstate.prepared_build) {
				const auto ptrs = FlatVector::GetDataMutable<data_ptr_t>(row_pointers);
				for (idx_t i = 0; i < scan_count; i++) {
					lstate.build_side_pointers[output_index + i] = ptrs[lstate.scan.matches_idx + i];
				}
			}

			// Increment the output and match index
			output_index += scan_count;
			lstate.scan.matches_idx += scan_count;

			// Update vector sizes to match the data we have written so far
			FlatVector::SetSize(lstate.build_side_key_chunk.data[0], count_t(output_index));
			for (idx_t i = 0; i < build_side_output_columns.size(); i++) {
				auto &target = chunk.data[probe_side_output_columns.size() + i];
				FlatVector::SetSize(target, count_t(output_index));
			}

			if (output_index != output_count) {
				// We still have space left. Scan more!
				continue;
			}

			lstate.state = SpatialJoinState::EMIT;
		} // fall through
		//--------------------------------------------------------------------------------------------------------------
		// EMIT
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::EMIT: {

			// Start by adding the lhs columns
			chunk.Slice(lstate.probe_side_row_chunk, lstate.probe_side_source_sel, output_index);

			// Now, lets actually evaluate the predicate
			idx_t filtered = 0;
#ifdef SPATIAL_USE_GEOS
			if (lstate.prepared_build) {
				const auto probe_data = UnifiedVectorFormat::GetData<string_t>(lstate.probe_side_key_vformat);
				auto &build_vec = lstate.build_side_key_chunk.data[0];
				const auto build_data = FlatVector::GetData<string_t>(build_vec);
				const auto &build_validity = FlatVector::Validity(build_vec);

				for (idx_t i = 0; i < output_index; i++) {
					const auto probe_row = lstate.probe_side_source_sel.get_index(i);
					const auto probe_idx = lstate.probe_side_key_vformat.sel->get_index(probe_row);
					if (!lstate.probe_side_key_vformat.validity.RowIsValid(probe_idx) ||
					    !build_validity.RowIsValid(i)) {
						continue;
					}
					const auto &probe_geom = lstate.prepared_build->GetProbe(probe_row, probe_data[probe_idx]);
					if (lstate.prepared_build->Evaluate(lstate.prepared_predicate, lstate.build_side_pointers[i],
					                                    build_data[i], probe_geom)) {
						lstate.match_sel.set_index(filtered++, i);
					}
				}
			} else
#endif
			{
				lstate.match_pred_arg_chunk.data[0].Slice(lstate.probe_side_key_chunk.data[0],
				                                          lstate.probe_side_source_sel, output_index);
				lstate.match_pred_arg_chunk.data[1].Reference(lstate.build_side_key_chunk.data[0]);
				lstate.match_pred_arg_chunk.SetCardinality(output_index);

				filtered = lstate.join_match_executor.SelectExpression(lstate.match_pred_arg_chunk, lstate.match_sel);
			}

			if (IsLeftOuterJoin(join_type)) {
				for (idx_t i = 0; i < filtered; i++) {
					// This is kinda crazy
					// We're first selecting all the output rows that matched.
					// And then figuring out which lhs row that belongs to, by using the probe_side_source_sel vector
					const auto match_source_idx = lstate.match_sel.get_index(i);
					const auto probe_source_idx = lstate.probe_side_source_sel.get_index(match_source_idx);
					lstate.left_outer_marker[probe_source_idx] = 1;
				}
			}

			if (IsRightOuterJoin(join_type)) {
				// Mark each row in the build side as being matched.
				// We need to do this so we dont emit them again in the right-outer join phase

				// This is where the bool match offset is
				const auto tuple_size = build_side_match_offset;

				for (idx_t i = 0; i < filtered; i++) {
					const auto match_source_idx = lstate.match_sel.get_index(i);
					const auto data_ptr = lstate.build_side_pointers[match_source_idx] + tuple_size;
					// Set the match column to true
					Store<bool>(true, data_ptr);
				}
			}

			chunk.Slice(lstate.match_sel, filtered);

			if (lstate.input_index != input.size()) {
				// We still have more input rows to process
				lstate.state = SpatialJoinState::SCAN;
				return OperatorResultType::HAVE_MORE_OUTPUT;
			}

			if (IsLeftOuterJoin(join_type)) {
				// Before we can ask for more input, we need to emit the outer left side
				// But we need a clean output chunk, so we need to return here (cant just fall through)

				lstate.state = SpatialJoinState::EMIT_LHS;
				return OperatorResultType::HAVE_MORE_OUTPUT;
			}

			// Otherwise, we need to wait for more input
			lstate.state = SpatialJoinState::INIT;
			return OperatorResultType::NEED_MORE_INPUT;
		}
		//--------------------------------------------------------------------------------------------------------------
		// EMIT LEFT OUTER
		//--------------------------------------------------------------------------------------------------------------
		case SpatialJoinState::EMIT_LHS: {
			// Emit the outer left side

			idx_t remaining_count = 0;
			for (idx_t i = 0; i < input.size(); i++) {
				if (!lstate.left_outer_marker[i]) {
					lstate.lhs_match_sel.set_index(remaining_count++, i);
				}
			}

			if (remaining_count > 0) {
				chunk.Slice(lstate.probe_side_row_chunk, lstate.lhs_match_sel, remaining_count);

				// Null the RHS columns
				for (idx_t i = 0; i < build_side_output_columns.size(); i++) {
					auto &target = chunk.data[probe_side_output_columns.size() + i];
					ConstantVector::SetNull(target, count_t(remaining_count));
				}
			}

			lstate.state = SpatialJoinState::INIT;
			return OperatorResultType::NEED_MORE_INPUT;
		}
		default:
			D_ASSERT(false);
			break;
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Source Interface
//----------------------------------------------------------------------------------------------------------------------
class SpatialJoinGlobalSourceState final : public GlobalSourceState {
public:
	explicit SpatialJoinGlobalSourceState(const PhysicalSpatialJoin &op) : op(op) {
		D_ASSERT(op.sink_state);
		const auto &state = op.op_state->Cast<SpatialJoinGlobalOperatorState>();

		// Initialize a parallel scan
		vector<column_t> column_ids;

		column_ids.insert(column_ids.end(), op.build_side_output_columns.begin(), op.build_side_output_columns.end());

		// Also add the match column
		column_ids.push_back(op.build_side_key_types.size() + op.build_side_payload_types.size());

		// We dont need to keep the tuples aroun after scanning
		state.collection->InitializeScan(scan_state, std::move(column_ids), TupleDataPinProperties::UNPIN_AFTER_DONE);

		tuples_maximum = state.collection->Count();
	}

	const PhysicalSpatialJoin &op;

	mutex scan_lock;
	TupleDataParallelScanState scan_state;

	// How many tuples we have scanned so far
	idx_t tuples_maximum = 0;
	atomic<idx_t> tuples_scanned = {0};

public:
	idx_t MaxThreads() override {
		const auto &state = op.op_state->Cast<SpatialJoinGlobalOperatorState>();
		return state.collection->ChunkCount();
	}

	bool Scan(TupleDataLocalScanState &local_scan, DataChunk &chunk) {
		const auto &collection = op.op_state->Cast<SpatialJoinGlobalOperatorState>().collection;

		lock_guard<mutex> guard(scan_lock);
		const auto not_empty = collection->Scan(scan_state, local_scan, chunk);
		tuples_scanned += chunk.size();

		return not_empty;
	}
};

class SpatialJoinLocalSourceState final : public LocalSourceState {
public:
	explicit SpatialJoinLocalSourceState(const PhysicalSpatialJoin &op) : match_sel(STANDARD_VECTOR_SIZE) {

		D_ASSERT(op.sink_state);
		const auto &state = op.op_state->Cast<SpatialJoinGlobalOperatorState>();

		vector<column_t> column_ids;

		column_ids.insert(column_ids.end(), op.build_side_output_columns.begin(), op.build_side_output_columns.end());

		// Also add the match column
		column_ids.push_back(op.build_side_key_types.size() + op.build_side_payload_types.size());

		// We dont need to keep the tuples aroun after scanning
		state.collection->InitializeScan(scan_state, std::move(column_ids));
		state.collection->InitializeScanChunk(scan_state, scan_chunk);
	}

	TupleDataLocalScanState scan_state;
	DataChunk scan_chunk;
	SelectionVector match_sel;
};

unique_ptr<GlobalSourceState> PhysicalSpatialJoin::GetGlobalSourceState(ClientContext &context) const {
	auto gstate = make_uniq<SpatialJoinGlobalSourceState>(*this);
	return std::move(gstate);
}

unique_ptr<LocalSourceState> PhysicalSpatialJoin::GetLocalSourceState(ExecutionContext &context,
                                                                      GlobalSourceState &gstate_p) const {
	auto lstate = make_uniq<SpatialJoinLocalSourceState>(*this);
	return std::move(lstate);
}

SourceResultType PhysicalSpatialJoin::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                      OperatorSourceInput &input) const {
	D_ASSERT(PropagatesBuildSide(join_type));

	auto &gstate = input.global_state.Cast<SpatialJoinGlobalSourceState>();
	auto &lstate = input.local_state.Cast<SpatialJoinLocalSourceState>();

	if (!gstate.Scan(lstate.scan_state, lstate.scan_chunk)) {
		return SourceResultType::FINISHED;
	}

	const auto matches = FlatVector::GetDataMutable<bool>(lstate.scan_chunk.data.back());

	idx_t result_count = 0;
	for (idx_t i = 0; i < lstate.scan_chunk.size(); i++) {
		if (!matches[i]) {
			lstate.match_sel.set_index(result_count++, i);
		}
	}

	if (result_count > 0) {

		const auto lhs_col_count = probe_side_output_columns.size();
		const auto rhs_col_count = build_side_output_columns.size();

		// Null the LHS columns
		for (idx_t i = 0; i < lhs_col_count; i++) {
			auto &target = chunk.data[i];
			ConstantVector::SetNull(target, count_t(result_count));
		}

		// Set the RHS columns
		for (idx_t i = 0; i < rhs_col_count; i++) {
			auto &target = chunk.data[lhs_col_count + i];
			// Offset by one here to skip the match column
			target.Slice(lstate.scan_chunk.data[i], lstate.match_sel, result_count);
		}
	}

	chunk.SetCardinality(result_count);
	return SourceResultType::HAVE_MORE_OUTPUT;
}

//----------------------------------------------------------------------------------------------------------------------
// Misc
//----------------------------------------------------------------------------------------------------------------------

ProgressData PhysicalSpatialJoin::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	const auto &state = gstate.Cast<SpatialJoinGlobalSourceState>();
	ProgressData res;
	if (state.tuples_maximum) {
		res.done = state.tuples_scanned.load();
		res.total = state.tuples_maximum;
	} else {
		res.SetInvalid();
	}
	return res;
}

} // namespace duckdb
