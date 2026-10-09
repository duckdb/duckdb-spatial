#pragma once

#include "spatial/index/rtree/rtree_node.hpp"
#include "duckdb/storage/table/index_entry.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {
class DuckTableEntry;
class Index;

// This is created by the optimizer rule
struct RTreeIndexScanBindData final : public TableFunctionData {
	explicit RTreeIndexScanBindData(DuckTableEntry &table, shared_ptr<IndexEntry> index_entry,
	                                Identifier index_name_p, const RTreeBounds &bbox)
	    : table(table), index_name(std::move(index_name_p)), index_entry(std::move(index_entry)), bbox(bbox) {
	}

	//! The table to scan
	DuckTableEntry &table;

	//! The index name used for display and serialization
	Identifier index_name;

	//! The index to use
	shared_ptr<IndexEntry> index_entry;

	//! The bounds to scan
	RTreeBounds bbox;

public:
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RTreeIndexScanBindData>();
		return &other.table == &table;
	}
};

struct RTreeIndexScanFunction {
	static TableFunction GetFunction();
};

} // namespace duckdb
