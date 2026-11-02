#include "spatial/geometry/bbox.hpp"
#include "spatial/index/rtree/rtree_index.hpp"
#include "spatial/index/rtree/rtree_index_create_logical.hpp"
#include "spatial/index/rtree/rtree_index_scan.hpp"
#include "spatial/index/rtree/rtree_module.hpp"
#include "spatial/operators/spatial_join_logical.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/util/distance_extract.hpp"
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
#include "duckdb/planner/filter/table_filter_functions.hpp"
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

	//! Rewrite the index expression to refer to the columns of the given LogicalGet, so that it can be matched against
	//! a filter expression on top of the get.
	static bool RewriteIndexExpression(const IndexReadHandle<RTreeIndex> &index, const ColumnList &column_list,
	                                   LogicalGet &get, unique_ptr<Expression> &index_expr) {
		auto &indexed_columns = index->GetColumnIds();
		if (indexed_columns.size() != 1) {
			// Only single column indexes are supported right now
			return false;
		}
		auto &get_column_ids = get.GetColumnIds();

		// If the get has the indexed struct field extract pushed down, the index expression is that extracted column
		for (idx_t i = 0; i < get_column_ids.size(); i++) {
			auto &column_id = get_column_ids[i];
			if (column_id.IsPushdownExtract() && index->IsIndexedColumn(column_list, column_id)) {
				index_expr = make_uniq<BoundColumnRefExpression>(column_id.GetScanType(),
				                                                 ColumnBinding(get.table_index, ProjectionIndex(i)));
				return true;
			}
		}

		// Otherwise, the get has to scan the whole indexed column (not just extract fields from it).
		// The index stores *physical* column ids, while the get's column ids are *logical*.
		const auto indexed_column = column_list.GetColumn(PhysicalIndex(indexed_columns[0])).Logical().index;
		for (idx_t i = 0; i < get_column_ids.size(); i++) {
			auto &column_id = get_column_ids[i];
			if (column_id.IsPushdownExtract() || column_id.GetPrimaryIndex() != indexed_column) {
				continue;
			}
			ExpressionIterator::EnumerateExpression(index_expr, [&](Expression &expr) {
				if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					expr.Cast<BoundColumnRefExpression>().BindingMutable() =
					    ColumnBinding(get.table_index, ProjectionIndex(i));
				}
			});
			return true;
		}
		// The indexed column is not scanned by the get
		return false;
	}

	//! Rewrite the index expression to refer to the input of a table filter on the given column, so that it can be
	//! matched against the filter expression.
	static bool RewriteIndexExpressionForFilter(const IndexReadHandle<RTreeIndex> &index, const ColumnList &column_list,
	                                            const ColumnIndex &filter_idx, unique_ptr<Expression> &index_expr) {
		auto &indexed_columns = index->GetColumnIds();
		if (indexed_columns.size() != 1) {
			// Only single column indexes are supported right now
			return false;
		}

		if (filter_idx.IsPushdownExtract()) {
			// The filter applies to a struct field extracted by the scan, which only works if the index is built over
			// that same field. The index expression then is the filter input itself.
			if (!index->IsIndexedColumn(column_list, filter_idx)) {
				return false;
			}
			index_expr = make_uniq<BoundReferenceExpression>(filter_idx.GetScanType(), 0ULL);
			return true;
		}

		// The index stores *physical* column ids, while the filter column id is *logical*.
		auto &col = column_list.GetColumn(PhysicalIndex(indexed_columns[0]));
		if (filter_idx.GetPrimaryIndex() != col.Logical().index) {
			// RTree does not match the filter column
			return false;
		}

		// The filter applies to the indexed column - turn the references to it into a BoundReference
		ExpressionIterator::EnumerateExpression(index_expr, [&](unique_ptr<Expression> &expr) {
			if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				expr = make_uniq<BoundReferenceExpression>(expr->GetReturnType(), 0ULL);
			}
		});
		return true;
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
		// Fold the constant geometry expression and extract the bounds from the serialized blob directly
		Value geom;
		if (!ExpressionExecutor::TryEvaluateScalar(context, expr, geom)) {
			return false;
		}
		return Serde::TryGetBounds(geom, bbox);
	}

	//! Match "ST_DWithin(<indexed column>, <constant geometry>, <constant distance>)" and compute the constant's bbox
	//! expanded by the distance.
	//! ST_DWithin(a, b, d) implies that the bounding box of a intersects the bounding box of b widened by d, so the
	//! expanded bbox is a valid query for the index scan.
	static bool TryMatchDWithinPredicate(ClientContext &context, Expression &filter_expr, const Expression &index_expr,
	                                     Box2D<float> &bbox) {
		if (filter_expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			return false;
		}
		auto &func = filter_expr.Cast<BoundFunctionExpression>();
		// The first two children are the geometry arguments. A constant distance stays as the third child, but its
		// folded value is captured in the bind data, which is what we read below.
		if (!StringUtil::CIEquals(func.Function().GetName().GetIdentifierName(), "ST_DWithin") ||
		    func.GetChildren().size() < 2) {
			return false;
		}

		// The distance must be a constant (captured in the bind data, like the spatial join extracts it)
		double distance;
		if (!ST_DWithinHelper::TryGetConstDistance(func.BindInfo(), distance)) {
			return false;
		}
		if (!(distance >= 0)) {
			// Negative (or NaN) distance: the predicate never matches, no point in an index scan
			return false;
		}

		// One geometry argument must be the indexed column, the other a constant
		auto &lhs = *func.GetChildren()[0];
		auto &rhs = *func.GetChildren()[1];
		optional_ptr<const Expression> const_geom;
		if (lhs.Equals(index_expr) && rhs.IsFoldable()) {
			const_geom = &rhs;
		} else if (rhs.Equals(index_expr) && lhs.IsFoldable()) {
			const_geom = &lhs;
		} else {
			return false;
		}
		if (!TryGetBoundingBox(context, *const_geom, bbox)) {
			return false;
		}

		// Expand the bounding box by the distance, rounding outwards
		const auto f_dist = MathUtil::DoubleToFloatUp(distance);
		bbox.min.x -= f_dist;
		bbox.min.y -= f_dist;
		bbox.max.x += f_dist;
		bbox.max.y += f_dist;
		return true;
	}

	static bool TryOptimize(Binder &binder, ClientContext &context, unique_ptr<LogicalOperator> &plan,
	                        unique_ptr<LogicalOperator> &root,
	                        const unordered_set<DynamicTableFilterSet *> &join_filter_sets) {
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
			// No constant predicate: check if a spatial join will push a bounding-box filter into this scan at runtime,
			// in which case we can use a deferred-bounds index scan
			return TryOptimizeDeferredGet(context, plan, join_filter_sets);
		}
		return false;
	}

	//! Whether the get can be turned into an index scan at all
	static bool CanRewriteToIndexScan(const LogicalGet &get) {
		if (get.function.GetName() != "seq_scan") {
			return false;
		}
		for (auto &col : get.GetColumnIds()) {
			if (col.IsRowNumberColumn()) {
				// The index scan does not produce rows in table order
				return false;
			}
		}
		return true;
	}

	//! Replace the seq_scan function of the given LogicalGet with the RTree index scan function, pulling any
	//! pushed-down table filters back up into a LogicalFilter (index scan does not support regular filter pushdown).
	static void RewriteGetToIndexScan(ClientContext &context, unique_ptr<LogicalOperator> &get_ptr,
	                                  unique_ptr<RTreeIndexScanBindData> bind_data) {
		auto &get = get_ptr->Cast<LogicalGet>();
		get.function = BoundTableFunction(RTreeIndexScanFunction::GetFunction());
		const auto cardinality = get.function.cardinality(context, bind_data.get());
		get.has_estimated_cardinality = cardinality->has_estimated_cardinality;
		get.estimated_cardinality = cardinality->estimated_cardinality;
		get.bind_data = std::move(bind_data);
		if (!get.table_filters.HasFilters()) {
			return;
		}

		// We need to pullup the filters from the table scan as our index scan does not support regular filter pushdown.
		auto new_filter = make_uniq<LogicalFilter>();

		// Clearing the projection ids below makes the get emit all scanned columns (including filter columns),
		// which shifts its column bindings. Give the pulled-up filter a projection map that restores the get's old
		// projected output, so the bindings seen by whatever parent sits above stay exactly the same.
		if (!get.projection_ids.empty()) {
			new_filter->projection_map = get.projection_ids;
		}

		get.projection_ids.clear();
		get.types.clear();
		vector<pair<ProjectionIndex, unique_ptr<TableFilter>>> optional_filters;
		for (const auto &entry : get.table_filters) {
			auto index_ref = entry.GetIndex();
			auto &column_id = get.GetColumnIndex(index_ref);
			// Use the scanned type of the column, which differs from the table column type if a struct field extract
			// has been pushed down into the scan (the filter then applies to the extracted field)
			auto &type = get.GetColumnType(column_id);
			auto column = make_uniq<BoundColumnRefExpression>(type, ColumnBinding(get.table_index, index_ref));
			new_filter->expressions.push_back(entry.Filter().ToExpression(*column));

			// The pulled-up filter now does the row-level filtering. Keep the filter in the scan too,
			// but wrapped as optional: it still prunes row groups via zonemaps (which matters when a deferred scan
			// falls back to a full table scan), without rows being filtered twice.
			auto &expr_filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "RewriteGetToIndexScan");
			auto optional_expr = CreateOptionalFilterExpression(expr_filter.expr->Copy(), type);
			optional_filters.emplace_back(index_ref, make_uniq<ExpressionFilter>(std::move(optional_expr)));
		}
		for (auto &entry : optional_filters) {
			get.table_filters.SetFilterByColumnIndex(entry.first, std::move(entry.second));
		}
		new_filter->children.push_back(std::move(get_ptr));
		new_filter->ResolveOperatorTypes();
		get_ptr = std::move(new_filter);
	}

	//! Rewrite a plain seq_scan into a deferred-bounds RTree index scan, if a spatial join is going to push a bbox
	//! filter into it at runtime and the table has an R-tree index on the probed geometry column.
	//! The actual scan bounds (and whether to use the index at all) are decided when the scan is initialized.
	static bool TryOptimizeDeferredGet(ClientContext &context, unique_ptr<LogicalOperator> &get_ptr,
	                                   const unordered_set<DynamicTableFilterSet *> &join_filter_sets) {
		auto &get = get_ptr->Cast<LogicalGet>();
		if (!CanRewriteToIndexScan(get)) {
			return false;
		}
		// Only rewrite scans that a spatial join is going to push a bounding-box filter into
		if (!get.dynamic_filters || join_filter_sets.find(get.dynamic_filters.get()) == join_filter_sets.end()) {
			return false;
		}
		auto table = get.GetTable();
		if (!table || !table->IsDuckTable()) {
			return false;
		}
		auto &duck_table = table->Cast<DuckTableEntry>();
		auto &table_info = *table->GetStorage().GetDataTableInfo();
		table_info.BindIndexes(context, RTreeIndex::TYPE_NAME);

		unique_ptr<RTreeIndexScanBindData> bind_data = nullptr;
		for (auto index_entry : table_info.GetIndexes().IndexEntries()) {
			if (index_entry->GetBindState() != IndexBindState::BOUND ||
			    RTreeIndex::TYPE_NAME != index_entry->GetIndexType()) {
				continue;
			}
			auto guard = index_entry->GetReadHandle<RTreeIndex>();
			// The indexed column (or indexed struct field) must be scanned by this get, so that the pushed filter can
			// refer to it. Note that a different field of the same struct column does not count.
			bool found = false;
			for (auto &col : get.GetColumnIds()) {
				if (guard->IsIndexedColumn(duck_table.GetColumns(), col)) {
					found = true;
					break;
				}
			}
			if (!found) {
				continue;
			}
			bind_data =
			    make_uniq<RTreeIndexScanBindData>(duck_table, index_entry, guard->GetIndexName(), RTreeBounds(), true);
			break;
		}
		if (!bind_data) {
			return false;
		}
		RewriteGetToIndexScan(context, get_ptr, std::move(bind_data));
		return true;
	}

	static bool TryOptimizeGet(Binder &binder, ClientContext &context, unique_ptr<LogicalOperator> &get_ptr,
	                           unique_ptr<LogicalOperator> &root, optional_ptr<LogicalFilter> filter,
	                           optional_ptr<const ColumnIndex> filter_column_idx, unique_ptr<Expression> &filter_expr) {
		auto &get = get_ptr->Cast<LogicalGet>();
		if (!CanRewriteToIndexScan(get)) {
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
			auto index_expr = guard->CopyUnboundExpression(0);
			const auto rewrite_possible =
			    filter_column_idx
			        ? RewriteIndexExpressionForFilter(guard, duck_table.GetColumns(), *filter_column_idx, index_expr)
			        : RewriteIndexExpression(guard, duck_table.GetColumns(), get, index_expr);
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

			Box2D<float> bbox;
			vector<reference<Expression>> bindings;
			if (matcher.Match(*filter_expr, bindings)) {
				// 		bindings[0] = the expression
				// 		bindings[1] = the index expression
				// 		bindings[2] = the constant

				// Compute the bounding box
				if (!TryGetBoundingBox(context, bindings[2], bbox)) {
					continue;
				}
			} else if (!TryMatchDWithinPredicate(context, *filter_expr, *index_expr, bbox)) {
				// Not a supported spatial predicate over the indexed column and a constant
				continue;
			}

			// Only use the index if the predicate is estimated to be selective enough that random row fetches beat a
			// sequential scan. If not, keep the regular table scan.
			// (which also keeps the filters pushed down, so zonemap pruning still applies).
			const auto total_rows = duck_table.GetStorage().GetTotalRows();
			if (!guard->ShouldUseIndexScan(context, bbox, total_rows)) {
				continue;
			}

			bind_data = make_uniq<RTreeIndexScanBindData>(duck_table, index_entry, guard->GetIndexName(), bbox);
			break;
		};

		if (!bind_data) {
			// No index found
			return false;
		}

		RewriteGetToIndexScan(context, get_ptr, std::move(bind_data));
		return true;
	}

	//! Collect the dynamic filter sets that spatial joins in the plan will push bounding-box filters into
	static void CollectSpatialJoinFilterSets(LogicalOperator &op, unordered_set<DynamicTableFilterSet *> &sets) {
		if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR && op.GetName() == "SPATIAL_JOIN") {
			auto &join = op.Cast<LogicalSpatialJoin>();
			for (auto &target : join.filter_pushdown_targets) {
				sets.insert(target.dynamic_filters.get());
			}
		}
		for (auto &child : op.children) {
			CollectSpatialJoinFilterSets(*child, sets);
		}
	}

	static void OptimizeRecursive(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
	                              unique_ptr<LogicalOperator> &root,
	                              const unordered_set<DynamicTableFilterSet *> &join_filter_sets) {
		if (!TryOptimize(input.optimizer.binder, input.context, plan, root, join_filter_sets)) {
			// No match: continue with the children
			for (auto &child : plan->children) {
				OptimizeRecursive(input, child, root, join_filter_sets);
			}
		}
	}

	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		unordered_set<DynamicTableFilterSet *> join_filter_sets;
		CollectSpatialJoinFilterSets(*plan, join_filter_sets);
		OptimizeRecursive(input, plan, plan, join_filter_sets);
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
