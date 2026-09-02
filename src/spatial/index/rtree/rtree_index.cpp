#include "spatial/index/rtree/rtree_index.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/main/database.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/geometry/geometry_serialization.hpp"

#include "spatial/index/rtree/rtree_module.hpp"
#include "spatial/index/rtree/rtree_node.hpp"
#include "spatial/index/rtree/rtree_scanner.hpp"
#include "spatial/spatial_settings.hpp"
#include "spatial/util/math.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// RTree Index Scan State
//------------------------------------------------------------------------------
class RTreeIndexScanState final : public IndexScanState {
public:
	RTreeBounds query_bounds;
	RTreeScanner scanner;
};

//------------------------------------------------------------------------------
// RTree Configuration
//------------------------------------------------------------------------------

static RTreeConfig ParseOptions(const case_insensitive_map_t<Value> &options) {
	RTreeConfig config = {};

	const auto max_cap_param_search = options.find("max_node_capacity");
	if (max_cap_param_search != options.end()) {
		const auto val = max_cap_param_search->second.GetValue<int32_t>();
		if (val < 4) {
			throw InvalidInputException("RTree: max_node_capacity must be at least 4");
		}
		if (val > 255) {
			throw InvalidInputException("RTree: max_node_capacity must be at most 255");
		}
		config.max_node_capacity = UnsafeNumericCast<idx_t>(val);
	}

	const auto min_cap_search = options.find("min_node_capacity");
	if (min_cap_search != options.end()) {
		const auto val = min_cap_search->second.GetValue<int32_t>();
		if (val < 0) {
			throw InvalidInputException("RTree: min_node_capacity must be at least 0");
		}
		if (val > config.max_node_capacity / 2) {
			throw InvalidInputException("RTree: min_node_capacity must be at most 'max_node_capacity / 2'");
		}
		config.min_node_capacity = UnsafeNumericCast<idx_t>(val);
	} else {
		// If no min capacity is set, set it to 40% of the max capacity
		if (max_cap_param_search != options.end()) {
			config.min_node_capacity = std::ceil(static_cast<double>(config.max_node_capacity) * 0.4);
		}
	}

	return config;
}

//------------------------------------------------------------------------------
// RTreeIndex Methods
//------------------------------------------------------------------------------

// Constructor
RTreeIndex::RTreeIndex(const string &name, IndexConstraintType index_constraint_type,
                       const vector<column_t> &column_ids, TableIOManager &table_io_manager,
                       const vector<unique_ptr<Expression>> &unbound_expressions, AttachedDatabase &db,
                       const case_insensitive_map_t<Value> &options, ClientContext &context,
                       const IndexStorageInfo &info, idx_t estimated_cardinality)
    : BoundIndex(name, TYPE_NAME, index_constraint_type, column_ids, table_io_manager, unbound_expressions, db) {

	if (index_constraint_type != IndexConstraintType::NONE) {
		throw NotImplementedException("RTree indexes do not support unique or primary key constraints");
	}

	// Create the configuration from the options
	RTreeConfig config = ParseOptions(options);

	// Create the RTree
	auto &block_manager = table_io_manager.GetIndexBlockManager();

	const auto max_alloc_size = block_manager.GetBlockSize() - sizeof(validity_t);
	if (config.GetNodeByteSize() > max_alloc_size || config.GetLeafByteSize() > max_alloc_size) {
		throw InvalidInputException("Cannot instantiate RTree index: The node and/or leaf capacity of RTree index '%s' "
		                            "is too large to fit within the configured block size of this database",
		                            name);
	}

	tree = make_uniq<RTree>(block_manager, config);

	if (info.IsValid()) {
		// This is an old index that needs to be loaded
		// Initialize the allocators
		tree->GetLeafAllocator().Init(info.allocator_infos[0]);
		tree->GetNodeAllocator().Init(info.allocator_infos[1]);
		// Set the root node and recalculate the bounds
		tree->SetRoot(info.root);
	}

	// Construct the key expression executor
	auto &source_type = unbound_expressions[0]->return_type;
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, DEFAULT_SCHEMA, "ST_Extent_Approx");
	auto func = entry.functions.GetFunctionByArguments(context, {source_type});
	auto child_expr = make_uniq<BoundReferenceExpression>(source_type, 0);

	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child_expr));

	key_expr = make_uniq<BoundFunctionExpression>(GeoTypes::BOX_2DF(), func, std::move(children), nullptr);
	key_executor = make_uniq<ExpressionExecutor>(context);
	key_executor->AddExpression(*key_expr);
	key_chunk.Initialize(context, {GeoTypes::BOX_2DF()});
}

