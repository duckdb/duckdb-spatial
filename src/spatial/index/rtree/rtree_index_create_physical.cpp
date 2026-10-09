#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "spatial/index/rtree/rtree_index_create_physical.hpp"
#include "spatial/index/rtree/rtree_index.hpp"
#include "spatial/index/rtree/rtree_node.hpp"
#include "spatial/util/managed_collection.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//-------------------------------------------------------------
// Physical Create RTree Index
//-------------------------------------------------------------
PhysicalCreateRTreeIndex::PhysicalCreateRTreeIndex(PhysicalPlan &physical_plan, LogicalOperator &op,
                                                   TableCatalogEntry &table, const vector<column_t> &column_ids,
                                                   unique_ptr<CreateIndexInfo> info,
                                                   vector<unique_ptr<Expression>> unbound_expressions,
                                                   idx_t estimated_cardinality)
    // Declare this operators as a EXTENSION operator
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, estimated_cardinality),
      table(table.Cast<DuckTableEntry>()), info(std::move(info)), unbound_expressions(std::move(unbound_expressions)) {

	// convert virtual column ids to storage column ids
	for (auto &column_id : column_ids) {
		storage_ids.push_back(table.GetColumns().LogicalToPhysical(LogicalIndex(column_id)).index);
	}
}

//-------------------------------------------------------------
// Global State
//-------------------------------------------------------------
class CreateRTreeIndexGlobalState final : public GlobalSinkState {
public:
	//! Global index to be added to the table
	unique_ptr<RTreeIndex> rtree;

	//! The total number of leaf nodes in the RTree
	idx_t rtree_size = 0;
	idx_t slice_size = 0;
	idx_t rtree_level = 0;

	AllocatedData slice_buffer;

	//! The bottom most layer of the RTree
	ManagedCollection<RTreeEntry> curr_layer;
	ManagedCollection<RTreeEntry> next_layer;

	ManagedCollectionAppendState append_state;
	ManagedCollectionScanState scan_state;

	ManagedCollection<RTreeEntry> *curr_layer_ptr;
	ManagedCollection<RTreeEntry> *next_layer_ptr;

	RTreePointer current_pointer;

	idx_t entry_idx;
	idx_t max_node_capacity;

	explicit CreateRTreeIndexGlobalState(ClientContext &context)
	    : curr_layer(BufferManager::GetBufferManager(context)), next_layer(BufferManager::GetBufferManager(context)),
	      curr_layer_ptr(&next_layer), // We swap the order here so that it initializes properly later
	      next_layer_ptr(&curr_layer) {
	}
};

unique_ptr<GlobalSinkState> PhysicalCreateRTreeIndex::GetGlobalSinkState(ClientContext &context) const {
	auto gstate = make_uniq<CreateRTreeIndexGlobalState>(context);

	// Create the index
	auto &storage = table.GetStorage();
	auto &table_manager = TableIOManager::Get(storage);
	auto &constraint_type = info->constraint_type;
	auto &db = storage.db;
	gstate->rtree =
	    make_uniq<RTreeIndex>(Identifier(info->GetIndexName().GetIdentifierName()), constraint_type, storage_ids, table_manager, unbound_expressions, db,
	                          info->options, context, IndexStorageInfo(), estimated_cardinality);

	gstate->max_node_capacity = gstate->rtree->tree->GetConfig().max_node_capacity;
	gstate->entry_idx = gstate->max_node_capacity;

	gstate->curr_layer.InitializeAppend(gstate->append_state);

	return std::move(gstate);
}

//-------------------------------------------------------------
// Sink
//-------------------------------------------------------------
SinkResultType PhysicalCreateRTreeIndex::Sink(ExecutionContext &context, DataChunk &chunk,
                                              OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CreateRTreeIndexGlobalState>();

	if (chunk.size() == 0) {
		return SinkResultType::NEED_MORE_INPUT;
	}

	// TODO: Dont flatten chunk
	chunk.Flatten();

	auto &bbox_vecs = StructVector::GetEntries(chunk.data[0]);
	const auto &rowid_data = FlatVector::GetData<row_t>(chunk.data[1]);
	const auto min_x_data = FlatVector::GetData<float>(bbox_vecs[0]);
	const auto min_y_data = FlatVector::GetData<float>(bbox_vecs[1]);
	const auto max_x_data = FlatVector::GetData<float>(bbox_vecs[2]);
	const auto max_y_data = FlatVector::GetData<float>(bbox_vecs[3]);

	// Vectorized conversion from columnar to row-wise
	RTreeEntry entries[STANDARD_VECTOR_SIZE];
	for (idx_t elem_idx = 0; elem_idx < chunk.size(); elem_idx++) {
		auto &entry = entries[elem_idx];
		entry.pointer = RTree::MakeRowId(rowid_data[elem_idx]);
		entry.bounds.min.x = min_x_data[elem_idx];
		entry.bounds.min.y = min_y_data[elem_idx];
		entry.bounds.max.x = max_x_data[elem_idx];
		entry.bounds.max.y = max_y_data[elem_idx];
	}

	// Append the chunk to the current layer
	gstate.curr_layer.Append(gstate.append_state, entries, entries + chunk.size());

	// Count the number of entries
	gstate.rtree_size += chunk.size();

	return SinkResultType::NEED_MORE_INPUT;
}

