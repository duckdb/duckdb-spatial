#include "spatial/index/rtree/rtree_index_create_logical.hpp"
#include "spatial/index/rtree/rtree_index.hpp"
#include "spatial/index/rtree/rtree_index_create_physical.hpp"
#include "spatial/spatial_types.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/operator/order/physical_order.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"

namespace duckdb {

LogicalCreateRTreeIndex::LogicalCreateRTreeIndex(unique_ptr<CreateIndexInfo> info_p,
                                                 vector<unique_ptr<Expression>> expressions_p,
                                                 TableCatalogEntry &table_p)
    : LogicalExtensionOperator(), info(std::move(info_p)), table(table_p) {
	for (auto &expr : expressions_p) {
		this->unbound_expressions.push_back(expr->Copy());
	}
	this->expressions = std::move(expressions_p);
}

void LogicalCreateRTreeIndex::ResolveTypes() {
	types.emplace_back(LogicalType::BIGINT);
}

void LogicalCreateRTreeIndex::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	bindings = LogicalOperator::GenerateColumnBindings(TableIndex(0), table.GetColumns().LogicalColumnCount());

	// Visit the operator's expressions
	LogicalOperatorVisitor::EnumerateExpressions(*this,
	                                             [&](unique_ptr<Expression> *child) { res.VisitExpression(child); });
}

static PhysicalOperator &CreateNullFilter(PhysicalPlanGenerator &generator, const LogicalOperator &op,
                                          const vector<LogicalType> &types, ClientContext &context) {
	vector<unique_ptr<Expression>> filter_select_list;

	// Filter NOT NULL on the GEOMETRY column
	auto is_not_null_expr =
	    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
	auto bound_ref = make_uniq<BoundReferenceExpression>(types[0], 0);
	is_not_null_expr->GetChildrenMutable().push_back(bound_ref->Copy());

	// Filter IS_NOT_EMPTY on the GEOMETRY column
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &is_empty_entry = catalog.GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY, Identifier::DefaultSchema(), "ST_IsEmpty")
	                           .Cast<ScalarFunctionCatalogEntry>();

	auto is_empty_func = is_empty_entry.functions.GetFunctionByArguments(context, {LogicalType::GEOMETRY()});
	vector<unique_ptr<Expression>> is_empty_args;
	is_empty_args.push_back(std::move(bound_ref));
	auto is_empty_expr = is_empty_func.Bind(context, std::move(is_empty_args));

	auto is_not_empty_expr = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
	is_not_empty_expr->GetChildrenMutable().push_back(std::move(is_empty_expr));

	// Combine into an AND
	auto and_expr = make_uniq_base<Expression, BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(is_not_null_expr), std::move(is_not_empty_expr));
	filter_select_list.push_back(std::move(and_expr));

	return generator.Make<PhysicalFilter>(types, std::move(filter_select_list), op.estimated_cardinality);
}

static PhysicalOperator &CreateBoundingBoxProjection(PhysicalPlanGenerator &planner, const LogicalOperator &op,
                                                     const vector<LogicalType> &types, ClientContext &context) {
	auto &catalog = Catalog::GetSystemCatalog(context);

	// Get the bounding box function
	auto &bbox_func_entry =
	    catalog.GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY, Identifier::DefaultSchema(), "ST_Extent_Approx")
	        .Cast<ScalarFunctionCatalogEntry>();
	const auto &bbox_func = bbox_func_entry.functions.GetFunctionByArguments(context, {LogicalType::GEOMETRY()});

	auto geom_ref_expr = make_uniq_base<Expression, BoundReferenceExpression>(LogicalType::GEOMETRY(), 0);
	vector<unique_ptr<Expression>> bbox_args;
	bbox_args.push_back(std::move(geom_ref_expr));

	auto bbox_expr = bbox_func.Bind(context, std::move(bbox_args));

	// Also project the rowid column
	auto rowid_expr = make_uniq_base<Expression, BoundReferenceExpression>(LogicalType::ROW_TYPE, 1);

	vector<unique_ptr<Expression>> select_list;
	select_list.push_back(std::move(bbox_expr));
	select_list.push_back(std::move(rowid_expr));

	return planner.Make<PhysicalProjection>(types, std::move(select_list), op.estimated_cardinality);
}

