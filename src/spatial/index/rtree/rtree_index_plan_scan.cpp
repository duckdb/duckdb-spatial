#include "spatial/geometry/bbox.hpp"
#include "spatial/index/rtree/rtree_index.hpp"
#include "spatial/index/rtree/rtree_index_create_logical.hpp"
#include "spatial/index/rtree/rtree_index_scan.hpp"
#include "spatial/index/rtree/rtree_module.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/util/math.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/optimizer/matcher/function_matcher.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {
//-----------------------------------------------------------------------------
// Plan rewriter
//-----------------------------------------------------------------------------
class RTreeIndexScanOptimizer : public OptimizerExtension {
public:
	RTreeIndexScanOptimizer() {
		optimize_function = RTreeIndexScanOptimizer::Optimize;
	}

	static void RewriteIndexExpression(const vector<column_t> &column_ids, LogicalGet &get, Expression &expr,
	                                   bool &rewrite_possible) {
		if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &bound_colref = expr.Cast<BoundColumnRefExpression>();
			// bound column ref: rewrite to fit in the current set of bound column ids
			bound_colref.BindingMutable().table_index = get.table_index;
			auto &get_column_ids = get.GetColumnIds();
			column_t referenced_column = column_ids[bound_colref.Binding().column_index];
			// search for the referenced column in the set of column_ids
			for (idx_t i = 0; i < get_column_ids.size(); i++) {
				if (get_column_ids[i].GetPrimaryIndex() == referenced_column) {
					bound_colref.BindingMutable().column_index = ProjectionIndex(i);
					return;
				}
			}
			// column id not found in bound columns in the LogicalGet: rewrite not possible
			rewrite_possible = false;
		}
		ExpressionIterator::EnumerateChildren(
		    expr, [&](Expression &child) { RewriteIndexExpression(column_ids, get, child, rewrite_possible); });
	}

	static void RewriteIndexExpressionForFilter(const vector<column_t> &indexed_columns, LogicalGet &get,
	                                            unique_ptr<Expression> &expr, const ColumnIndex &filter_idx,
	                                            bool &rewrite_possible) {
		if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &bound_colref = expr->Cast<BoundColumnRefExpression>();

			if (indexed_columns.size() != 1) {
				// Only single column indexes are supported right now
				rewrite_possible = false;
				return;
			}

			const auto &duck_table = get.GetTable()->Cast<DuckTableEntry>();
			const auto &column_list = duck_table.GetColumns();

			auto &col = column_list.GetColumn(LogicalIndex(indexed_columns[0]));
			if (filter_idx.GetPrimaryIndex() != col.Physical().index) {
				// RTree does not match the filter column
				rewrite_possible = false;
				return;
			}

			// this column matches the index column - turn it into a BoundReference
			expr = make_uniq<BoundReferenceExpression>(bound_colref.GetReturnType(), 0ULL);
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
			RewriteIndexExpressionForFilter(indexed_columns, get, child, filter_idx, rewrite_possible);
		});
	}

	static bool IsSpatialPredicate(const BoundScalarFunction &function, const unordered_set<string> &predicates) {

		if (predicates.find(function.GetName().GetIdentifierName()) == predicates.end()) {
			return false;
		}
		if (function.GetArguments().size() < 2) {
			// We can only optimize if there are two children
			return false;
		}
		if (function.GetArguments()[0] != LogicalType::GEOMETRY()) {
			// We can only optimize if the first child is a GEOMETRY
			return false;
		}
		if (function.GetArguments()[1] != LogicalType::GEOMETRY()) {
			// We can only optimize if the second child is a GEOMETRY
			return false;
		}
		if (function.GetReturnType() != LogicalType::BOOLEAN) {
			// We can only optimize if the return type is a BOOLEAN
			return false;
		}
		return true;
	}

	static bool TryGetBoundingBox(ClientContext &context, const Expression &expr, Box2D<float> &bbox) {

		// make a new box expression
		auto &catalog = Catalog::GetSystemCatalog(context);
		auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
		    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "ST_Extent_Approx"));
		const auto &func = *entry.functions.GetFunctionByArguments(context, {LogicalType::GEOMETRY()});

		vector<unique_ptr<Expression>> children;
		children.push_back(expr.Copy());

		const auto bbox_expr = func.Bind(context, std::move(children));

		Value result;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *bbox_expr, result)) {
			return false;
		}
		if (result.IsNull()) {
			return false;
		}

		auto &parts = StructValue::GetChildren(result);
		if (parts.size() != 4) {
			return false;
		}

		bbox.min.x = parts[0].GetValue<float>();
		bbox.min.y = parts[1].GetValue<float>();
		bbox.max.x = parts[2].GetValue<float>();
		bbox.max.y = parts[3].GetValue<float>();

		return true;
	}

	static bool TryOptimize(Binder &binder, ClientContext &context, unique_ptr<LogicalOperator> &plan,
	                        unique_ptr<LogicalOperator> &root) {
		// Look for a FILTER with a spatial predicate followed by a LOGICAL_GET table scan
		// OR for a seq_scan with an ExpressionFilter
		auto &op = *plan;

		if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			// extract the filter from the filter node
			// Look for a spatial predicate
			auto &filter = op.Cast<LogicalFilter>();

			if (filter.expressions.size() != 1) {
				// We can only optimize if there is a single expression right now
				return false;
			}
			auto &filter_expr = filter.expressions[0];
			// Look for a table scan
			if (filter.children.front()->type != LogicalOperatorType::LOGICAL_GET) {
				return false;
			}
			auto &get_ptr = filter.children.front();
			return TryOptimizeGet(binder, context, get_ptr, root, filter, nullptr, filter_expr);
		}
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			// this is a LogicalGet - check if there is an ExpressionFilter
			auto &get = op.Cast<LogicalGet>();
			for (auto &entry : get.table_filters) {
				auto proj_id = entry.GetIndex();
				auto &filter = entry.Filter();
				if (filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
					// not an expression filter
					continue;
				}
				auto &column_id = get.GetColumnIndex(proj_id);
				auto &expr_filter = filter.Cast<ExpressionFilter>();
				if (TryOptimizeGet(binder, context, plan, root, nullptr, column_id, expr_filter.expr)) {
					return true;
				}
			}
			return false;
		}
		return false;
	}

	static bool TryOptimizeGet(Binder &binder, ClientContext &context, unique_ptr<LogicalOperator> &get_ptr,
	                           unique_ptr<LogicalOperator> &root, optional_ptr<LogicalFilter> filter,
	                           optional_ptr<const ColumnIndex> filter_column_idx, unique_ptr<Expression> &filter_expr) {
		auto &get = get_ptr->Cast<LogicalGet>();
		if (get.function.name != "seq_scan") {
			return false;
		}

		// We cant optimize if the table already has filters pushed down :(
		if (get.dynamic_filters && get.dynamic_filters->HasFilters()) {
			return false;
		}

		// We can replace the scan function with a rtree index scan (if the table has a rtree index)
		// Get the table
		auto &table = *get.GetTable();
		if (!table.IsDuckTable()) {
			// We can only replace the scan if the table is a duck table
			return false;
		}

		// Find the index
		auto &duck_table = table.Cast<DuckTableEntry>();
		auto &table_info = *table.GetStorage().GetDataTableInfo();
		unique_ptr<RTreeIndexScanBindData> bind_data = nullptr;

		identifier_set_t spatial_predicates = {
		    "ST_Equals",   "ST_Intersects", "ST_Touches",   "ST_Crosses",          "ST_Within", "ST_Contains",
		    "ST_Overlaps", "ST_Covers",     "ST_CoveredBy", "ST_ContainsProperly", "&&",        "ST_Intersects_Extent"};

		table_info.BindIndexes(context, RTreeIndex::TYPE_NAME);

		for (auto index_entry : table_info.GetIndexes().IndexEntries()) {
			if (index_entry->GetBindState() != IndexBindState::BOUND ||
			    RTreeIndex::TYPE_NAME != index_entry->GetIndexType()) {
				continue;
			}
			auto guard = index_entry->GetReadHandle<RTreeIndex>();

			// Create the bind data for this index given the bounding box
			bool rewrite_possible = true;
			auto index_expr = guard->CopyUnboundExpression(0);
			auto indexed_columns = guard->GetColumnIds();
			if (filter_column_idx) {
				RewriteIndexExpressionForFilter(indexed_columns, get, index_expr, *filter_column_idx, rewrite_possible);
			} else {
				RewriteIndexExpression(indexed_columns, get, *index_expr, rewrite_possible);
			}
			if (!rewrite_possible) {
				// Could not rewrite!
				continue;
			}

			FunctionExpressionMatcher matcher;
			matcher.function = make_uniq<ManyFunctionMatcher>(spatial_predicates);
			matcher.expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::BOUND_FUNCTION);
			matcher.policy = SetMatcher::Policy::UNORDERED;

			matcher.matchers.push_back(make_uniq<ExpressionEqualityMatcher>(*index_expr));
			matcher.matchers.push_back(make_uniq<ConstantExpressionMatcher>());

			vector<reference<Expression>> bindings;
			if (!matcher.Match(*filter_expr, bindings)) {
				continue;
			}

			// 		bindings[0] = the expression
			// 		bindings[1] = the index expression
			// 		bindings[2] = the constant

			// Compute the bounding box
			Box2D<float> bbox;
			if (!TryGetBoundingBox(context, bindings[2], bbox)) {
				continue;
			}

			bind_data =
			    make_uniq<RTreeIndexScanBindData>(duck_table, index_entry, guard->GetIndexName(), bbox);
			break;
		};

		if (!bind_data) {
			// No index found
			return false;
		}

		// If there are no table filters pushed down into the get, we can just replace the get with the index scan
		get.function = RTreeIndexScanFunction::GetFunction();
		const auto cardinality = get.function.cardinality(context, bind_data.get());
		get.has_estimated_cardinality = cardinality->has_estimated_cardinality;
		get.estimated_cardinality = cardinality->estimated_cardinality;
		get.bind_data = std::move(bind_data);
		if (!get.table_filters.HasFilters()) {
			return true;
		}

		// Before we clear projection ids, replace projection map in the filter
		if (!get.projection_ids.empty() && filter) {
			for (auto &id : filter->projection_map) {
				id = get.projection_ids[id];
			}
		}

		get.projection_ids.clear();
		get.types.clear();

		// Otherwise, things get more complicated. We need to pullup the filters from the table scan as our index scan
		// does not support regular filter pushdown.
		auto new_filter = make_uniq<LogicalFilter>();
		for (const auto &entry : get.table_filters) {
			auto index_ref = entry.GetIndex();
			auto &column_id = get.GetColumnIndex(index_ref);
			auto &type = get.returned_types[column_id.GetPrimaryIndex()];
			auto column = make_uniq<BoundColumnRefExpression>(type, ColumnBinding(get.table_index, index_ref));
			new_filter->expressions.push_back(entry.Filter().ToExpression(*column));
		}
		new_filter->children.push_back(std::move(get_ptr));
		new_filter->ResolveOperatorTypes();
		get_ptr = std::move(new_filter);
		return true;
	}

	static void OptimizeRecursive(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
	                              unique_ptr<LogicalOperator> &root) {
		if (!TryOptimize(input.optimizer.binder, input.context, plan, root)) {
			// No match: continue with the children
			for (auto &child : plan->children) {
				OptimizeRecursive(input, child, root);
			}
		}
	}

	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		OptimizeRecursive(input, plan, plan);
	}
};

//-----------------------------------------------------------------------------
// Register
//-----------------------------------------------------------------------------
void RTreeModule::RegisterIndexPlanScan(ExtensionLoader &loader) {
	// Register the optimizer extension
	auto &db = loader.GetDatabaseInstance();
	RTreeIndexScanOptimizer optimizer;

	OptimizerExtension::Register(db.config, optimizer);
}

} // namespace duckdb
