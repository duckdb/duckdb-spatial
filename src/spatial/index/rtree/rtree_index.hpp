#pragma once

#include "spatial/geometry/bbox.hpp"
#include "spatial/index/rtree/rtree_node.hpp"
#include "spatial/index/rtree/rtree.hpp"

#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/index_pointer.hpp"

namespace duckdb {

class PhysicalOperator;

class RTreeIndex final : public BoundIndex {
public:
	// The type name of the RTreeIndex
	static constexpr auto TYPE_NAME = "RTREE";

public:
	RTreeIndex(const Identifier &name, IndexConstraintType index_constraint_type, const vector<column_t> &column_ids,
	           TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
	           AttachedDatabase &db, const case_insensitive_map_t<Value> &options, ClientContext &context,
	           const IndexStorageInfo &info = IndexStorageInfo(), idx_t estimated_cardinality = 0);

	unique_ptr<RTree> tree;

	// To compute bounding boxes when inserting
	DataChunk key_chunk;
	unique_ptr<Expression> key_expr;
	unique_ptr<ExpressionExecutor> key_executor;

	unique_ptr<IndexScanState> InitializeScan(const Box2D<float> &query) const;
	idx_t Scan(IndexScanState &state, Vector &result) const;

	static unique_ptr<BoundIndex> Create(CreateIndexInput &input) {
		auto res = make_uniq<RTreeIndex>(input.name, input.constraint_type, input.column_ids, input.table_io_manager,
		                                 input.unbound_expressions, input.db, input.options, input.context,
		                                 input.storage_info);
		return std::move(res);
	}

	static PhysicalOperator &CreatePlan(PlanIndexInput &input);

public:
	//! Called when data is appended to the index. The lock obtained from InitializeLock must be held
	ErrorData Append(IndexLock &lock, DataChunk &entries, Vector &row_identifiers) override;

	//! Deletes all data from the index. The lock obtained from InitializeLock must be held
	void ResetStorage(IndexLock &index_lock) override;
	//! Delete a chunk of entries from the index. The lock obtained from InitializeLock must be held
	void Delete(IndexLock &lock, DataChunk &entries, Vector &row_identifiers) override;
	//! Insert a chunk of entries into the index
	ErrorData Insert(IndexLock &lock, DataChunk &data, Vector &row_ids) override;

	//! Serializes RTree memory to disk and returns the index storage information.
	IndexStorageInfo SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) override;
	//! Serializes RTree memory to the WAL and returns the index storage information.
	IndexStorageInfo SerializeToWAL(const case_insensitive_map_t<Value> &options) override;

	idx_t GetInMemorySize(IndexLock &state) override;

	//! Merge another index into this index. The lock obtained from InitializeLock must be held, and the other
	//! index must also be locked during the merge
	bool MergeIndexes(IndexLock &state, BoundIndex &other_index) override;

	//! Traverses an RTreeIndex and vacuums the qualifying nodes. The lock obtained from InitializeLock must be held
	void Vacuum(IndexLock &state) override;

	//! Traverses and verifies the index.
	//! Currently not implemented.
	void Verify(IndexLock &l) override;

	//! Returns the string representation of an index.
	//! Currently not implemented.
	string ToString(IndexLock &l, bool display_ascii = false) override;

	//! Ensures that the node allocation counts match the node counts.
	void VerifyAllocations(IndexLock &state) override;
	void VerifyBuffers(IndexLock &l) override;

	string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
	                                     DataChunk &input) override {
		return "Constraint violation in RTree index";
	}
};

} // namespace duckdb
