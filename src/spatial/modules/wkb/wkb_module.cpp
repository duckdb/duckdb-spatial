#include "wkb_module.hpp"

#include "duckdb/common/types.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/geometry.hpp"

namespace duckdb {
namespace {

struct WKBTypes {

	static LogicalType WKB_BLOB() {
		return LogicalType(LogicalTypeId::BLOB).WithAlias("WKB_BLOB");
	}

	static bool ToWKBCast(Vector &source, Vector &result, idx_t count, CastParameters &) {
		Geometry::ToBinary(source, result);
		return true;
	}

	static bool FromWKBCast(Vector &source, Vector &result, idx_t count, CastParameters &params) {
		try {
			return Geometry::FromBinary(source, result, count, params.strict);
		} catch (...) {
			HandleCastError::AssignError("Failed to cast WKB_BLOB to GEOMETRY", params);
			return false;
		}
	}

	static void Register(ExtensionLoader &loader) {

		// Register the WKB_BLOB type
		loader.RegisterType("WKB_BLOB", WKB_BLOB());

		// Also register casts
		// WKB_BLOB -> GEOMETRY (Implicit)
		loader.RegisterCastFunction(WKB_BLOB(), LogicalType::GEOMETRY(), FromWKBCast, 1);

		// WKB_BLOB -> BLOB (Implicit)
		loader.RegisterCastFunction(WKB_BLOB(), LogicalType::BLOB, DefaultCasts::ReinterpretCast, 1);

		// TODO: Remove support for this in the future
		// GEOMETRY -> WKB_BLOB (Explicit)
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), WKB_BLOB(), ToWKBCast);

		// TODO: Remove support for this in the future
		// BLOB -> WKB_BLOB (Explicit)
		loader.RegisterCastFunction(LogicalType::BLOB, WKB_BLOB(), DefaultCasts::ReinterpretCast);
	}
};

} // namespace

void RegisterWKBModule(ExtensionLoader &loader) {
	WKBTypes::Register(loader);
}

} // namespace duckdb
