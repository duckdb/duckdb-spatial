#include "spatial/operators/spatial_knn_join_logical.hpp"
#include "spatial/operators/spatial_knn_join_physical.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

LogicalSpatialKNNJoin::LogicalSpatialKNNJoin(JoinType join_type_p) : join_type(join_type_p) {
}

vector<ColumnBinding> LogicalSpatialKNNJoin::GetColumnBindings() {
	auto left_bindings = MapBindings(children[0]->GetColumnBindings(), left_projection_map);
	auto right_bindings = MapBindings(children[1]->GetColumnBindings(), right_projection_map);
	left_bindings.insert(left_bindings.end(), right_bindings.begin(), right_bindings.end());
	return left_bindings;
}

void LogicalSpatialKNNJoin::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	auto &cond = spatial_predicate->Cast<BoundFunctionExpression>();

	// After the optimizer, children are always in correct order:
	// children[0] = probe (arg0), children[1] = build (arg1).
	res.VisitOperator(*children[0]);
	res.VisitExpression(&cond.children[0]);
	res.VisitOperator(*children[1]);
	res.VisitExpression(&cond.children[1]);

	bindings = GetColumnBindings();
}

void LogicalSpatialKNNJoin::ResolveTypes() {
	types = MapTypes(children[0]->types, left_projection_map);
	auto right_types = MapTypes(children[1]->types, right_projection_map);
	types.insert(types.end(), right_types.begin(), right_types.end());
}

PhysicalOperator &LogicalSpatialKNNJoin::CreatePlan(ClientContext &context, PhysicalPlanGenerator &generator) {
	auto &left = generator.CreatePlan(*children[0]);
	auto &right = generator.CreatePlan(*children[1]);

	return generator.Make<PhysicalSpatialKNNJoin>(*this, left, right, std::move(spatial_predicate), join_type,
	                                              estimated_cardinality, k);
}

void LogicalSpatialKNNJoin::Serialize(Serializer &writer) const {
	LogicalExtensionOperator::Serialize(writer);
	writer.WritePropertyWithDefault(300, "operator_type", string(OPERATOR_TYPE_NAME));
	writer.WritePropertyWithDefault<JoinType>(400, "join_type", join_type, JoinType::INNER);
	writer.WritePropertyWithDefault<vector<idx_t>>(402, "left_projection_map", left_projection_map);
	writer.WritePropertyWithDefault<vector<idx_t>>(403, "right_projection_map", right_projection_map);
	writer.WritePropertyWithDefault<unique_ptr<Expression>>(404, "spatial_predicate", spatial_predicate);
	writer.WritePropertyWithDefault<int32_t>(405, "k", k, 1);
	writer.WritePropertyWithDefault<idx_t>(406, "build_child_idx", build_child_idx, static_cast<idx_t>(1));
}

unique_ptr<LogicalExtensionOperator> LogicalSpatialKNNJoin::Deserialize(Deserializer &reader) {
	auto join_type = reader.ReadPropertyWithExplicitDefault<JoinType>(400, "join_type", JoinType::INNER);
	auto left_projection_map = reader.ReadPropertyWithDefault<vector<idx_t>>(402, "left_projection_map");
	auto right_projection_map = reader.ReadPropertyWithDefault<vector<idx_t>>(403, "right_projection_map");
	auto spatial_predicate = reader.ReadPropertyWithDefault<unique_ptr<Expression>>(404, "spatial_predicate");
	auto k = reader.ReadPropertyWithExplicitDefault<int32_t>(405, "k", 1);
	auto build_child_idx = reader.ReadPropertyWithExplicitDefault<idx_t>(406, "build_child_idx", static_cast<idx_t>(1));

	auto result = make_uniq<LogicalSpatialKNNJoin>(join_type);
	result->left_projection_map = std::move(left_projection_map);
	result->right_projection_map = std::move(right_projection_map);
	result->spatial_predicate = std::move(spatial_predicate);
	result->k = k;
	result->build_child_idx = build_child_idx;

	return std::move(result);
}

} // namespace duckdb
