#pragma once
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/common/types/row/tuple_data_layout.hpp"

namespace duckdb {

class DynamicTableFilterSet;

//! A target into which the spatial join pushes a bounding-box filter derived from the build-side R-tree.
struct SpatialJoinPushdownTarget {
	//! The dynamic table filter set of the probe-side LogicalGet to push the filter into
	shared_ptr<DynamicTableFilterSet> dynamic_filters;
	//! The projection index of the probe-side geometry column in the LogicalGet
	ProjectionIndex probe_column_index;
	//! The type of the probe-side geometry column. The pushed filter's column reference must use this
	//! exact type (it may be a GEOMETRY with a CRS, which does not compare equal to plain GEOMETRY).
	LogicalType column_type;
};

class PhysicalSpatialJoin final : public PhysicalJoin {
public:
	static constexpr auto TYPE = PhysicalOperatorType::EXTENSION;

public:
	PhysicalSpatialJoin(PhysicalPlan &physical_plan, LogicalOperator &op, PhysicalOperator &left,
	                    PhysicalOperator &right, unique_ptr<Expression> spatial_predicate, JoinType join_type,
	                    idx_t estimated_cardinality, bool has_const_distance, double const_distance,
	                    vector<SpatialJoinPushdownTarget> filter_pushdown_targets);

	//! The condition of the join
	unique_ptr<Expression> condition;
	optional_ptr<Expression> build_side_key;
	optional_ptr<Expression> probe_side_key;

	vector<column_t> build_side_output_columns;
	vector<column_t> probe_side_output_columns;
	vector<column_t> build_side_payload_columns;

	vector<LogicalType> probe_side_output_types;
	vector<LogicalType> build_side_output_types;
	vector<LogicalType> build_side_payload_types;

	vector<LogicalType> build_side_key_types;

	shared_ptr<TupleDataLayout> layout;
	idx_t build_side_match_offset = 0; // This is the byte offset to the match column for right/outer joins

	// In case this is a ST_DWithin join, we store the constant distance here
	bool has_const_distance = false;
	double const_distance = 0.0;

	// Probe-side targets into which we push a bounding-box filter derived from the build-side R-tree
	vector<SpatialJoinPushdownTarget> filter_pushdown_targets;

public:
	// Operator Interface
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override;

	bool ParallelOperator() const override {
		return true;
	}

protected:
	// CachingOperatorState Interface
	OperatorResultType ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                                   GlobalOperatorState &gstate, OperatorState &state) const override;

public:
	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		// The PhysicalSpatialJoin is only a source if the join type is RIGHT/OUTER
		return PropagatesBuildSide(join_type);
	}

	bool ParallelSource() const override {
		return true;
	}

public:
	//! Returns the current progress percentage, or a negative value if progress bars are not supported
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

	InsertionOrderPreservingMap<string> ParamsToString() const override;
	string GetName() const override;
};

} // namespace duckdb
