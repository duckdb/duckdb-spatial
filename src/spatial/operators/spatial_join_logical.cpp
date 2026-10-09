#include "spatial_join_logical.hpp"
#include "spatial_join_physical.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

LogicalSpatialJoin::LogicalSpatialJoin(JoinType join_type_p) : join_type(join_type_p) {
}

vector<ColumnBinding> LogicalSpatialJoin::GetColumnBindings() {
	auto left_bindings = MapBindings(children[0]->GetColumnBindings(), left_projection_map);
	if (join_type == JoinType::SEMI || join_type == JoinType::ANTI) {
		// for SEMI and ANTI join we only project the left hand side
		return left_bindings;
	}

	if (join_type == JoinType::MARK) {
		// for MARK join we project the left hand side plus the MARK column
		left_bindings.emplace_back(mark_index, ProjectionIndex(0));
		return left_bindings;
	}
	// for other join types we project both the LHS and the RHS
	auto right_bindings = MapBindings(children[1]->GetColumnBindings(), right_projection_map);
	if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
		return right_bindings;
	}
	left_bindings.insert(left_bindings.end(), right_bindings.begin(), right_bindings.end());
	return left_bindings;
}

void LogicalSpatialJoin::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {

	auto &cond = spatial_predicate->Cast<BoundFunctionExpression>();

	res.VisitOperator(*children[0]);
	res.VisitExpression(&cond.GetChildrenMutable()[0]);

	// TODO: Duplicate eliminated joins?

	res.VisitOperator(*children[1]);
	res.VisitExpression(&cond.GetChildrenMutable()[1]);

	// Finally, update the bindings
	bindings = GetColumnBindings();
}

void LogicalSpatialJoin::ResolveTypes() {
	types = MapTypes(children[0]->types, left_projection_map);
	if (join_type == JoinType::SEMI || join_type == JoinType::ANTI) {
		// for SEMI and ANTI join we only project the left hand side
		return;
	}
	if (join_type == JoinType::MARK) {
		// for MARK join we project the left hand side, plus a BOOLEAN column indicating the MARK
		types.emplace_back(LogicalType::BOOLEAN);
		return;
	}
	// for any other join we project both sides
	auto right_types = MapTypes(children[1]->types, right_projection_map);
	if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
		types = right_types;
		return;
	}
	types.insert(types.end(), right_types.begin(), right_types.end());
}

PhysicalOperator &LogicalSpatialJoin::CreatePlan(ClientContext &context, PhysicalPlanGenerator &generator) {

	// Return a new PhysicalSpatialJoin operator
	auto &left = generator.CreatePlan(*children[0]);
	auto &right = generator.CreatePlan(*children[1]);

	return generator.Make<PhysicalSpatialJoin>(*this, left, right, std::move(spatial_predicate), join_type,
	                                           estimated_cardinality, has_const_distance, const_distance,
	                                           std::move(filter_pushdown_targets));
}

void LogicalSpatialJoin::Serialize(Serializer &writer) const {
	LogicalExtensionOperator::Serialize(writer);
	writer.WritePropertyWithDefault(300, "operator_type", string(OPERATOR_TYPE_NAME));

	writer.WritePropertyWithDefault<JoinType>(400, "join_type", join_type, JoinType::INNER);
	writer.WritePropertyWithDefault<TableIndex>(401, "mark_index", mark_index);
	writer.WritePropertyWithDefault<vector<ProjectionIndex>>(402, "left_projection_map", left_projection_map);
	writer.WritePropertyWithDefault<vector<ProjectionIndex>>(403, "right_projection_map", right_projection_map);
	writer.WritePropertyWithDefault<unique_ptr<Expression>>(404, "spatial_predicate", spatial_predicate);
	writer.WritePropertyWithDefault<vector<unique_ptr<Expression>>>(405, "extra_conditions", extra_conditions);
	writer.WritePropertyWithDefault<bool>(406, "has_const_distance", has_const_distance);
	if (has_const_distance) {
		writer.WritePropertyWithDefault<double>(407, "const_distance", const_distance);
	}
}

unique_ptr<LogicalExtensionOperator> LogicalSpatialJoin::Deserialize(Deserializer &reader) {
	auto join_type = reader.ReadPropertyWithExplicitDefault<JoinType>(400, "join_type", JoinType::INNER);
	auto mark_index = reader.ReadPropertyWithDefault<TableIndex>(401, "mark_index");
	auto left_projection_map = reader.ReadPropertyWithDefault<vector<ProjectionIndex>>(402, "left_projection_map");
	auto right_projection_map = reader.ReadPropertyWithDefault<vector<ProjectionIndex>>(403, "right_projection_map");
	auto spatial_predicate = reader.ReadPropertyWithDefault<unique_ptr<Expression>>(404, "spatial_predicate");
	auto extra_conditions = reader.ReadPropertyWithDefault<vector<unique_ptr<Expression>>>(405, "extra_conditions");
	auto has_const_distance = reader.ReadPropertyWithExplicitDefault<bool>(406, "has_const_distance", false);
	double const_distance = 0.0;
	if (has_const_distance) {
		const_distance = reader.ReadPropertyWithDefault<double>(407, "const_distance");
	}

	auto result = make_uniq<LogicalSpatialJoin>(join_type);
	result->mark_index = mark_index;
	result->left_projection_map = std::move(left_projection_map);
	result->right_projection_map = std::move(right_projection_map);
	result->spatial_predicate = std::move(spatial_predicate);
	result->extra_conditions = std::move(extra_conditions);
	result->has_const_distance = has_const_distance;
	if (has_const_distance) {
		result->const_distance = const_distance;
	}

	return std::move(result);
}

} // namespace duckdb
