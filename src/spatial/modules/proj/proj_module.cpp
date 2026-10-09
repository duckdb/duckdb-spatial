#include "spatial/modules/proj/proj_module.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/spatial_settings.hpp"
#include "spatial/util/function_builder.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/geometry/geometry_serialization.hpp"

#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/parser/parsed_data/create_coordinate_system_info.hpp"
#include "duckdb/catalog/catalog_entry/coordinate_system_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

#include "proj.h"
#include "geodesic.h"
#include "sqlite3.h"
#include "fmt/format.h"

// We embed the whole proj.db in the proj_db.c file, which we then link into the extension binary
// We can then use the sqlite3 "memvfs" (which we also statically link to) to point to the proj.db database in memory
// To genereate the proj_db.c file, we use the following command:
// `xxd -i proj.db > proj_db.c`
// Then rename the array to proj_db and the length to proj_db_len if necessary
// We link these from the proj_db.c file externally instead of #include:ing so our IDE doesnt go haywire
extern "C" unsigned char proj_db[];
extern "C" unsigned int proj_db_len;
extern "C" int sqlite3_memvfs_init(sqlite3 *, char **, const sqlite3_api_routines *);

// Specialize hash for std::pair<std::string, std::string> so we can use it as a key in an unordered_map
template <>
struct std::hash<std::pair<std::string, std::string>> {
	size_t operator()(pair<string, string> const &v) const noexcept {
		const auto lhs = std::hash<string> {}(v.first);
		const auto rhs = std::hash<string> {}(v.second);
		// Shift by one so we dont match the hash of the reversed pair
		return lhs ^ (rhs << 1);
	}
};

namespace duckdb {

namespace {

//######################################################################################################################
// PROJ Module & SQLITE VFS Registration
//######################################################################################################################

struct ProjModule {
	static void RegisterVFS(ExtensionLoader &loader);
	static PJ_CONTEXT *GetThreadProjContext();
};

PJ_CONTEXT *ProjModule::GetThreadProjContext() {

	const auto ctx = proj_context_create();

	// We set the default context proj.db path to the one in the binary here
	// Otherwise GDAL will try to load the proj.db from the system
	// Any PJ_CONTEXT we create after this will inherit these settings
	auto path = StringUtil::Format("file:/proj.db?immutable=1&ptr=%llu&sz=%lu&max=%lu", static_cast<void *>(proj_db),
	                               proj_db_len, proj_db_len);

	proj_context_set_sqlite3_vfs_name(ctx, "memvfs");
	const auto ok = proj_context_set_database_path(ctx, path.c_str(), nullptr, nullptr);
	if (!ok) {
		throw InternalException("Could not set proj.db path");
	}

	// Dont log errors to stderr
	proj_log_level(ctx, PJ_LOG_NONE);

	// Dont allow network
	proj_context_set_enable_network(ctx, false);

	return ctx;
}

// IMPORTANT: Make sure this module is loaded before any other modules that use proj (like GDAL)
void ProjModule::RegisterVFS(ExtensionLoader &loader) {

	// Initialization lock around global proj state
	// Make sure this is only loaded once per process
	// So that multiple duckdb connections that `LOAD spatial` don't overwrite the default sqlite vfs again

	static std::once_flag loaded;
	std::call_once(loaded, [&]() {
		// we use the sqlite "memvfs" to store the proj.db database in the extension binary itself
		// this way we don't have to worry about the user having the proj.db database installed
		// on their system. We therefore have to tell proj to use memvfs as the sqlite3 vfs and
		// point it to the segment of the binary that contains the proj.db database

		sqlite3_initialize();
		sqlite3_memvfs_init(nullptr, nullptr, nullptr);
		const auto vfs = sqlite3_vfs_find("memvfs");
		if (!vfs) {
			throw InternalException("Could not find sqlite memvfs extension");
		}
		sqlite3_vfs_register(vfs, 0);

		// We set the default context proj.db path to the one in the binary here
		// Otherwise GDAL will try to load the proj.db from the system
		// Any PJ_CONTEXT we create after this will inherit these settings (on this thread?)
		auto path = StringUtil::Format("file:/proj.db?immutable=1&ptr=%llu&sz=%lu&max=%lu",
		                               static_cast<void *>(proj_db), proj_db_len, proj_db_len);

		proj_context_set_sqlite3_vfs_name(nullptr, "memvfs");

		// Try to open the database
		sqlite3 *sdb = nullptr;
		const auto sok = sqlite3_open_v2(path.c_str(), &sdb, SQLITE_OPEN_READONLY, "memvfs");
		if (sok != SQLITE_OK) {
			sqlite3_close_v2(sdb);
			throw InternalException("Could not open sqlite3 memvfs database");
		}

		const auto ok = proj_context_set_database_path(nullptr, path.c_str(), nullptr, nullptr);
		if (!ok) {
			sqlite3_close_v2(sdb);
			throw InternalException("Could not set proj.db path");
		}

		sqlite3_close_v2(sdb);
	});
}

//######################################################################################################################
// Coordinate Transformation Functions
//######################################################################################################################

//======================================================================================================================
// Local State
//======================================================================================================================

struct ProjCRSDelete {
	void operator()(PJ *crs) const {
		proj_destroy(crs);
	}
};

using ProjCRS = unique_ptr<PJ, ProjCRSDelete>;

//======================================================================================================================
// ST_Transform
//======================================================================================================================

struct ST_Transform {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : FunctionData {
		bool normalize = false;

		unique_ptr<FunctionData> Copy() const override {
			auto result = make_uniq<BindData>();
			result->normalize = normalize;
			return std::move(result);
		}

		bool Equals(const FunctionData &other) const override {
			auto &data = other.Cast<BindData>();
			return normalize == data.normalize;
		}
	};

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &ctx = input.GetClientContext();
		auto &args = input.GetArguments();

		auto result = make_uniq<BindData>();