unique_ptr<IndexScanState> RTreeIndex::InitializeScan(const RTreeBounds &query) const {
	auto state = make_uniq<RTreeIndexScanState>();
	state->query_bounds = query;
	auto &root = tree->GetRoot();
	if (root.pointer.Get() != 0 && state->query_bounds.Intersects(root.bounds)) {
		state->scanner.Init(root);
	}
	return std::move(state);
}

idx_t RTreeIndex::Scan(IndexScanState &state, Vector &result) const {
	auto &sstate = state.Cast<RTreeIndexScanState>();
	const auto row_ids = FlatVector::GetData<row_t>(result);

	idx_t output_idx = 0;
	sstate.scanner.Scan(*tree, [&](const RTreeEntry &entry, const idx_t &) {
		// Does this entry intersect with the query bounds?
		if (!sstate.query_bounds.Intersects(entry.bounds)) {
			// No, skip it
			return RTreeScanResult::SKIP;
		}
		// Is this a row id?
		if (entry.pointer.IsRowId()) {
			row_ids[output_idx++] = entry.pointer.GetRowId();
			// Have we filled the result vector?
			if (output_idx == STANDARD_VECTOR_SIZE) {
				return RTreeScanResult::YIELD;
			}
		}
		// Continue scanning
		return RTreeScanResult::CONTINUE;
	});
	return output_idx;
}

//! Estimate the fraction of indexed rows whose bounds intersect the query, by descending the top levels of the R-tree
//! and partition each node's weight equally over its children. Node capacities are bounded (min/max capacity), so
//! same-level subtrees hold roughly equal row counts, which makes this a much better estimate on spatially skewed data
//! After the node budget is exhausted, remaining partial overlaps fall back to a fractional area estimate.
static double EstimateOverlap(const RTree &tree, const RTreeEntry &entry, const RTreeBounds &query,
                              idx_t &node_budget) {
	if (!query.Intersects(entry.bounds)) {
		// Disjoint: nothing below this entry can match
		return 0.0;
	}
	if (query.Contains(entry.bounds)) {
		// Fully contained: everything below this entry matches
		return 1.0;
	}
	// Partial overlap: refine by descending into the node, while we still have budget
	if (entry.pointer.IsPage() && node_budget != 0) {
		node_budget--;
		auto &node = tree.Ref(entry.pointer);
		const auto count = node.GetCount();
		if (count == 0) {
			return 0.0;
		}
		double sum = 0;
		for (idx_t i = 0; i < count; i++) {
			sum += EstimateOverlap(tree, node.begin()[i], query, node_budget);
		}
		// Each child holds roughly an equal share of this subtree's rows
		return sum / static_cast<double>(count);
	}
	// Budget exhausted (or this is a row id): fall back to the fractional bounding-box overlap
	const auto area = entry.bounds.Area();
	if (area <= 0) {
		// Degenerate bounds (e.g. a point): it intersects the query, so count it fully
		return 1.0;
	}
	return static_cast<double>(entry.bounds.OverlapArea(query)) / static_cast<double>(area);
}

