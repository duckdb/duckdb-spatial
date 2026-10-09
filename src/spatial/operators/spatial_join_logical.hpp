#pragma once

#include "duckdb/planner/operator/logical_extension_operator.hpp"

#include "spatial/operators/spatial_join_physical.hpp"

namespace duckdb {

class LogicalSpatialJoin final : public LogicalExtensionOperator {
public:
	static constexpr auto TYPE = LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR;
	static constexpr auto OPERATOR_TYPE_NAME = "logical_spatial_join";

public:
	//! The type of the join (INNER, OUTER, etc...)
	JoinType join_type;
	//! Table index used to refer to the MARK column (in case of a MARK join)
	TableIndex mark_index {};

	//! The spatial predicate of the join
	unique_ptr<Expression> spatial_predicate;

	//! Extra conditions to be applied after the join, e.g. for filtering
	vector<unique_ptr<Expression>> extra_conditions;

	//! The columns of the LHS that are output by the join
	vector<ProjectionIndex> left_projection_map;
	//! The columns of the RHS that are output by the join
	vector<ProjectionIndex> right_projection_map;
	//! Join Keys statistics (optional)
	vector<unique_ptr<BaseStatistics>> join_stats;

	bool has_const_distance = false;
	double const_distance = 0.0;

	//! Probe-side targets into which we push a bounding-box filter derived from the build-side R-tree.
	//! Populated by the optimizer; not serialized (holds runtime shared_ptrs).
	vector<SpatialJoinPushdownTarget> filter_pushdown_targets;

public:
	explicit LogicalSpatialJoin(JoinType join_type_p);

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
		return "SPATIAL_JOIN";
	}
	string GetExtensionName() const override {
		return "duckdb_spatial";
	}

protected:
	void ResolveTypes() override;
};

} // namespace duckdb
