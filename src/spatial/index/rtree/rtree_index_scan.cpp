#include "spatial/index/rtree/rtree_module.hpp"
#include "spatial/index/rtree/rtree_index.hpp"
#include "spatial/index/rtree/rtree_index_scan.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {

BindInfo RTreeIndexScanBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<RTreeIndexScanBindData>();
	return BindInfo(bind_data.table);
}

//-------------------------------------------------------------------------
// Global State
//-------------------------------------------------------------------------
struct RTreeIndexScanGlobalState final : public GlobalTableFunctionState {
	//! The DataChunk containing all read columns.
	//! This includes filter columns, which are immediately removed.
	DataChunk all_columns;
	vector<idx_t> projection_ids;

	ColumnFetchState fetch_state;
	TableScanState local_storage_state;
	vector<StorageIndex> column_ids;

	// Index scan state
	unique_ptr<IndexReadHandle<RTreeIndex>> index_handle;
	unique_ptr<IndexScanState> index_state;
	Vector row_ids = Vector(LogicalType::ROW_TYPE);
};

static unique_ptr<GlobalTableFunctionState> RTreeIndexScanInitGlobal(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RTreeIndexScanBindData>();
	auto result = make_uniq<RTreeIndexScanGlobalState>();

	// Setup the scan state for the local storage
	auto &local_storage = LocalStorage::Get(context, bind_data.table.catalog);
	result->column_ids.reserve(input.column_ids.size());

	// Figure out the storage column ids
	for (auto &id : input.column_ids) {
		storage_t col_id = id;
		if (id != DConstants::INVALID_INDEX) {
			col_id = bind_data.table.GetColumn(LogicalIndex(id)).StorageOid();
		}
		result->column_ids.emplace_back(col_id);
	}

	// Initialize the storage scan state
	result->local_storage_state.Initialize(result->column_ids, context, input.filters);
	local_storage.InitializeScan(bind_data.table.GetStorage(), result->local_storage_state.local_state, input.filters);

	// Initialize the scan state for the index
	result->index_handle =
	    make_uniq<IndexReadHandle<RTreeIndex>>(bind_data.index_entry->GetReadHandle<RTreeIndex>());
	result->index_state = (*result->index_handle)->InitializeScan(bind_data.bbox);

	// Early out if there is nothing to project
	if (!input.CanRemoveFilterColumns()) {
		return std::move(result);
	}

	// We need this to project out what we scan from the underlying table.
	result->projection_ids = input.projection_ids;

	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	const auto &columns = duck_table.GetColumns();
	vector<LogicalType> scanned_types;
	for (const auto &col_idx : input.column_indexes) {
		if (col_idx.IsRowIdColumn()) {
			scanned_types.emplace_back(LogicalType::ROW_TYPE);
		} else {
			scanned_types.push_back(columns.GetColumn(col_idx.ToLogical()).Type());
		}
	}
	result->all_columns.Initialize(context, scanned_types);

	return std::move(result);
}

//-------------------------------------------------------------------------
// Execute
//-------------------------------------------------------------------------
static void RTreeIndexScanExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {

	auto &bind_data = data_p.bind_data->Cast<RTreeIndexScanBindData>();
	auto &state = data_p.global_state->Cast<RTreeIndexScanGlobalState>();
	auto &transaction = DuckTransaction::Get(context, bind_data.table.catalog);

	if (state.index_handle) {
		// Scan the index for row id's
		auto row_count = (*state.index_handle)->Scan(*state.index_state, state.row_ids);
		if (row_count != 0) {
			// Fetch the data from the main storage given the row ids
			if (state.projection_ids.empty()) {
				bind_data.table.GetStorage().Fetch(transaction, output, state.column_ids, state.row_ids, row_count,
				                                   state.fetch_state);
				return;
			}

			// Otherwise, we need to first fetch into our scan chunk, and then project out the result
			state.all_columns.Reset();
			bind_data.table.GetStorage().Fetch(transaction, state.all_columns, state.column_ids, state.row_ids,
			                                   row_count, state.fetch_state);
			output.ReferenceColumns(state.all_columns, state.projection_ids);
			return;
		}

		// The persistent index is exhausted and no longer needs to be kept stable.
		state.index_state.reset();
		state.index_handle.reset();
	}

	// Scan transaction-local rows, which are not included in the index.
	auto &local_storage = LocalStorage::Get(transaction);
	if (state.projection_ids.empty()) {
		local_storage.Scan(state.local_storage_state.local_state, state.column_ids, output);
		return;
	}

	// Otherwise we need to scan into our scan chunk, and then project out the result
	state.all_columns.Reset();
	local_storage.Scan(state.local_storage_state.local_state, state.column_ids, state.all_columns);
	output.ReferenceColumns(state.all_columns, state.projection_ids);
}

//-------------------------------------------------------------------------
// Statistics
//-------------------------------------------------------------------------
static unique_ptr<BaseStatistics> RTreeIndexScanStatistics(ClientContext &context, const FunctionData *bind_data_p,
                                                           column_t column_id) {
	auto &bind_data = bind_data_p->Cast<RTreeIndexScanBindData>();
	auto &local_storage = LocalStorage::Get(context, bind_data.table.catalog);
	if (local_storage.Find(bind_data.table.GetStorage())) {
		// we don't emit any statistics for tables that have outstanding transaction-local data
		return nullptr;
	}
	return bind_data.table.GetStatistics(context, column_id);
}