double RTreeIndex::EstimateSelectivity(const RTreeBounds &query) const {
	// Bounds the number of nodes the estimate may visit.
	// Only nodes *partially* overlapping the query consume budget (disjoint and contained subtrees resolve immediately)
	// so this covers the query boundary of trees far larger than 256 nodes.
	// With the min node capacity of 50, two fully descended levels resolve to ~1/(50*50) = 0.04% of the indexed rows,
	// far below the default 7.5% rtree_index_scan_ratio threshold the estimate is compared to.
	static constexpr idx_t ESTIMATE_NODE_BUDGET = 256;

	idx_t node_budget = ESTIMATE_NODE_BUDGET;
	return EstimateOverlap(*tree, tree->GetRoot(), query, node_budget);
}

bool RTreeIndex::ShouldUseIndexScan(ClientContext &context, const RTreeBounds &query, idx_t total_rows) const {
	const auto estimated_rows = EstimateSelectivity(query) * static_cast<double>(total_rows);
	const auto max_ratio = SpatialSettings::RTreeIndexScanRatio(context);
	const auto min_rows = SpatialSettings::RTreeIndexScanMinRows(context);
	// Use the index if the estimated number of matching rows is below the ratio threshold, or small enough in absolute
	// terms that the plan choice does not matter
	return estimated_rows <= MaxValue(max_ratio * static_cast<double>(total_rows), static_cast<double>(min_rows));
}

void RTreeIndex::CommitDrop(IndexLock &index_lock) {
	// TODO: Maybe we can drop these much earlier?
	tree->Reset();
}

template <class CALLBACK = std::function<void(const RTreeEntry &)>>
static void ConvertToEntries(Vector &box_vec, Vector &rowid_vec, idx_t count, CALLBACK &&callback) {
	const auto &box_validity = FlatVector::Validity(box_vec);

	const auto &box_entries = StructVector::GetEntries(box_vec);
	const auto box_xmin_data = FlatVector::GetData<float>(*box_entries[0]);
	const auto box_ymin_data = FlatVector::GetData<float>(*box_entries[1]);
	const auto box_xmax_data = FlatVector::GetData<float>(*box_entries[2]);
	const auto box_ymax_data = FlatVector::GetData<float>(*box_entries[3]);

	UnifiedVectorFormat rowid_format;
	rowid_vec.ToUnifiedFormat(count, rowid_format);
	const auto row_data = UnifiedVectorFormat::GetData<row_t>(rowid_format);

	for (idx_t i = 0; i < count; i++) {
		const auto row_idx = rowid_format.sel->get_index(i);
		if (!box_validity.RowIsValid(i) || !rowid_format.validity.RowIsValid(row_idx)) {
			continue;
		}

		Box2D<float> box;
		box.min.x = box_xmin_data[i];
		box.min.y = box_ymin_data[i];
		box.max.x = box_xmax_data[i];
		box.max.y = box_ymax_data[i];

		const auto row = row_data[row_idx];

		RTreeEntry new_entry = {RTree::MakeRowId(row), box};

		// Invoke the callback with the new entry
		callback(new_entry);
	}
}

ErrorData RTreeIndex::Insert(IndexLock &lock, DataChunk &input, Vector &row_vec) {
	const auto count = input.size();

	key_chunk.Reset();
	key_executor->ExecuteExpression(input, key_chunk.data[0]);
	key_chunk.SetCardinality(count);
	key_chunk.Flatten();

	auto &box_vec = key_chunk.data[0];

	ConvertToEntries(box_vec, row_vec, count, [&](const RTreeEntry &entry) { tree->Insert(entry); });

	return ErrorData {};
}

ErrorData RTreeIndex::Append(IndexLock &lock, DataChunk &appended_data, Vector &row_identifiers) {
	DataChunk expr_chunk;
	expr_chunk.Initialize(Allocator::DefaultAllocator(), logical_types);
	ExecuteExpressions(appended_data, expr_chunk);
	return Insert(lock, expr_chunk, row_identifiers);
}