		// If always_xy is set, then always normalize
		if (args.size() == 4) {
			// Ensure the "always_xy" parameter is a constant
			const auto &arg = args[3];
			if (arg->HasParameter()) {
				throw InvalidInputException("The 'always_xy' parameter must be a constant");
			}
			if (!arg->IsFoldable()) {
				throw InvalidInputException("The 'always_xy' parameter must be a constant");
			}
			result->normalize = BooleanValue::Get(ExpressionExecutor::EvaluateScalar(ctx, *arg));
		} else {

			// Otherwise check the setting for whether to ignore axis order in PROJ transformations, and set the normalize flag accordingly
			auto is_set = false;
			result->normalize = SpatialSettings::AlwaysXY(ctx, is_set);

			if (!is_set) {
				constexpr auto message =
				    "'ST_Transform' assumes the axis order of the input geometry to be the same as defined by the "
				    "source CRS.\n"
				    "E.g., EPSG:4326 expects [NORTHING, EASTING] (= LATITUDE, LONGITUDE).\n"
				    "In the future this will change to always assume [EASTING, NORTHING] regardless of CRS "
				    "definition.\n"
				    "To avoid unexpected results when this changes:\n"
				    " * 'SET geometry_always_xy = true' to always expect [EASTING, NORTHING]\n"
				    " * 'SET geometry_always_xy = false' to keep current behavior\n"
				    " * Pass 'true' or 'false' as last optional 'always_xy' parameter to override per-call";

				auto &logger = Logger::Get(ctx);
				logger.WriteLog("Spatial", LogLevel::LOG_WARNING, message);
			}
		}

		return std::move(result);
	}

	struct TypedBindData final : FunctionData {

		bool normalize = false;
		string source_crs = "";
		string target_crs = "";

		unique_ptr<FunctionData> Copy() const override {
			auto result = make_uniq<TypedBindData>();
			result->normalize = normalize;
			result->source_crs = source_crs;
			result->target_crs = target_crs;
			return std::move(result);
		}

		bool Equals(const FunctionData &other) const override {
			auto &data = other.Cast<TypedBindData>();
			return normalize == data.normalize && source_crs == data.source_crs && target_crs == data.target_crs;
		}

		static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
		                      const BoundScalarFunction &function) {
			auto &bind_data = bind_data_p->Cast<TypedBindData>();
			serializer.WritePropertyWithDefault(100, "normalize", bind_data.normalize);
			serializer.WritePropertyWithDefault(101, "source", bind_data.source_crs);
			serializer.WritePropertyWithDefault(102, "target", bind_data.target_crs);
		}