//-------------------------------------------------------------------------
// Dependency
//-------------------------------------------------------------------------
void RTreeIndexScanDependency(LogicalDependencyList &entries, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<RTreeIndexScanBindData>();
	entries.AddDependency(bind_data.table);

	// TODO: Add dependency to index here?
}

//-------------------------------------------------------------------------
// Cardinality
//-------------------------------------------------------------------------
unique_ptr<NodeStatistics> RTreeIndexScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<RTreeIndexScanBindData>();
	auto &local_storage = LocalStorage::Get(context, bind_data.table.catalog);
	const auto &storage = bind_data.table.GetStorage();
	idx_t table_rows = storage.GetTotalRows();
	idx_t estimated_cardinality = table_rows + local_storage.AddedRows(bind_data.table.GetStorage());
	return make_uniq<NodeStatistics>(table_rows, estimated_cardinality);
}

//-------------------------------------------------------------------------
// ToString
//-------------------------------------------------------------------------
static InsertionOrderPreservingMap<string> RTreeIndexScanToString(TableFunctionToStringInput &input) {
	D_ASSERT(input.bind_data);
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<RTreeIndexScanBindData>();
	result["Table"] = bind_data.table.name.GetIdentifierName();
	result["Index"] = bind_data.index_name.GetIdentifierName();
	return result;
}

//-------------------------------------------------------------------------
// De/Serialize
//-------------------------------------------------------------------------
static void RTreeScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                               const TableFunction &function) {
	auto &bind_data = bind_data_p->Cast<RTreeIndexScanBindData>();
	serializer.WriteProperty(100, "catalog", bind_data.table.schema.catalog.GetName());
	serializer.WriteProperty(101, "schema", bind_data.table.schema.name);
	serializer.WriteProperty(102, "table", bind_data.table.name);
	serializer.WriteProperty(103, "index_name", bind_data.index_name);

	serializer.WriteObject(104, "bbox", [&](Serializer &ser) {
		ser.WriteProperty<float>(10, "min_x", bind_data.bbox.min.x);
		ser.WriteProperty<float>(11, "min_y", bind_data.bbox.min.y);
		ser.WriteProperty<float>(20, "max_x", bind_data.bbox.max.x);
		ser.WriteProperty<float>(21, "max_y", bind_data.bbox.max.y);
	});
}

static unique_ptr<FunctionData> RTreeScanDeserialize(Deserializer &deserializer, TableFunction &function) {
	auto &context = deserializer.Get<ClientContext &>();

	const auto catalog = deserializer.ReadProperty<string>(100, "catalog");
	const auto schema = deserializer.ReadProperty<string>(101, "schema");
	const auto table = deserializer.ReadProperty<string>(102, "table");
	auto &catalog_entry =
	    Catalog::GetEntry<TableCatalogEntry>(context, Identifier(catalog), Identifier(schema), Identifier(table));
	if (catalog_entry.type != CatalogType::TABLE_ENTRY) {
		throw SerializationException("Cant find table for %s.%s", schema, table);
	}

	// Now also lookup the index by name
	const auto index_name = deserializer.ReadProperty<string>(103, "index_name");
	RTreeBounds bbox;
	deserializer.ReadObject(104, "bbox", [&](Deserializer &ser) {
		bbox.min.x = ser.ReadProperty<float>(10, "min_x");
		bbox.min.y = ser.ReadProperty<float>(11, "min_y");
		bbox.max.x = ser.ReadProperty<float>(20, "max_x");
		bbox.max.y = ser.ReadProperty<float>(21, "max_y");
	});

	auto &duck_table = catalog_entry.Cast<DuckTableEntry>();
	auto &table_info = *catalog_entry.GetStorage().GetDataTableInfo();

	unique_ptr<RTreeIndexScanBindData> result = nullptr;

	table_info.BindIndexes(context, RTreeIndex::TYPE_NAME);
	for (auto index_entry : table_info.GetIndexes().IndexEntries()) {
		if (index_entry->GetBindState() != IndexBindState::BOUND ||
		    RTreeIndex::TYPE_NAME != index_entry->GetIndexType()) {
			continue;
		}
		if (index_entry->GetName() == index_name) {
			result = make_uniq<RTreeIndexScanBindData>(duck_table, index_entry, Identifier(index_name), bbox);
			break;
		}
	};

	if (!result) {
		throw SerializationException("Could not find index %s on table %s.%s", index_name, schema, table);
	}
	return std::move(result);
}

//-------------------------------------------------------------------------
// Get Function
//-------------------------------------------------------------------------
TableFunction RTreeIndexScanFunction::GetFunction() {
	TableFunction func("rtree_index_scan", {}, RTreeIndexScanExecute);
	func.init_local = nullptr;
	func.init_global = RTreeIndexScanInitGlobal;
	func.statistics = RTreeIndexScanStatistics;
	func.dependency = RTreeIndexScanDependency;
	func.cardinality = RTreeIndexScanCardinality;
	func.pushdown_complex_filter = nullptr;
	func.to_string = RTreeIndexScanToString;
	func.table_scan_progress = nullptr;
	func.projection_pushdown = true;
	func.filter_pushdown = false;
	func.get_bind_info = RTreeIndexScanBindInfo;
	func.serialize = RTreeScanSerialize;
	func.deserialize = RTreeScanDeserialize;
	func.parallelism = TableFunctionParallelism::SEQUENTIAL;

	return func;
}

//-------------------------------------------------------------------------
// Register
//-------------------------------------------------------------------------
void RTreeModule::RegisterIndexScan(ExtensionLoader &loader) {
	loader.RegisterFunction(RTreeIndexScanFunction::GetFunction());
}

} // namespace duckdb
