#include "spatial/index/rtree/rtree_module.hpp"
#include "spatial/index/rtree/rtree_index.hpp"
#include "spatial/index/rtree/rtree_index_scan.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/spatial_types.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

BindInfo RTreeIndexScanBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<RTreeIndexScanBindData>();
	return BindInfo(bind_data.table);
}

//-------------------------------------------------------------------------
// Global State
//-------------------------------------------------------------------------
struct RTreeIndexScanGlobalState final : public GlobalTableFunctionState {
	//! How to actually produce the rows.
	//! - INDEX_SCAN fetches the rows matching the query bounds, using the row ids collected from the index,
	//! - TABLE_SCAN falls back to a regular parallel table scan.
	//! (used when the index cannot be used, or is not selective enough to make random fetches worth it).
	enum class ScanMode { INDEX_SCAN, TABLE_SCAN };
	ScanMode mode = ScanMode::INDEX_SCAN;

	//! The maximum number of threads for this scan
	idx_t max_threads = 1;

	//! The storage column ids to fetch
	vector<StorageIndex> column_ids;
	//! The types of all scanned columns, including filter columns that are removed afterwards
	vector<LogicalType> scanned_types;
	vector<idx_t> projection_ids;

	//! The row ids matching the query bounds, collected from the index when the scan is initialized (sorted)
	vector<row_t> row_ids;
	//! Keeps the collected row ids valid, by preventing a rowid-shifting vacuum while we fetch them
	unique_ptr<StorageLockKey> vacuum_lock;

	//! Lock protecting the shared state below
	mutex lock;
	//! The offset of the next batch of row ids to fetch
	idx_t next_row_offset = 0;
	//! Whether a thread has been assigned to scan the transaction-local storage
	bool local_storage_scan_assigned = false;

	//! The parallel scan state for TABLE_SCAN mode (also covers the transaction-local storage)
	ParallelTableScanState table_scan_state;

	idx_t MaxThreads() const override {
		return max_threads;
	}
	bool CanRemoveFilterColumns() const {
		return !projection_ids.empty();
	}
};

//-------------------------------------------------------------------------
// Deferred bounds resolution
//-------------------------------------------------------------------------

//! Look for bounding-box filters (as pushed by the spatial join, i.e. "ST_Intersects_Extent(col, <const>)")
//! and intersect the bounds of all constants found
static void ExtractBoundsFromExpression(const Expression &expr, RTreeBounds &bounds, bool &found) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			ExtractBoundsFromExpression(*child, bounds, found);
		}
		return;
	}
	// Optional filters only skip row-level evaluation; their bounds are still valid for the scan
	auto optional_child = ExpressionFilter::GetOptionalFilterChild(expr);
	if (optional_child) {
		ExtractBoundsFromExpression(*optional_child, bounds, found);
		return;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	const auto &name = func.Function().GetName().GetIdentifierName();
	if (!StringUtil::CIEquals(name, "ST_Intersects_Extent") && name != "&&") {
		return;
	}
	for (auto &child : func.GetChildren()) {
		if (child->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			continue;
		}
		auto &value = child->Cast<BoundConstantExpression>().GetValue();
		RTreeBounds child_bounds;
		if (!Serde::TryGetBounds(value, child_bounds)) {
			continue;
		}
		if (!found) {
			bounds = child_bounds;
			found = true;
		} else {
			// Intersect with the bounds we already have. Note that this per-axis meet may come out "inverted"
			// (min > max) when the boxes are disjoint, and that is important: a row box that overlaps both inputs
			// on an axis always spans [max-of-mins, min-of-maxes] on that axis, so the standard overlap test
			// against the raw (possibly inverted) meet still admits every row that can pass both filters.
			// Do NOT normalize or empty-check this box.
			bounds.min.x = MaxValue(bounds.min.x, child_bounds.min.x);
			bounds.min.y = MaxValue(bounds.min.y, child_bounds.min.y);
			bounds.max.x = MinValue(bounds.max.x, child_bounds.max.x);
			bounds.max.y = MinValue(bounds.max.y, child_bounds.max.y);
		}
	}
}

static void ExtractBoundsFromFilter(const TableFilter &filter, RTreeBounds &bounds, bool &found) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "RTreeIndexScan::ExtractBoundsFromFilter");
	ExtractBoundsFromExpression(*expr_filter.expr, bounds, found);
}

//! Collect the row ids of all index entries intersecting the query bounds.
//! Returns false (and stops early) if there are more than max_rows of them.
static bool CollectRowIds(const IndexReadHandle<RTreeIndex> &index, const RTreeBounds &query_bounds, idx_t max_rows,
                          vector<row_t> &result) {
	auto scan_state = index->InitializeScan(query_bounds);
	Vector row_ids(LogicalType::ROW_TYPE);
	while (true) {
		const auto count = index->Scan(*scan_state, row_ids);
		if (count == 0) {
			return true;
		}
		if (result.size() + count > max_rows) {
			return false;
		}
		const auto data = FlatVector::GetData<row_t>(row_ids);
		result.insert(result.end(), data, data + count);
	}
}