static PhysicalOperator &CreateOrderByMinX(PhysicalPlanGenerator &planner, const LogicalOperator &op,
                                           const vector<LogicalType> &types, ClientContext &context) {
	auto &catalog = Catalog::GetSystemCatalog(context);

	// Get the centroid value function
	auto &centroid_func_entry =
	    catalog.GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY, Identifier::DefaultSchema(), "st_centroid")
	        .Cast<ScalarFunctionCatalogEntry>();
	const auto &centroid_func = centroid_func_entry.functions.GetFunctionByArguments(context, {GeoTypes::BOX_2DF()});
	vector<unique_ptr<Expression>> centroid_func_args;

	// Reference the geometry column
	auto geom_ref_expr = make_uniq_base<Expression, BoundReferenceExpression>(GeoTypes::BOX_2DF(), 0);
	centroid_func_args.push_back(make_uniq_base<Expression, BoundReferenceExpression>(GeoTypes::BOX_2DF(), 0));

	auto centroid_expr = centroid_func.Bind(context, std::move(centroid_func_args));

	// Get the xmin value function
	auto &xmin_func_entry = catalog.GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY, Identifier::DefaultSchema(), "st_xmin")
	                            .Cast<ScalarFunctionCatalogEntry>();
	const auto &xmin_func = xmin_func_entry.functions.GetFunctionByArguments(context, {GeoTypes::POINT_2D()});
	vector<unique_ptr<Expression>> xmin_func_args;

	// Reference the centroid
	xmin_func_args.push_back(std::move(centroid_expr));
	auto xmin_expr = xmin_func.Bind(context, std::move(xmin_func_args));

	vector<BoundOrderByNode> orders;
	orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, std::move(xmin_expr));
	vector<idx_t> projections = {0, 1};
	return planner.Make<PhysicalOrder>(types, std::move(orders), projections, op.estimated_cardinality);
}

PhysicalOperator &RTreeIndex::CreatePlan(PlanIndexInput &input) {

	auto &op = input.op;
	auto &table_scan = input.table_scan;
	auto &context = input.context;
	auto &planner = input.planner;

	// generate a physical plan for the parallel index creation which consists of the following operators
	// table scan - projection (for expression execution) - filter (NOT NULL) - order - create index
	D_ASSERT(op.children.size() == 1);

	// Validate that we only have one expression
	if (op.unbound_expressions.size() != 1) {
		throw BinderException("RTree indexes can only be created over a single column.");
	}

	auto &expr = op.unbound_expressions[0];

	// Validate that we have the right type of expression (float array)
	if (expr->GetReturnType() != LogicalType::GEOMETRY()) {
		throw BinderException("RTree indexes can only be created over GEOMETRY columns.");
	}

	// Validate that the expression does not have side effects
	if (!expr->IsConsistent()) {
		throw BinderException("RTree index keys cannot contain expressions with side "
		                      "effects.");
	}

	// projection to execute expressions on the key columns
	vector<LogicalType> new_column_types;
	vector<unique_ptr<Expression>> select_list;

	// Add the geometry expression to the select list
	auto geom_expr = op.expressions[0]->Copy();
	new_column_types.push_back(geom_expr->GetReturnType());
	select_list.push_back(std::move(geom_expr));

	// Add the row ID to the select list
	new_column_types.emplace_back(LogicalType::ROW_TYPE);
	select_list.push_back(make_uniq<BoundReferenceExpression>(LogicalType::ROW_TYPE, op.info->scan_types.size() - 1));

	// Project the expressions
	auto &projection =
	    planner.Make<PhysicalProjection>(new_column_types, std::move(select_list), op.estimated_cardinality);
	projection.children.push_back(table_scan);

	// Filter operator for (IS_NOT_NULL) and (NOT ST_IsEmpty) on the geometry column
	auto &null_filter = CreateNullFilter(planner, op, new_column_types, context);
	null_filter.children.push_back(projection);

	// Project the bounding box and the row ID
	vector<LogicalType> projected_types = {GeoTypes::BOX_2DF(), LogicalType::ROW_TYPE};
	auto &bbox_proj = CreateBoundingBoxProjection(planner, op, projected_types, context);
	bbox_proj.children.push_back(null_filter);

	// Create an ORDER_BY operator to sort the bounding boxes by the xmin value
	auto &physical_order = CreateOrderByMinX(planner, op, projected_types, context);
	physical_order.children.push_back(bbox_proj);

	// Now finally create the actual physical create index operator
	auto &physical_create_index =
	    planner.Make<PhysicalCreateRTreeIndex>(op, op.table, op.info->column_ids, std::move(op.info),
	                                           std::move(op.unbound_expressions), op.estimated_cardinality);
	physical_create_index.children.push_back(physical_order);
	return physical_create_index;
}