		static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &function) {
			auto result = make_uniq<TypedBindData>();
			deserializer.ReadPropertyWithDefault(100, "normalize", result->normalize);
			deserializer.ReadPropertyWithDefault(101, "source", result->source_crs);
			deserializer.ReadPropertyWithDefault(102, "target", result->target_crs);
			return std::move(result);
		}
	};

	static unique_ptr<FunctionData> BindTyped(BindScalarFunctionInput &input) {
		auto &ctx = input.GetClientContext();
		auto &func = input.GetBoundFunction();
		auto &args = input.GetArguments();

		auto result = make_uniq<TypedBindData>();

		// Get CRS from source geometry
		const auto &geo_arg = args[0];
		if (!GeoType::HasCRS(geo_arg->GetReturnType())) {
			throw BinderException(geo_arg->GetQueryLocation(), "Source geometry must have a coordinate reference system");
		}
		result->source_crs = GeoType::GetCRS(geo_arg->GetReturnType()).GetDefinition();
		if (result->source_crs.empty()) {
			throw BinderException(geo_arg->GetQueryLocation(), "Source geometry must have a coordinate reference system");
		}

		// Constant-fold target_crs
		const auto &crs_arg = args[1];
		if (crs_arg->HasParameter()) {
			throw BinderException(crs_arg->GetQueryLocation(), "The 'target_crs' parameter must be a constant");
		}
		if (!crs_arg->IsFoldable()) {
			throw BinderException(crs_arg->GetQueryLocation(), "The 'target_crs' parameter must be a constant");
		}
		if (crs_arg->GetReturnType().id() != LogicalTypeId::VARCHAR) {
			throw BinderException(crs_arg->GetQueryLocation(), "The 'target_crs' parameter must be a string");
		}
		result->target_crs = StringValue::Get(ExpressionExecutor::EvaluateScalar(ctx, *crs_arg));
		if (result->target_crs.empty()) {
			throw BinderException(crs_arg->GetQueryLocation(), "The 'target_crs' parameter cannot be empty");
		}
		const auto result_crs = CoordinateReferenceSystem::TryIdentify(ctx, result->target_crs);
		if (!result_crs) {
			throw BinderException(crs_arg->GetQueryLocation(),
			                      "The 'target_crs' parameter '%s' is not a recognized coordinate reference system",
			                      result->target_crs);
		}

		// Constant-fold always-xy, if present
		auto explicit_normalize = false;
		if (args.size() == 3) {
			const auto &xy_arg = args[2];
			if (xy_arg->HasParameter()) {
				throw BinderException(xy_arg->GetQueryLocation(), "The 'always_xy' parameter must be a constant");
			}
			if (!xy_arg->IsFoldable()) {
				throw BinderException(xy_arg->GetQueryLocation(), "The 'always_xy' parameter must be a constant");
			}
			if (xy_arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException(xy_arg->GetQueryLocation(), "The 'always_xy' parameter must be a boolean");
			}
			result->normalize = BooleanValue::Get(ExpressionExecutor::EvaluateScalar(ctx, *xy_arg));
			explicit_normalize = true;
		} else {
			explicit_normalize = false;
		}

		// Set return types
		func.GetArguments()[0] = geo_arg->GetReturnType();
		func.SetReturnType(LogicalType::GEOMETRY(*result_crs));

		// Check if we need to warn for this
		if (!explicit_normalize) {

			auto is_set = false;
			result->normalize = SpatialSettings::AlwaysXY(ctx, is_set);

			if (!is_set) {
				constexpr auto message =
				    "'ST_Transform' assumes the axis order of the input geometry to be the same as defined by the "
				    "source CRS.\n"
				    "E.g., EPSG:4326 expects [NORTHING, EASTING] (= LATITUDE, LONGITUDE).\n"
				    "In the future this will change to always assume [EASTING, NORTHING] regardless of CRS "
				    "definition.\n"
				    "To avoid unexpected results when this changes:\n"
				    " * 'SET geometry_always_xy = true' to always expect [EASTING, NORTHING]\n"
				    " * 'SET geometry_always_xy = false' to keep current behavior\n"
				    " * Pass 'true' or 'false' as last optional 'always_xy' parameter to override per-call";

				auto &logger = Logger::Get(ctx);
				logger.WriteLog("Spatial", LogLevel::LOG_WARNING, message);
			}
		}

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Local State
	//------------------------------------------------------------------------------------------------------------------
	struct LocalState final : FunctionLocalState {

		PJ_CONTEXT *proj_ctx;
		ArenaAllocator arena;
		GeometryAllocator allocator;

		// Cache for PJ* objects
		unordered_map<std::pair<string, string>, ProjCRS> crs_cache;

		// Not copyable
		LocalState(const LocalState &) = delete;
		LocalState &operator=(const LocalState &) = delete;

		// Not movable
		LocalState(LocalState &&) = delete;
		LocalState &operator=(LocalState &&) = delete;

		explicit LocalState(ClientContext &context)
		    : proj_ctx(ProjModule::GetThreadProjContext()), arena(BufferAllocator::Get(context)), allocator(arena) {
		}

		~LocalState() override {
			// We need to clear the cache so that the unique_ptrs are destroyed before the context
			crs_cache.clear();
			proj_context_destroy(proj_ctx);
		}

		static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
		                                           FunctionData *bind_data) {
			auto result = make_uniq<LocalState>(state.GetContext());
			return std::move(result);
		}

		static LocalState &ResetAndGet(ExpressionState &state) {
			auto &local_state = ExecuteFunctionState::GetFunctionState(state)->Cast<LocalState>();
			local_state.arena.Reset();
			return local_state;
		}

		PJ *GetOrCreateProjection(const string &source, const string &target, bool normalize) {
			const auto crs_entry = crs_cache.find({source, target});
			if (crs_entry != crs_cache.end()) {
				return crs_entry->second.get();
			}

			auto crs = proj_create_crs_to_crs(proj_ctx, source.c_str(), target.c_str(), nullptr);
			if (!crs) {
				throw InvalidInputException("Could not create projection: " + source + " -> " + target);
			}

			if (normalize) {
				const auto normalized_crs = proj_normalize_for_visualization(proj_ctx, crs);
				proj_destroy(crs);
				if (!normalized_crs) {
					throw InvalidInputException("Could not normalize projection: " + source + " -> " + target);
				}
				crs = normalized_crs;
			}

			crs_cache[{source, target}] = ProjCRS(crs);
			return crs;
		}

		string_t Serialize(Vector &vector, const sgl::geometry &geom) {
			const auto size = Serde::GetRequiredSize(geom);
			auto blob = StringVector::EmptyString(vector, size);
			Serde::Serialize(geom, blob.GetDataWriteable(), size);
			blob.Finalize();
			return blob;
		}

		void Deserialize(const string_t &blob, sgl::geometry &geom) {
			Serde::Deserialize(geom, arena, blob.GetDataUnsafe(), blob.GetSize());
		}
	};

	//------------------------------------------------------------------------------------------------------------------
	// Execute (POINT_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using PROJ_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(state);
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.BindInfo()->Cast<BindData>();

		GenericExecutor::ExecuteTernary<POINT_TYPE, PROJ_TYPE, PROJ_TYPE, POINT_TYPE>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
		    [&](const POINT_TYPE &point_in, const PROJ_TYPE &source, const PROJ_TYPE target) {
			    const auto source_str = source.val.GetString();
			    const auto target_str = target.val.GetString();

			    const auto crs = lstate.GetOrCreateProjection(source_str, target_str, info.normalize);

			    POINT_TYPE point_out;
			    const auto transformed = proj_trans(crs, PJ_FWD, proj_coord(point_in.a_val, point_in.b_val, 0, 0)).xy;
			    point_out.a_val = transformed.x;
			    point_out.b_val = transformed.y;

			    return point_out;
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (BOX_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {
		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
		using PROJ_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(state);
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.BindInfo()->Cast<BindData>();

		GenericExecutor::ExecuteTernary<BOX_TYPE, PROJ_TYPE, PROJ_TYPE, BOX_TYPE>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
		    [&](const BOX_TYPE &box_in, const PROJ_TYPE source, const PROJ_TYPE &target) {
			    const auto source_str = source.val.GetString();
			    const auto target_str = target.val.GetString();

			    const auto crs = lstate.GetOrCreateProjection(source_str, target_str, info.normalize);

			    // TODO: this may be interesting to use, but at that point we can only return a BOX_TYPE
			    constexpr int densify_pts = 0;
			    BOX_TYPE box_out;
			    proj_trans_bounds(lstate.proj_ctx, crs, PJ_FWD, box_in.a_val, box_in.b_val, box_in.c_val, box_in.d_val,
			                      &box_out.a_val, &box_out.b_val, &box_out.c_val, &box_out.d_val, densify_pts);
			    return box_out;
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.allocator;
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.BindInfo()->Cast<BindData>();

		TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
		    args.data[0], args.data[1], args.data[2], result,
		    [&](const string_t &blob, const string_t &source, const string_t &target) {
			    const auto source_str = source.GetString();
			    const auto target_str = target.GetString();

			    const auto crs = lstate.GetOrCreateProjection(source_str, target_str, info.normalize);

			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    sgl::ops::transform_vertices(alloc, geom, crs, [](void *arg, sgl::vertex_xyzm &vertex) {
				    const auto crs_ptr = static_cast<PJ *>(arg);
				    const auto transformed =
				        proj_trans(crs_ptr, PJ_FWD, proj_coord(vertex.x, vertex.y, vertex.z, 0)).xy;
				    vertex.x = transformed.x;
				    vertex.y = transformed.y;
			    });

			    return lstate.Serialize(result, geom);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY, TYPED)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometryTyped(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.allocator;
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.BindInfo()->Cast<TypedBindData>();

		const auto crs = lstate.GetOrCreateProjection(info.source_crs, info.target_crs, info.normalize);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			sgl::ops::transform_vertices(alloc, geom, crs, [](void *arg, sgl::vertex_xyzm &vertex) {
				const auto crs_ptr = static_cast<PJ *>(arg);
				const auto transformed = proj_trans(crs_ptr, PJ_FWD, proj_coord(vertex.x, vertex.y, vertex.z, 0)).xy;
				vertex.x = transformed.x;
				vertex.y = transformed.y;
			});

			return lstate.Serialize(result, geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	Transforms a geometry between two coordinate systems

	The source and target coordinate systems can be specified using any format that the [PROJ library](https://proj.org) supports.

	The third optional `always_xy` parameter can be used to force the input and output geometries to be interpreted as having a [easting, northing] coordinate axis order regardless of what the source and target coordinate system definition says. This is particularly useful when transforming to/from the [WGS84/EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (what most people think of when they hear "longitude"/"latitude" or "GPS coordinates"), which is defined as having a [latitude, longitude] axis order even though [longitude, latitude] is commonly used in practice (e.g. in [GeoJSON](https://tools.ietf.org/html/rfc7946)). More details available in the [PROJ documentation](https://proj.org/en/9.3/faq.html#why-is-the-axis-ordering-in-proj-not-consistent).

	DuckDB spatial vendors its own static copy of the PROJ database of coordinate systems, so if you have your own installation of PROJ on your system the available coordinate systems may differ to what's available in other GIS software.
	)";

	static constexpr auto EXAMPLE = R"(
	-- Transform a geometry from EPSG:4326 to EPSG:3857 (WGS84 to WebMercator)
	-- Note that since WGS84 is defined as having a [latitude, longitude] axis order
	-- we follow the standard and provide the input geometry using that axis order,
	-- but the output will be [easting, northing] because that is what's defined by
	-- WebMercator.

	SELECT
	    ST_Transform(
	        st_point(52.373123, 4.892360),
	        'EPSG:4326',
	        'EPSG:3857'
		);
	----
	POINT (544615.0239773799 6867874.103539125)

	-- Alternatively, let's say we got our input point from e.g. a GeoJSON file,
	-- which uses WGS84 but with [longitude, latitude] axis order. We can use the
	-- `always_xy` parameter to force the input geometry to be interpreted as having
	-- a [northing, easting] axis order instead, even though the source coordinate
	-- reference system definition (WGS84) says otherwise.

	SELECT 
	    ST_Transform(
	        -- note the axis order is reversed here
	        st_point(4.892360, 52.373123),
	        'EPSG:4326',
	        'EPSG:3857',
	        always_xy := true
		);
	----
	POINT (544615.0239773799 6867874.103539125)

	-- Transform a geometry from OSG36 British National Grid EPSG:27700 to EPSG:4326 WGS84
	-- Standard transform is often fine for the first few decimal places before being wrong
	-- which could result in an error starting at about 10m and possibly much more
	SELECT ST_Transform(bng, 'EPSG:27700', 'EPSG:4326', xy := true) AS without_grid_file
	FROM (SELECT ST_GeomFromText('POINT( 170370.718 11572.405 )') AS bng);
	----
	POINT (-5.202992651563592 49.96007490162923)

	-- By using an official NTv2 grid file, we can reduce the error down around the 9th decimal place
	-- which in theory is below a millimetre, and in practise unlikely that your coordinates are that precise
	-- British National Grid "NTv2 format files" download available here:
	-- https://www.ordnancesurvey.co.uk/products/os-net/for-developers
	SELECT ST_Transform(bng
		, '+proj=tmerc +lat_0=49 +lon_0=-2 +k=0.9996012717 +x_0=400000 +y_0=-100000 +ellps=airy +units=m +no_defs +nadgrids=/full/path/to/OSTN15-NTv2/OSTN15_NTv2_OSGBtoETRS.gsb +type=crs'
		, 'EPSG:4326', xy := true) AS with_grid_file
	FROM (SELECT ST_GeomFromText('POINT( 170370.718 11572.405 )') AS bng) t;
	----
	POINT (-5.203046090608746 49.96006137018598)
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Transform", [](ScalarFunctionBuilder &func) {
			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.SetReturnType(GeoTypes::BOX_2D());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteBox);
				variant.CanThrowErrors();
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.AddParameter("always_xy", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::BOX_2D());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteBox);
				variant.CanThrowErrors();
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecutePoint);
				variant.CanThrowErrors();
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.AddParameter("always_xy", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecutePoint);
				variant.CanThrowErrors();
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.AddParameter("always_xy", LogicalType::BOOLEAN);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalTypeId::GEOMETRY);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetBind(BindTyped);
				variant.SetSerialize(TypedBindData::Serialize);
				variant.SetDeserialize(TypedBindData::Deserialize);
				variant.SetFunction(ExecuteGeometryTyped);
				variant.CanThrowErrors();
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalTypeId::GEOMETRY);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.AddParameter("always_xy", LogicalType::BOOLEAN);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetBind(BindTyped);
				variant.SetSerialize(TypedBindData::Serialize);
				variant.SetDeserialize(TypedBindData::Deserialize);
				variant.SetFunction(ExecuteGeometryTyped);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//######################################################################################################################
// Geodesic Functions
//######################################################################################################################

constexpr auto EARTH_A = 6378137;
constexpr auto EARTH_F = 1 / 298.257223563;

//======================================================================================================================
// Bind Data
//======================================================================================================================
struct GeodesicBindData final : FunctionData {

	bool always_xy = false;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<GeodesicBindData>();
		result->always_xy = always_xy;
		return std::move(result);
	}

	bool Equals(const FunctionData &other) const override {
		auto &data = other.Cast<GeodesicBindData>();
		return always_xy == data.always_xy;
	}

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &ctx = input.GetClientContext();
		auto &func = input.GetBoundFunction();

		auto result = make_uniq<GeodesicBindData>();

		bool is_set = false;
		result->always_xy = SpatialSettings::AlwaysXY(ctx, is_set);

		if (!is_set) {
			constexpr auto raw_message =
			    "The '%s' function is sensitive to the coordinate axis order of the input geometry.\n"
			    "The current default for this function is to assume [LATITUDE, LONGITUDE] axis order.\n"
			    "This is expected to change to [LONGITUDE, LATITUDE] in the future.\n "
			    "Please explicitly set the 'geometry_always_xy' setting to avoid unexpected changes in behavior.\n"
			    " * 'SET geometry_always_xy = true' to make this function assume all geometries are [LONGITUDE, "
			    "LATITUDE]\n"
			    " * 'SET geometry_always_xy = false' to keep the current behavior and make this warning go away.";

			auto &logger = Logger::Get(ctx);
			logger.WriteLog("Spatial", LogLevel::LOG_WARNING, StringUtil::Format(raw_message, func.GetName().c_str()));
		}

		return std::move(result);
	}
};

//======================================================================================================================
// Local State
//======================================================================================================================

struct GeodesicLocalState final : FunctionLocalState {

	ArenaAllocator arena;
	GeometryAllocator alloc;
	geod_geodesic geod = {};
	geod_polygon poly = {};
	double accum = 0;

	explicit GeodesicLocalState(ClientContext &context, bool is_line)
	    : arena(BufferAllocator::Get(context)), alloc(arena) {

		// Initialize the geodesic object for earth
		geod_init(&geod, EARTH_A, EARTH_F);
		geod_polygon_init(&poly, is_line ? 1 : 0);
	}

	static unique_ptr<FunctionLocalState> InitPolygon(ExpressionState &state, const BoundFunctionExpression &expr,
	                                                  FunctionData *bind_data) {
		return make_uniq<GeodesicLocalState>(state.GetContext(), false);
	}

	static unique_ptr<FunctionLocalState> InitLine(ExpressionState &state, const BoundFunctionExpression &expr,
	                                               FunctionData *bind_data) {
		return make_uniq<GeodesicLocalState>(state.GetContext(), true);
	}

	static GeodesicLocalState &ResetAndGet(ExpressionState &state) {
		auto &local_state = ExecuteFunctionState::GetFunctionState(state)->Cast<GeodesicLocalState>();
		local_state.arena.Reset();
		return local_state;
	}

	void Deserialize(const string_t &blob, sgl::geometry &geom) {
		Serde::Deserialize(geom, arena, blob.GetDataUnsafe(), blob.GetSize());
	}
};

//======================================================================================================================
// ST_Area_Spheroid
//======================================================================================================================

struct ST_Area_Spheroid {

	//--------------------------------------------------------------------------------------o----------------------------
	// Execute (POLYGON_2D)
	//------------------------------------------------------------------------------------------------------------------

	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();

		auto &input = args.data[0];
		auto count = args.size();

		auto &ring_vec = ListVector::GetChild(input);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &coord_vec = ListVector::GetChild(ring_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetData<double>(coord_vec_children[0]);
		auto y_data = FlatVector::GetData<double>(coord_vec_children[1]);

		if (bdata.always_xy) {
			std::swap(x_data, y_data);
		}

		geod_geodesic geod = {};
		geod_init(&geod, EARTH_A, EARTH_F);

		geod_polygon poly = {};
		geod_polygon_init(&poly, 0);

		UnaryExecutor::Execute<list_entry_t, double>(input, result, count, [&](list_entry_t polygon) {
			const auto polygon_offset = polygon.offset;
			const auto polygon_length = polygon.length;

			bool first = true;
			double area = 0;
			for (idx_t ring_idx = polygon_offset; ring_idx < polygon_offset + polygon_length; ring_idx++) {
				const auto ring = ring_entries[ring_idx];
				const auto ring_offset = ring.offset;
				const auto ring_length = ring.length;

				geod_polygon_clear(&poly);
				// Note: the last point is the same as the first point, but geographiclib doesn't know that,
				// so skip it.
				for (idx_t coord_idx = ring_offset; coord_idx < ring_offset + ring_length - 1; coord_idx++) {
					geod_polygon_addpoint(&geod, &poly, x_data[coord_idx], y_data[coord_idx]);
				}
				double ring_area;
				geod_polygon_compute(&geod, &poly, 0, 1, &ring_area, nullptr);

				if (first) {
					// Add outer ring
					area += std::abs(ring_area);
					first = false;
				} else {
					// Subtract holes
					area -= std::abs(ring_area);
				}
			}
			return std::abs(area);
		});

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	template <bool ALWAYS_XY>
	struct Accumulate {
		static void Operation(void *arg, const sgl::geometry &part) {
			if (part.get_type() != sgl::geometry_type::POLYGON) {
				return;
			}

			auto &sstate = *static_cast<GeodesicLocalState *>(arg);

			// Calculate the area of the polygon
			const auto tail = part.get_last_part();
			auto ring = tail;
			if (!ring) {
				return;
			}

			const auto head = ring->get_next();

			do {
				ring = ring->get_next();

				const auto vertex_count = ring->get_vertex_count();
				if (vertex_count < 4) {
					continue;
				}

				geod_polygon_clear(&sstate.poly);

				// Dont add the last vertex
				for (uint32_t i = 0; i < vertex_count - 1; i++) {
					const auto vertex = ring->get_vertex_xy(i);
					if (ALWAYS_XY) {
						geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.y, vertex.x);
					} else {
						geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.x, vertex.y);
					}
				}

				double area = 0;
				geod_polygon_compute(&sstate.geod, &sstate.poly, 0, 1, &area, nullptr);

				if (ring == head) {
					sstate.accum += std::abs(area);
				} else {
					sstate.accum -= std::abs(area);
				}
			} while (ring != tail);
		}
	};

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		const auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();
		auto &lstate = GeodesicLocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &input) {
			sgl::geometry geom;
			lstate.Deserialize(input, geom);

			// Reset the state
			lstate.accum = 0;

			// Visit all polygons
			if (bdata.always_xy) {
				sgl::ops::visit_polygon_geometries(geom, &lstate, Accumulate<true>::Operation);
			} else {
				sgl::ops::visit_polygon_geometries(geom, &lstate, Accumulate<false>::Operation);
			}

			return lstate.accum;
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
    Returns the area of a geometry in meters, using an ellipsoidal model of the earth

    The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the area is returned in square meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the area using an ellipsoidal model of the earth. This is a highly accurate method for calculating the area of a polygon taking the curvature of the earth into account, but is also the slowest.

    Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon geometries.
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Area_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(GeodesicLocalState::InitPolygon);
				variant.SetBind(GeodesicBindData::Bind);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("poly", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetBind(GeodesicBindData::Bind);
				variant.SetFunction(ExecutePolygon);
				variant.CanThrowErrors();
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
			func.SetTag("category", "spheroid");
		});
	}
};