static unique_ptr<GlobalTableFunctionState> RTreeIndexScanInitGlobal(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RTreeIndexScanBindData>();
	auto result = make_uniq<RTreeIndexScanGlobalState>();

	// Both the parallel fetch of the index scan and the fallback table scan parallelize over the storage
	result->max_threads = bind_data.table.GetStorage().MaxThreads(context);

	// Figure out the storage column ids
	// (this also carries over any struct field extracts pushed down into the scan)
	result->column_ids.reserve(input.column_indexes.size());
	for (auto &col_idx : input.column_indexes) {
		result->column_ids.push_back(bind_data.table.GetStorageIndex(col_idx));
	}

	auto &storage = bind_data.table.GetStorage();
	if (bind_data.index_entry->GetBindState() != IndexBindState::BOUND) {
		// The index has been dropped since the plan was made
		result->mode = RTreeIndexScanGlobalState::ScanMode::TABLE_SCAN;
	} else {
		// Exclude rowid-shifting vacuum until we have fetched the collected row ids (mirroring the ART index scan)
		auto &attached = storage.GetAttached();
		if (attached.GetVacuumRebuildIndexThreshold() > 0 ||
		    StorageCompatibility::FromDatabase(attached).CanPersistRowIdGaps()) {
			result->vacuum_lock = DuckTransactionManager::Get(attached).SharedVacuumLock();
		}

		// Only keep the index locked while collecting the matching row ids, so that we do not block concurrent
		// commits and checkpoints (which need exclusive access to the index) for the duration of the scan.
		auto index_handle = bind_data.index_entry->GetReadHandle<RTreeIndex>();
		const auto total_rows = storage.GetTotalRows();

		// Resolve the query bounds
		auto query_bounds = bind_data.bbox;
		if (bind_data.deferred_bounds) {
			// The bounds were not known at plan time: try to resolve them from a bounding-box filter pushed into this
			// scan at runtime (e.g. by a spatial join build side).
			RTreeBounds filter_bounds;
			bool found = false;
			if (input.filters) {
				// The index stores *physical* column ids, while the scan's column ids are *logical*.
				// These diverge when the table has generated columns, so convert before comparing.
				const auto &indexed_columns = index_handle->GetColumnIds();
				const auto indexed_column =
				    bind_data.table.GetColumns().PhysicalToLogical(PhysicalIndex(indexed_columns[0])).index;
				for (const auto &entry : *input.filters) {
					// Only consider filters on the indexed column (the filter keys index into the scanned columns)
					const auto proj_idx = entry.GetIndex().GetIndex();
					if (proj_idx >= input.column_ids.size() || input.column_ids[proj_idx] != indexed_column) {
						continue;
					}
					ExtractBoundsFromFilter(entry.Filter(), filter_bounds, found);
				}
			}
			if (!found) {
				// No filter arrived: fall back to a full table scan
				result->mode = RTreeIndexScanGlobalState::ScanMode::TABLE_SCAN;
			} else if (index_handle->ShouldUseIndexScan(context, filter_bounds, total_rows)) {
				// The filter is estimated to be selective enough that random fetches beat a seq scan
				query_bounds = filter_bounds;
			} else {
				result->mode = RTreeIndexScanGlobalState::ScanMode::TABLE_SCAN;
			}
		}

		if (result->mode == RTreeIndexScanGlobalState::ScanMode::INDEX_SCAN) {
			const auto max_rows = RTreeIndex::MaxIndexScanRows(context, total_rows);
			if (!CollectRowIds(index_handle, query_bounds, max_rows, result->row_ids)) {
				// Many more rows match than estimated: a table scan is cheaper
				result->mode = RTreeIndexScanGlobalState::ScanMode::TABLE_SCAN;
			} else if (index_handle->MayMissVisibleRows(DuckTransaction::Get(context, bind_data.table.catalog))) {
				// Rows deleted by a commit that this transaction cannot see yet are already gone from the index.
				// This is checked after collecting the row ids, as any such deletes are only applied with the index
				// exclusively locked.
				result->mode = RTreeIndexScanGlobalState::ScanMode::TABLE_SCAN;
			}
		}
	}

	if (result->mode == RTreeIndexScanGlobalState::ScanMode::INDEX_SCAN) {
		// Fetch the rows in storage order
		std::sort(result->row_ids.begin(), result->row_ids.end());
	} else {
		result->row_ids.clear();
		result->vacuum_lock.reset();
		// Initialize the parallel table scan state for the fallback
		storage.InitializeParallelScan(context, result->table_scan_state, input.column_indexes);
	}

	// Early out if there is nothing to project
	if (!input.CanRemoveFilterColumns()) {
		return std::move(result);
	}

	// We need this to project out what we scan from the underlying table.
	result->projection_ids = input.projection_ids;

	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	const auto &columns = duck_table.GetColumns();
	for (const auto &col_idx : input.column_indexes) {
		if (col_idx.IsRowIdColumn()) {
			result->scanned_types.emplace_back(LogicalType::ROW_TYPE);
		} else if (col_idx.HasType()) {
			// The column may have a pushed down struct field extract, in which case we scan the extracted type
			result->scanned_types.push_back(col_idx.GetScanType());
		} else {
			result->scanned_types.push_back(columns.GetColumn(col_idx.ToLogical()).Type());
		}
	}

	return std::move(result);
}