//-------------------------------------------------------------
// RTree Construction
//-------------------------------------------------------------
static TaskExecutionResult BuildRTreeBottomUp(CreateRTreeIndexGlobalState &state, TaskExecutionMode mode,
                                              Event &event) {
	auto &tree = *state.rtree->tree;

	auto slice_begin = reinterpret_cast<RTreeEntry *>(state.slice_buffer.get());
	auto slice_end = slice_begin + state.slice_size;

	// Now, we have our base layer with all the leaves, we need to build the rest of the tree layer by layer
	while (state.curr_layer_ptr->Count() != 1) {
		if (state.scan_state.IsDone()) {

			// Swap the layers and initialize the next layer
			std::swap(state.curr_layer_ptr, state.next_layer_ptr);

			if (state.curr_layer_ptr->Count() == 1) {
				// We are done!
				break;
			}

			// Current layer size, divided by the node capacity (rounded up)
			const auto next_layer_size =
			    (state.curr_layer_ptr->Count() + state.max_node_capacity - 1) / state.max_node_capacity;
			state.next_layer_ptr->Clear();
			state.next_layer_ptr->InitializeAppend(state.append_state, next_layer_size);
			state.curr_layer_ptr->InitializeScan(state.scan_state, true);
		}

		idx_t child_idx = state.max_node_capacity;
		RTreePointer current_ptr;
		bool needs_insertion = false;

		auto scan_count = state.curr_layer_ptr->Scan(state.scan_state, slice_begin, slice_end);

		while (scan_count != 0) {

			// Sort the slice by the bounding box y-min value
			std::sort(slice_begin, slice_begin + scan_count, [&](const RTreeEntry &a, const RTreeEntry &b) {
				return a.bounds.Center().y < b.bounds.Center().y;
			});

			// Now we can build the next layer
			idx_t scan_idx = 0;
			while (scan_idx < scan_count) {

				// Initialize a new node if we have to
				if (child_idx == state.max_node_capacity) {
					auto node_type = state.rtree_level == 0 ? RTreeNodeType::LEAF_PAGE : RTreeNodeType::BRANCH_PAGE;
					current_ptr = tree.MakePage(node_type);
					child_idx = 0;
					needs_insertion = true;
				}

				const auto remaining_capacity = state.max_node_capacity - child_idx;
				const auto remaining_elements = scan_count - scan_idx;

				// Dereference the current node
				auto &node = tree.RefMutable(current_ptr);

				for (idx_t j = 0; j < MinValue<idx_t>(remaining_capacity, remaining_elements); j++) {
					node.PushEntry(slice_begin[scan_idx++]);
					child_idx++;
				}

				if (child_idx == state.max_node_capacity) {
					// Append the current node to the layer
					if (current_ptr.GetType() == RTreeNodeType::LEAF_PAGE) {
						// If the node is a leaf node, sort it by row id
						node.SortEntriesByRowId();
					}
					auto node_bounds = node.GetBounds();
					state.next_layer_ptr->Append(state.append_state, RTreeEntry {current_ptr, node_bounds});
					needs_insertion = false;

					node.Verify(state.max_node_capacity);
				}
			}

			// Scan the next batch
			scan_count = state.curr_layer_ptr->Scan(state.scan_state, slice_begin, slice_end);
		}

		// If the layer was exhausted before we filled the last node, we need to insert it now
		if (needs_insertion) {
			auto &node = tree.RefMutable(current_ptr);
			if (current_ptr.GetType() == RTreeNodeType::LEAF_PAGE) {
				// If the node is a leaf node, sort it by row id
				node.SortEntriesByRowId();
			}
			auto node_bounds = node.GetBounds();
			state.next_layer_ptr->Append(state.append_state, RTreeEntry {current_ptr, node_bounds});
			needs_insertion = false;
		}

		// We are done with this layer, pop it
		state.rtree_level++;

		// Yield if we are in partial mode and the scan is exhausted
		if (mode == TaskExecutionMode::PROCESS_PARTIAL) {
			return TaskExecutionResult::TASK_NOT_FINISHED;
		}
	}

	// Set the root node!
	auto root = state.curr_layer_ptr->Fetch(0);

	if (root.pointer.GetType() == RTreeNodeType::ROW_ID) {
		// Create a leaf node to hold this row id
		auto root_leaf_ptr = tree.MakePage(RTreeNodeType::LEAF_PAGE);
		auto &node = tree.RefMutable(root_leaf_ptr);
		node.PushEntry(RTreeEntry {root.pointer, root.bounds});
		tree.SetRoot(RTreeEntry {root_leaf_ptr, root.bounds});
	} else {
		// Else, just set the root node
		D_ASSERT(root.pointer.IsPage());
		auto root_entry = state.curr_layer_ptr->Fetch(0);
		tree.SetRoot(root_entry);
	}

	event.FinishTask();
	return TaskExecutionResult::TASK_FINISHED;
}