//======================================================================================================================
// ST_Perimeter_Spheroid
//======================================================================================================================

struct ST_Perimeter_Spheroid {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (POLYGON_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		const auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();

		auto &input = args.data[0];
		auto count = args.size();

		auto &ring_vec = ListVector::GetChild(input);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &coord_vec = ListVector::GetChild(ring_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetData<double>(coord_vec_children[0]);
		auto y_data = FlatVector::GetData<double>(coord_vec_children[1]);

		if (bdata.always_xy) {
			std::swap(x_data, y_data);
		}

		geod_geodesic geod = {};
		geod_init(&geod, EARTH_A, EARTH_F);

		geod_polygon poly = {};
		geod_polygon_init(&poly, 0);

		UnaryExecutor::Execute<list_entry_t, double>(input, result, count, [&](list_entry_t polygon) {
			const auto polygon_offset = polygon.offset;
			const auto polygon_length = polygon.length;
			double perimeter = 0;
			for (idx_t ring_idx = polygon_offset; ring_idx < polygon_offset + polygon_length; ring_idx++) {
				const auto ring = ring_entries[ring_idx];
				const auto ring_offset = ring.offset;
				const auto ring_length = ring.length;

				geod_polygon_clear(&poly);
				// Note: the last point is the same as the first point, but geographiclib doesn't know that,
				// so skip it.
				for (idx_t coord_idx = ring_offset; coord_idx < ring_offset + ring_length - 1; coord_idx++) {
					geod_polygon_addpoint(&geod, &poly, x_data[coord_idx], y_data[coord_idx]);
				}

				double ring_perimeter;
				geod_polygon_compute(&geod, &poly, 0, 1, nullptr, &ring_perimeter);

				perimeter += ring_perimeter;
			}
			return perimeter;
		});

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	template <bool ALWAYS_XY>
	struct Accumulate {
		static void Operation(void *arg, const sgl::geometry &part) {
			if (part.get_type() != sgl::geometry_type::POLYGON) {
				return;
			}

			auto &sstate = *static_cast<GeodesicLocalState *>(arg);

			// Calculate the perimeter of the polygon
			const auto tail = part.get_last_part();
			auto ring = tail;
			if (!ring) {
				return;
			}
			do {
				ring = ring->get_next();

				const auto vertex_count = ring->get_vertex_count();
				if (vertex_count < 4) {
					continue;
				}

				geod_polygon_clear(&sstate.poly);

				// Dont add the last vertex
				for (uint32_t i = 0; i < vertex_count - 1; i++) {
					const auto vertex = ring->get_vertex_xy(i);
					if (ALWAYS_XY) {
						geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.y, vertex.x);
					} else {
						geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.x, vertex.y);
					}
				}

				double perimeter = 0;
				geod_polygon_compute(&sstate.geod, &sstate.poly, 0, 1, nullptr, &perimeter);
				// Add the perimeter of the ring
				sstate.accum += perimeter;

			} while (ring != tail);
		}
	};

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &lstate = GeodesicLocalState::ResetAndGet(state);
		auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &input) {
			sgl::geometry geom;
			lstate.Deserialize(input, geom);

			// Reset the state
			lstate.accum = 0;

			// Visit all polygons
			if (bdata.always_xy) {
				sgl::ops::visit_polygon_geometries(geom, &lstate, Accumulate<true>::Operation);
			} else {
				sgl::ops::visit_polygon_geometries(geom, &lstate, Accumulate<false>::Operation);
			}

			return lstate.accum;
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the length of the perimeter in meters using an ellipsoidal model of the earths surface

		The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the length is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the perimeter using an ellipsoidal model of the earth. This is a highly accurate method for calculating the perimeter of a polygon taking the curvature of the earth into account, but is also the slowest.

		Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon geometries.
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Perimeter_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(GeodesicLocalState::InitPolygon);
				variant.SetBind(GeodesicBindData::Bind);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("poly", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetBind(GeodesicBindData::Bind);
				variant.SetFunction(ExecutePolygon);
				variant.CanThrowErrors();
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
			func.SetTag("category", "spheroid");
		});
	}
};