// TODO: Remove this
PhysicalOperator &LogicalCreateRTreeIndex::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {

	auto &table_scan = planner.CreatePlan(*children[0]);
	auto &op = *this;

	// generate a physical plan for the parallel index creation which consists of the following operators
	// table scan - projection (for expression execution) - filter (NOT NULL) - order - create index
	D_ASSERT(op.children.size() == 1);

	// Validate that we only have one expression
	if (op.unbound_expressions.size() != 1) {
		throw BinderException("RTree indexes can only be created over a single column.");
	}

	auto &expr = op.unbound_expressions[0];

	// Validate that we have the right type of expression (float array)
	if (expr->GetReturnType() != LogicalType::GEOMETRY()) {
		throw BinderException("RTree indexes can only be created over GEOMETRY columns.");
	}

	// Validate that the expression does not have side effects
	if (!expr->IsConsistent()) {
		throw BinderException("RTree index keys cannot contain expressions with side "
		                      "effects.");
	}

	// Assert that we got the right index type
	D_ASSERT(op.info->index_type == RTreeIndex::TYPE_NAME);

	// table scan operator for index key columns and row IDs
	planner.dependencies.AddDependency(op.table);

	D_ASSERT(op.info->scan_types.size() - 1 <= op.info->names.size());
	D_ASSERT(op.info->scan_types.size() - 1 <= op.info->column_ids.size());

	// projection to execute expressions on the key columns

	vector<LogicalType> new_column_types;
	vector<unique_ptr<Expression>> select_list;

	// Add the geometry expression to the select list
	auto geom_expr = op.expressions[0]->Copy();
	new_column_types.push_back(geom_expr->GetReturnType());
	select_list.push_back(std::move(geom_expr));

	// Add the row ID to the select list
	new_column_types.emplace_back(LogicalType::ROW_TYPE);
	select_list.push_back(make_uniq<BoundReferenceExpression>(LogicalType::ROW_TYPE, op.info->scan_types.size() - 1));

	// Project the expressions
	auto &projection =
	    planner.Make<PhysicalProjection>(new_column_types, std::move(select_list), op.estimated_cardinality);
	projection.children.push_back(table_scan);

	// Filter operator for (IS_NOT_NULL) and (NOT ST_IsEmpty) on the geometry column
	auto &null_filter = CreateNullFilter(planner, op, new_column_types, context);
	null_filter.children.push_back(projection);

	// Project the bounding box and the row ID
	vector<LogicalType> projected_types = {GeoTypes::BOX_2DF(), LogicalType::ROW_TYPE};
	auto &bbox_proj = CreateBoundingBoxProjection(planner, op, projected_types, context);
	bbox_proj.children.push_back(null_filter);

	// Create an ORDER_BY operator to sort the bounding boxes by the xmin value
	auto &physical_order = CreateOrderByMinX(planner, op, projected_types, context);
	physical_order.children.push_back(bbox_proj);

	// Now finally create the actual physical create index operator
	auto &physical_create_index =
	    planner.Make<PhysicalCreateRTreeIndex>(op, op.table, op.info->column_ids, std::move(op.info),
	                                           std::move(op.unbound_expressions), op.estimated_cardinality);
	physical_create_index.children.push_back(physical_order);
	return physical_create_index;
}

void LogicalCreateRTreeIndex::Serialize(Serializer &writer) const {
	LogicalExtensionOperator::Serialize(writer);
	writer.WritePropertyWithDefault(300, "operator_type", string(OPERATOR_TYPE_NAME));
	writer.WritePropertyWithDefault<unique_ptr<CreateIndexInfo>>(400, "info", info);
	writer.WritePropertyWithDefault<vector<unique_ptr<Expression>>>(401, "unbound_expressions", unbound_expressions);
}

unique_ptr<LogicalExtensionOperator> LogicalCreateRTreeIndex::Deserialize(Deserializer &reader) {
	auto create_info = reader.ReadPropertyWithDefault<unique_ptr<CreateInfo>>(400, "info");
	auto unbound_expressions =
	    reader.ReadPropertyWithDefault<vector<unique_ptr<Expression>>>(401, "unbound_expressions");

	auto info = unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(create_info));

	// We also need to rebind the table
	auto &context = reader.Get<ClientContext &>();
	const auto &catalog = info->GetQualifiedName().Catalog();
	const auto &schema = info->GetQualifiedName().Schema();
	const auto &table_name = info->table;
	auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context, QualifiedName(catalog, schema, table_name));

	// Return the new operator
	return make_uniq_base<LogicalExtensionOperator, LogicalCreateRTreeIndex>(
	    std::move(info), std::move(unbound_expressions), table_entry);
}

} // namespace duckdb
