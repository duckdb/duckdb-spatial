
#include "spatial/spatial_extension.hpp"

#include "duckdb.hpp"
#include "index/rtree/rtree.hpp"
#include "spatial/index/rtree/rtree_module.hpp"
#include "spatial/modules/gdal/gdal_module.hpp"
#if SPATIAL_USE_GEOS
#include "spatial/modules/geos/geos_module.hpp"
#endif
#include "spatial/modules/mvt/mvt_module.hpp"
#include "operators/spatial_operator_extension.hpp"
#include "spatial/modules/main/spatial_functions.hpp"
#include "spatial/modules/osm/osm_module.hpp"
#include "spatial/modules/proj/proj_module.hpp"
#include "spatial/modules/shapefile/shapefile_module.hpp"
#include "spatial/modules/wkb/wkb_module.hpp"
#include "spatial/operators/spatial_operator_extension.hpp"
#include "spatial/operators/spatial_join_optimizer.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/spatial_settings.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {

	// Register the types
	GeoTypes::Register(loader);

	// Register Settings
	SpatialSettings::Register(loader);

	RegisterSpatialCastFunctions(loader);
	RegisterSpatialScalarFunctions(loader);
	RegisterSpatialAggregateFunctions(loader);
	RegisterSpatialWindowFunctions(loader);
	RegisterSpatialTableFunctions(loader);
	SpatialJoinOptimizer::Register(loader);

	RegisterProjModule(loader);
	RegisterGDALModule(loader);
#if SPATIAL_USE_GEOS
	RegisterGEOSModule(loader);
#endif
	RegisterOSMModule(loader);
	RegisterShapefileModule(loader);
	RegisterMapboxVectorTileModule(loader);
	RegisterWKBModule(loader);

	RTreeModule::RegisterIndex(loader);
	RTreeModule::RegisterIndexPragmas(loader);
	RTreeModule::RegisterIndexScan(loader);
	RTreeModule::RegisterIndexPlanScan(loader);

	RegisterSpatialOperatorExtension(loader.GetDatabaseInstance());
}

void SpatialExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SpatialExtension::Name() {
	return "spatial";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(spatial, loader) {
	duckdb::LoadInternal(loader);
}
}