//======================================================================================================================
// ST_Length_Spheroid
//======================================================================================================================

struct ST_Length_Spheroid {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (LINESTRING)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		const auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();

		auto &line_vec = args.data[0];
		auto count = args.size();

		auto &coord_vec = ListVector::GetChild(line_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetData<double>(coord_vec_children[0]);
		auto y_data = FlatVector::GetData<double>(coord_vec_children[1]);

		if (bdata.always_xy) {
			std::swap(x_data, y_data);
		}

		geod_geodesic geod = {};
		geod_init(&geod, EARTH_A, EARTH_F);

		geod_polygon poly = {};
		geod_polygon_init(&poly, 1);

		UnaryExecutor::Execute<list_entry_t, double>(line_vec, result, count, [&](list_entry_t line) {
			geod_polygon_clear(&poly);

			const auto offset = line.offset;
			const auto length = line.length;
			// Loop over the segments
			for (idx_t j = offset; j < offset + length; j++) {
				geod_polygon_addpoint(&geod, &poly, x_data[j], y_data[j]);
			}
			double linestring_length;
			geod_polygon_compute(&geod, &poly, 0, 1, &linestring_length, nullptr);
			return linestring_length;
		});

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	template <bool ALWAYS_XY>
	struct Accumulate {
		static void Operation(void *arg, const sgl::geometry &part) {
			if (part.get_type() != sgl::geometry_type::LINESTRING) {
				return;
			}

			auto &sstate = *static_cast<GeodesicLocalState *>(arg);

			const auto vertex_count = part.get_vertex_count();
			if (vertex_count < 2) {
				return;
			}

			geod_polygon_clear(&sstate.poly);

			for (uint32_t i = 0; i < vertex_count; i++) {
				const auto vertex = part.get_vertex_xy(i);
				if (ALWAYS_XY) {
					geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.y, vertex.x);
				} else {
					geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.x, vertex.y);
				}
			}

			// Calculate the length of the linestring
			double length = 0;
			geod_polygon_compute(&sstate.geod, &sstate.poly, 0, 1, nullptr, &length);

			sstate.accum += length;
		}
	};

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		const auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();
		auto &lstate = GeodesicLocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &input) {
			sgl::geometry geom;
			lstate.Deserialize(input, geom);

			// Reset the state
			lstate.accum = 0;

			// Visit all linestrings
			if (bdata.always_xy) {
				sgl::ops::visit_linestring_geometries(geom, &lstate, Accumulate<true>::Operation);
			} else {
				sgl::ops::visit_linestring_geometries(geom, &lstate, Accumulate<false>::Operation);
			}

			return lstate.accum;
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the length of the input geometry in meters, using an ellipsoidal model of the earth

		The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the length is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the length using an ellipsoidal model of the earth. This is a highly accurate method for calculating the length of a line geometry taking the curvature of the earth into account, but is also the slowest.

		Returns `0.0` for any geometry that is not a `LINESTRING`, `MULTILINESTRING` or `GEOMETRYCOLLECTION` containing line geometries.
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Length_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(GeodesicLocalState::InitLine);
				variant.SetBind(GeodesicBindData::Bind);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetBind(GeodesicBindData::Bind);
				variant.SetFunction(ExecuteLineString);
				variant.CanThrowErrors();
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
			func.SetTag("category", "spheroid");
		});
	}
};

