#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
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
RTreeIndex::RTreeIndex(const Identifier &name, IndexConstraintType index_constraint_type,
                       const vector<column_t> &column_ids, TableIOManager &table_io_manager,
                       const vector<unique_ptr<Expression>> &unbound_expressions, AttachedDatabase &db,
                       const case_insensitive_map_t<Value> &options, ClientContext &context,
                       const IndexStorageInfo &info, idx_t estimated_cardinality)
    : BoundIndex(Identifier(name), TYPE_NAME, index_constraint_type, column_ids, table_io_manager, unbound_expressions,
                 db) {

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
	auto &source_type = unbound_expressions[0]->GetReturnType();
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "ST_Extent_Approx"));
	const auto &func = *entry.functions.GetFunctionByArguments(context, {source_type});
	auto child_expr = make_uniq<BoundReferenceExpression>(source_type, 0);

	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child_expr));

	key_expr = func.Bind(context, std::move(children));
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
	const auto row_ids = FlatVector::GetDataMutable<row_t>(result);

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

void RTreeIndex::ResetStorage(IndexLock &index_lock) {
	// TODO: Maybe we can drop these much earlier?
	tree->Reset();
}

template <class CALLBACK = std::function<void(const RTreeEntry &)>>
static void ConvertToEntries(Vector &box_vec, Vector &rowid_vec, idx_t count, CALLBACK &&callback) {
	const auto &box_validity = FlatVector::Validity(box_vec);
	const auto &row_validity = FlatVector::Validity(rowid_vec);

	auto &box_entries = StructVector::GetEntries(box_vec);
	const auto box_xmin_data = FlatVector::GetData<float>(box_entries[0]);
	const auto box_ymin_data = FlatVector::GetData<float>(box_entries[1]);
	const auto box_xmax_data = FlatVector::GetData<float>(box_entries[2]);
	const auto box_ymax_data = FlatVector::GetData<float>(box_entries[3]);

	const auto row_data = FlatVector::GetData<row_t>(rowid_vec);

	for (idx_t i = 0; i < count; i++) {
		if (!box_validity.RowIsValid(i) || !row_validity.RowIsValid(i)) {
			continue;
		}

		Box2D<float> box;
		box.min.x = box_xmin_data[i];
		box.min.y = box_ymin_data[i];
		box.max.x = box_xmax_data[i];
		box.max.y = box_ymax_data[i];

		const auto row = row_data[i];

		RTreeEntry new_entry = {RTree::MakeRowId(row), box};

		// Invoke the callback with the new entry
		callback(new_entry);
	}
}

ErrorData RTreeIndex::Insert(IndexLock &lock, DataChunk &input, Vector &row_vec) {

	key_chunk.Reset();
	key_executor->ExecuteExpression(input, key_chunk.data[0]);
	key_chunk.Flatten();

	auto &box_vec = key_chunk.data[0];

	ConvertToEntries(box_vec, row_vec, input.size(), [&](const RTreeEntry &entry) { tree->Insert(entry); });

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

idx_t RTreeIndex::GetInMemorySize(IndexLock &state) const {
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