class RTreeIndexConstructionTask final : public ExecutorTask {
public:
	RTreeIndexConstructionTask(shared_ptr<Event> event_p, ClientContext &context, CreateRTreeIndexGlobalState &gstate,
	                           const PhysicalCreateRTreeIndex &op)
	    : ExecutorTask(context, std::move(event_p), op), state(gstate) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		return BuildRTreeBottomUp(state, mode, *event);
	}

private:
	CreateRTreeIndexGlobalState &state;
};

static void AddIndexToCatalog(ClientContext &context, CreateRTreeIndexGlobalState &gstate, CreateIndexInfo &info,
                              DuckTableEntry &table) {

	// Now actually add the index to the storage
	auto &storage = table.GetStorage();

	if (!storage.IsRoot()) {
		throw TransactionException("Cannot create index on non-root transaction");
	}

	// Create the index entry in the catalog
	auto &schema = table.schema;

	if (schema.GetEntry(schema.GetCatalogTransaction(context), CatalogType::INDEX_ENTRY, info.GetIndexName())) {
		if (info.on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
			throw CatalogException("Index with name \"%s\" already exists", info.GetIndexName());
		}
		// IF NOT EXISTS on existing index. We are done.
		// TODO: Early out before this.
		return;
	}

	const auto index_entry = schema.CreateIndex(schema.GetCatalogTransaction(context), info, table).get();
	D_ASSERT(index_entry);
	auto &duck_index = index_entry->Cast<DuckIndexEntry>();
	duck_index.initial_index_size = gstate.rtree->Cast<BoundIndex>().GetInMemorySize();

	// Finally add it to storage
	storage.AddIndex(std::move(gstate.rtree), duck_index.oid);
}

class RTreeIndexConstructionEvent final : public BasePipelineEvent {
public:
	RTreeIndexConstructionEvent(CreateRTreeIndexGlobalState &gstate_p, Pipeline &pipeline_p, CreateIndexInfo &info_p,
	                            DuckTableEntry &table_p, const PhysicalCreateRTreeIndex &op_p)
	    : BasePipelineEvent(pipeline_p), gstate(gstate_p), info(info_p), table(table_p), op(op_p) {
	}

	void Schedule() override {
		auto &context = pipeline->GetClientContext();

		// We only schedule 1 task, as the bottom-up construction is single-threaded.
		vector<shared_ptr<Task>> tasks;
		tasks.push_back(make_uniq<RTreeIndexConstructionTask>(shared_from_this(), context, gstate, op));
		SetTasks(std::move(tasks));
	}

	void FinishEvent() override {
		AddIndexToCatalog(pipeline->GetClientContext(), gstate, info, table);
	}

private:
	CreateRTreeIndexGlobalState &gstate;
	CreateIndexInfo &info;
	DuckTableEntry &table;
	const PhysicalCreateRTreeIndex &op;
};

//-------------------------------------------------------------
// Finalize
//-------------------------------------------------------------
SinkFinalizeType PhysicalCreateRTreeIndex::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                    OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<CreateRTreeIndexGlobalState>();
	info->column_ids = storage_ids;

	if (gstate.rtree_size == 0) {
		// No entries to build the RTree from, we are done
		AddIndexToCatalog(context, gstate, *info, table);
		return SinkFinalizeType::READY;
	}

	// Otherwise, we need to build the RTree

	// Calculate the vertical slice size
	// square root of the total number of entries divide by the capacity of a node, rounded up
	gstate.slice_size = ExactNumericCast<idx_t>(std::ceil(
	                        std::sqrt((gstate.rtree_size + gstate.max_node_capacity - 1) / gstate.max_node_capacity))) *
	                    gstate.max_node_capacity;

	// Allocate a buffer for the vertical slice
	// (this can get quite large, so we allocate it on the buffer manager)
	gstate.slice_buffer =
	    BufferManager::GetBufferManager(context).GetBufferAllocator().Allocate(gstate.slice_size * sizeof(RTreeEntry));

	// Schedule the construction of the RTree
	auto construction_event = make_uniq<RTreeIndexConstructionEvent>(gstate, pipeline, *info, table, *this);
	event.InsertEvent(std::move(construction_event));

	return SinkFinalizeType::READY;
}

} // namespace duckdb
