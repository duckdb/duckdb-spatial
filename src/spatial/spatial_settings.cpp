#include "spatial/spatial_settings.hpp"

#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

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

void SpatialSettings::Register(ExtensionLoader &loader) {

	auto &db = loader.GetDatabaseInstance();

	db.config.AddExtensionOption(GEOMETRY_ALWAYS_XY,
	                             "ignore the defined axis order of coordinate reference systems and always treat them "
	                             "as (easting, northing) instead of e.g. (latitude, longitude)",
	                             LogicalType::BOOLEAN, Value(LogicalTypeId::BOOLEAN));

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
