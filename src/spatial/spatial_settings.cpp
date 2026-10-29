#include "spatial/spatial_settings.hpp"

#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/logging/logger.hpp"

namespace duckdb {

static constexpr auto SPATIAL_JOIN_PREPARED_CACHE_SIZE = "spatial_join_prepared_cache_size";
static constexpr idx_t SPATIAL_JOIN_PREPARED_CACHE_SIZE_DEFAULT = 32ULL * 1024 * 1024;
static constexpr auto GEOMETRY_ALWAYS_XY = "geometry_always_xy";
static constexpr auto RTREE_INDEX_SCAN_RATIO = "rtree_index_scan_ratio";
// Benchmarks (2M uniform points/small polygons, payload columns projected, warm cache) put the
// index-scan/table-scan crossover at ~6-8% of the table's rows: below that, fetching matching rows
// by row id beats evaluating the spatial predicate over the whole table. Narrower projections push
// the crossover higher, wider payloads and cold caches push it lower.
static constexpr auto RTREE_INDEX_SCAN_RATIO_DEFAULT = 0.075;
static constexpr auto RTREE_INDEX_SCAN_MIN_ROWS = "rtree_index_scan_min_rows";
static constexpr idx_t RTREE_INDEX_SCAN_MIN_ROWS_DEFAULT = 8192;

bool SpatialSettings::AlwaysXY(ClientContext &context, bool &is_set) {
	Value value;
	if (context.TryGetCurrentSetting(GEOMETRY_ALWAYS_XY, value)) {
		is_set = true;
		return value.GetValue<bool>();
	}

	is_set = false;
	return false;
}

bool SpatialSettings::AlwaysXYOrThrow(ClientContext &context, const string &message) {
	auto is_set = false;
	const auto always_xy = AlwaysXY(context, is_set);
	if (is_set) {
		return always_xy;
	}

	const auto full_message =
	    message +
	    "\n"
	    "In DuckDB v2.0, the 'geometry_always_xy' setting must be set explicitly to use this functionality.\n"
	    "In DuckDB v2.1, 'geometry_always_xy = true' will become the default.\n"
	    "To make this error go away:\n"
	    " * 'SET geometry_always_xy = true' to always assume [EASTING, NORTHING] (= LONGITUDE, LATITUDE) axis order "
	    "(recommended)\n"
	    " * 'SET geometry_always_xy = false' to keep the pre-v2.0 behavior";

	auto &logger = Logger::Get(context);
	logger.WriteLog("Spatial", LogLevel::LOG_WARNING, full_message);

	throw InvalidConfigurationException(full_message);
}

double SpatialSettings::RTreeIndexScanRatio(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting(RTREE_INDEX_SCAN_RATIO, value) && !value.IsNull()) {
		return value.GetValue<double>();
	}
	return RTREE_INDEX_SCAN_RATIO_DEFAULT;
}

idx_t SpatialSettings::RTreeIndexScanMinRows(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting(RTREE_INDEX_SCAN_MIN_ROWS, value) && !value.IsNull()) {
		return value.GetValue<idx_t>();
	}
	return RTREE_INDEX_SCAN_MIN_ROWS_DEFAULT;
}

idx_t SpatialSettings::JoinPreparedCacheSize(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting(SPATIAL_JOIN_PREPARED_CACHE_SIZE, value) && !value.IsNull()) {
		return value.GetValue<uint64_t>();
	}
	return SPATIAL_JOIN_PREPARED_CACHE_SIZE_DEFAULT;
}

void SpatialSettings::Register(ExtensionLoader &loader) {

	auto &db = loader.GetDatabaseInstance();

	db.config.AddExtensionOption(GEOMETRY_ALWAYS_XY,
	                             "ignore the defined axis order of coordinate reference systems and always treat them "
	                             "as (easting, northing) instead of e.g. (latitude, longitude)",
	                             LogicalType::BOOLEAN, Value(LogicalTypeId::BOOLEAN));

	db.config.AddExtensionOption(SPATIAL_JOIN_PREPARED_CACHE_SIZE,
	                             "serialized bytes of build-side geometries each spatial join thread keeps prepared "
	                             "(0 evaluates the join predicate without preparing the build side)",
	                             LogicalType::UBIGINT, Value::UBIGINT(SPATIAL_JOIN_PREPARED_CACHE_SIZE_DEFAULT));

	db.config.AddExtensionOption(
	    RTREE_INDEX_SCAN_RATIO,
	    "the maximum estimated fraction of a table's rows that a bounding-box filter may match for an R-tree index "
	    "scan to be used instead of a full table scan",
	    LogicalType::DOUBLE, Value::DOUBLE(RTREE_INDEX_SCAN_RATIO_DEFAULT));

	db.config.AddExtensionOption(
	    RTREE_INDEX_SCAN_MIN_ROWS,
	    "always allow an R-tree index scan when the estimated number of matching rows is at or below this count, "
	    "regardless of rtree_index_scan_ratio",
	    LogicalType::UBIGINT, Value::UBIGINT(RTREE_INDEX_SCAN_MIN_ROWS_DEFAULT));
}

} // namespace duckdb