//======================================================================================================================
// ST_Distance_Spheroid
//======================================================================================================================

struct ST_Distance_Spheroid {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using DISTANCE_TYPE = PrimitiveType<double>;

		geod_geodesic geod = {};
		geod_init(&geod, EARTH_A, EARTH_F);

		const auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();

		if (bdata.always_xy) {
			GenericExecutor::ExecuteBinary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE>(
			    args.data[0], args.data[1], result, args.size(), [&](const POINT_TYPE &p1, const POINT_TYPE &p2) {
				    double distance;
				    geod_inverse(&geod, p1.b_val, p1.a_val, p2.b_val, p2.a_val, &distance, nullptr, nullptr);
				    return distance;
			    });
		} else {
			GenericExecutor::ExecuteBinary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE>(
			    args.data[0], args.data[1], result, args.size(), [&](const POINT_TYPE &p1, const POINT_TYPE &p2) {
				    double distance;
				    geod_inverse(&geod, p1.a_val, p1.b_val, p2.a_val, p2.b_val, &distance, nullptr, nullptr);
				    return distance;
			    });
		}
	}

	static constexpr auto DESCRIPTION = R"(
    Returns the distance between two geometries in meters using an ellipsoidal model of the earths surface

	The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the distance limit is expected to be in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library to solve the [inverse geodesic problem](https://en.wikipedia.org/wiki/Geodesics_on_an_ellipsoid#Solution_of_the_direct_and_inverse_problems), calculating the distance between two points using an ellipsoidal model of the earth. This is a highly accurate method for calculating the distance between two arbitrary points taking the curvature of the earths surface into account, but is also the slowest.
	)";

	static constexpr auto EXAMPLE = R"(
	-- Note: the coordinates are in WGS84 and [latitude, longitude] axis order
	-- Whats the distance between New York and Amsterdam (JFK and AMS airport)?
	SELECT st_distance_spheroid(
	st_point(40.6446, -73.7797),
	st_point(52.3130, 4.7725)
	);
	----
	5863418.7459356235
	-- Roughly 5863km!
	)";

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Distance_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("p1", GeoTypes::POINT_2D());
				variant.AddParameter("p2", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetBind(GeodesicBindData::Bind);

				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
			func.SetTag("category", "spheroid");
		});
	}
};

