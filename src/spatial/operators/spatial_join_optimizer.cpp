#include "spatial_join_optimizer.hpp"
#include "spatial_join_logical.hpp"
#include "spatial/util/distance_extract.hpp"
#include "spatial/spatial_types.hpp"

#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

// All of these imply bounding box intersection
static const case_insensitive_set_t spatial_predicate_map = {
    "&&",
    "ST_Intersects_Extent",
    "ST_Equals",
    "ST_Intersects",
    "ST_Touches",
    "ST_Crosses",
    "ST_Within",
    "ST_Contains",
    "ST_Overlaps",
    "ST_Covers",
    "ST_CoveredBy",
    "ST_ContainsProperly",
    "ST_WithinProperly",
    "ST_DWithin",
};

static const case_insensitive_map_t<string> spatial_predicate_inverse_map = {
    {"ST_Equals", "ST_Equals"},
    {"&&", "&&"},                                     // Symmetric
    {"ST_Intersects_Extent", "ST_Intersects_Extent"}, // Symmetric
    {"ST_Intersects", "ST_Intersects"},               // Symmetric
    {"ST_Touches", "ST_Touches"},                     // Symmetric
    {"ST_Crosses", "ST_Crosses"},                     // Symmetric
    {"ST_Within", "ST_Contains"},                     // Inverse
    {"ST_Contains", "ST_Within"},                     // Inverse
    {"ST_Overlaps", "ST_Overlaps"},                   // Symmetric
    {"ST_Covers", "ST_CoveredBy"},                    // Inverse
    {"ST_CoveredBy", "ST_Covers"},                    // Inverse
    {"ST_WithinProperly", "ST_ContainsProperly"},     // Inverse
    {"ST_ContainsProperly", "ST_WithinProperly"},     // Inverse
    {"ST_DWithin", "ST_DWithin"},                     // Symmetric (when distance is constant)
};

static bool HasInversePredicate(const string &func_name) {
	return spatial_predicate_inverse_map.find(func_name) != spatial_predicate_inverse_map.end();
}

static unique_ptr<Expression> GetInversePredicate(ClientContext &context, unique_ptr<Expression> expr) {
	auto &func = expr->Cast<BoundFunctionExpression>();

	const auto it = spatial_predicate_inverse_map.find(func.function.name);
	D_ASSERT(it != spatial_predicate_inverse_map.end());

	// Swap the arguments
	std::swap(func.children[0], func.children[1]);

	if (it->first == it->second) {
		// We've already swapped the child, so just return the expression
		return expr;
	}

	// Get the function from the catalog
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, DEFAULT_SCHEMA, it->second);
	auto inverse_func =
	    entry.functions.GetFunctionByArguments(context, {func.children[0]->return_type, func.children[1]->return_type});

	return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, inverse_func, std::move(func.children),
	                                                           nullptr, func.is_operator);
}

static bool IsSpatialJoinPredicate(const unique_ptr<Expression> &expr, const unordered_set<idx_t> &left_bindings,
                                   const unordered_set<idx_t> &right_bindings, bool &needs_flipping) {

	const auto total_side = JoinSide::GetJoinSide(*expr, left_bindings, right_bindings);

	if (total_side != JoinSide::BOTH) {
		return false;
	}

	// Check if the expression is a spatial predicate
	if (expr->type != ExpressionType::BOUND_FUNCTION) {
		return false;
	}

	auto &func = expr->Cast<BoundFunctionExpression>();

	// The function must be a binary predicate
	if (func.children.size() != 2) {
		return false;
	}

	// The function must return a boolean
	if (func.return_type != LogicalType::BOOLEAN) {
		return false;
	}

	// The function must be a recognized spatial predicate
	if (spatial_predicate_map.count(func.function.name) == 0) {
		return false;
	}
	
	// The function's operands must be GEOMETRY 
	if (func.children[0]->return_type.id() != LogicalTypeId::GEOMETRY ||
	    func.children[1]->return_type.id() != LogicalTypeId::GEOMETRY) {
		return false;
	}

	const auto left_side = JoinSide::GetJoinSide(*func.children[0], left_bindings, right_bindings);
	const auto right_side = JoinSide::GetJoinSide(*func.children[1], left_bindings, right_bindings);

	// Can the condition can be cleanly split into two sides?
	if (left_side == JoinSide::BOTH || right_side == JoinSide::BOTH) {
		return false;
	}

	if (left_side == JoinSide::RIGHT) {
		if (!HasInversePredicate(func.function.name)) {
			return false;
		}
		needs_flipping = true;
	}

	return true;
}