void RTreeIndex::Delete(IndexLock &lock, DataChunk &input, Vector &row_vec) {
	const auto count = input.size();

	DataChunk expr_chunk;
	expr_chunk.Initialize(Allocator::DefaultAllocator(), logical_types);
	ExecuteExpressions(input, expr_chunk);

	key_chunk.Reset();
	key_executor->ExecuteExpression(expr_chunk, key_chunk.data[0]);
	key_chunk.SetCardinality(count);
	key_chunk.Flatten();

	auto &box_vec = key_chunk.data[0];
	ConvertToEntries(box_vec, row_vec, count, [&](const RTreeEntry &entry) { tree->Delete(entry); });
}

IndexStorageInfo RTreeIndex::SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) {

	IndexStorageInfo info;
	info.name = name;
	info.root = tree->GetRoot().pointer.Get();

	auto &leaf_allocator = tree->GetLeafAllocator();
	auto &node_allocator = tree->GetNodeAllocator();

	leaf_allocator.RemoveEmptyBuffers();
	node_allocator.RemoveEmptyBuffers();

	// Use the partial block manager to serialize allocator data.
	auto &block_manager = table_io_manager.GetIndexBlockManager();
	PartialBlockManager partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT);
	leaf_allocator.SerializeBuffers(partial_block_manager);
	node_allocator.SerializeBuffers(partial_block_manager);
	partial_block_manager.FlushPartialBlocks();

	info.allocator_infos.push_back(leaf_allocator.GetInfo());
	info.allocator_infos.push_back(node_allocator.GetInfo());

	return info;
}

IndexStorageInfo RTreeIndex::SerializeToWAL(const case_insensitive_map_t<Value> &options) {

	IndexStorageInfo info;
	info.name = name;
	info.root = tree->GetRoot().pointer.Get();

	auto &leaf_allocator = tree->GetLeafAllocator();
	auto &node_allocator = tree->GetNodeAllocator();

	leaf_allocator.RemoveEmptyBuffers();
	node_allocator.RemoveEmptyBuffers();

	info.buffers.push_back(leaf_allocator.InitSerializationToWAL());
	info.buffers.push_back(node_allocator.InitSerializationToWAL());

	info.allocator_infos.push_back(leaf_allocator.GetInfo());
	info.allocator_infos.push_back(node_allocator.GetInfo());

	return info;
}

idx_t RTreeIndex::GetInMemorySize(IndexLock &state) {
	const auto &leaf_alloc = tree->GetLeafAllocator();
	const auto &node_alloc = tree->GetNodeAllocator();
	return leaf_alloc.GetInMemorySize() + node_alloc.GetInMemorySize();
}

bool RTreeIndex::MergeIndexes(IndexLock &state, BoundIndex &other_index) {
	throw NotImplementedException("RTreeIndex::MergeIndexes() not implemented");
}

void RTreeIndex::Vacuum(IndexLock &state) {
}

void RTreeIndex::Verify(IndexLock &l) {
	throw NotImplementedException("RTreeIndex::Verify() not implemented");
}

string RTreeIndex::ToString(IndexLock &l, bool display_ascii) {
	throw NotImplementedException("RTreeIndex::ToString() not implemented");
}

void RTreeIndex::VerifyAllocations(IndexLock &state) {
}

void RTreeIndex::VerifyBuffers(IndexLock &l) {
}

//------------------------------------------------------------------------------
// Register Index Type
//------------------------------------------------------------------------------
void RTreeModule::RegisterIndex(ExtensionLoader &loader) {

	IndexType index_type;

	index_type.name = RTreeIndex::TYPE_NAME;
	index_type.create_instance = RTreeIndex::Create;
	index_type.create_plan = RTreeIndex::CreatePlan;

	// Register the index type
	auto &db = loader.GetDatabaseInstance();
	db.config.GetIndexTypes().RegisterIndexType(index_type);
}

} // namespace duckdb