//======================================================================================================================
// ST_DWithin_Spheroid
//======================================================================================================================

struct ST_DWithin_Spheroid {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using DISTANCE_TYPE = PrimitiveType<double>;
		using BOOL_TYPE = PrimitiveType<bool>;

		geod_geodesic geod = {};
		geod_init(&geod, EARTH_A, EARTH_F);

		const auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<GeodesicBindData>();

		if (bdata.always_xy) {
			GenericExecutor::ExecuteTernary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE, BOOL_TYPE>(
			    args.data[0], args.data[1], args.data[2], result, args.size(),
			    [&](const POINT_TYPE &p1, const POINT_TYPE &p2, const DISTANCE_TYPE &limit) {
				    double distance;
				    geod_inverse(&geod, p1.b_val, p1.a_val, p2.b_val, p2.a_val, &distance, nullptr, nullptr);
				    return distance <= limit.val;
			    });
		} else {
			GenericExecutor::ExecuteTernary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE, BOOL_TYPE>(
			    args.data[0], args.data[1], args.data[2], result, args.size(),
			    [&](const POINT_TYPE &p1, const POINT_TYPE &p2, const DISTANCE_TYPE &limit) {
				    double distance;
				    geod_inverse(&geod, p1.a_val, p1.b_val, p2.a_val, p2.b_val, &distance, nullptr, nullptr);
				    return distance <= limit.val;
			    });
		}
	}

	static constexpr auto DESCRIPTION = R"(
		Returns if two POINT_2D's are within a target distance in meters, using an ellipsoidal model of the earths surface

		The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the distance is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library to solve the [inverse geodesic problem](https://en.wikipedia.org/wiki/Geodesics_on_an_ellipsoid#Solution_of_the_direct_and_inverse_problems), calculating the distance between two points using an ellipsoidal model of the earth. This is a highly accurate method for calculating the distance between two arbitrary points taking the curvature of the earths surface into account, but is also the slowest.
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_DWithin_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("p1", GeoTypes::POINT_2D());
				variant.AddParameter("p2", GeoTypes::POINT_2D());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::BOOLEAN);
				variant.SetBind(GeodesicBindData::Bind);

				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
			func.SetTag("category", "spheroid");
		});
	}
};

struct DuckDB_Proj_Version {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 0);
		PJ_INFO pj_info = proj_info();
		string_t version(pj_info.version);
		auto val = Value(version);
		result.Reference(val, count_t(args.size()));
	}

	static constexpr auto DESCRIPTION = R"(
		Returns a text description of the PROJ library version that is being used by this instance of DuckDB.
	)";

	static constexpr auto EXAMPLE = R"(
	SELECT duckdb_proj_version();
	┌───────────────────────┐
	│ duckdb_proj_version() │
	│        varchar        │
	├───────────────────────┤
	│ 9.1.1                 │geometry_always_xy
	└───────────────────────┘
	)";

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "DuckDB_Proj_Version", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "meta");
		});
	}
};

struct DuckDB_Proj_Compiled_Version {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 0);
		string_t version(pj_release);
		auto val = Value(version);
		result.Reference(val, count_t(args.size()));
	}

	static constexpr auto DESCRIPTION = R"(
		Returns a text description of the PROJ library version that this instance of DuckDB was compiled against.
	)";

	static constexpr auto EXAMPLE = R"(
	SELECT duckdb_proj_compiled_version();
	┌────────────────────────────────┐
	│ duckdb_proj_compiled_version() │
	│            varchar             │
	├────────────────────────────────┤
	│ Rel. 9.1.1, December 1st, 2022 │
	└────────────────────────────────┘
	)";

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "DuckDB_PROJ_Compiled_Version", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "meta");
		});
	}
};

} // namespace

//======================================================================================================================
// CRS Provider
//======================================================================================================================
namespace {

class SpatialCoordinateSystemGenerator : public DefaultGenerator {
private:
	SchemaCatalogEntry &schema;
	PJ_CONTEXT *ctx = nullptr;
	mutex proj_mutex;

public:
	SpatialCoordinateSystemGenerator(Catalog &catalog, SchemaCatalogEntry &schema)
	    : DefaultGenerator(catalog), schema(schema) {
		ctx = ProjModule::GetThreadProjContext();
	}