//-------------------------------------------------------------------------
// Local State
//-------------------------------------------------------------------------
struct RTreeIndexScanLocalState final : public LocalTableFunctionState {
	//! The fetch state used to fetch rows from the main storage
	ColumnFetchState fetch_state;
	//! Scan state for the transaction-local storage
	TableScanState local_storage_state;
	vector<StorageIndex> column_ids;
	//! The DataChunk containing all read columns.
	//! This includes filter columns, which are immediately removed.
	DataChunk all_columns;
	//! Whether this thread is in charge of scanning the transaction-local storage
	bool in_charge_of_local_storage = false;
};

static unique_ptr<LocalTableFunctionState> RTreeIndexScanInitLocal(ExecutionContext &context,
                                                                   TableFunctionInitInput &input,
                                                                   GlobalTableFunctionState *global_state) {
	auto &bind_data = input.bind_data->Cast<RTreeIndexScanBindData>();
	auto &g_state = global_state->Cast<RTreeIndexScanGlobalState>();
	auto result = make_uniq<RTreeIndexScanLocalState>();

	result->column_ids = g_state.column_ids;
	result->local_storage_state.Initialize(result->column_ids, context.client, input.filters);

	if (g_state.mode == RTreeIndexScanGlobalState::ScanMode::INDEX_SCAN) {
		// Setup the scan state for the local storage
		auto &local_storage = LocalStorage::Get(context.client, bind_data.table.catalog);
		local_storage.InitializeScan(bind_data.table.GetStorage(), result->local_storage_state.local_state,
		                             input.filters);
	} else {
		// Fallback table scan: grab the first range to scan (this also covers the transaction-local storage)
		bind_data.table.GetStorage().NextParallelScan(context.client, g_state.table_scan_state,
		                                              result->local_storage_state);
	}

	if (g_state.CanRemoveFilterColumns()) {
		result->all_columns.Initialize(context.client, g_state.scanned_types);
	}
	return std::move(result);
}

