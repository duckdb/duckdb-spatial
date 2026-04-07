#pragma once

#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

class LogicalSpatialKNNJoin final : public LogicalExtensionOperator {
public:
	static constexpr auto TYPE = LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR;
	static constexpr auto OPERATOR_TYPE_NAME = "logical_spatial_knn_join";

public:
	JoinType join_type;

	unique_ptr<Expression> spatial_predicate;

	int32_t k = 1;
	

	// Which child is the build side (R-tree). 0 = left child, 1 = right child.
	// Default is 1 (right child is build, as is standard for DuckDB joins).
	// Set to 0 when the join order optimizer swapped children.
	idx_t build_child_idx = 1;

	vector<idx_t> left_projection_map;
	vector<idx_t> right_projection_map;
	vector<unique_ptr<BaseStatistics>> join_stats;

public:
	explicit LogicalSpatialKNNJoin(JoinType join_type_p);

	vector<ColumnBinding> GetColumnBindings() override;

	void ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) override;

	bool HasProjectionMap() const override {
		return !left_projection_map.empty() || !right_projection_map.empty();
	}

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &generator) override;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &reader);

public:
	string GetName() const override {
		return "SPATIAL_KNN_JOIN";
	}
	string GetExtensionName() const override {
		return "duckdb_spatial";
	}

protected:
	void ResolveTypes() override;
};

} // namespace duckdb
