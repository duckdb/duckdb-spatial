#include "spatial/operators/spatial_operator_extension.hpp"
#include "spatial/index/rtree/rtree_index_create_logical.hpp"
#include "spatial/operators/spatial_join_logical.hpp"
#include "spatial/operators/spatial_knn_join_logical.hpp"

#include "duckdb/main/database.hpp"
#include "duckdb/planner/operator_extension.hpp"

namespace duckdb {

namespace {
class SpatialOperatorExtension final : public OperatorExtension {
public:
	SpatialOperatorExtension() {
		Bind = [](ClientContext &, Binder &, OperatorExtensionInfo *, SQLStatement &) -> BoundStatement {
			// For some reason all operator extensions require this callback to be implemented
			// even though it is useless for us as we construct this operator through the optimizer instead.
			BoundStatement result;
			result.plan = nullptr;
			return result;
		};
	}

	std::string GetName() override {
		return "duckdb_spatial";
	}

	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &reader) override {
		const auto operator_type = reader.ReadPropertyWithDefault<string>(300, "operator_type");

		// These are the two custom operators we support now
		if (operator_type == LogicalCreateRTreeIndex::OPERATOR_TYPE_NAME) {
			return LogicalCreateRTreeIndex::Deserialize(reader);
		}
		if (operator_type == LogicalSpatialJoin::OPERATOR_TYPE_NAME) {
			return LogicalSpatialJoin::Deserialize(reader);
		}
		if (operator_type == LogicalSpatialKNNJoin::OPERATOR_TYPE_NAME) {
			return LogicalSpatialKNNJoin::Deserialize(reader);
		}

		throw SerializationException("This version of the spatial extension does not support operator type '%s'!",
		                             operator_type);
	}
};

} // namespace

void RegisterSpatialOperatorExtension(DatabaseInstance &db) {
	OperatorExtension::Register(db.config, make_shared_ptr<SpatialOperatorExtension>());
}

} // namespace duckdb