//-------------------------------------------------------------------------
// Execute
//-------------------------------------------------------------------------
static void RTreeIndexScanExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {

	auto &bind_data = data_p.bind_data->Cast<RTreeIndexScanBindData>();
	auto &g_state = data_p.global_state->Cast<RTreeIndexScanGlobalState>();
	auto &l_state = data_p.local_state->Cast<RTreeIndexScanLocalState>();
	auto &transaction = DuckTransaction::Get(context, bind_data.table.catalog);

	if (g_state.mode == RTreeIndexScanGlobalState::ScanMode::TABLE_SCAN) {
		// Fallback: a regular parallel table scan (mirroring the seq_scan table function)
		auto &storage = bind_data.table.GetStorage();
		while (true) {
			if (!g_state.CanRemoveFilterColumns()) {
				storage.Scan(transaction, output, l_state.local_storage_state);
			} else {
				l_state.all_columns.Reset();
				storage.Scan(transaction, l_state.all_columns, l_state.local_storage_state);
				output.ReferenceColumns(l_state.all_columns, g_state.projection_ids);
			}
			if (output.size() > 0) {
				return;
			}
			const auto next = storage.NextParallelScan(context, g_state.table_scan_state, l_state.local_storage_state);
			if (data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR) {
				// We can avoid looping, and just return as appropriate
				data_p.async_result = next.IsValid() ? AsyncResultType::HAVE_MORE_OUTPUT : AsyncResultType::FINISHED;
				return;
			}
			if (!next.IsValid()) {
				return;
			}
		}
	}

	enum class ExecutionPhase { NONE, STORAGE, LOCAL_STORAGE };

	// We might need to loop back if a fetched batch turns out to be empty
	while (true) {
		idx_t row_offset = 0;
		idx_t row_count = 0;
		auto phase = ExecutionPhase::NONE;
		{
			// Grab the next batch of row ids while holding the lock
			lock_guard<mutex> guard(g_state.lock);
			if (g_state.next_row_offset < g_state.row_ids.size()) {
				row_offset = g_state.next_row_offset;
				row_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, g_state.row_ids.size() - row_offset);
				g_state.next_row_offset += row_count;
				phase = ExecutionPhase::STORAGE;
			} else {
				// All row ids have been handed out: the first thread to get here is assigned to scan whatever is in the
				// transaction-local storage, all other threads are done.
				if (!g_state.local_storage_scan_assigned) {
					g_state.local_storage_scan_assigned = true;
					l_state.in_charge_of_local_storage = true;
				}
				if (l_state.in_charge_of_local_storage) {
					phase = ExecutionPhase::LOCAL_STORAGE;
				}
			}
		}

		switch (phase) {
		case ExecutionPhase::NONE: {
			// No more work to pick up
			return;
		}
		case ExecutionPhase::STORAGE: {
			// Fetch the data from the main storage given the row ids, in parallel, without holding the lock
			// (the collected row ids are not modified after initialization)
			Vector row_ids(LogicalType::ROW_TYPE, data_ptr_cast(g_state.row_ids.data() + row_offset), row_count);
			if (!g_state.CanRemoveFilterColumns()) {
				bind_data.table.GetStorage().Fetch(transaction, output, g_state.column_ids, row_ids, row_count,
				                                   l_state.fetch_state);
			} else {
				// We need to first fetch into our scan chunk, and then project out the result
				l_state.all_columns.Reset();
				bind_data.table.GetStorage().Fetch(transaction, l_state.all_columns, g_state.column_ids, row_ids,
				                                   row_count, l_state.fetch_state);
				output.ReferenceColumns(l_state.all_columns, g_state.projection_ids);
			}
			if (output.size() == 0) {
				if (data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR) {
					// We can avoid looping, and just return as appropriate
					data_p.async_result = AsyncResultType::HAVE_MORE_OUTPUT;
					return;
				}
				// The whole batch got filtered out (e.g. deleted rows), loop back and grab more work
				continue;
			}
			return;
		}
		case ExecutionPhase::LOCAL_STORAGE: {
			// Scan the transaction-local storage, sequentially, always on the same thread.
			// This won't be indexed, but at least we get the correct results.
			auto &local_storage = LocalStorage::Get(transaction);
			if (!g_state.CanRemoveFilterColumns()) {
				local_storage.Scan(l_state.local_storage_state.local_state, l_state.column_ids, output);
			} else {
				// We need to scan into our scan chunk, and then project out the result
				l_state.all_columns.Reset();
				local_storage.Scan(l_state.local_storage_state.local_state, l_state.column_ids, l_state.all_columns);
				output.ReferenceColumns(l_state.all_columns, g_state.projection_ids);
			}
			return;
		}
		}
	}
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
// Virtual Columns
//-------------------------------------------------------------------------
static virtual_column_map_t RTreeIndexScanGetVirtualColumns(ClientContext &context,
                                                            optional_ptr<FunctionData> bind_data_p) {
	// Only the row id is supported (not the row number, which requires scanning the table in order)
	virtual_column_map_t result;
	result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::ROW_TYPE)));
	return result;
}

static vector<column_t> RTreeIndexScanGetRowIdColumns(ClientContext &context, optional_ptr<FunctionData> bind_data) {
	vector<column_t> result;
	result.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	return result;
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
	if (bind_data.deferred_bounds) {
		result["Bounds"] = "deferred (from join filter)";
	}
	return result;
}

//-------------------------------------------------------------------------
// De/Serialize
//-------------------------------------------------------------------------
static void RTreeScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                               const BoundTableFunction &function) {
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
	serializer.WritePropertyWithDefault<bool>(105, "deferred_bounds", bind_data.deferred_bounds, false);
}

static unique_ptr<FunctionData> RTreeScanDeserialize(Deserializer &deserializer, BoundTableFunction &function) {
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

	const auto deferred_bounds = deserializer.ReadPropertyWithExplicitDefault<bool>(105, "deferred_bounds", false);

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
			result = make_uniq<RTreeIndexScanBindData>(duck_table, index_entry, Identifier(index_name), bbox,
			                                           deferred_bounds);
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
	func.init_local = RTreeIndexScanInitLocal;
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
	func.get_virtual_columns = RTreeIndexScanGetVirtualColumns;
	func.get_row_id_columns = RTreeIndexScanGetRowIdColumns;

	return func;
}

//-------------------------------------------------------------------------
// Register
//-------------------------------------------------------------------------
void RTreeModule::RegisterIndexScan(ExtensionLoader &loader) {
	loader.RegisterFunction(RTreeIndexScanFunction::GetFunction());
}

} // namespace duckdb