// Look through GEOMETRY->GEOMETRY casts down to a plain column reference
static bool TryGetProbeColumnBinding(const Expression &expr, ColumnBinding &binding) {
	reference<const Expression> current = expr;
	while (current.get().GetExpressionType() == ExpressionType::OPERATOR_CAST) {
		auto &cast = current.get().Cast<BoundCastExpression>();
		if (cast.child->return_type.id() != LogicalTypeId::GEOMETRY) {
			return false;
		}
		current = *cast.child;
	}
	if (current.get().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	binding = current.get().Cast<BoundColumnRefExpression>().binding;
	return true;
}

// Set up a bbox filter pushdown into the probe-side scan(s), mirroring the hash join's JoinFilterPushdownOptimizer.
// The build-side R-tree's bounding box is computed at runtime (in the physical operator's Finalize) and pushed as an
// ST_Intersects_Extent expression filter, which the geometry zonemap can use to prune row groups on the probe side.
static void SetupFilterPushdown(LogicalSpatialJoin &join) {
	// We can only filter the probe (left) side when unmatched probe rows are dropped, i.e. for INNER and RIGHT joins.
	// LEFT/OUTER must emit every probe row.
	if (join.join_type != JoinType::INNER && join.join_type != JoinType::RIGHT) {
		return;
	}

	auto &pred = join.spatial_predicate->Cast<BoundFunctionExpression>();

	// The probe side is always children[0] of the (possibly flipped) predicate.
	ColumnBinding probe_binding;
	if (!TryGetProbeColumnBinding(*pred.children[0], probe_binding)) {
		// Probe key is not a plain geometry column reference, cannot push down
		return;
	}

	// Reuse the core traversal to find the LogicalGet(s) the probe column maps to (through projections, filters, etc.)
	vector<JoinFilterPushdownColumn> columns;
	JoinFilterPushdownColumn column;
	column.probe_column_index = probe_binding;
	columns.push_back(column);

	vector<PushdownFilterTarget> targets;
	JoinFilterPushdownOptimizer::GetPushdownFilterTargets(*join.children[0], std::move(columns), targets);

	for (auto &target : targets) {
		auto &get = target.get;
		for (auto &col : target.columns) {
			// The geometry zonemap pruning only applies to GEOMETRY columns
			if (col.storage_type.id() != LogicalTypeId::GEOMETRY) {
				continue;
			}
			if (!get.dynamic_filters) {
				get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
			}
			SpatialJoinPushdownTarget pushdown_target;
			pushdown_target.dynamic_filters = get.dynamic_filters;
			pushdown_target.probe_column_index = col.probe_column_index.column_index;
			pushdown_target.column_type = col.storage_type;
			join.filter_pushdown_targets.push_back(std::move(pushdown_target));
		}
	}
}

static bool TrySwapComparisonJoin(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto &op = *plan;

	if (op.type != LogicalOperatorType::LOGICAL_FILTER) {
		return false;
	}

	auto &filter = op.Cast<LogicalFilter>();
	if (filter.expressions.size() != 1) {
		return false;
	}

	// TODO: This is rarely the case, because there might be projections inbetween.
	// TODO: Handle projections between filter and join
	auto &child = *op.children[0];
	if (child.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}

	// Can only do this safely for INNER joins
	auto &cmp_join = child.Cast<LogicalComparisonJoin>();
	if (cmp_join.join_type != JoinType::INNER) {
		return false;
	}

	if (cmp_join.HasProjectionMap() || filter.HasProjectionMap()) {
		// We can't handle this right now.
		// We need to recompute the projection maps, but it's a pain.
		return false;
	}

	// Get the table indexes that are reachable from the left and right children
	const auto &left_child = cmp_join.children[0];
	const auto &right_child = cmp_join.children[1];
	unordered_set<idx_t> left_bindings;
	unordered_set<idx_t> right_bindings;
	LogicalJoin::GetTableReferences(*left_child, left_bindings);
	LogicalJoin::GetTableReferences(*right_child, right_bindings);

	// Check if the filter expression contains a spatial predicate
	auto expr = filter.expressions[0]->Copy();
	bool needs_flipping = false;
	if (!IsSpatialJoinPredicate(expr, left_bindings, right_bindings, needs_flipping)) {
		return false;
	}

	if (needs_flipping) {
		expr = GetInversePredicate(input.context, std::move(expr));
	}

	// Cool. Now pull up the join condition into a filter, and create a spatial join
	auto spatial_join = make_uniq<LogicalSpatialJoin>(cmp_join.join_type);
	spatial_join->spatial_predicate = std::move(expr);
	spatial_join->children = std::move(cmp_join.children);
	spatial_join->expressions = std::move(cmp_join.expressions);
	spatial_join->types = std::move(cmp_join.types);
	spatial_join->left_projection_map = std::move(cmp_join.left_projection_map);
	spatial_join->right_projection_map = std::move(cmp_join.right_projection_map);
	spatial_join->join_stats = std::move(cmp_join.join_stats);
	spatial_join->mark_index = cmp_join.mark_index;
	spatial_join->has_estimated_cardinality = cmp_join.has_estimated_cardinality;
	spatial_join->estimated_cardinality = cmp_join.estimated_cardinality;

	// If this is ST_DWithin, try to extract the constant distance value
	const auto &pred_func = spatial_join->spatial_predicate->Cast<BoundFunctionExpression>();
	if (pred_func.function.name == "ST_DWithin") {
		// Try to get the constant distance value from the bind data;
		spatial_join->has_const_distance =
		    ST_DWithinHelper::TryGetConstDistance(pred_func.bind_info, spatial_join->const_distance);
	}

	// Try to set up bounding-box filter pushdown into the probe-side scan(s)
	SetupFilterPushdown(*spatial_join);

	// Also take all the conditions from the comparison join and add them as filters
	filter.expressions.clear();
	filter.expressions.push_back(JoinCondition::CreateExpression(std::move(cmp_join.conditions)));
	filter.children[0] = std::move(spatial_join);

	return true;
}

static void TrySwapAnyJoin(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto &op = *plan;

	// We only care about ANY_JOIN operators
	if (op.type != LogicalOperatorType::LOGICAL_ANY_JOIN) {
		return;
	}

	auto &any_join = op.Cast<LogicalAnyJoin>();

	// We also only support simple join types
	auto join_supported = false;
	switch (any_join.join_type) {
	case JoinType::INNER:
	case JoinType::LEFT:
	case JoinType::RIGHT:
	case JoinType::OUTER:
		join_supported = true;
		break;
	default:
		break;
	}
	if (!join_supported) {
		return;
	}

	// Inspect the join condition
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(any_join.condition->Copy()); // TODO: Maybe move instead of copy

	// Split by AND
	LogicalFilter::SplitPredicates(expressions);

	// Get the table indexes that are reachable from the left and right children
	auto &left_child = any_join.children[0];
	auto &right_child = any_join.children[1];
	unordered_set<idx_t> left_bindings;
	unordered_set<idx_t> right_bindings;
	LogicalJoin::GetTableReferences(*left_child, left_bindings);
	LogicalJoin::GetTableReferences(*right_child, right_bindings);

	// The spatial join condition
	unique_ptr<Expression> spatial_pred_expr = nullptr;

	// Extra predicates that are not spatial predicates
	vector<unique_ptr<Expression>> extra_predicates;

	// Now, check each expression to see if it contains a spatial predicate
	for (auto &expr : expressions) {

		// This is a valid spatial predicate, and we haven't found a spatial predicate yet.
		bool needs_flipping = false;
		if (IsSpatialJoinPredicate(expr, left_bindings, right_bindings, needs_flipping) && !spatial_pred_expr) {

			if (needs_flipping) {
				expr = GetInversePredicate(input.context, std::move(expr));
			}

			spatial_pred_expr = std::move(expr);
			continue;
		}

		extra_predicates.push_back(std::move(expr));
	}

	// Nope! No spatial predicate found
	if (!spatial_pred_expr) {
		return;
	}

	// If, and only if this is INNER join, we can push the extra predicates as filters
	if (any_join.join_type != JoinType::INNER && !extra_predicates.empty()) {
		return;
	}

	// Cool, now we have spatial join conditions. Proceed to create a new LogicalSpatialJoin operator
	auto spatial_join = make_uniq<LogicalSpatialJoin>(any_join.join_type);

	// Steal the properties from the any-join
	spatial_join->spatial_predicate = std::move(spatial_pred_expr);
	// spatial_join->extra_conditions = std::move(extra_predicates);
	spatial_join->children = std::move(any_join.children);
	spatial_join->expressions = std::move(any_join.expressions);
	spatial_join->types = std::move(any_join.types);
	spatial_join->left_projection_map = std::move(any_join.left_projection_map);
	spatial_join->right_projection_map = std::move(any_join.right_projection_map);
	spatial_join->join_stats = std::move(any_join.join_stats);
	spatial_join->mark_index = any_join.mark_index;
	spatial_join->has_estimated_cardinality = any_join.has_estimated_cardinality;
	spatial_join->estimated_cardinality = any_join.estimated_cardinality;

	// If this is ST_DWithin, try to extract the constant distance value
	const auto &pred_func = spatial_join->spatial_predicate->Cast<BoundFunctionExpression>();
	if (pred_func.function.name == "ST_DWithin") {
		// Try to get the constant distance value from the bind data;
		spatial_join->has_const_distance =
		    ST_DWithinHelper::TryGetConstDistance(pred_func.bind_info, spatial_join->const_distance);
	}

	// Try to set up bounding-box filter pushdown into the probe-side scan(s)
	SetupFilterPushdown(*spatial_join);

	if (spatial_join->join_type == JoinType::INNER && !extra_predicates.empty()) {
		// Create a filter on top of the spatial join for the extra predicates
		auto filter = make_uniq<LogicalFilter>();
		filter->expressions = std::move(extra_predicates);
		filter->children.push_back(std::move(spatial_join));

		// Replace the operator
		plan = std::move(filter);
		return;
	}

	// Replace the operator
	plan = std::move(spatial_join);
}

static void InsertSpatialJoin(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (TrySwapComparisonJoin(input, plan)) {
		return;
	}

	TrySwapAnyJoin(input, plan);
}

static void TryInsertSpatialJoin(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {

	InsertSpatialJoin(input, plan);

	// Recursively call this function on all children
	for (auto &child : plan->children) {
		TryInsertSpatialJoin(input, child);
	}
}

void SpatialJoinOptimizer::Register(ExtensionLoader &loader) {

	OptimizerExtension optimizer;
	optimizer.optimize_function = TryInsertSpatialJoin;

	auto &db = loader.GetDatabaseInstance();
	OptimizerExtension::Register(db.config, optimizer);
}

} // namespace duckdb