	~SpatialCoordinateSystemGenerator() override {
		if (ctx) {
			proj_context_destroy(ctx);
			ctx = nullptr;
		}
	}

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override {

		if (schema.name != DEFAULT_SCHEMA) {
			return nullptr;
		}

		// Try to split name by ":"
		auto parts = StringUtil::Split(entry_name.GetIdentifierName(), ":");
		if (parts.size() != 2) {
			return nullptr;
		}

		const auto &auth_name = parts[0];
		const auto &auth_code = parts[1];

		// We only support OGC and EPSG for now
		if (!StringUtil::CIEquals(auth_name, "EPSG") && !StringUtil::CIEquals(auth_name, "OGC")) {
			return nullptr;
		}

		// Create PJ object
		lock_guard<mutex> lock(proj_mutex);

		PJ *crs = proj_create_from_database(ctx, auth_name.c_str(), auth_code.c_str(), PJ_CATEGORY_CRS, false, nullptr);
		if (!crs) {
			return nullptr;
		}

		// Export to WKT2_2019
		string wkt_text;
		static const char *const options[] = {"MULTILINE=NO", nullptr};
		const auto wkt = proj_as_wkt(ctx, crs, PJ_WKT2_2019, options);
		if (wkt) {
			wkt_text = wkt;
		}

		// Export to PROJJSON
		string projjson_text;
		const auto pj_json = proj_as_projjson(ctx, crs, options);
		if (pj_json) {
			projjson_text = pj_json;
		}

		proj_destroy(crs);

		auto info = CreateCoordinateSystemInfo(entry_name, auth_name, auth_code, projjson_text, wkt_text);
		info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;

		auto result = make_uniq<CoordinateSystemCatalogEntry>(catalog, schema, info);
		return std::move(result);
	}

	vector<Identifier> GetDefaultEntries() override {

		if (schema.name != DEFAULT_SCHEMA) {
			return {};
		}

		vector<Identifier> entries;

		auto scan_authority = [&](const char *auth) {
			int ncrs = 0;
			PROJ_CRS_INFO **crs_info = proj_get_crs_info_list_from_database(ctx, auth, nullptr, &ncrs);

			if (crs_info) {
				for (int i = 0; i < ncrs; i++) {
					auto &auth_name = crs_info[i]->auth_name;
					auto &auth_code = crs_info[i]->code;

					if (!auth_name || !auth_code) {
						continue;
					}

					entries.emplace_back(StringUtil::Format("%s:%s", auth_name, auth_code));
				}
			}

			proj_crs_info_list_destroy(crs_info);
		};

		// Scan EPSG and OGC authority lists
		lock_guard<mutex> lock(proj_mutex);
		scan_authority("epsg");
		scan_authority("OGC");

		return entries;
	}

	static void Register(ExtensionLoader &loader) {

		auto &db = loader.GetDatabaseInstance();
		auto system_transaction = CatalogTransaction::GetSystemTransaction(db);
		auto &catalog = Catalog::GetSystemCatalog(db);
		auto &schema = catalog.GetSchema(system_transaction, Identifier::DefaultSchema());
		auto &duck_schema = schema.Cast<DuckSchemaEntry>();

		auto &set = duck_schema.GetCatalogSet(CatalogType::COORDINATE_SYSTEM_ENTRY);
		set.SetDefaultGenerator(make_uniq<SpatialCoordinateSystemGenerator>(catalog, schema));
	}
};

} // namespace
//######################################################################################################################
// Module Registration
//######################################################################################################################
bool IdentifyProjCRS(const char *crs, string &auth_name, string &auth_code) {
	PJ_CONTEXT *ctx = ProjModule::GetThreadProjContext();

	// Try to parse the CRS string as WKT or PROJJSON
	PJ *pj = proj_create(ctx, crs);
	if (!pj) {
		proj_context_destroy(ctx);
		return false;
	}

	int *confidence = nullptr;
	const auto candidates = proj_identify(ctx, pj, nullptr, nullptr, &confidence);
	if (!candidates) {
		proj_destroy(pj);
		proj_context_destroy(ctx);
		return false;
	}

	auto n_candidates = proj_list_get_count(candidates);
	if (n_candidates == 0) {
		proj_list_destroy(candidates);
		proj_int_list_destroy(confidence);
		proj_destroy(pj);
		proj_context_destroy(ctx);
		return false;
	}

	// We only care about the first candidate, which is the one with the highest confidence
	auto candidate = proj_list_get(ctx, candidates, 0);
	if (!candidate) {
		proj_list_destroy(candidates);
		proj_int_list_destroy(confidence);
		proj_destroy(pj);
		proj_context_destroy(ctx);
		return false;
	}

	if (confidence[0] < 70) {
		// The confidence is too low, so we consider it a failed identification
		proj_list_destroy(candidates);
		proj_int_list_destroy(confidence);
		proj_destroy(pj);
		proj_destroy(candidate);
		proj_context_destroy(ctx);
		return false;
	}

	const auto proj_auth_name = proj_get_id_auth_name(candidate, 0);
	const auto proj_auth_code = proj_get_id_code(candidate, 0);

	if (!proj_auth_name || !proj_auth_code) {
		proj_list_destroy(candidates);
		proj_int_list_destroy(confidence);
		proj_destroy(pj);
		proj_destroy(candidate);
		proj_context_destroy(ctx);
		return false;
	}

	auth_name = proj_auth_name;
	auth_code = proj_auth_code;

	proj_list_destroy(candidates);
	proj_int_list_destroy(confidence);
	proj_destroy(pj);
	proj_destroy(candidate);
	proj_context_destroy(ctx);

	return true;
}

void RegisterProjModule(ExtensionLoader &loader) {

	// Register the VFS for the proj.db database
	ProjModule::RegisterVFS(loader);

	// Coordinate Transform Function
	ST_Transform::Register(loader);

	// Geodesic Functions
	ST_Area_Spheroid::Register(loader);
	ST_Perimeter_Spheroid::Register(loader);
	ST_Length_Spheroid::Register(loader);
	ST_Distance_Spheroid::Register(loader);
	ST_DWithin_Spheroid::Register(loader);

	// Meta functions for proj lib
	DuckDB_Proj_Version::Register(loader);
	DuckDB_Proj_Compiled_Version::Register(loader);

	SpatialCoordinateSystemGenerator::Register(loader);
}

} // namespace duckdb
