#include "spatial/modules/proj/proj_module.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/function_builder.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/geometry/geometry_serialization.hpp"

#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/extension_util.hpp"

#include "proj.h"
#include "geodesic.h"
#include "sqlite3.h"

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
	static void RegisterVFS(DatabaseInstance &db);
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
void ProjModule::RegisterVFS(DatabaseInstance &db) {

	// Initialization lock around global proj state
	static mutex lock;

	lock_guard<mutex> g(lock);

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
	sqlite3_vfs_register(vfs, 1);

	// We set the default context proj.db path to the one in the binary here
	// Otherwise GDAL will try to load the proj.db from the system
	// Any PJ_CONTEXT we create after this will inherit these settings (on this thread?)
	auto path = StringUtil::Format("file:/proj.db?immutable=1&ptr=%llu&sz=%lu&max=%lu", static_cast<void *>(proj_db),
	                               proj_db_len, proj_db_len);

	proj_context_set_sqlite3_vfs_name(nullptr, "memvfs");

	// Try to open the database
	sqlite3 *sdb = nullptr;
	const auto sok = sqlite3_open_v2(path.c_str(), &sdb, SQLITE_OPEN_READONLY, "memvfs");
	if (sok != SQLITE_OK) {
		throw InternalException("Could not open sqlite3 memvfs database");
	}

	const auto ok = proj_context_set_database_path(nullptr, path.c_str(), nullptr, nullptr);
	if (!ok) {
		throw InternalException("Could not set proj.db path");
	}
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

struct ProjFunctionLocalState final : FunctionLocalState {

	PJ_CONTEXT *proj_ctx;
	ArenaAllocator arena;
	GeometryAllocator allocator;

	// Cache for PJ* objects
	unordered_map<std::pair<string, string>, ProjCRS> crs_cache;

	// Not copyable
	ProjFunctionLocalState(const ProjFunctionLocalState &) = delete;
	ProjFunctionLocalState &operator=(const ProjFunctionLocalState &) = delete;

	// Not movable
	ProjFunctionLocalState(ProjFunctionLocalState &&) = delete;
	ProjFunctionLocalState &operator=(ProjFunctionLocalState &&) = delete;

	explicit ProjFunctionLocalState(ClientContext &context)
	    : proj_ctx(ProjModule::GetThreadProjContext()), arena(BufferAllocator::Get(context)), allocator(arena) {
	}

	~ProjFunctionLocalState() override {
		// We need to clear the cache so that the unique_ptrs are destroyed before the context
		crs_cache.clear();
		proj_context_destroy(proj_ctx);
	}

	void Deserialize(const string_t &blob, sgl::geometry &geom);
	string_t Serialize(Vector &vector, const sgl::geometry &geom);

	static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
	                                           FunctionData *bind_data) {
		auto result = make_uniq<ProjFunctionLocalState>(state.GetContext());
		return std::move(result);
	}

	static ProjFunctionLocalState &ResetAndGet(ExpressionState &state) {
		auto &local_state = ExecuteFunctionState::GetFunctionState(state)->Cast<ProjFunctionLocalState>();
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
};

void ProjFunctionLocalState::Deserialize(const string_t &blob, sgl::geometry &geom) {
	Serde::Deserialize(geom, arena, blob.GetDataUnsafe(), blob.GetSize());
}

string_t ProjFunctionLocalState::Serialize(Vector &vector, const sgl::geometry &geom) {
	const auto size = Serde::GetRequiredSize(geom);
	auto blob = StringVector::EmptyString(vector, size);
	Serde::Serialize(geom, blob.GetDataWriteable(), size);
	blob.Finalize();
	return blob;
}

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

	static unique_ptr<FunctionData> Bind(ClientContext &ctx, ScalarFunction &, vector<unique_ptr<Expression>> &args) {
		auto result = make_uniq<BindData>();
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
		}
		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (POINT_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using PROJ_TYPE = PrimitiveType<string_t>;

		auto &lstate = ProjFunctionLocalState::ResetAndGet(state);
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.bind_info->Cast<BindData>();

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

		auto &lstate = ProjFunctionLocalState::ResetAndGet(state);
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.bind_info->Cast<BindData>();

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
		auto &lstate = ProjFunctionLocalState::ResetAndGet(state);
		auto &alloc = lstate.allocator;
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.bind_info->Cast<BindData>();

		TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
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
	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "ST_Transform", [](ScalarFunctionBuilder &func) {
			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.SetReturnType(GeoTypes::BOX_2D());

				variant.SetInit(ProjFunctionLocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteBox);
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.AddParameter("always_xy", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::BOX_2D());

				variant.SetInit(ProjFunctionLocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteBox);
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetInit(ProjFunctionLocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecutePoint);
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.AddParameter("always_xy", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetInit(ProjFunctionLocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecutePoint);
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(ProjFunctionLocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("source_crs", LogicalType::VARCHAR);
				variant.AddParameter("target_crs", LogicalType::VARCHAR);
				variant.AddParameter("always_xy", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(ProjFunctionLocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(ExecuteGeometry);
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

	//------------------------------------------------------------------------------------------------------------------
	// Execute (POLYGON_2D)
	//------------------------------------------------------------------------------------------------------------------

	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &input = args.data[0];
		auto count = args.size();

		auto &ring_vec = ListVector::GetEntry(input);
		auto ring_entries = ListVector::GetData(ring_vec);
		auto &coord_vec = ListVector::GetEntry(ring_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetData<double>(*coord_vec_children[0]);
		auto y_data = FlatVector::GetData<double>(*coord_vec_children[1]);

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
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &lstate = GeodesicLocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &input) {
			sgl::geometry geom;
			lstate.Deserialize(input, geom);

			// Reset the state
			lstate.accum = 0;

			// Visit all polygons
			sgl::ops::visit_polygon_geometries(geom, &lstate, [](void *arg, const sgl::geometry &part) {
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
						geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.x, vertex.y);
					}

					double area = 0;
					geod_polygon_compute(&sstate.geod, &sstate.poly, 0, 1, &area, nullptr);

					if (ring == head) {
						sstate.accum += std::abs(area);
					} else {
						sstate.accum -= std::abs(area);
					}
				} while (ring != tail);
			});

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
	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "ST_Area_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(GeodesicLocalState::InitPolygon);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("poly", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecutePolygon);
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

		auto &input = args.data[0];
		auto count = args.size();

		auto &ring_vec = ListVector::GetEntry(input);
		auto ring_entries = ListVector::GetData(ring_vec);
		auto &coord_vec = ListVector::GetEntry(ring_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetData<double>(*coord_vec_children[0]);
		auto y_data = FlatVector::GetData<double>(*coord_vec_children[1]);

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
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &lstate = GeodesicLocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &input) {
			sgl::geometry geom;
			lstate.Deserialize(input, geom);

			// Reset the state
			lstate.accum = 0;

			// Visit all polygons
			sgl::ops::visit_polygon_geometries(geom, &lstate, [](void *arg, const sgl::geometry &part) {
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
						geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.x, vertex.y);
					}

					double perimeter = 0;
					geod_polygon_compute(&sstate.geod, &sstate.poly, 0, 1, nullptr, &perimeter);
					// Add the perimeter of the ring
					sstate.accum += perimeter;

				} while (ring != tail);
			});

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
	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "ST_Perimeter_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(GeodesicLocalState::InitPolygon);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("poly", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecutePolygon);
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

		auto &line_vec = args.data[0];
		auto count = args.size();

		auto &coord_vec = ListVector::GetEntry(line_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetData<double>(*coord_vec_children[0]);
		auto y_data = FlatVector::GetData<double>(*coord_vec_children[1]);

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
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &lstate = GeodesicLocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &input) {
			sgl::geometry geom;
			lstate.Deserialize(input, geom);

			// Reset the state
			lstate.accum = 0;

			// Visit all linestrings
			sgl::ops::visit_linestring_geometries(geom, &lstate, [](void *arg, const sgl::geometry &part) {
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
					geod_polygon_addpoint(&sstate.geod, &sstate.poly, vertex.x, vertex.y);
				}

				// Calculate the length of the linestring
				double length = 0;
				geod_polygon_compute(&sstate.geod, &sstate.poly, 0, 1, nullptr, &length);

				sstate.accum += length;
			});

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
	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "ST_Length_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(GeodesicLocalState::InitLine);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecuteLineString);
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

		GenericExecutor::ExecuteBinary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE>(
		    args.data[0], args.data[1], result, args.size(), [&](const POINT_TYPE &p1, const POINT_TYPE &p2) {
			    double distance;
			    geod_inverse(&geod, p1.a_val, p1.b_val, p2.a_val, p2.b_val, &distance, nullptr, nullptr);
			    return distance;
		    });
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

	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "ST_Distance_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("p1", GeoTypes::POINT_2D());
				variant.AddParameter("p2", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetFunction(Execute);
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

		GenericExecutor::ExecuteTernary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE, BOOL_TYPE>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
		    [&](const POINT_TYPE &p1, const POINT_TYPE &p2, const DISTANCE_TYPE &limit) {
			    double distance;
			    geod_inverse(&geod, p1.a_val, p1.b_val, p2.a_val, p2.b_val, &distance, nullptr, nullptr);
			    return distance <= limit.val;
		    });
	}

	static constexpr auto DESCRIPTION = R"(
		Returns if two POINT_2D's are within a target distance in meters, using an ellipsoidal model of the earths surface

		The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the distance is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library to solve the [inverse geodesic problem](https://en.wikipedia.org/wiki/Geodesics_on_an_ellipsoid#Solution_of_the_direct_and_inverse_problems), calculating the distance between two points using an ellipsoidal model of the earth. This is a highly accurate method for calculating the distance between two arbitrary points taking the curvature of the earths surface into account, but is also the slowest.
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "ST_DWithin_Spheroid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("p1", GeoTypes::POINT_2D());
				variant.AddParameter("p2", GeoTypes::POINT_2D());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetFunction(Execute);
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
		result.Reference(val);
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
	│ 9.1.1                 │
	└───────────────────────┘
	)";

	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "DuckDB_Proj_Version", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(Execute);
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
		result.Reference(val);
	}

	static constexpr auto DESCRIPTION = R"(
		Returns a text description of the PROJ library version that that this instance of DuckDB was compiled against.
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

	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "DuckDB_PROJ_Compiled_Version", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(Execute);
			});

			func.SetExample(EXAMPLE);
			func.SetDescription(DESCRIPTION);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "meta");
		});
	}
};

} // namespace

//######################################################################################################################
// Module Registration
//######################################################################################################################
void RegisterProjModule(DatabaseInstance &db) {

	// Register the VFS for the proj.db database
	ProjModule::RegisterVFS(db);

	// Coordinate Transform Function
	ST_Transform::Register(db);

	// Geodesic Functions
	ST_Area_Spheroid::Register(db);
	ST_Perimeter_Spheroid::Register(db);
	ST_Length_Spheroid::Register(db);
	ST_Distance_Spheroid::Register(db);
	ST_DWithin_Spheroid::Register(db);

	// Meta functions for proj lib
	DuckDB_Proj_Version::Register(db);
	DuckDB_Proj_Compiled_Version::Register(db);
}

} // namespace duckdb