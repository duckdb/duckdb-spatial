#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

class ExtensionLoader;
class ClientContext;

struct SpatialSettings {

	static bool AlwaysXY(ClientContext &context, bool &is_set);

	// Returns the value of the 'geometry_always_xy' setting. If the setting has not been explicitly set,
	// logs 'message' as a warning and throws an InvalidConfigurationException with the same message.
	// 'message' should describe the axis-order sensitivity of the calling function and is suffixed with
	// instructions on how to set the setting.
	static bool AlwaysXYOrThrow(ClientContext &context, const string &message);

	static double RTreeIndexScanRatio(ClientContext &context);
	static idx_t RTreeIndexScanMinRows(ClientContext &context);

	//! Serialized bytes of build-side geometries each spatial join thread keeps deserialized and prepared.
	//! 0 disables the prepared build side, and the join evaluates its predicate as a regular expression.
	static idx_t JoinPreparedCacheSize(ClientContext &context);

	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
