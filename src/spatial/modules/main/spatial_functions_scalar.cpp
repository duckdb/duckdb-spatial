// Spatial
#include "spatial/modules/main/spatial_functions.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/geometry/vertex.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/binary_reader.hpp"
#include "spatial/util/function_builder.hpp"
#include "spatial/util/math.hpp"

// DuckDB
#include "duckdb/common/constants.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/vector_operations/variadic_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

#include "spatial/util/distance_extract.hpp"
#include "spatial/spatial_settings.hpp"

// Extra
#include "yyjson.h"

namespace duckdb {

namespace {

//######################################################################################################################
// Util
//######################################################################################################################

//======================================================================================================================
// LocalState
//======================================================================================================================

class LocalState final : public FunctionLocalState {
public:
	explicit LocalState(ClientContext &context) : arena(BufferAllocator::Get(context)), allocator(arena) {
	}

	static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
	                                           FunctionData *bind_data);
	static LocalState &ResetAndGet(ExpressionState &state);

	// De/Serialize geometries
	void Deserialize(const string_t &blob, sgl::geometry &geom);
	void Deserialize(const string_t &blob, sgl::prepared_geometry &geom);
	sgl::geometry *DeserializeToHeap(const string_t &blob);
	string_t Serialize(Vector &vector, const sgl::geometry &geom);

	ArenaAllocator &GetArena() {
		return arena;
	}
	GeometryAllocator &GetAllocator() {
		return allocator;
	}

private:
	ArenaAllocator arena;
	GeometryAllocator allocator;
};

unique_ptr<FunctionLocalState> LocalState::Init(ExpressionState &state, const BoundFunctionExpression &expr,
                                                FunctionData *bind_data) {
	return make_uniq_base<FunctionLocalState, LocalState>(state.GetContext());
}

LocalState &LocalState::ResetAndGet(ExpressionState &state) {
	auto &local_state = ExecuteFunctionState::GetFunctionState(state)->Cast<LocalState>();
	local_state.arena.Reset();
	return local_state;
}

void LocalState::Deserialize(const string_t &blob, sgl::geometry &geom) {
	Serde::Deserialize(geom, arena, blob.GetDataUnsafe(), blob.GetSize());
}

void LocalState::Deserialize(const string_t &blob, sgl::prepared_geometry &geom) {
	Serde::DeserializePrepared(geom, arena, blob.GetDataUnsafe(), blob.GetSize());
}

sgl::geometry *LocalState::DeserializeToHeap(const string_t &blob) {
	const auto mem = arena.AllocateAligned(sizeof(sgl::geometry));
	const auto geom = new (mem) sgl::geometry();
	Serde::Deserialize(*geom, arena, blob.GetDataUnsafe(), blob.GetSize());
	return geom;
}

string_t LocalState::Serialize(Vector &vector, const sgl::geometry &geom) {
	const auto size = Serde::GetRequiredSize(geom);
	auto blob = StringVector::EmptyString(vector, size);
	Serde::Serialize(geom, blob.GetDataWriteable(), size);
	blob.Finalize();
	return blob;
}
} // namespace

namespace {

//######################################################################################################################
// Functions
//######################################################################################################################

//======================================================================================================================
// ST_Affine
//======================================================================================================================

struct ST_Affine {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute3D(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		const auto row_count = args.size();

		UnifiedVectorFormat geom_format;
		args.data[0].ToUnifiedFormat(geom_format);

		UnifiedVectorFormat matrix_elems[12];
		idx_t matrix_idx[12];

		for (idx_t i = 1; i < 13; i++) {
			args.data[i].ToUnifiedFormat(matrix_elems[i - 1]);
		}

		for (idx_t out_idx = 0; out_idx < args.size(); out_idx++) {

			// Reset the arena after every iteration, to avoid holding onto too much memory
			lstate.GetArena().Reset();

			const auto geom_idx = geom_format.sel->get_index(out_idx);
			if (!geom_format.validity.RowIsValid(geom_idx)) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			bool all_valid = true;
			for (idx_t j = 0; j < 12; j++) {
				matrix_idx[j] = matrix_elems[j].sel->get_index(out_idx);
				all_valid = all_valid && matrix_elems[j].validity.RowIsValid(matrix_idx[j]);
			}

			if (!all_valid) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			// Setup the matrix
			auto matrix = sgl::affine_matrix::identity();
			matrix.v[0] = UnifiedVectorFormat::GetData<double>(matrix_elems[0])[matrix_idx[0]]; // a
			matrix.v[1] = UnifiedVectorFormat::GetData<double>(matrix_elems[1])[matrix_idx[1]]; // b
			matrix.v[2] = UnifiedVectorFormat::GetData<double>(matrix_elems[2])[matrix_idx[2]]; // c

			matrix.v[3] = UnifiedVectorFormat::GetData<double>(matrix_elems[9])[matrix_idx[9]]; // xoff

			matrix.v[4] = UnifiedVectorFormat::GetData<double>(matrix_elems[3])[matrix_idx[3]]; // d
			matrix.v[5] = UnifiedVectorFormat::GetData<double>(matrix_elems[4])[matrix_idx[4]]; // e
			matrix.v[6] = UnifiedVectorFormat::GetData<double>(matrix_elems[5])[matrix_idx[5]]; // f

			matrix.v[7] = UnifiedVectorFormat::GetData<double>(matrix_elems[10])[matrix_idx[10]]; // yoff

			matrix.v[8] = UnifiedVectorFormat::GetData<double>(matrix_elems[6])[matrix_idx[6]];  // g
			matrix.v[9] = UnifiedVectorFormat::GetData<double>(matrix_elems[7])[matrix_idx[7]];  // h
			matrix.v[10] = UnifiedVectorFormat::GetData<double>(matrix_elems[8])[matrix_idx[8]]; // i

			matrix.v[11] = UnifiedVectorFormat::GetData<double>(matrix_elems[11])[matrix_idx[11]]; // zoff

			// Deserialize the geometry
			auto geom_blob = UnifiedVectorFormat::GetData<string_t>(geom_format)[geom_idx];
			sgl::geometry geom;
			lstate.Deserialize(geom_blob, geom);

			// Apply the transformation
			sgl::ops::affine_transform(alloc, geom, matrix);

			// Serialize the result
			FlatVector::GetDataMutable<string_t>(result)[out_idx] = lstate.Serialize(result, geom);
		}

		if (row_count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static void Execute2D(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		VariadicExecutor::Execute<string_t, string_t, double, double, double, double, double, double>(
		    args, result,
		    [&](const string_t &geom_blob, const double a, const double b, const double d, const double e,
		        const double xoff, const double yoff) {
			    // Reset the arena after every iteration, to avoid holding onto too much memory
			    lstate.GetArena().Reset();

			    // Deserialize the geometry
			    sgl::geometry geom;
			    lstate.Deserialize(geom_blob, geom);

			    // Setup the matrix
			    auto matrix = sgl::affine_matrix::identity();
			    matrix.v[0] = a;    // a
			    matrix.v[1] = b;    // b
			    matrix.v[3] = xoff; // xoff
			    matrix.v[4] = d;    // d
			    matrix.v[5] = e;    // e
			    matrix.v[7] = yoff; // yoff

			    // Transform the geometry
			    sgl::ops::affine_transform(alloc, geom, matrix);

			    // Serialize the result
			    return lstate.Serialize(result, geom);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Affine", [](ScalarFunctionBuilder &func) {
			// GEOMETRY (3D)
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("a", LogicalType::DOUBLE);
				variant.AddParameter("b", LogicalType::DOUBLE);
				variant.AddParameter("c", LogicalType::DOUBLE);
				variant.AddParameter("d", LogicalType::DOUBLE);
				variant.AddParameter("e", LogicalType::DOUBLE);
				variant.AddParameter("f", LogicalType::DOUBLE);
				variant.AddParameter("g", LogicalType::DOUBLE);
				variant.AddParameter("h", LogicalType::DOUBLE);
				variant.AddParameter("i", LogicalType::DOUBLE);
				variant.AddParameter("xoff", LogicalType::DOUBLE);
				variant.AddParameter("yoff", LogicalType::DOUBLE);
				variant.AddParameter("zoff", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute3D);
			});

			// GEOMETRY (2D)
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("a", LogicalType::DOUBLE);
				variant.AddParameter("b", LogicalType::DOUBLE);
				variant.AddParameter("d", LogicalType::DOUBLE);
				variant.AddParameter("e", LogicalType::DOUBLE);
				variant.AddParameter("xoff", LogicalType::DOUBLE);
				variant.AddParameter("yoff", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute2D);
			});

			func.SetDescription(R"(
				Applies an affine transformation to a geometry.

				For the 2D variant, the transformation matrix is defined as follows:
				```
				| a b xoff |
				| d e yoff |
				| 0 0 1    |
				```

				For the 3D variant, the transformation matrix is defined as follows:
				```
				| a b c xoff |
				| d e f yoff |
				| g h i zoff |
				| 0 0 0 1    |
				```

				The transformation is applied to all vertices of the geometry.
			)");

			func.SetExample(R"(
				-- Translate a point by (2, 3)
				SELECT ST_Affine(ST_Point(1, 1),
				                 1, 0,   -- a, b
				                 0, 1,   -- d, e
				                 2, 3);  -- xoff, yoff
				----
				POINT (3 4)

				-- Scale a geometry by factor 2 in X and Y
				SELECT ST_Affine(ST_Point(1, 1),
				                 2, 0, 0,   -- a, b, c
				                 0, 2, 0,   -- d, e, f
				                 0, 0, 1,   -- g, h, i
				                 0, 0, 0);  -- xoff, yoff, zoff
				----
				POINT (2 2)
			)");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});

		// Add helper macros
		FunctionBuilder::RegisterMacro(loader, "ST_Scale", [](MacroFunctionBuilder &builder) {
			builder.AddDefinition(
			    {"geom", "xs", "ys", "zs"}, "ST_Affine(geom, xs, 0, 0, 0, ys, 0, 0, 0, zs, 0, 0, 0)",
			    "Scales a geometry in X, Y and Z direction. This is a shorthand macro for calling ST_Affine.",
			    R"(
					-- Scale a point by factor 2 in X and 3 in Y
					SELECT ST_Scale(ST_Point(1, 1), 2, 3);
					----
					POINT (2 3)

					-- Scale a 3D point
					SELECT ST_Scale(
						ST_GeomFromText('POINT Z(1 2 3)'),
						2, 2, 2
					);
					----
					POINT Z (2 4 6)
				)");
			builder.AddDefinition(
			    {"geom", "xs", "ys"}, "ST_Affine(geom, xs, 0, 0, 0, ys, 0, 0, 0, 1, 0, 0, 0)",
			    "Scales a geometry in X and Y direction. This is a shorthand macro for calling ST_Affine.");
		});

		FunctionBuilder::RegisterMacro(loader, "ST_Translate", [](MacroFunctionBuilder &builder) {
			builder.AddDefinition(
			    {"geom", "dx", "dy", "dz"}, "ST_Affine(geom, 1, 0, dx, 0, 1, dy, 0, 0, 1, dz, 0, 0)",
			    "Translates a geometry in X, Y and Z direction. This is a shorthand macro for calling ST_Affine.",
			    R"(
					-- Translate a point by (2, 3)
					SELECT ST_Translate(ST_Point(1, 1), 2, 3);
					----
					POINT (3 4)

					-- Translate a linestring
					SELECT ST_Translate(
						ST_GeomFromText('LINESTRING(0 0, 1 1)'),
						5, -2
					);
					----
					LINESTRING (5 -2, 6 -1)
				)");
			builder.AddDefinition(
			    {"geom", "dx", "dy"}, "ST_Affine(geom, 1, 0, dx, 0, 1, dy, 0, 0, 1, 0, 0, 0)",
			    "Translates a geometry in X and Y direction. This is a shorthand macro for calling ST_Affine.");
		});

		FunctionBuilder::RegisterMacro(loader, "ST_TransScale", [](MacroFunctionBuilder &builder) {
			builder.AddDefinition({"geom", "dx", "dy", "xs", "ys"},
			                      "ST_Affine(geom, xs, 0, 0, 0, ys, 0, 0, 0, 1, dx * xs, dy * ys, 0)",
			                      "Translates and then scales a geometry in X and Y direction. This is a shorthand "
			                      "macro for calling ST_Affine.",
			                      R"(
                                      -- Translate by (1, 2) then scale by (2, 3)
                                      SELECT ST_TransScale(ST_Point(1, 1), 1, 2, 2, 3);
                                      ----
                                      POINT (4 9)
                                  )");
		});

		FunctionBuilder::RegisterMacro(loader, "ST_RotateX", [](MacroFunctionBuilder &builder) {
			builder.AddDefinition(
			    {"geom", "radians"},
			    "ST_Affine(geom, 1, 0, 0, 0, COS(radians), -SIN(radians), 0, SIN(radians), COS(radians), 0, 0, 0)",
			    "Rotates a geometry around the X axis. This is a shorthand macro for calling ST_Affine.",
			    R"(
					-- Rotate a 3D point 90 degrees (π/2 radians) around the X-axis
					SELECT ST_RotateX(ST_GeomFromText('POINT Z(0 1 0)'), pi()/2);
					----
					POINT Z (0 0 1)
				)");
		});

		FunctionBuilder::RegisterMacro(loader, "ST_RotateY", [](MacroFunctionBuilder &builder) {
			builder.AddDefinition(
			    {"geom", "radians"},
			    "ST_Affine(geom, COS(radians), 0, SIN(radians), 0, 1, 0, -SIN(radians), 0, COS(radians), 0, 0, 0)",
			    "Rotates a geometry around the Y axis. This is a shorthand macro for calling ST_Affine.",
			    R"(
					-- Rotate a 3D point 90 degrees (π/2 radians) around the Y-axis
					SELECT ST_RotateY(ST_GeomFromText('POINT Z(1 0 0)'), pi()/2);
					----
					POINT Z (0 0 -1)
				)");
		});

		FunctionBuilder::RegisterMacro(loader, "ST_RotateZ", [](MacroFunctionBuilder &builder) {
			builder.AddDefinition(
			    {"geom", "radians"},
			    "ST_Affine(geom, COS(radians), -SIN(radians), 0, SIN(radians), COS(radians), 0, 0, 0, 1, 0, 0, 0)",
			    "Rotates a geometry around the Z axis. This is a shorthand macro for calling ST_Affine.",
			    R"(
					-- Rotate a point 90 degrees (π/2 radians) around the Z-axis
					SELECT ST_RotateZ(ST_Point(1, 0), pi()/2);
					----
					POINT (0 1)
				)");
		});

		// Alias for ST_RotateZ
		FunctionBuilder::RegisterMacro(loader, "ST_Rotate", [](MacroFunctionBuilder &builder) {
			builder.AddDefinition({"geom", "radians"}, "ST_RotateZ(geom, radians)", "Alias of ST_RotateZ");
		});
	}
};

//======================================================================================================================
// ST_Area
//======================================================================================================================

struct ST_Area {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);
			return sgl::ops::get_area(geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void PolygonAreaFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &input = args.data[0];
		auto count = args.size();

		auto &ring_vec = ListVector::GetChildMutable(input);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &coord_vec = ListVector::GetChildMutable(ring_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetDataMutable<double>(coord_vec_children[0]);
		auto y_data = FlatVector::GetDataMutable<double>(coord_vec_children[1]);

		UnaryExecutor::Execute<list_entry_t, double>(input, result, count, [&](list_entry_t polygon) {
			auto polygon_offset = polygon.offset;
			auto polygon_length = polygon.length;

			bool first = true;
			double area = 0;
			for (idx_t ring_idx = polygon_offset; ring_idx < polygon_offset + polygon_length; ring_idx++) {
				auto ring = ring_entries[ring_idx];
				auto ring_offset = ring.offset;
				auto ring_length = ring.length;

				double sum = 0;
				for (idx_t coord_idx = ring_offset; coord_idx < ring_offset + ring_length - 1; coord_idx++) {
					sum += (x_data[coord_idx] * y_data[coord_idx + 1]) - (x_data[coord_idx + 1] * y_data[coord_idx]);
				}
				sum = std::abs(sum);
				if (first) {
					// Add outer ring
					area = sum * 0.5;
					first = false;
				} else {
					// Subtract holes
					area -= sum * 0.5;
				}
			}
			return area;
		});

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void LineStringAreaFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		UnaryExecutor::Execute<list_entry_t, double>(input, result, args.size(), [](list_entry_t) { return 0; });
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static void PointAreaFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using AREA_TYPE = PrimitiveType<double>;
		GenericExecutor::ExecuteUnary<POINT_TYPE, AREA_TYPE>(args.data[0], result, args.size(),
		                                                     [](POINT_TYPE) { return 0; });
	}

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D
	//------------------------------------------------------------------------------------------------------------------
	static void BoxAreaFunction(DataChunk &args, ExpressionState &state, Vector &result) {

		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
		using AREA_TYPE = PrimitiveType<double>;

		GenericExecutor::ExecuteUnary<BOX_TYPE, AREA_TYPE>(args.data[0], result, args.size(), [&](BOX_TYPE &box) {
			auto minx = box.a_val;
			auto miny = box.b_val;
			auto maxx = box.c_val;
			auto maxy = box.d_val;
			return AREA_TYPE {(maxx - minx) * (maxy - miny)};
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr const char *DESCRIPTION = R"(
    Compute the area of a geometry.

    Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon
	geometries.

	The area is in the same units as the spatial reference system of the geometry.

    The `POINT_2D` and `LINESTRING_2D` overloads of this function always return `0.0` but are included for completeness.
	)";

	static constexpr const char *EXAMPLE = R"(
    SELECT ST_Area('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY);
	-- 1.0
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {

		FunctionBuilder::RegisterScalar(loader, "ST_Area", [](ScalarFunctionBuilder &func) {
			// GEOMETRY
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			// POLYGON_2D
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetFunction(PolygonAreaFunction);
			});

			// LINESTRING_2D
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetFunction(LineStringAreaFunction);
			});

			// POINT_2D
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetFunction(PointAreaFunction);
			});

			// BOX_2D
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetFunction(BoxAreaFunction);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_AsGeoJSON
//======================================================================================================================

using namespace duckdb_yyjson_spatial;

class JSONAllocator {
	// Stolen from the JSON extension :)
public:
	explicit JSONAllocator(ArenaAllocator &allocator)
	    : allocator(allocator), yyjson_allocator({Allocate, Reallocate, Free, &allocator}) {
	}
	yyjson_alc *GetYYJSONAllocator() {
		return &yyjson_allocator;
	}
	void Reset() {
		allocator.Reset();
	}

private:
	static void *Allocate(void *ctx, size_t size) {
		const auto alloc = static_cast<ArenaAllocator *>(ctx);
		return alloc->AllocateAligned(size);
	}
	static void *Reallocate(void *ctx, void *ptr, size_t old_size, size_t size) {
		const auto alloc = static_cast<ArenaAllocator *>(ctx);
		return alloc->ReallocateAligned(data_ptr_cast(ptr), old_size, size);
	}
	static void Free(void *ctx, void *ptr) {
		// NOP because ArenaAllocator can't free
	}
	ArenaAllocator &allocator;
	yyjson_alc yyjson_allocator;
};

struct ST_AsGeoJSON {

	//------------------------------------------------------------------------------------------------------------------
	// JSON Formatting Functions
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Move these into SGL at some point, make non-recursive
	static void FormatCoord(const sgl::geometry *geom, yyjson_mut_doc *doc, yyjson_mut_val *obj) {
		const auto vertex_type = geom->get_vertex_type();
		const auto vertex_count = geom->get_vertex_count();

		if (vertex_count == 0) {
			// Make empty
			const auto coord = yyjson_mut_arr(doc);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coord);
			return;
		}

		// GeoJSON does not support M values, so we ignore them
		switch (vertex_type) {
		case sgl::vertex_type::XY:
		case sgl::vertex_type::XYM: {
			const auto coord = yyjson_mut_arr(doc);
			const auto vert = geom->get_vertex_xy(0);
			yyjson_mut_arr_add_real(doc, coord, vert.x);
			yyjson_mut_arr_add_real(doc, coord, vert.y);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coord);

		} break;
		case sgl::vertex_type::XYZ:
		case sgl::vertex_type::XYZM: {
			const auto coord = yyjson_mut_arr(doc);
			const auto vert = geom->get_vertex_xyzm(0);

			yyjson_mut_arr_add_real(doc, coord, vert.x);
			yyjson_mut_arr_add_real(doc, coord, vert.y);
			yyjson_mut_arr_add_real(doc, coord, vert.z);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coord);

		} break;
		default:
			D_ASSERT(false);
			break;
		}
	}

	static void FormatCoords(const sgl::geometry *geom, yyjson_mut_doc *doc, yyjson_mut_val *obj) {
		const auto vertex_type = geom->get_vertex_type();
		const auto vertex_count = geom->get_vertex_count();

		// GeoJSON does not support M values, so we ignore them
		switch (vertex_type) {
		case sgl::vertex_type::XY:
		case sgl::vertex_type::XYM: {
			for (uint32_t i = 0; i < vertex_count; i++) {
				const auto coord = yyjson_mut_arr(doc);
				const auto vert = geom->get_vertex_xy(i);
				yyjson_mut_arr_add_real(doc, coord, vert.x);
				yyjson_mut_arr_add_real(doc, coord, vert.y);
				yyjson_mut_arr_append(obj, coord);
			}
		} break;
		case sgl::vertex_type::XYZ:
		case sgl::vertex_type::XYZM: {
			for (uint32_t i = 0; i < vertex_count; i++) {
				const auto coord = yyjson_mut_arr(doc);
				const auto vert = geom->get_vertex_xyzm(i);

				yyjson_mut_arr_add_real(doc, coord, vert.x);
				yyjson_mut_arr_add_real(doc, coord, vert.y);
				yyjson_mut_arr_add_real(doc, coord, vert.z);
				yyjson_mut_arr_append(obj, coord);
			}
		} break;
		default:
			D_ASSERT(false);
			break;
		}
	}

	static void FormatRecursive(const sgl::geometry *geom, yyjson_mut_doc *doc, yyjson_mut_val *obj) {
		switch (geom->get_type()) {
		case sgl::geometry_type::POINT: {
			yyjson_mut_obj_add_str(doc, obj, "type", "Point");
			FormatCoord(geom, doc, obj);
		} break;
		case sgl::geometry_type::LINESTRING: {
			yyjson_mut_obj_add_str(doc, obj, "type", "LineString");
			const auto coords = yyjson_mut_arr(doc);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coords);
			FormatCoords(geom, doc, coords);
		} break;
		case sgl::geometry_type::POLYGON: {
			yyjson_mut_obj_add_str(doc, obj, "type", "Polygon");
			const auto coords = yyjson_mut_arr(doc);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coords);

			const auto tail = geom->get_last_part();
			auto head = tail;
			if (head) {
				do {
					head = head->get_next();
					const auto ring = yyjson_mut_arr(doc);
					FormatCoords(head, doc, ring);
					yyjson_mut_arr_append(coords, ring);
				} while (head != tail);
			}
		} break;
		case sgl::geometry_type::MULTI_POINT: {
			yyjson_mut_obj_add_str(doc, obj, "type", "MultiPoint");

			const auto coords = yyjson_mut_arr(doc);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coords);

			const auto tail = geom->get_last_part();
			auto head = tail;

			if (head) {
				do {
					head = head->get_next();
					FormatCoords(head, doc, coords);
				} while (head != tail);
			}
		} break;
		case sgl::geometry_type::MULTI_LINESTRING: {
			yyjson_mut_obj_add_str(doc, obj, "type", "MultiLineString");

			const auto coords = yyjson_mut_arr(doc);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coords);

			const auto tail = geom->get_last_part();
			auto head = tail;

			if (head) {
				do {
					head = head->get_next();
					const auto line = yyjson_mut_arr(doc);
					FormatCoords(head, doc, line);
					yyjson_mut_arr_append(coords, line);
				} while (head != tail);
			}
		} break;
		case sgl::geometry_type::MULTI_POLYGON: {
			yyjson_mut_obj_add_str(doc, obj, "type", "MultiPolygon");

			const auto coords = yyjson_mut_arr(doc);
			yyjson_mut_obj_add_val(doc, obj, "coordinates", coords);

			const auto tail = geom->get_last_part();
			auto head = tail;

			if (head) {
				do {
					head = head->get_next();
					const auto poly = yyjson_mut_arr(doc);

					const auto ring_tail = head->get_last_part();
					auto ring_head = ring_tail;
					if (ring_head) {
						do {
							ring_head = ring_head->get_next();
							const auto ring = yyjson_mut_arr(doc);
							FormatCoords(ring_head, doc, ring);
							yyjson_mut_arr_append(poly, ring);
						} while (ring_head != ring_tail);
					}
					yyjson_mut_arr_append(coords, poly);
				} while (head != tail);
			}
		} break;
		case sgl::geometry_type::GEOMETRY_COLLECTION: {
			yyjson_mut_obj_add_str(doc, obj, "type", "GeometryCollection");

			const auto geoms = yyjson_mut_arr(doc);
			yyjson_mut_obj_add_val(doc, obj, "geometries", geoms);

			const auto tail = geom->get_last_part();
			auto head = tail;

			if (head) {
				do {
					head = head->get_next();
					const auto sub_geom = yyjson_mut_obj(doc);
					FormatRecursive(head, doc, sub_geom);
					yyjson_mut_arr_append(geoms, sub_geom);
				} while (head != tail);
			}
		} break;
		default:
			D_ASSERT(false);
			break;
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		JSONAllocator allocator(lstate.GetArena());

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			const auto doc = yyjson_mut_doc_new(allocator.GetYYJSONAllocator());
			const auto obj = yyjson_mut_obj(doc);
			yyjson_mut_doc_set_root(doc, obj);

			FormatRecursive(&geom, doc, obj);

			size_t json_size = 0;
			char *json_data = yyjson_mut_write_opts(doc, 0, allocator.GetYYJSONAllocator(), &json_size, nullptr);
			// Because the arena allocator only resets after each pipeline invocation, we can safely just point into the
			// arena here without needing to copy the data to the string heap with StringVector::AddString
			return string_t {json_data, static_cast<uint32_t>(json_size)};
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	    Returns the geometry as a GeoJSON fragment

	    This does not return a complete GeoJSON document, only the geometry fragment.
		To construct a complete GeoJSON document or feature, look into using the DuckDB JSON extension in conjunction with this function.
		This function supports geometries with Z values, but not M values. M values are ignored.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_AsGeoJSON('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY);
		----
		{"type":"Polygon","coordinates":[[[0.0, 0.0], [0.0, 1.0], [1.0, 1.0], [1.0, 0.0], [0.0, 0.0]]]}

		-- Convert a geometry into a full GeoJSON feature (requires the JSON extension to be loaded)
		SELECT CAST({
			type: 'Feature',
			geometry: ST_AsGeoJSON(ST_Point(1, 2)),
			properties: {
				name: 'my_point'
			}
		} AS JSON);
		----
		{"type":"Feature","geometry":{"type":"Point","coordinates":[1.0, 2.0]},"properties":{"name":"my_point"}}
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_AsGeoJSON", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::JSON());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_AsText
//======================================================================================================================

struct ST_AsText {

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto &input = args.data[0];
		auto count = args.size();
		CoreVectorOperations::Point2DToVarchar(input, result, count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	// TODO: We want to format these to trim trailing zeros
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto &input = args.data[0];
		auto count = args.size();
		CoreVectorOperations::LineString2DToVarchar(input, result, count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	// TODO: We want to format these to trim trailing zeros
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto count = args.size();
		auto &input = args.data[0];
		CoreVectorOperations::Polygon2DToVarchar(input, result, count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto count = args.size();
		auto &input = args.data[0];
		CoreVectorOperations::Box2DToVarchar(input, result, count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr const char *DESCRIPTION = R"(
		Returns the geometry as a WKT string
	)";

	static constexpr const char *EXAMPLE = R"(
		SELECT ST_MakeEnvelope(0, 0, 1, 1);
		----
		POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_AsText", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(ExecutePoint);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(ExecuteLineString);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(ExecutePolygon);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(ExecuteBox);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_AsWKB
//======================================================================================================================

struct ST_AsWKB {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		return Geometry::ToBinary(args.data[0], result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = "Returns the geometry as a WKB (Well-Known-Binary) blob";
	static constexpr auto EXAMPLE = R"(
		SELECT ST_AsWKB('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY)::BLOB;
		----
		\x01\x03\x00\x00\x00\x01\x00\x00\x00\x05...
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_AsWKB", [](ScalarFunctionBuilder &func) {
			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_AsHEXWKB
//======================================================================================================================

struct ST_AsHEXWKB {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			const auto size = blob.GetSize();
			const auto data = const_data_ptr_cast(blob.GetData());

			auto blob_size = size * 2; // every byte is rendered as two characters
			auto blob_str = StringVector::EmptyString(result, blob_size);
			auto blob_ptr = blob_str.GetDataWriteable();

			idx_t str_idx = 0;
			for (idx_t i = 0; i < size; i++) {
				const auto byte = data[i];
				const auto byte_a = byte >> 4;
				const auto byte_b = byte & 0x0F;
				blob_ptr[str_idx++] = Blob::HEX_TABLE[byte_a];
				blob_ptr[str_idx++] = Blob::HEX_TABLE[byte_b];
			}

			blob_str.Finalize();
			return blob_str;
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr const char *DESCRIPTION = R"(
		Returns the geometry as a HEXWKB string
	)";

	static constexpr const char *EXAMPLE = R"(
		SELECT ST_AsHexWKB('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY);
		----
		01030000000100000005000000000000000000000000000...
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_AsHEXWKB", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_AsSVG
//======================================================================================================================

struct ST_AsSVG {

	//------------------------------------------------------------------------------------------------------------------
	// SVG Formatting Functions
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Move this to sgl once we have proper double formatting. And make non-recursive please.

	static void FormatPoint(const sgl::geometry *geom, vector<char> &buffer, int32_t max_digits, bool rel) {
		D_ASSERT(geom->get_type() == sgl::geometry_type::POINT);
		if (geom->is_empty()) {
			return;
		}
		const auto vert = geom->get_vertex_xy(0);
		if (rel) {
			constexpr auto x = "x=\"";
			constexpr auto y = "y=\"";
			buffer.insert(buffer.end(), x, x + 3);
			MathUtil::format_coord(vert.x, buffer, max_digits);
			buffer.push_back('"');
			buffer.push_back(' ');
			buffer.insert(buffer.end(), y, y + 3);
			MathUtil::format_coord(-vert.y, buffer, max_digits);
			buffer.push_back('"');
		} else {
			constexpr auto cx = "cx=\"";
			constexpr auto cy = "cy=\"";
			buffer.insert(buffer.end(), cx, cx + 4);
			MathUtil::format_coord(vert.x, buffer, max_digits);
			buffer.push_back('"');
			buffer.push_back(' ');
			buffer.insert(buffer.end(), cy, cy + 4);
			MathUtil::format_coord(-vert.y, buffer, max_digits);
			buffer.push_back('"');
		}
	}

	static void FormatLineString(const sgl::geometry *geom, vector<char> &buffer, int32_t max_digits, bool rel,
	                             bool close) {
		D_ASSERT(geom->get_type() == sgl::geometry_type::LINESTRING);

		const auto vertex_count = geom->get_vertex_count();
		if (vertex_count == 0) {
			return;
		}

		sgl::vertex_xy last_vert = geom->get_vertex_xy(0);
		buffer.push_back('M');
		buffer.push_back(' ');
		MathUtil::format_coord(last_vert.x, -last_vert.y, buffer, max_digits);

		if (vertex_count == 1) {
			return;
		}

		buffer.push_back(' ');
		buffer.push_back(rel ? 'l' : 'L');

		if (rel) {
			for (uint32_t i = 1; i < vertex_count; i++) {
				if (i == vertex_count - 1 && close) {
					buffer.push_back(' ');
					buffer.push_back('z');
				} else {
					const auto vert = geom->get_vertex_xy(i);
					const auto delta = vert - last_vert;
					last_vert = vert;
					buffer.push_back(' ');
					MathUtil::format_coord(delta.x, -delta.y, buffer, max_digits);
				}
			}
		} else {
			for (uint32_t i = 1; i < vertex_count; i++) {
				if (i == vertex_count - 1 && close) {
					buffer.push_back(' ');
					buffer.push_back('Z');
				} else {
					const auto vert = geom->get_vertex_xy(i);
					buffer.push_back(' ');
					MathUtil::format_coord(vert.x, -vert.y, buffer, max_digits);
				}
			}
		}
	}

	static void FormatPolygon(const sgl::geometry *geom, vector<char> &buffer, int32_t max_digits, bool rel) {
		const auto tail = geom->get_last_part();
		auto head = tail;
		if (head) {
			do {
				head = head->get_next();
				FormatLineString(head, buffer, max_digits, rel, true);
			} while (head != tail);
		}
	}

	static void FormatRecursive(const sgl::geometry *geom, vector<char> &buffer, int32_t max_digits, bool rel) {
		switch (geom->get_type()) {
		case sgl::geometry_type::POINT:
			FormatPoint(geom, buffer, max_digits, rel);
			break;
		case sgl::geometry_type::LINESTRING:
			FormatLineString(geom, buffer, max_digits, rel, false);
			break;
		case sgl::geometry_type::POLYGON:
			FormatPolygon(geom, buffer, max_digits, rel);
			break;
		case sgl::geometry_type::MULTI_POINT: {
			const auto tail = geom->get_last_part();
			auto head = tail;
			if (head) {
				do {
					head = head->get_next();
					FormatPoint(head, buffer, max_digits, rel);
					if (head != tail) {
						buffer.push_back(',');
					}
				} while (head != tail);
			}
		} break;
		case sgl::geometry_type::MULTI_LINESTRING: {
			const auto tail = geom->get_last_part();
			auto head = tail;
			if (head) {
				do {
					head = head->get_next();
					FormatLineString(head, buffer, max_digits, rel, false);
					if (head != tail) {
						buffer.push_back(' ');
					}
				} while (head != tail);
			}
		} break;
		case sgl::geometry_type::MULTI_POLYGON: {
			const auto tail = geom->get_last_part();
			auto head = tail;
			if (head) {
				do {
					head = head->get_next();
					FormatPolygon(head, buffer, max_digits, rel);
					if (head != tail) {
						buffer.push_back(' ');
					}
				} while (head != tail);
			}
		} break;
		case sgl::geometry_type::GEOMETRY_COLLECTION: {
			const auto tail = geom->get_last_part();
			auto head = tail;
			if (head) {
				do {
					head = head->get_next();
					FormatRecursive(head, buffer, max_digits, rel);
					if (head != tail) {
						buffer.push_back(';');
					}
				} while (head != tail);
			}
		} break;
		default:
			D_ASSERT(false);
			break;
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		vector<char> buffer;

		TernaryExecutor::Execute<string_t, bool, int32_t, string_t>(
		    args.data[0], args.data[1], args.data[2], result,
		    [&](const string_t &blob, const bool rel, const int32_t max_digits) {
			    // Clear buffer
			    buffer.clear();

			    // Deserialize geometry
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (max_digits < 0 || max_digits > 15) {
				    throw InvalidInputException("ST_AsSVG: Precision must be between 0 and 15");
			    }

			    FormatRecursive(&geom, buffer, max_digits, rel);

			    return StringVector::AddString(result, buffer.data(), buffer.size());
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	    Convert the geometry into a SVG fragment or path

		The SVG fragment is returned as a string. The fragment is a path element that can be used in an SVG document.
		The second boolean argument specifies whether the path should be relative or absolute.
		The third argument specifies the maximum number of digits to use for the coordinates.

		Points are formatted as cx/cy using absolute coordinates or x/y using relative coordinates.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_AsSVG('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY, false, 15);
		----
		M 0 0 L 0 -1 1 -1 1 0 Z
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_AsSVG", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("relative", LogicalType::BOOLEAN);
				variant.AddParameter("precision", LogicalType::INTEGER);

				variant.SetReturnType(LogicalType::VARCHAR);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_Centroid
//======================================================================================================================
// The GEOMETRY version is currently implemented in the GEOS module

struct ST_Centroid {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			sgl::vertex_xyzm centroid = {0, 0, 0, 0};
			if (!sgl::ops::get_centroid(geom, centroid)) {
				// Couldnt get the centroid, return an empty point.
				// NOTE: This is the PostGIS behavior, the docs are wrong.
				sgl::geometry empty(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
				return lstate.Serialize(result, empty);
			}

			// Otherwise, create a point geometry with the centroid
			sgl::geometry point(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
			point.set_vertex_array(&centroid, 1);

			// Serialize the point
			return lstate.Serialize(result, point);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	// Provided for completeness sake
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		result.Reference(args.data[0]);
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto count = args.size();
		UnifiedVectorFormat format;
		input.ToUnifiedFormat(format);

		auto line_vertex_entries = FlatVector::GetData<list_entry_t>(input);
		auto &line_vertex_vec = ListVector::GetChildMutable(input);
		auto &line_vertex_vec_children = StructVector::GetEntries(line_vertex_vec);
		auto line_x_data = FlatVector::GetDataMutable<double>(line_vertex_vec_children[0]);
		auto line_y_vec = FlatVector::GetDataMutable<double>(line_vertex_vec_children[1]);

		auto &point_vertex_children = StructVector::GetEntries(result);
		auto point_x_data = FlatVector::GetDataMutable<double>(point_vertex_children[0]);
		auto point_y_data = FlatVector::GetDataMutable<double>(point_vertex_children[1]);
		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {

			auto in_row_idx = format.sel->get_index(out_row_idx);
			if (format.validity.RowIsValid(in_row_idx)) {
				auto line = line_vertex_entries[in_row_idx];
				auto line_offset = line.offset;
				auto line_length = line.length;

				double total_x = 0;
				double total_y = 0;
				double total_length = 0;

				// To calculate the centroid of a line, we calculate the centroid of each segment
				// and then weight the segment centroids by the length of the segment.
				// The final centroid is the sum of the weighted segment centroids divided by the total length.
				for (idx_t coord_idx = line_offset; coord_idx < line_offset + line_length - 1; coord_idx++) {
					auto x1 = line_x_data[coord_idx];
					auto y1 = line_y_vec[coord_idx];
					auto x2 = line_x_data[coord_idx + 1];
					auto y2 = line_y_vec[coord_idx + 1];

					auto segment_length = sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
					total_length += segment_length;
					total_x += (x1 + x2) * 0.5 * segment_length;
					total_y += (y1 + y2) * 0.5 * segment_length;
				}

				point_x_data[out_row_idx] = total_x / total_length;
				point_y_data[out_row_idx] = total_y / total_length;

			} else {
				FlatVector::SetNull(result, out_row_idx, true);
			}
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto count = args.size();
		UnifiedVectorFormat format;
		input.ToUnifiedFormat(format);

		auto poly_entries = FlatVector::GetData<list_entry_t>(input);
		auto &ring_vec = ListVector::GetChildMutable(input);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &vertex_vec = ListVector::GetChildMutable(ring_vec);
		auto &vertex_vec_children = StructVector::GetEntries(vertex_vec);
		auto x_data = FlatVector::GetDataMutable<double>(vertex_vec_children[0]);
		auto y_data = FlatVector::GetDataMutable<double>(vertex_vec_children[1]);

		auto &centroid_children = StructVector::GetEntries(result);
		auto centroid_x_data = FlatVector::GetDataMutable<double>(centroid_children[0]);
		auto centroid_y_data = FlatVector::GetDataMutable<double>(centroid_children[1]);

		for (idx_t in_row_idx = 0; in_row_idx < count; in_row_idx++) {
			if (format.validity.RowIsValid(in_row_idx)) {
				auto poly = poly_entries[in_row_idx];
				auto poly_offset = poly.offset;
				auto poly_length = poly.length;

				double poly_centroid_x = 0;
				double poly_centroid_y = 0;
				double poly_area = 0;

				// To calculate the centroid of a polygon, we calculate the centroid of each ring
				// and then weight the ring centroids by the area of the ring.
				// The final centroid is the sum of the weighted ring centroids divided by the total area.
				for (idx_t ring_idx = poly_offset; ring_idx < poly_offset + poly_length; ring_idx++) {
					auto ring = ring_entries[ring_idx];
					auto ring_offset = ring.offset;
					auto ring_length = ring.length;

					double ring_centroid_x = 0;
					double ring_centroid_y = 0;
					double ring_area = 0;

					// To calculate the centroid of a ring, we calculate the centroid of each triangle
					// and then weight the triangle centroids by the area of the triangle.
					// The final centroid is the sum of the weighted triangle centroids divided by the ring area.
					for (idx_t coord_idx = ring_offset; coord_idx < ring_offset + ring_length - 1; coord_idx++) {
						auto x1 = x_data[coord_idx];
						auto y1 = y_data[coord_idx];
						auto x2 = x_data[coord_idx + 1];
						auto y2 = y_data[coord_idx + 1];

						auto tri_area = (x1 * y2) - (x2 * y1);
						ring_centroid_x += (x1 + x2) * tri_area;
						ring_centroid_y += (y1 + y2) * tri_area;
						ring_area += tri_area;
					}
					ring_area *= 0.5;

					ring_centroid_x /= (ring_area * 6);
					ring_centroid_y /= (ring_area * 6);

					if (ring_idx == poly_offset) {
						// The first ring is the outer ring, and the remaining rings are holes.
						// For the outer ring, we add the area and centroid to the total area and centroid.
						poly_area += ring_area;
						poly_centroid_x += ring_centroid_x * ring_area;
						poly_centroid_y += ring_centroid_y * ring_area;
					} else {
						// For holes, we subtract the area and centroid from the total area and centroid.
						poly_area -= ring_area;
						poly_centroid_x -= ring_centroid_x * ring_area;
						poly_centroid_y -= ring_centroid_y * ring_area;
					}
				}
				centroid_x_data[in_row_idx] = poly_centroid_x / poly_area;
				centroid_y_data[in_row_idx] = poly_centroid_y / poly_area;
			} else {
				FlatVector::SetNull(result, in_row_idx, true);
			}
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D/F
	//------------------------------------------------------------------------------------------------------------------
	template <class T>
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto count = args.size();
		UnifiedVectorFormat format;
		input.ToUnifiedFormat(format);
		auto &box_children = StructVector::GetEntries(input);
		auto minx_data = FlatVector::GetDataMutable<T>(box_children[0]);
		auto miny_data = FlatVector::GetDataMutable<T>(box_children[1]);
		auto maxx_data = FlatVector::GetDataMutable<T>(box_children[2]);
		auto maxy_data = FlatVector::GetDataMutable<T>(box_children[3]);

		auto &centroid_children = StructVector::GetEntries(result);
		auto centroid_x_data = FlatVector::GetDataMutable<double>(centroid_children[0]);
		auto centroid_y_data = FlatVector::GetDataMutable<double>(centroid_children[1]);

		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {
			auto in_row_idx = format.sel->get_index(out_row_idx);
			if (format.validity.RowIsValid(in_row_idx)) {
				centroid_x_data[out_row_idx] = (minx_data[in_row_idx] + maxx_data[in_row_idx]) * 0.5;
				centroid_y_data[out_row_idx] = (miny_data[in_row_idx] + maxy_data[in_row_idx]) * 0.5;
			} else {
				FlatVector::SetNull(result, out_row_idx, true);
			}
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// TODO: add example & desc
	static constexpr auto DESCRIPTION = "Returns the centroid of a geometry";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Centroid", [&](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.SetReturnType(GeoTypes::POINT_2D());
				variant.SetFunction(ExecutePoint);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(GeoTypes::POINT_2D());
				variant.SetFunction(ExecuteLineString);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(GeoTypes::POINT_2D());
				variant.SetFunction(ExecutePolygon);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.SetReturnType(GeoTypes::POINT_2D());
				variant.SetFunction(ExecuteBox<double>);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2DF());
				variant.SetReturnType(GeoTypes::POINT_2D());
				variant.SetFunction(ExecuteBox<float>);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Collect
//======================================================================================================================

struct ST_Collect {

	//------------------------------------------------------------------------------------------------------------------
	// Execution
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto &child_vec = ListVector::GetChild(args.data[0]);

		UnifiedVectorFormat input_vdata;
		child_vec.ToUnifiedFormat(input_vdata);

		UnaryExecutor::Execute<list_entry_t, string_t>(
		    args.data[0], result, args.size(), [&](const list_entry_t &entry) {
			    const auto offset = entry.offset;
			    const auto length = entry.length;

			    if (length == 0) {
				    const sgl::geometry empty(sgl::geometry_type::GEOMETRY_COLLECTION, false, false);
				    return lstate.Serialize(result, empty);
			    }

			    // First figure out if we have Z or M
			    bool has_z = false;
			    bool has_m = false;

			    // First pass, check if we have Z or M
			    for (idx_t out_idx = offset; out_idx < offset + length; out_idx++) {
				    const auto row_idx = input_vdata.sel->get_index(out_idx);
				    if (!input_vdata.validity.RowIsValid(row_idx)) {
					    continue;
				    }

				    auto &blob = UnifiedVectorFormat::GetData<string_t>(input_vdata)[row_idx];

				    // TODO: Peek dont deserialize
				    sgl::geometry geom;
				    lstate.Deserialize(blob, geom);
				    has_z = has_z || geom.has_z();
				    has_m = has_m || geom.has_m();
			    }

			    bool all_points = true;
			    bool all_lines = true;
			    bool all_polygons = true;

			    sgl::geometry collection(sgl::geometry_type::INVALID, has_z, has_m);

			    for (idx_t out_idx = offset; out_idx < offset + length; out_idx++) {
				    const auto row_idx = input_vdata.sel->get_index(out_idx);
				    if (!input_vdata.validity.RowIsValid(row_idx)) {
					    continue;
				    }

				    auto &blob = UnifiedVectorFormat::GetData<string_t>(input_vdata)[row_idx];

				    // Deserialize and allocate on heap
				    const auto geom = lstate.DeserializeToHeap(blob);

				    // TODO: Peek dont deserialize
				    if (geom->is_empty()) {
					    continue;
				    }

				    all_points = all_points && geom->get_type() == sgl::geometry_type::POINT;
				    all_lines = all_lines && geom->get_type() == sgl::geometry_type::LINESTRING;
				    all_polygons = all_polygons && geom->get_type() == sgl::geometry_type::POLYGON;

				    // Force Z and M so that the dimensions match
				    sgl::ops::force_zm(lstate.GetAllocator(), *geom, has_z, has_m, 0, 0);

				    // Append to collection
				    collection.append_part(geom);
			    }

			    if (collection.is_empty()) {
				    // NULL's and EMPTY do not contribute to the result.
				    sgl::geometry empty(sgl::geometry_type::GEOMETRY_COLLECTION, has_z, has_m);
				    return lstate.Serialize(result, empty);
			    }

			    // Figure out the type of the collection
			    if (all_points) {
				    collection.set_type(sgl::geometry_type::MULTI_POINT);
			    } else if (all_lines) {
				    collection.set_type(sgl::geometry_type::MULTI_LINESTRING);
			    } else if (all_polygons) {
				    collection.set_type(sgl::geometry_type::MULTI_POLYGON);
			    } else {
				    collection.set_type(sgl::geometry_type::GEOMETRY_COLLECTION);
			    }

			    // Serialize the collection
			    return lstate.Serialize(result, collection);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	Collects a list of geometries into a collection geometry.
	- If all geometries are `POINT`'s, a `MULTIPOINT` is returned.
	- If all geometries are `LINESTRING`'s, a `MULTILINESTRING` is returned.
	- If all geometries are `POLYGON`'s, a `MULTIPOLYGON` is returned.
	- Otherwise if the input collection contains a mix of geometry types, a `GEOMETRYCOLLECTION` is returned.

	Empty and `NULL` geometries are ignored. If all geometries are empty or `NULL`, a `GEOMETRYCOLLECTION EMPTY` is returned.
	)";

	static constexpr auto EXAMPLE = R"(
	-- With all POINT's, a MULTIPOINT is returned
	SELECT ST_Collect([ST_Point(1, 2), ST_Point(3, 4)]);
	----
	MULTIPOINT (1 2, 3 4)

	-- With mixed geometry types, a GEOMETRYCOLLECTION is returned
	SELECT ST_Collect([ST_Point(1, 2), ST_GeomFromText('LINESTRING(3 4, 5 6)')]);
	----
	GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (3 4, 5 6))

	-- Note that the empty geometry is ignored, so the result is a MULTIPOINT
	SELECT ST_Collect([ST_Point(1, 2), NULL, ST_GeomFromText('GEOMETRYCOLLECTION EMPTY')]);
	----
	MULTIPOINT (1 2)

	-- If all geometries are empty or NULL, a GEOMETRYCOLLECTION EMPTY is returned
	SELECT ST_Collect([NULL, ST_GeomFromText('GEOMETRYCOLLECTION EMPTY')]);
	----
	GEOMETRYCOLLECTION EMPTY

	-- Tip: You can use the `ST_Collect` function together with the `list()` aggregate function to collect multiple rows of geometries into a single geometry collection:

	CREATE TABLE points (geom GEOMETRY);

	INSERT INTO points VALUES (ST_Point(1, 2)), (ST_Point(3, 4));

	SELECT ST_Collect(list(geom)) FROM points;
	----
	MULTIPOINT (1 2, 3 4)
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Collect", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geoms", LogicalType::LIST(LogicalType::GEOMETRY()));
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_CollectionExtract
//======================================================================================================================

struct ST_CollectionExtract {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (TYPED)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteTyped(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, int32_t, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &blob, int32_t requested_type) {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    const auto type = geom.get_type();
			    const auto has_z = geom.has_z();
			    const auto has_m = geom.has_m();

			    // The output geometry to fill with the extracted geometries
			    sgl::geometry output(sgl::geometry_type::INVALID, has_z, has_m);

			    switch (requested_type) {
			    case 1:
				    switch (type) {
				    case sgl::geometry_type::MULTI_POINT:
				    case sgl::geometry_type::POINT:
					    return blob;
				    case sgl::geometry_type::GEOMETRY_COLLECTION: {
					    // collect all points
					    sgl::ops::extract_points(geom, output);
					    return lstate.Serialize(result, output);
				    }
				    case sgl::geometry_type::MULTI_LINESTRING:
				    case sgl::geometry_type::MULTI_POLYGON:
					    output.set_type(sgl::geometry_type::MULTI_POINT);
					    return lstate.Serialize(result, output);
				    default:
					    output.set_type(sgl::geometry_type::POINT);
					    return lstate.Serialize(result, output);
				    }
				    break;
			    case 2:
				    switch (type) {
				    case sgl::geometry_type::MULTI_LINESTRING:
				    case sgl::geometry_type::LINESTRING:
					    return blob;
				    case sgl::geometry_type::GEOMETRY_COLLECTION: {
					    // collect all lines
					    sgl::ops::extract_linestrings(geom, output);
					    return lstate.Serialize(result, output);
				    }
				    case sgl::geometry_type::MULTI_POINT:
				    case sgl::geometry_type::MULTI_POLYGON:
					    output.set_type(sgl::geometry_type::MULTI_LINESTRING);
					    return lstate.Serialize(result, output);
				    default:
					    output.set_type(sgl::geometry_type::LINESTRING);
					    return lstate.Serialize(result, output);
				    }
				    break;
			    case 3:
				    switch (type) {
				    case sgl::geometry_type::MULTI_POLYGON:
				    case sgl::geometry_type::POLYGON:
					    return blob;
				    case sgl::geometry_type::GEOMETRY_COLLECTION: {
					    // collect all polygons
					    sgl::ops::extract_polygons(geom, output);
					    return lstate.Serialize(result, output);
				    }
				    case sgl::geometry_type::MULTI_POINT:
				    case sgl::geometry_type::MULTI_LINESTRING:
					    output.set_type(sgl::geometry_type::MULTI_POLYGON);
					    return lstate.Serialize(result, output);
				    default:
					    output.set_type(sgl::geometry_type::POLYGON);
					    return lstate.Serialize(result, output);
				    }
				    break;
			    default:
				    throw InvalidInputException("Invalid requested type parameter for collection extract, must be 1 "
				                                "(POINT), 2 (LINESTRING) or 3 (POLYGON)");
			    }
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (AUTO)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteAuto(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &input) {
			// TODO: Peek without deserialize
			sgl::geometry geom;
			lstate.Deserialize(input, geom);

			if (geom.get_type() != sgl::geometry_type::GEOMETRY_COLLECTION) {
				return input;
			}
			if (geom.is_empty()) {
				return input;
			}

			// Find the highest dimension of the geometries in the collection
			// Empty geometries are ignored
			const auto dim = sgl::ops::get_max_surface_dimension(geom, true);

			switch (dim) {
			// Empty GeometryCollection case
			case -1: {
				const sgl::geometry empty(sgl::geometry_type::GEOMETRY_COLLECTION, geom.has_z(), geom.has_m());
				return lstate.Serialize(result, empty);
			}
			// Point case
			case 0: {
				sgl::geometry multi;
				sgl::ops::extract_points(geom, multi);
				return lstate.Serialize(result, multi);
			}
			// LineString case
			case 1: {
				sgl::geometry multi;
				sgl::ops::extract_linestrings(geom, multi);
				return lstate.Serialize(result, multi);
			}
			// Polygon case
			case 2: {
				sgl::geometry multi;
				sgl::ops::extract_polygons(geom, multi);
				return lstate.Serialize(result, multi);
			}
			default: {
				throw InternalException("Invalid dimension in collection extract");
			}
			}
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Extracts geometries from a GeometryCollection into a typed multi geometry.

		If the input geometry is a GeometryCollection, the function will return a multi geometry, determined by the `type` parameter.
		- if `type` = 1, returns a MultiPoint containing all the Points in the collection
		- if `type` = 2, returns a MultiLineString containing all the LineStrings in the collection
		- if `type` = 3, returns a MultiPolygon containing all the Polygons in the collection

		If no `type` parameters is provided, the function will return a multi geometry matching the highest "surface dimension"
		of the contained geometries. E.g. if the collection contains only Points, a MultiPoint will be returned. But if the
		collection contains both Points and LineStrings, a MultiLineString will be returned. Similarly, if the collection
		contains Polygons, a MultiPolygon will be returned. Contained geometries of a lower surface dimension will be ignored.

		If the input geometry contains nested GeometryCollections, their geometries will be extracted recursively and included
		into the final multi geometry as well.

		If the input geometry is not a GeometryCollection, the function will return the input geometry as is.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_CollectionExtract('MULTIPOINT(1 2, 3 4)'::GEOMETRY, 1);
		-- MULTIPOINT (1 2, 3 4)
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_CollectionExtract", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("type", LogicalType::INTEGER);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteTyped);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteAuto);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_Contains
//======================================================================================================================

struct ST_Contains {

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D -> POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	// TODO: This should probably be revised. Im not sure if the current implementation is entirely accurate

	static void Operation(Vector &in_polygon, Vector &in_point, Vector &result, idx_t count) {
		enum class Side { LEFT, RIGHT, ON };

		in_polygon.Flatten();
		in_point.Flatten();

		// Setup point vectors
		auto &p_children = StructVector::GetEntries(in_point);
		auto p_x_data = FlatVector::GetDataMutable<double>(p_children[0]);
		auto p_y_data = FlatVector::GetDataMutable<double>(p_children[1]);

		// Setup polygon vectors
		auto polygon_entries = FlatVector::GetData<list_entry_t>(in_polygon);
		auto &ring_vec = ListVector::GetChildMutable(in_polygon);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &coord_vec = ListVector::GetChildMutable(ring_vec);
		auto &coord_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetDataMutable<double>(coord_children[0]);
		auto y_data = FlatVector::GetDataMutable<double>(coord_children[1]);

		auto result_data = FlatVector::GetDataMutable<bool>(result);
		for (idx_t polygon_idx = 0; polygon_idx < count; polygon_idx++) {
			auto polygon = polygon_entries[polygon_idx];
			auto polygon_offset = polygon.offset;
			auto polygon_length = polygon.length;
			bool first = true;

			// does the point lie inside the polygon?
			bool contains = false;

			auto x = p_x_data[polygon_idx];
			auto y = p_y_data[polygon_idx];

			for (idx_t ring_idx = polygon_offset; ring_idx < polygon_offset + polygon_length; ring_idx++) {
				auto ring = ring_entries[ring_idx];
				auto ring_offset = ring.offset;
				auto ring_length = ring.length;

				auto x1 = x_data[ring_offset];
				auto y1 = y_data[ring_offset];
				int winding_number = 0;

				for (idx_t coord_idx = ring_offset + 1; coord_idx < ring_offset + ring_length; coord_idx++) {
					// foo foo foo
					auto x2 = x_data[coord_idx];
					auto y2 = y_data[coord_idx];

					if (x1 == x2 && y1 == y2) {
						x1 = x2;
						y1 = y2;
						continue;
					}

					auto y_min = std::min(y1, y2);
					auto y_max = std::max(y1, y2);

					if (y > y_max || y < y_min) {
						x1 = x2;
						y1 = y2;
						continue;
					}

					auto side = Side::ON;
					double side_v = ((x - x1) * (y2 - y1) - (x2 - x1) * (y - y1));
					if (side_v == 0) {
						side = Side::ON;
					} else if (side_v < 0) {
						side = Side::LEFT;
					} else {
						side = Side::RIGHT;
					}

					if (side == Side::ON && (((x1 <= x && x < x2) || (x1 >= x && x > x2)) ||
					                         ((y1 <= y && y < y2) || (y1 >= y && y > y2)))) {

						// return Contains::ON_EDGE;
						contains = false;
						break;
					} else if (side == Side::LEFT && (y1 < y && y <= y2)) {
						winding_number++;
					} else if (side == Side::RIGHT && (y2 <= y && y < y1)) {
						winding_number--;
					}

					x1 = x2;
					y1 = y2;
				}
				bool in_ring = winding_number != 0;
				if (first) {
					if (!in_ring) {
						// if the first ring is not inside, then the point is not inside the polygon
						contains = false;
						break;
					} else {
						// if the first ring is inside, then the point is inside the polygon
						// but might be inside a hole, so we continue
						contains = true;
					}
				} else {
					if (in_ring) {
						// if the hole is inside, then the point is not inside the polygon
						contains = false;
						break;
					} // else continue
				}
				first = false;
			}
			result_data[polygon_idx] = contains;
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		Operation(args.data[0], args.data[1], result, args.size());
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------

	// TODO: Add example
	static constexpr auto DESCRIPTION = "";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Contains", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::POLYGON_2D());
				variant.AddParameter("geom2", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

//======================================================================================================================
// ST_Dimension
//======================================================================================================================

struct ST_Dimension {

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			// The standard says that empty geometrycollections should return 0
			return std::max(0, sgl::ops::get_max_surface_dimension(geom, false));
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the "topological dimension" of a geometry.

		- For POINT and MULTIPOINT geometries, returns `0`
		- For LINESTRING and MULTILINESTRING, returns `1`
		- For POLYGON and MULTIPOLYGON, returns `2`
		- For GEOMETRYCOLLECTION, returns the maximum dimension of the contained geometries, or 0 if the collection is empty
	)";

	static constexpr auto EXAMPLE = R"(
	SELECT ST_Dimension('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY);
	----
	2
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Dimension", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::INTEGER);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Distance
//======================================================================================================================

struct ST_Azimuth {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, double>(
		    args.data[0], args.data[1], result, args.size(),
		    [&](const string_t &left, const string_t &right) -> optional<double> {
			    sgl::geometry left_geom;
			    sgl::geometry right_geom;

			    lstate.Deserialize(left, left_geom);
			    lstate.Deserialize(right, right_geom);

			    if (left_geom.get_type() != sgl::geometry_type::POINT ||
			        right_geom.get_type() != sgl::geometry_type::POINT) {
				    throw InvalidInputException("ST_Azimuth only accepts POINT geometries");
			    }

			    if (left_geom.is_empty() || right_geom.is_empty()) {
				    return nullopt;
			    }

			    const auto left_xy = left_geom.get_vertex_xy(0);
			    const auto right_xy = right_geom.get_vertex_xy(0);

			    // If the points are the same, return NULL
			    if (left_xy.x == right_xy.x && left_xy.y == right_xy.y) {
				    return nullopt;
			    }

			    return CalcAngle(left_xy.x, left_xy.y, right_xy.x, right_xy.y);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 2);
		auto &left = args.data[0];
		auto &right = args.data[1];
		auto count = args.size();

		// Note: GenericExecutor::ExecuteBinary is preferable, but it cannot return NULL.
		// So, let's flatten the vectors for simplicity.
		left.Flatten();
		right.Flatten();

		auto &left_entries = StructVector::GetEntries(left);
		auto &right_entries = StructVector::GetEntries(right);

		auto left_x = FlatVector::GetDataMutable<double>(left_entries[0]);
		auto left_y = FlatVector::GetDataMutable<double>(left_entries[1]);
		auto right_x = FlatVector::GetDataMutable<double>(right_entries[0]);
		auto right_y = FlatVector::GetDataMutable<double>(right_entries[1]);

		auto out_data = FlatVector::GetDataMutable<double>(result);
		for (idx_t i = 0; i < count; i++) {
			// If the points are the same, return NULL
			if (left_x[i] == right_x[i] && left_y[i] == right_y[i]) {
				FlatVector::SetNull(result, i, true);
				continue;
			}
			out_data[i] = CalcAngle(left_x[i], left_y[i], right_x[i], right_y[i]);
		}

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static double CalcAngle(double x1, double y1, double x2, double y2) {
		// atan2 returns angle from positive X axis, counter-clockwise while
		// we want angle from positive Y axis, clockwise.
		double azimuth = PI / 2.0 - std::atan2(y2 - y1, x2 - x1);

		// ensure angle is positive
		if (azimuth < 0) {
			azimuth += 2.0 * PI;
		}

		return azimuth;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the azimuth (a clockwise angle measured from north) of two points in radian.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT degrees(ST_Azimuth(ST_Point(0, 0), ST_Point(0, 1)));
		----
		90.0
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Azimuth", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("origin", LogicalType::GEOMETRY());
				variant.AddParameter("target", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("origin", GeoTypes::POINT_2D());
				variant.AddParameter("target", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecutePoint);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Distance
//======================================================================================================================

struct ST_Distance {

	//------------------------------------------------------------------------------------------------------------------
	// Helpers
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Move this to SGL, into VectorOperations or deprecate.
	template <class T>
	static PointXY<T> ClosestPointOnSegment(const PointXY<T> &p, const PointXY<T> &p1, const PointXY<T> &p2) {
		// If the segment is a Vertex, then return that Vertex
		if (p1.ApproxEqualTo(p2)) {
			return p1;
		}
		auto n1 = ((p.x - p1.x) * (p2.x - p1.x) + (p.y - p1.y) * (p2.y - p1.y));
		auto n2 = ((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y));
		auto r = n1 / n2;
		// If r is less than 0, then the Point is outside the segment in the p1 direction
		if (r <= 0) {
			return p1;
		}
		// If r is greater than 1, then the Point is outside the segment in the p2 direction
		if (r >= 1) {
			return p2;
		}
		// Interpolate between p1 and p2
		return PointXY<T>(p1.x + r * (p2.x - p1.x), p1.y + r * (p2.y - p1.y));
	}

	template <class T>
	static double DistanceToSegmentSquared(const PointXY<T> &px, const PointXY<T> &ax, const PointXY<T> &bx) {
		auto point = ClosestPointOnSegment(px, ax, bx);
		auto dx = px.x - point.x;
		auto dy = px.y - point.y;
		return dx * dx + dy * dy;
	}

	//------------------------------------------------------------------------------
	// POINT_2D/POINT_2D
	//------------------------------------------------------------------------------
	static void ExecutePointPoint(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 2);
		auto &left = args.data[0];
		auto &right = args.data[1];
		auto count = args.size();

		left.Flatten();
		right.Flatten();

		auto &left_entries = StructVector::GetEntries(left);
		auto &right_entries = StructVector::GetEntries(right);

		auto left_x = FlatVector::GetDataMutable<double>(left_entries[0]);
		auto left_y = FlatVector::GetDataMutable<double>(left_entries[1]);
		auto right_x = FlatVector::GetDataMutable<double>(right_entries[0]);
		auto right_y = FlatVector::GetDataMutable<double>(right_entries[1]);

		auto out_data = FlatVector::GetDataMutable<double>(result);
		for (idx_t i = 0; i < count; i++) {
			out_data[i] = std::sqrt(std::pow(left_x[i] - right_x[i], 2) + std::pow(left_y[i] - right_y[i], 2));
		}

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------
	// POINT_2D/LINESTRING_2D
	//------------------------------------------------------------------------------
	static void PointLineStringOperation(Vector &in_point, Vector &in_line, Vector &result, idx_t count) {

		// Set up the point vectors
		in_point.Flatten();
		auto &p_children = StructVector::GetEntries(in_point);
		auto &p_x = p_children[0];
		auto &p_y = p_children[1];
		auto p_x_data = FlatVector::GetDataMutable<double>(p_x);
		auto p_y_data = FlatVector::GetDataMutable<double>(p_y);

		// Set up the line vectors
		in_line.Flatten();

		auto &inner = ListVector::GetChildMutable(in_line);
		auto &children = StructVector::GetEntries(inner);
		auto &x = children[0];
		auto &y = children[1];
		auto x_data = FlatVector::GetDataMutable<double>(x);
		auto y_data = FlatVector::GetDataMutable<double>(y);
		auto lines = FlatVector::GetData<list_entry_t>(in_line);

		auto result_data = FlatVector::GetDataMutable<double>(result);
		for (idx_t i = 0; i < count; i++) {
			auto offset = lines[i].offset;
			auto length = lines[i].length;

			double min_distance = std::numeric_limits<double>::max();
			auto p = PointXY<double>(p_x_data[i], p_y_data[i]);

			// Loop over the segments and find the closes one to the point
			for (idx_t j = 0; j < length - 1; j++) {
				auto a = PointXY<double>(x_data[offset + j], y_data[offset + j]);
				auto b = PointXY<double>(x_data[offset + j + 1], y_data[offset + j + 1]);

				auto distance = DistanceToSegmentSquared(p, a, b);
				if (distance < min_distance) {
					min_distance = distance;

					if (min_distance == 0) {
						break;
					}
				}
			}
			result_data[i] = std::sqrt(min_distance);
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static void ExecutePointLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 2);
		auto &in_point = args.data[0];
		auto &in_line = args.data[1];
		auto count = args.size();
		PointLineStringOperation(in_point, in_line, result, count);
	}

	static void ExecuteLineStringPoint(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 2);
		auto &in_line = args.data[0];
		auto &in_point = args.data[1];
		auto count = args.size();
		PointLineStringOperation(in_point, in_line, result, count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		const auto count = args.size();

		auto &lhs_vec = args.data[0];
		auto &rhs_vec = args.data[1];

		const auto lhs_is_const =
		    lhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(lhs_vec);
		;
		const auto rhs_is_const =
		    rhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(rhs_vec);

		if (lhs_is_const && rhs_is_const) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			const auto &lhs_blob = ConstantVector::GetData<string_t>(lhs_vec)[0];
			const auto &rhs_blob = ConstantVector::GetData<string_t>(rhs_vec)[0];

			sgl::prepared_geometry lhs_geom;
			sgl::prepared_geometry rhs_geom;

			lstate.Deserialize(lhs_blob, lhs_geom);
			lstate.Deserialize(rhs_blob, rhs_geom);

			double dist = 0.0;
			if (sgl::ops::get_euclidean_distance(lhs_geom, rhs_geom, dist)) {
				ConstantVector::GetData<double>(result)[0] = dist;
			} else {
				ConstantVector::GetData<double>(result)[0] = 0.0; // TODO: Null
			}
		} else if (lhs_is_const != rhs_is_const) {

			// One of the two is const, prepare the const one and execute on the non-const one
			auto &const_vec = lhs_is_const ? lhs_vec : rhs_vec;
			auto &probe_vec = lhs_is_const ? rhs_vec : lhs_vec;

			const auto &const_blob = ConstantVector::GetData<string_t>(const_vec)[0];

			sgl::prepared_geometry const_geom;
			lstate.Deserialize(const_blob, const_geom);

			UnaryExecutor::Execute<string_t, double>(probe_vec, result, count, [&](const string_t &probe_blob) {
				sgl::prepared_geometry probe_geom;
				lstate.Deserialize(probe_blob, probe_geom);

				// Calculate the distance
				double distance = 0.0;
				if (sgl::ops::get_euclidean_distance(const_geom, probe_geom, distance)) {
					return distance;
				}
				return 0.0; // TODO: Null
			});
		} else {
			// Both are non-const
			BinaryExecutor::Execute<string_t, string_t, double>(
			    args.data[0], args.data[1], result, count, [&](const string_t &blob1, const string_t &blob2) {
				    sgl::prepared_geometry geom1;
				    sgl::prepared_geometry geom2;

				    lstate.Deserialize(blob1, geom1);
				    lstate.Deserialize(blob2, geom2);

				    // Calculate the distance
				    double distance = 0.0;
				    if (sgl::ops::get_euclidean_distance(geom1, geom2, distance)) {
					    return distance;
				    }

				    // TODO: Null
				    return 0.0;
			    });
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// TODO: add example/description
	static constexpr auto DESCRIPTION = "Returns the planar distance between two geometries";
	static constexpr auto EXAMPLE = R"(
		SELECT ST_Distance('POINT (0 0)'::GEOMETRY, 'POINT (3 4)'::GEOMETRY);
		----
		5.0

		-- Z coordinates are ignored
		SELECT ST_Distance('POINT Z (0 0 0)'::GEOMETRY, 'POINT Z (3 4 5)'::GEOMETRY);
		----
		5.0
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Distance", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point1", GeoTypes::POINT_2D());
				variant.AddParameter("point2", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecutePointPoint);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecutePointLineString);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecuteLineStringPoint);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", LogicalType::GEOMETRY());
				variant.AddParameter("geom2", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_DistanceWithin
//======================================================================================================================

struct ST_DistanceWithin {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	class BindData final : public FunctionData {
	public:
		double distance = 0.0;
		bool is_constant = false;

		explicit BindData(double distance, bool is_constant) : distance(distance), is_constant(is_constant) {
		}

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindData>(distance, is_constant);
		}

		bool Equals(const FunctionData &other) const override {
			auto &other_data = other.Cast<BindData>();
			return is_constant == other_data.is_constant && distance == other_data.distance;
		}

		static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                                     const BoundScalarFunction &function) {

			const auto &bind_data = bind_data_p->Cast<BindData>();
			serializer.WritePropertyWithDefault<bool>(100, "is_constant", bind_data.is_constant);
			serializer.WritePropertyWithDefault(101, "distance", bind_data.distance);
		}

		static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &function) {
			auto is_constant = deserializer.ReadPropertyWithDefault<bool>(100, "is_constant");
			auto distance = deserializer.ReadPropertyWithDefault<double>(101, "distance");
			return make_uniq<BindData>(distance, is_constant);
		}
	};

	// We try to constant-fold the distance parameter here, because it's a very common have a constant distance
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {

		auto &arguments = input.GetArguments();
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();

		if (arguments.back()->IsFoldable()) {
			const auto dist_expr = ExpressionExecutor::EvaluateScalar(context, *arguments.back());
			const auto dist_value = dist_expr.GetValue<double>();

			// Erase argument
			Function::EraseArgument(bound_function, arguments, 2);
			return make_uniq<BindData>(dist_value, true);
		}

		return make_uniq<BindData>(0.0, false);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		const auto count = args.size();

		auto &lhs_vec = args.data[0];
		auto &rhs_vec = args.data[1];

		if (args.ColumnCount() == 3) {
			auto &dst_vec = args.data[2];

			TernaryExecutor::Execute<string_t, string_t, double, bool>(
			    lhs_vec, rhs_vec, dst_vec, result,
			    [&](const string_t &lhs_blob, const string_t &rhs_blob, double distance) {
				    sgl::prepared_geometry lhs_geom;
				    sgl::prepared_geometry rhs_geom;

				    lstate.Deserialize(lhs_blob, lhs_geom);
				    lstate.Deserialize(rhs_blob, rhs_geom);

				    // Calculate the distance
				    double dist = 0.0;
				    if (sgl::ops::get_euclidean_distance(lhs_geom, rhs_geom, dist)) {
					    return dist <= distance;
				    }
				    return false; // TODO: Null
			    });
		} else {
			// No distance argument, so we use the bind data
			const auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
			const auto &bind_data = func_expr.BindInfo()->Cast<BindData>();

			const auto distance = bind_data.distance;

			const auto lhs_is_const =
			    lhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(lhs_vec);
			const auto rhs_is_const =
			    rhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(rhs_vec);

			if (lhs_is_const && rhs_is_const) {
				result.SetVectorType(VectorType::CONSTANT_VECTOR);
				const auto &lhs_blob = ConstantVector::GetData<string_t>(lhs_vec)[0];
				const auto &rhs_blob = ConstantVector::GetData<string_t>(rhs_vec)[0];

				sgl::prepared_geometry lhs_geom;
				sgl::prepared_geometry rhs_geom;

				lstate.Deserialize(lhs_blob, lhs_geom);
				lstate.Deserialize(rhs_blob, rhs_geom);

				// Calculate the distance
				double dist = 0.0;
				if (sgl::ops::get_euclidean_distance(lhs_geom, rhs_geom, dist)) {
					ConstantVector::GetData<bool>(result)[0] = dist <= distance;
				} else {
					ConstantVector::GetData<bool>(result)[0] = false; // TODO: Null
				}
			} else if (lhs_is_const != rhs_is_const) {
				auto &const_vec = lhs_is_const ? lhs_vec : rhs_vec;
				auto &probe_vec = lhs_is_const ? rhs_vec : lhs_vec;

				const auto &const_blob = ConstantVector::GetData<string_t>(const_vec)[0];
				sgl::prepared_geometry const_geom;
				lstate.Deserialize(const_blob, const_geom);

				UnaryExecutor::Execute<string_t, bool>(probe_vec, result, count, [&](const string_t &probe_blob) {
					sgl::prepared_geometry probe_geom;
					lstate.Deserialize(probe_blob, probe_geom);

					// Calculate the distance
					double dist = 0.0;
					if (sgl::ops::get_euclidean_distance(const_geom, probe_geom, dist)) {
						return dist <= distance;
					}
					return false; // TODO: Null
				});
			} else {
				BinaryExecutor::Execute<string_t, string_t, bool>(
				    lhs_vec, rhs_vec, result, count, [&](const string_t &lhs_blob, const string_t &rhs_blob) {
					    sgl::prepared_geometry lhs_geom;
					    sgl::prepared_geometry rhs_geom;

					    lstate.Deserialize(lhs_blob, lhs_geom);
					    lstate.Deserialize(rhs_blob, rhs_geom);

					    // Calculate the distance
					    double dist = 0.0;
					    if (sgl::ops::get_euclidean_distance(lhs_geom, rhs_geom, dist)) {
						    return dist <= distance;
					    }
					    return false; // TODO: Null
				    });
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_DWithin", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", LogicalType::GEOMETRY());
				variant.AddParameter("geom2", LogicalType::GEOMETRY());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.SetBind(GeoTypes::PropagateCRS<Bind>);
				variant.SetSerialize(BindData::Serialize);
				variant.SetDeserialize(BindData::Deserialize);
			});

			func.SetDescription(R"(
				Returns if two geometries are within a target distance of each-other
			)");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

//======================================================================================================================
// ST_Dump
//======================================================================================================================

struct ST_Dump {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto count = args.size();

		auto &geom_vec = args.data[0];
		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(geom_format);

		idx_t total_geom_count = 0;
		idx_t total_path_count = 0;

		vector<std::tuple<const sgl::geometry *, vector<int32_t>>> items;
		vector<int32_t> path;

		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {
			auto in_row_idx = geom_format.sel->get_index(out_row_idx);

			if (!geom_format.validity.RowIsValid(in_row_idx)) {
				FlatVector::SetNull(result, out_row_idx, true);
				continue;
			}

			auto &blob = UnifiedVectorFormat::GetData<string_t>(geom_format)[in_row_idx];

			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			// Traverse the geometries
			// TODO: Move this to SGL
			const sgl::geometry *part = &geom;
			const sgl::geometry *root = part->get_parent();

			path.clear();
			items.clear();

			bool is_done = false;
			while (!is_done) {
				switch (part->get_type()) {
				case sgl::geometry_type::POINT:
				case sgl::geometry_type::LINESTRING:
				case sgl::geometry_type::POLYGON: {
					// Add the path
					items.emplace_back(part, path);
				} break;
				case sgl::geometry_type::MULTI_POINT:
				case sgl::geometry_type::MULTI_LINESTRING:
				case sgl::geometry_type::MULTI_POLYGON:
				case sgl::geometry_type::GEOMETRY_COLLECTION: {
					if (!part->is_empty()) {
						part = part->get_first_part();
						path.push_back(1);
						continue;
					}
				} break;
				default: {
					throw NotImplementedException("Unsupported geometry type in ST_Dump");
				}
				}

				while (true) {
					const auto parent = part->get_parent();

					if (parent == root) {
						is_done = true;
						break;
					}

					if (part != parent->get_last_part()) {
						path.back()++;
						part = part->get_next();
						break;
					}

					part = parent;
					path.pop_back();
				}
			}

			// Push to the result vector
			auto result_entries = FlatVector::GetDataMutable<list_entry_t>(result);

			auto geom_offset = total_geom_count;
			auto geom_length = items.size();

			result_entries[out_row_idx].length = geom_length;
			result_entries[out_row_idx].offset = geom_offset;

			total_geom_count += geom_length;

			ListVector::Reserve(result, total_geom_count);
			ListVector::SetListSize(result, total_geom_count);

			auto &result_list = ListVector::GetChildMutable(result);
			auto &result_list_children = StructVector::GetEntries(result_list);
			auto &result_geom_vec = result_list_children[0];
			auto &result_path_vec = result_list_children[1];

			// The child geometries must share the same properties as the parent geometry
			auto geom_data = FlatVector::GetDataMutable<string_t>(result_geom_vec);
			for (idx_t i = 0; i < geom_length; i++) {
				// Write the geometry
				auto item_blob = std::get<0>(items[i]);
				geom_data[geom_offset + i] = lstate.Serialize(result_geom_vec, *item_blob);

				// Now write the paths
				auto &path = std::get<1>(items[i]);
				auto path_offset = total_path_count;
				auto path_length = path.size();

				total_path_count += path_length;

				ListVector::Reserve(result_path_vec, total_path_count);
				ListVector::SetListSize(result_path_vec, total_path_count);

				auto path_entries = FlatVector::GetDataMutable<list_entry_t>(result_path_vec);

				path_entries[geom_offset + i].offset = path_offset;
				path_entries[geom_offset + i].length = path_length;

				auto &path_data_vec = ListVector::GetChildMutable(result_path_vec);
				auto path_data = FlatVector::GetDataMutable<int32_t>(path_data_vec);

				for (idx_t j = 0; j < path_length; j++) {
					path_data[path_offset + j] = path[j];
				}
			}
		}

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	Dumps a geometry into a list of sub-geometries and their "path" in the original geometry.

	You can use the `unnest(res, recursive := true)` function to explode the resulting list of structs into multiple rows.
	)";

	static constexpr auto EXAMPLE = R"(
	SELECT ST_Dump('MULTIPOINT(1 2, 3 4)'::GEOMETRY);
	----
	[{'geom': 'POINT(1 2)', 'path': [0]}, {'geom': 'POINT(3 4)', 'path': [1]}]

	SELECT unnest(ST_Dump('MULTIPOINT(1 2, 3 4)'::GEOMETRY), recursive := true);
	-- ┌─────────────┬─────────┐
	-- │    geom     │  path   │
	-- │  geometry   │ int32[] │
	-- ├─────────────┼─────────┤
	-- │ POINT (1 2) │ [1]     │
	-- │ POINT (3 4) │ [2]     │
	-- └─────────────┴─────────┘
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {

		FunctionBuilder::RegisterScalar(loader, "ST_Dump", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());

				variant.SetReturnType(LogicalType::LIST(LogicalType::STRUCT(
				    {{"geom", LogicalType::GEOMETRY()}, {"path", LogicalType::LIST(LogicalType::INTEGER)}})));

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_Expand
//======================================================================================================================

struct ST_Expand {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, double, string_t>(
		    args.data[0], args.data[1], result, [&](const string_t &blob, double distance) {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);
			    auto bbox = sgl::extent_xy::smallest();

			    if (sgl::ops::get_total_extent_xy(geom, bbox) == 0) {
				    const sgl::geometry empty(sgl::geometry_type::GEOMETRY_COLLECTION, false, false);
				    return lstate.Serialize(result, empty);
			    } else {
				    sgl::geometry expanded(sgl::geometry_type::POLYGON, false, false);
				    const auto min_x = bbox.min.x - distance;
				    const auto min_y = bbox.min.y - distance;
				    const auto max_x = bbox.max.x + distance;
				    const auto max_y = bbox.max.y + distance;
				    const double buffer[10] = {min_x, min_y, min_x, max_y, max_x, max_y, max_x, min_y, min_x, min_y};

				    sgl::geometry ring(sgl::geometry_type::LINESTRING, false, false);
				    ring.set_vertex_array(buffer, 5);
				    expanded.append_part(&ring);
				    return lstate.Serialize(result, expanded);
			    }
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	    Expand the input geometry by the specified distance, returning a polygon.

	    `geom` is the input geometry.

	    `distance` is the target distance for the expansion, using the same units as the input geometry.

	    This is a planar operation and will not take into account the curvature of the earth.
	)";
	static constexpr auto EXAMPLE = R"(
		SELECT ST_AsText(ST_Expand(ST_GeomFromText('POINT(20 30)'), 0.1));
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Expand", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Extent
//======================================================================================================================

struct ST_Extent {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto &bbox_vec = StructVector::GetEntries(result);
		const auto min_x_data = FlatVector::GetDataMutable<double>(bbox_vec[0]);
		const auto min_y_data = FlatVector::GetDataMutable<double>(bbox_vec[1]);
		const auto max_x_data = FlatVector::GetDataMutable<double>(bbox_vec[2]);
		const auto max_y_data = FlatVector::GetDataMutable<double>(bbox_vec[3]);

		UnifiedVectorFormat input_vdata;
		args.data[0].ToUnifiedFormat(input_vdata);
		const auto input_data = UnifiedVectorFormat::GetData<string_t>(input_vdata);

		const auto count = args.size();

		for (idx_t out_idx = 0; out_idx < count; out_idx++) {
			const auto row_idx = input_vdata.sel->get_index(out_idx);
			if (!input_vdata.validity.RowIsValid(row_idx)) {
				// null in -> null out
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			const auto &blob = input_data[row_idx];
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			auto bbox = sgl::extent_xy::smallest();

			if (sgl::ops::get_total_extent_xy(geom, bbox) == 0) {
				// no vertices -> no extent -> return null
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			min_x_data[out_idx] = bbox.min.x;
			min_y_data[out_idx] = bbox.min.y;
			max_x_data[out_idx] = bbox.max.x;
			max_y_data[out_idx] = bbox.max.y;
		}

		if (args.AllConstant()) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the minimal bounding box enclosing the input geometry
	)";

	// TODO: Example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Extent", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(GeoTypes::BOX_2D());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Extent_Approx
//======================================================================================================================

struct ST_Extent_Approx {

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		const auto count = args.size();
		auto &input = args.data[0];

		auto &struct_vec = StructVector::GetEntries(result);
		const auto min_x_data = FlatVector::GetDataMutable<float>(struct_vec[0]);
		const auto min_y_data = FlatVector::GetDataMutable<float>(struct_vec[1]);
		const auto max_x_data = FlatVector::GetDataMutable<float>(struct_vec[2]);
		const auto max_y_data = FlatVector::GetDataMutable<float>(struct_vec[3]);

		UnifiedVectorFormat input_vdata;
		input.ToUnifiedFormat(input_vdata);
		const auto input_data = UnifiedVectorFormat::GetData<string_t>(input_vdata);

		for (idx_t i = 0; i < count; i++) {
			const auto row_idx = input_vdata.sel->get_index(i);
			if (input_vdata.validity.RowIsValid(row_idx)) {
				auto &blob = input_data[row_idx];

				// Try to get the cached bounding box from the blob
				Box2D<float> bbox;
				if (Serde::TryGetBounds(blob, bbox)) {
					min_x_data[i] = bbox.min.x;
					min_y_data[i] = bbox.min.y;
					max_x_data[i] = bbox.max.x;
					max_y_data[i] = bbox.max.y;
				} else {
					// No bounding box, return null
					FlatVector::SetNull(result, i, true);
				}
			} else {
				// Null input, return null
				FlatVector::SetNull(result, i, true);
			}
		}

		if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Add docs

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Extent_Approx", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(GeoTypes::BOX_2DF());

				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Returns the approximate bounding box of a geometry, if available.

				This function is only really used internally, and returns the cached bounding box of the geometry if it exists.
				This function may be removed or renamed in the future.
			)");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// &&
//======================================================================================================================
/*
struct Op_IntersectApprox {

    //------------------------------------------------------------------------------------------------------------------
    // Execute
    //------------------------------------------------------------------------------------------------------------------
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

        const auto count = args.size();
        auto &box = args.data[0];
        auto &geom = args.data[1];

        auto result_data = FlatVector::GetDataMutable<bool>(result);

        // Convert box to unified format
        UnifiedVectorFormat box_vdata;
        box.ToUnifiedFormat(box_vdata);

        // Get the struct entries and convert them to unified format
        auto &bbox_vec = StructVector::GetEntries(box);
        UnifiedVectorFormat box_min_x_vdata, box_min_y_vdata, box_max_x_vdata, box_max_y_vdata;
        bbox_vec[0].ToUnifiedFormat(box_min_x_vdata);
        bbox_vec[1].ToUnifiedFormat(box_min_y_vdata);
        bbox_vec[2].ToUnifiedFormat(box_max_x_vdata);
        bbox_vec[3].ToUnifiedFormat(box_max_y_vdata);

        const auto box_min_x_data = UnifiedVectorFormat::GetData<double>(box_min_x_vdata);
        const auto box_min_y_data = UnifiedVectorFormat::GetData<double>(box_min_y_vdata);
        const auto box_max_x_data = UnifiedVectorFormat::GetData<double>(box_max_x_vdata);
        const auto box_max_y_data = UnifiedVectorFormat::GetData<double>(box_max_y_vdata);

        // Convert geometry to unified format
        UnifiedVectorFormat input_geom_vdata;
        geom.ToUnifiedFormat(input_geom_vdata);
        const auto input_geom = UnifiedVectorFormat::GetData<geometry_t>(input_geom_vdata);

        for (idx_t i = 0; i < count; i++) {
            // Get the actual indices for box and geometry
            const auto box_idx = box_vdata.sel->get_index(i);
            const auto geom_idx = input_geom_vdata.sel->get_index(i);

            // Check validity of both inputs
            if (!box_vdata.validity.RowIsValid(box_idx) || !input_geom_vdata.validity.RowIsValid(geom_idx)) {
                FlatVector::SetNull(result, i, true);
                continue;
            }

            // Get box coordinate indices
            const auto box_min_x_idx = box_min_x_vdata.sel->get_index(i);
            const auto box_min_y_idx = box_min_y_vdata.sel->get_index(i);
            const auto box_max_x_idx = box_max_x_vdata.sel->get_index(i);
            const auto box_max_y_idx = box_max_y_vdata.sel->get_index(i);

            // Check validity of box coordinates
            if (!box_min_x_vdata.validity.RowIsValid(box_min_x_idx) ||
                !box_min_y_vdata.validity.RowIsValid(box_min_y_idx) ||
                !box_max_x_vdata.validity.RowIsValid(box_max_x_idx) ||
                !box_max_y_vdata.validity.RowIsValid(box_max_y_idx)) {
                FlatVector::SetNull(result, i, true);
                continue;
            }

            auto &geom_blob = input_geom[geom_idx];

            // Try to get the cached bounding box from the blob
            Box2D<float> geom_bbox;
            if (geom_blob.TryGetCachedBounds(geom_bbox)) {
                const auto box_min_x = box_min_x_data[box_min_x_idx];
                const auto box_min_y = box_min_y_data[box_min_y_idx];
                const auto box_max_x = box_max_x_data[box_max_x_idx];
                const auto box_max_y = box_max_y_data[box_max_y_idx];

                result_data[i] = (box_min_x <= geom_bbox.max.x && geom_bbox.min.x <= box_max_x) &&
                                 (box_min_y <= geom_bbox.max.y && geom_bbox.min.y <= box_max_y);
            } else {
                // No bounding box, return null
                FlatVector::SetNull(result, i, true);
            }
        }

        if (box.GetVectorType() == VectorType::CONSTANT_VECTOR) {
            result.SetVectorType(VectorType::CONSTANT_VECTOR);
        }
    }

    //------------------------------------------------------------------------------------------------------------------
    // Register
    //------------------------------------------------------------------------------------------------------------------
    static void Register(ExtensionLoader &loader) {
        FunctionBuilder::RegisterScalar(loader, "&&", [](ScalarFunctionBuilder &func) {
            func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
                variant.AddParameter("box", GeoTypes::BOX_2D());
                variant.AddParameter("geom", LogicalType::GEOMETRY());
                variant.SetReturnType(LogicalType::BOOLEAN);

                variant.SetFunction(Execute);
            });

            func.SetDescription(R"(
                Returns true if the bounding boxes intersects.

                Note that, this operation is not very accurate; `&&` compares the cached bbox of the geometry using
float precision. If you prefer accuracy, please use some other function like `ST_Intersects()`.
            )");

            func.SetExample(R"(
                SELECT ST_MakeBox2D('POINT (0 0)'::GEOMETRY, 'POINT (2 2)'::GEOMETRY) && ST_POINT(1, 1);
                ----
                true

                SELECT ST_MakeBox2D('POINT (0 0)'::GEOMETRY, 'POINT (2 2)'::GEOMETRY) && ST_POINT(5, 5);
                ----
                false
            )");

            func.SetTag("ext", "spatial");
            func.SetTag("category", "property");
        });
    }
};
*/
//======================================================================================================================
// ST_ExteriorRing
//======================================================================================================================

struct ST_ExteriorRing {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(
		    args.data[0], result, args.size(), [&](const string_t &blob) -> optional<string_t> {
			    // TODO: Peek dont deserialize
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::POLYGON) {
				    return nullopt;
			    }

			    if (geom.is_empty()) {
				    // Polygon empty -> return empty linestring
				    sgl::geometry empty_linestring(sgl::geometry_type::LINESTRING, geom.has_z(), geom.has_m());
				    return lstate.Serialize(result, empty_linestring);
			    }

			    const auto shell = geom.get_first_part();
			    return lstate.Serialize(result, *shell);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto &poly_vec = args.data[0];
		auto poly_entries = FlatVector::GetData<list_entry_t>(poly_vec);
		auto &ring_vec = ListVector::GetChildMutable(poly_vec);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &vertex_vec = ListVector::GetChildMutable(ring_vec);
		auto &vertex_vec_children = StructVector::GetEntries(vertex_vec);
		auto poly_x_data = FlatVector::GetDataMutable<double>(vertex_vec_children[0]);
		auto poly_y_data = FlatVector::GetDataMutable<double>(vertex_vec_children[1]);

		auto count = args.size();
		UnifiedVectorFormat poly_format;
		poly_vec.ToUnifiedFormat(poly_format);

		// First figure out how many vertices we need
		idx_t total_vertex_count = 0;
		for (idx_t i = 0; i < count; i++) {
			auto row_idx = poly_format.sel->get_index(i);
			if (poly_format.validity.RowIsValid(row_idx)) {
				auto poly = poly_entries[row_idx];
				if (poly.length != 0) {
					// We only care about the exterior ring (first entry)
					auto &ring = ring_entries[poly.offset];
					total_vertex_count += ring.length;
				}
			}
		}

		// Now we can allocate the result vector
		auto &line_vec = result;
		ListVector::Reserve(line_vec, total_vertex_count);
		ListVector::SetListSize(line_vec, total_vertex_count);

		auto line_entries = FlatVector::GetDataMutable<list_entry_t>(line_vec);
		auto &line_coord_vec = StructVector::GetEntries(ListVector::GetChildMutable(line_vec));
		auto line_data_x = FlatVector::GetDataMutable<double>(line_coord_vec[0]);
		auto line_data_y = FlatVector::GetDataMutable<double>(line_coord_vec[1]);

		// Now we can fill the result vector
		idx_t line_data_offset = 0;
		for (idx_t i = 0; i < count; i++) {
			auto row_idx = poly_format.sel->get_index(i);
			if (poly_format.validity.RowIsValid(row_idx)) {
				auto poly = poly_entries[row_idx];

				if (poly.length == 0) {
					line_entries[i].offset = 0;
					line_entries[i].length = 0;
					continue;
				}

				// We only care about the exterior ring (first entry)
				auto &ring = ring_entries[poly.offset];

				auto &line_entry = line_entries[i];
				line_entry.offset = line_data_offset;
				line_entry.length = ring.length;

				for (idx_t coord_idx = 0; coord_idx < ring.length; coord_idx++) {
					line_data_x[line_entry.offset + coord_idx] = poly_x_data[ring.offset + coord_idx];
					line_data_y[line_entry.offset + coord_idx] = poly_y_data[ring.offset + coord_idx];
				}

				line_data_offset += ring.length;
			} else {
				FlatVector::SetNull(line_vec, i, true);
			}
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = "Returns the exterior ring (shell) of a polygon geometry.";

	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_ExteriorRing", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(GeoTypes::LINESTRING_2D());

				variant.SetFunction(ExecutePolygon);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);
			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_FlipCoordinates
//======================================================================================================================

struct ST_FlipCoordinates {

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	// TODO: We should be able to optimize these and avoid the flatten
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto count = args.size();

		// TODO: Avoid flatten
		input.Flatten();

		auto &coords_in = StructVector::GetEntries(input);
		auto x_data_in = FlatVector::GetDataMutable<double>(coords_in[0]);
		auto y_data_in = FlatVector::GetDataMutable<double>(coords_in[1]);

		auto &coords_out = StructVector::GetEntries(result);
		auto x_data_out = FlatVector::GetDataMutable<double>(coords_out[0]);
		auto y_data_out = FlatVector::GetDataMutable<double>(coords_out[1]);

		memcpy(x_data_out, y_data_in, count * sizeof(double));
		memcpy(y_data_out, x_data_in, count * sizeof(double));

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto count = args.size();

		// TODO: Avoid flatten
		input.Flatten();

		auto &coord_vec_in = ListVector::GetChildMutable(input);
		auto &coords_in = StructVector::GetEntries(coord_vec_in);
		auto x_data_in = FlatVector::GetDataMutable<double>(coords_in[0]);
		auto y_data_in = FlatVector::GetDataMutable<double>(coords_in[1]);

		auto coord_count = ListVector::GetListSize(input);
		ListVector::Reserve(result, coord_count);
		ListVector::SetListSize(result, coord_count);

		auto line_entries_in = FlatVector::GetDataMutable<list_entry_t>(input);
		auto line_entries_out = FlatVector::GetDataMutable<list_entry_t>(result);
		memcpy(line_entries_out, line_entries_in, count * sizeof(list_entry_t));

		auto &coord_vec_out = ListVector::GetChildMutable(result);
		auto &coords_out = StructVector::GetEntries(coord_vec_out);
		auto x_data_out = FlatVector::GetDataMutable<double>(coords_out[0]);
		auto y_data_out = FlatVector::GetDataMutable<double>(coords_out[1]);

		memcpy(x_data_out, y_data_in, coord_count * sizeof(double));
		memcpy(y_data_out, x_data_in, coord_count * sizeof(double));

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto count = args.size();

		// TODO: Avoid flatten
		input.Flatten();

		auto &ring_vec_in = ListVector::GetChildMutable(input);
		auto ring_count = ListVector::GetListSize(input);

		auto &coord_vec_in = ListVector::GetChildMutable(ring_vec_in);
		auto &coords_in = StructVector::GetEntries(coord_vec_in);
		auto x_data_in = FlatVector::GetDataMutable<double>(coords_in[0]);
		auto y_data_in = FlatVector::GetDataMutable<double>(coords_in[1]);

		auto coord_count = ListVector::GetListSize(ring_vec_in);

		ListVector::Reserve(result, ring_count);
		ListVector::SetListSize(result, ring_count);
		auto &ring_vec_out = ListVector::GetChildMutable(result);
		ListVector::Reserve(ring_vec_out, coord_count);
		ListVector::SetListSize(ring_vec_out, coord_count);

		auto ring_entries_in = FlatVector::GetDataMutable<list_entry_t>(input);
		auto ring_entries_out = FlatVector::GetDataMutable<list_entry_t>(result);
		memcpy(ring_entries_out, ring_entries_in, count * sizeof(list_entry_t));

		auto coord_entries_in = FlatVector::GetDataMutable<list_entry_t>(ring_vec_in);
		auto coord_entries_out = FlatVector::GetDataMutable<list_entry_t>(ring_vec_out);
		memcpy(coord_entries_out, coord_entries_in, ring_count * sizeof(list_entry_t));

		auto &coord_vec_out = ListVector::GetChildMutable(ring_vec_out);
		auto &coords_out = StructVector::GetEntries(coord_vec_out);
		auto x_data_out = FlatVector::GetDataMutable<double>(coords_out[0]);
		auto y_data_out = FlatVector::GetDataMutable<double>(coords_out[1]);

		memcpy(x_data_out, y_data_in, coord_count * sizeof(double));
		memcpy(y_data_out, x_data_in, coord_count * sizeof(double));

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &input = args.data[0];
		auto count = args.size();

		// TODO: Avoid flatten
		input.Flatten();

		auto &children_in = StructVector::GetEntries(input);
		auto min_x_in = FlatVector::GetDataMutable<double>(children_in[0]);
		auto min_y_in = FlatVector::GetDataMutable<double>(children_in[1]);
		auto max_x_in = FlatVector::GetDataMutable<double>(children_in[2]);
		auto max_y_in = FlatVector::GetDataMutable<double>(children_in[3]);

		auto &children_out = StructVector::GetEntries(result);
		auto min_x_out = FlatVector::GetDataMutable<double>(children_out[0]);
		auto min_y_out = FlatVector::GetDataMutable<double>(children_out[1]);
		auto max_x_out = FlatVector::GetDataMutable<double>(children_out[2]);
		auto max_y_out = FlatVector::GetDataMutable<double>(children_out[3]);

		memcpy(min_x_out, min_y_in, count * sizeof(double));
		memcpy(min_y_out, min_x_in, count * sizeof(double));
		memcpy(max_x_out, max_y_in, count * sizeof(double));
		memcpy(max_y_out, max_x_in, count * sizeof(double));
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &input = args.data[0];
		auto count = args.size();

		UnaryExecutor::Execute<string_t, string_t>(input, result, count, [&](const string_t &blob) {
			// This is pretty memory intensive, so reset arena after each call
			auto &lstate = LocalState::ResetAndGet(state);

			// Deserialize the geometry
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			sgl::ops::flip_vertices(lstate.GetAllocator(), geom);

			// Serialize the result
			return lstate.Serialize(result, geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Description
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns a new geometry with the coordinates of the input geometry "flipped" so that x = y and y = x
	)";

	// TODO: Add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_FlipCoordinates", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetFunction(ExecutePoint);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(GeoTypes::LINESTRING_2D());

				variant.SetFunction(ExecuteLineString);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(GeoTypes::POLYGON_2D());

				variant.SetFunction(ExecutePolygon);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.SetReturnType(GeoTypes::BOX_2D());

				variant.SetFunction(ExecuteBox);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_Force 2D/3DZ/3DM/4D
//======================================================================================================================

template <class IMPL>
struct ST_ForceBase {

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		auto has_z = IMPL::HAS_Z;
		auto has_m = IMPL::HAS_M;

		auto &input = args.data[0];
		const auto count = args.size();

		// TODO: This can be optimized to avoid de/serialization if the vertex type already matches

		if (has_z && has_m) {
			auto &z_values = args.data[1];
			auto &m_values = args.data[2];

			TernaryExecutor::Execute<string_t, double, double, string_t>(
			    input, z_values, m_values, result, [&](const string_t &blob, double z, double m) {
				    sgl::geometry geom;
				    lstate.Deserialize(blob, geom);
				    sgl::ops::force_zm(alloc, geom, true, true, z, m);
				    return lstate.Serialize(result, geom);
			    });

			return;
		}

		if (has_z || has_m) {
			auto &zm_values = args.data[1];

			BinaryExecutor::Execute<string_t, double, string_t>(
			    input, zm_values, result, count, [&](const string_t &blob, double zm) {
				    const auto def_z = has_z ? zm : 0;
				    const auto def_m = has_m ? zm : 0;

				    sgl::geometry geom;
				    lstate.Deserialize(blob, geom);
				    sgl::ops::force_zm(alloc, geom, has_z, has_m, def_z, def_m);
				    return lstate.Serialize(result, geom);
			    });

			return;
		}

		UnaryExecutor::Execute<string_t, string_t>(input, result, count, [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);
			sgl::ops::force_zm(alloc, geom, false, false, 0, 0);
			return lstate.Serialize(result, geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, IMPL::NAME, [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());

				if (IMPL::HAS_Z) {
					variant.AddParameter("z", LogicalType::DOUBLE);
				}
				if (IMPL::HAS_M) {
					variant.AddParameter("m", LogicalType::DOUBLE);
				}

				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(IMPL::DESCRIPTION);
			func.SetExample(IMPL::EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Force2D : ST_ForceBase<ST_Force2D> {
	static auto constexpr NAME = "ST_Force2D";
	static auto constexpr HAS_Z = false;
	static auto constexpr HAS_M = false;
	static auto constexpr EXAMPLE = "";
	static auto constexpr DESCRIPTION = R"(
		Forces the vertices of a geometry to have X and Y components

		This function will drop any Z and M values from the input geometry, if present. If the input geometry is already 2D, it will be returned as is.
		)";
};

struct ST_Force3DZ : ST_ForceBase<ST_Force3DZ> {
	static auto constexpr NAME = "ST_Force3DZ";
	static auto constexpr HAS_Z = true;
	static auto constexpr HAS_M = false;
	static auto constexpr EXAMPLE = "";
	static auto constexpr DESCRIPTION = R"(
		Forces the vertices of a geometry to have X, Y and Z components

		The following cases apply:
		- If the input geometry has a M component but no Z component, the M component will be replaced with the new Z value.
		- If the input geometry has a Z component but no M component, it will be returned as is.
		- If the input geometry has both a Z component and a M component, the M component will be removed.
		- Otherwise, if the input geometry has neither a Z or M component, the new Z value will be added to the vertices of the input geometry.
		)";
};

struct ST_Force3DM : ST_ForceBase<ST_Force3DM> {
	static auto constexpr NAME = "ST_Force3DM";
	static auto constexpr HAS_Z = false;
	static auto constexpr HAS_M = true;
	static auto constexpr EXAMPLE = "";
	static auto constexpr DESCRIPTION = R"(
		Forces the vertices of a geometry to have X, Y and M components

		The following cases apply:
		- If the input geometry has a Z component but no M component, the Z component will be replaced with the new M value.
		- If the input geometry has a M component but no Z component, it will be returned as is.
		- If the input geometry has both a Z component and a M component, the Z component will be removed.
		- Otherwise, if the input geometry has neither a Z or M component, the new M value will be added to the vertices of the input geometry.
		)";
};

struct ST_Force4D : ST_ForceBase<ST_Force4D> {
	static auto constexpr NAME = "ST_Force4D";
	static auto constexpr HAS_Z = true;
	static auto constexpr HAS_M = true;
	static auto constexpr EXAMPLE = "";
	static auto constexpr DESCRIPTION = R"(
		Forces the vertices of a geometry to have X, Y, Z and M components

		The following cases apply:
		- If the input geometry has a Z component but no M component, the new M value will be added to the vertices of the input geometry.
		- If the input geometry has a M component but no Z component, the new Z value will be added to the vertices of the input geometry.
		- If the input geometry has both a Z component and a M component, the geometry will be returned as is.
		- Otherwise, if the input geometry has neither a Z or M component, the new Z and M values will be added to the vertices of the input geometry.
		)";
};

//======================================================================================================================
// ST_GeometryType
//======================================================================================================================

struct ST_GeometryType {

	//------------------------------------------------------------------------------------------------------------------
	// Binding
	//------------------------------------------------------------------------------------------------------------------
	// This function is a bit botched, but we cant change it without breaking backwards compatability
	// therefore, we use these constants for the geometry type values, instead of the normal type enum

	static constexpr uint8_t LEGACY_POINT_TYPE = 0;
	static constexpr uint8_t LEGACY_LINESTRING_TYPE = 1;
	static constexpr uint8_t LEGACY_POLYGON_TYPE = 2;
	static constexpr uint8_t LEGACY_MULTIPOINT_TYPE = 3;
	static constexpr uint8_t LEGACY_MULTILINESTRING_TYPE = 4;
	static constexpr uint8_t LEGACY_MULTIPOLYGON_TYPE = 5;
	static constexpr uint8_t LEGACY_GEOMETRYCOLLECTION_TYPE = 6;
	static constexpr uint8_t LEGACY_UNKNOWN_TYPE = 7;

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		// Create an enum type for all geometry types
		// Ensure that these are in the same order as the LegacyGeometryType enum
		const vector<string> enum_values = {"POINT", "LINESTRING", "POLYGON", "MULTIPOINT", "MULTILINESTRING",
		                                    "MULTIPOLYGON", "GEOMETRYCOLLECTION",
		                                    // or...
		                                    "UNKNOWN"};

		input.GetBoundFunction().SetReturnType(GeoTypes::CreateEnumType("GEOMETRY_TYPE", enum_values));
		return nullptr;
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, uint8_t>(args.data[0], result, [&](const string_t &blob) {
			// TODO: Peek dont deserialize

			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			switch (geom.get_type()) {
			case sgl::geometry_type::POINT:
				return LEGACY_POINT_TYPE;
			case sgl::geometry_type::LINESTRING:
				return LEGACY_LINESTRING_TYPE;
			case sgl::geometry_type::POLYGON:
				return LEGACY_POLYGON_TYPE;
			case sgl::geometry_type::MULTI_POINT:
				return LEGACY_MULTIPOINT_TYPE;
			case sgl::geometry_type::MULTI_LINESTRING:
				return LEGACY_MULTILINESTRING_TYPE;
			case sgl::geometry_type::MULTI_POLYGON:
				return LEGACY_MULTIPOLYGON_TYPE;
			case sgl::geometry_type::GEOMETRY_COLLECTION:
				return LEGACY_GEOMETRYCOLLECTION_TYPE;
			default:
				return LEGACY_UNKNOWN_TYPE;
			}
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		*ConstantVector::GetData<uint8_t>(result) = LEGACY_POINT_TYPE;
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		*ConstantVector::GetData<uint8_t>(result) = LEGACY_LINESTRING_TYPE;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		*ConstantVector::GetData<uint8_t>(result) = LEGACY_POLYGON_TYPE;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	Returns a 'GEOMETRY_TYPE' enum identifying the input geometry type. Possible enum return types are: `POINT`, `LINESTRING`, `POLYGON`, `MULTIPOINT`, `MULTILINESTRING`, `MULTIPOLYGON`, and `GEOMETRYCOLLECTION`.
	)";

	static constexpr auto EXAMPLE = R"(
	SELECT DISTINCT ST_GeometryType(ST_GeomFromText('POINT(1 1)'));
	----
	POINT
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_GeometryType", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalTypeId::ANY);

				variant.SetBind(Bind);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalTypeId::ANY);

				variant.SetBind(Bind);
				variant.SetFunction(ExecutePoint);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalTypeId::ANY);

				variant.SetBind(Bind);
				variant.SetFunction(ExecuteLineString);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalTypeId::ANY);

				variant.SetBind(Bind);
				variant.SetFunction(ExecutePolygon);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_GeomFromHEXWKB
//======================================================================================================================

struct ST_GeomFromHEXWKB {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Move this into SGL
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto &input = args.data[0];
		auto count = args.size();

		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		sgl::wkb_reader reader(lstate.GetAllocator());
		reader.set_allow_mixed_zm(true);
		reader.set_nan_as_empty(true);

		UnaryExecutor::Execute<string_t, string_t>(input, result, count, [&](const string_t &input_hex) {
			const auto hex_size = input_hex.GetSize();
			const auto hex_ptr = const_data_ptr_cast(input_hex.GetData());

			if (hex_size % 2 == 1) {
				throw InvalidInputException("Invalid HEX WKB string, length must be even.");
			}

			const auto blob_size = hex_size / 2;

			const unique_ptr<char[]> wkb_blob(new char[blob_size]);
			const auto blob_ptr = wkb_blob.get();
			auto blob_idx = 0;
			for (idx_t hex_idx = 0; hex_idx < hex_size; hex_idx += 2) {
				const auto byte_a = Blob::HEX_MAP[hex_ptr[hex_idx]];
				const auto byte_b = Blob::HEX_MAP[hex_ptr[hex_idx + 1]];
				if (byte_a == -1 || byte_b == -1) {
					throw InvalidInputException("Invalid character in HEX WKB string: '%c%c'", hex_ptr[hex_idx],
					                            hex_ptr[hex_idx + 1]);
				}

				blob_ptr[blob_idx++] = (byte_a << 4) + byte_b;
			}

			sgl::geometry geom;
			if (!reader.try_parse(geom, blob_ptr, blob_size)) {
				const auto error = reader.get_error_message();
				throw InvalidInputException("Could not parse HEX WKB string: %s", error);
			}

			// Enforce that we have a cohesive ZM layout
			if (reader.parsed_mixed_zm()) {
				sgl::ops::force_zm(alloc, geom, reader.parsed_any_z(), reader.parsed_any_m(), 0, 0);
			}

			return lstate.Serialize(result, geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Add docs
	static constexpr auto DESCRIPTION = R"(
		Deserialize a GEOMETRY from a HEX(E)WKB encoded string

		DuckDB spatial doesn't currently differentiate between `WKB` and `EWKB`, so `ST_GeomFromHEXWKB` and `ST_GeomFromHEXEWKB` are just aliases of each other.
	)";

	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {

		// Our WKB reader also parses EWKB, even though it will just ignore SRID's.
		// so we'll just add an alias for now. In the future, once we actually handle
		// EWKB and store SRID's, these functions should differentiate between
		// the two formats.

		for (const auto &alias : {"ST_GeomFromHEXWKB", "ST_GeomFromHEXEWKB"}) {
			FunctionBuilder::RegisterScalar(loader, alias, [](ScalarFunctionBuilder &func) {
				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("hexwkb", LogicalType::VARCHAR);
					variant.SetReturnType(LogicalType::GEOMETRY());

					variant.SetInit(LocalState::Init);
					variant.SetFunction(Execute);
					variant.CanThrowErrors();
				});

				func.SetDescription(DESCRIPTION);
				func.SetExample(EXAMPLE);

				func.SetTag("ext", "spatial");
				func.SetTag("category", "construction");
			});
		}
	}
};

//======================================================================================================================
// ST_GeomFromGeoJSON
//======================================================================================================================

struct ST_GeomFromGeoJSON {

	//------------------------------------------------------------------------------------------------------------------
	// GEOJSON -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Move this into SGL and make non-recursive
	// At least rewrite, its kind of a mess right now.

	static void PointFromGeoJSON(sgl::geometry *geom, yyjson_val *coord_array, ArenaAllocator &arena,
	                             const string_t &raw, bool &has_z) {

		// Point
		geom->set_type(sgl::geometry_type::POINT);
		geom->set_z(has_z);

		auto len = yyjson_arr_size(coord_array);
		if (len == 0) {
			// empty point, return
			return;
		}
		if (len < 2) {
			throw InvalidInputException("GeoJSON input coordinates field is not an array of at least length 2: %s",
			                            raw.GetString());
		}
		auto x_val = yyjson_arr_get_first(coord_array);
		if (!yyjson_is_num(x_val)) {
			throw InvalidInputException("GeoJSON input coordinates field is not an array of numbers: %s",
			                            raw.GetString());
		}
		auto y_val = yyjson_arr_get(coord_array, 1);
		if (!yyjson_is_num(y_val)) {
			throw InvalidInputException("GeoJSON input coordinates field is not an array of numbers: %s",
			                            raw.GetString());
		}

		auto x = yyjson_get_num(x_val);
		auto y = yyjson_get_num(y_val);

		auto geom_has_z = len > 2;
		if (geom_has_z) {
			has_z = true;
			auto z_val = yyjson_arr_get(coord_array, 2);
			if (!yyjson_is_num(z_val)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of numbers: %s",
				                            raw.GetString());
			}
			auto z = yyjson_get_num(z_val);
			auto mem = arena.AllocateAligned(sizeof(double) * 3);
			auto ptr = reinterpret_cast<double *>(mem);

			ptr[0] = x;
			ptr[1] = y;
			ptr[2] = z;

			geom->set_vertex_array(mem, 1);
			geom->set_z(true);
		} else {
			auto mem = arena.AllocateAligned(sizeof(double) * 2);
			auto ptr = reinterpret_cast<double *>(mem);

			ptr[0] = x;
			ptr[1] = y;

			geom->set_vertex_array(mem, 1);
		}
	}

	static void LineStringFromGeoJSON(sgl::geometry *geom, yyjson_val *coord_array, ArenaAllocator &arena,
	                                  const string_t &raw, bool &has_z) {

		geom->set_type(sgl::geometry_type::LINESTRING);
		geom->set_z(has_z);

		auto len = yyjson_arr_size(coord_array);
		if (len == 0) {
			// Empty, do nothing
			return;
		}

		// Sniff the coordinates to see if we have Z
		bool has_any_z = false;
		size_t idx, max;
		yyjson_val *coord;
		yyjson_arr_foreach(coord_array, idx, max, coord) {
			if (!yyjson_is_arr(coord)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of arrays: %s",
				                            raw.GetString());
			}
			auto coord_len = yyjson_arr_size(coord);
			if (coord_len > 2) {
				has_any_z = true;
			} else if (coord_len < 2) {
				throw InvalidInputException(
				    "GeoJSON input coordinates field is not an array of arrays of length >= 2: %s", raw.GetString());
			}
		}

		if (has_any_z) {
			has_z = true;
			geom->set_z(true);
		}

		const auto vertex_size = has_any_z ? 3 : 2;
		const auto vertex_mem = arena.AllocateAligned(sizeof(double) * vertex_size * len);
		geom->set_vertex_array(vertex_mem, len);

		const auto vertex_ptr = reinterpret_cast<double *>(vertex_mem);

		yyjson_arr_foreach(coord_array, idx, max, coord) {
			auto coord_len = yyjson_arr_size(coord);
			auto x_val = yyjson_arr_get_first(coord);
			if (!yyjson_is_num(x_val)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of arrays of numbers: %s",
				                            raw.GetString());
			}
			auto y_val = yyjson_arr_get(coord, 1);
			if (!yyjson_is_num(y_val)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of arrays of numbers: %s",
				                            raw.GetString());
			}
			auto x = yyjson_get_num(x_val);
			auto y = yyjson_get_num(y_val);
			auto z = 0.0;

			if (coord_len > 2) {
				auto z_val = yyjson_arr_get(coord, 2);
				if (!yyjson_is_num(z_val)) {
					throw InvalidInputException(
					    "GeoJSON input coordinates field is not an array of arrays of numbers: %s", raw.GetString());
				}
				z = yyjson_get_num(z_val);
			}

			vertex_ptr[idx * vertex_size] = x;
			vertex_ptr[idx * vertex_size + 1] = y;
			if (has_any_z) {
				vertex_ptr[idx * vertex_size + 2] = z;
			}
		}
	}

	static void PolygonFromGeoJSON(sgl::geometry *geom, yyjson_val *coord_array, ArenaAllocator &arena,
	                               const string_t &raw, bool &has_z) {
		// Polygon
		geom->set_type(sgl::geometry_type::POLYGON);
		geom->set_z(has_z);

		auto num_rings = yyjson_arr_size(coord_array);
		if (num_rings == 0) {
			// Empty, do nothig
			return;
		}

		size_t idx, max;
		yyjson_val *ring_val;
		yyjson_arr_foreach(coord_array, idx, max, ring_val) {
			if (!yyjson_is_arr(ring_val)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of arrays: %s",
				                            raw.GetString());
			}
			const auto mem = arena.AllocateAligned(sizeof(sgl::geometry));
			const auto ring = new (mem) sgl::geometry(sgl::geometry_type::LINESTRING, has_z, false);
			LineStringFromGeoJSON(ring, ring_val, arena, raw, has_z);

			geom->append_part(ring);
		}
	}

	static void MultiPointFromGeoJSON(sgl::geometry *geom, yyjson_val *coord_array, ArenaAllocator &arena,
	                                  const string_t &raw, bool &has_z) {

		// MultiPoint
		geom->set_type(sgl::geometry_type::MULTI_POINT);
		geom->set_z(has_z);

		auto num_points = yyjson_arr_size(coord_array);
		if (num_points == 0) {
			// Empty, do nothing
			return;
		}

		// MultiPoint
		size_t idx, max;
		yyjson_val *point_val;
		yyjson_arr_foreach(coord_array, idx, max, point_val) {
			if (!yyjson_is_arr(point_val)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of arrays: %s",
				                            raw.GetString());
			}
			if (yyjson_arr_size(point_val) < 2) {
				throw InvalidInputException(
				    "GeoJSON input coordinates field is not an array of arrays of length >= 2: %s", raw.GetString());
			}

			const auto mem = arena.AllocateAligned(sizeof(sgl::geometry));
			const auto point = new (mem) sgl::geometry(sgl::geometry_type::POINT, has_z, false);
			PointFromGeoJSON(point, point_val, arena, raw, has_z);

			geom->append_part(point);
		}
	}

	static void MultiLineStringFromGeoJSON(sgl::geometry *geom, yyjson_val *coord_array, ArenaAllocator &arena,
	                                       const string_t &raw, bool &has_z) {
		// MultiLineString
		geom->set_type(sgl::geometry_type::MULTI_LINESTRING);
		geom->set_z(has_z);

		auto num_linestrings = yyjson_arr_size(coord_array);
		if (num_linestrings == 0) {
			// Empty, do nothing
			return;
		}

		size_t idx, max;
		yyjson_val *linestring_val;
		yyjson_arr_foreach(coord_array, idx, max, linestring_val) {
			if (!yyjson_is_arr(linestring_val)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of arrays: %s",
				                            raw.GetString());
			}
			const auto mem = arena.AllocateAligned(sizeof(sgl::geometry));
			const auto line = new (mem) sgl::geometry(sgl::geometry_type::LINESTRING, has_z, false);
			LineStringFromGeoJSON(line, linestring_val, arena, raw, has_z);

			geom->append_part(line);
		}
	}

	static void MultiPolygonFromGeoJSON(sgl::geometry *geom, yyjson_val *coord_array, ArenaAllocator &arena,
	                                    const string_t &raw, bool &has_z) {

		// MultiPolygon
		geom->set_type(sgl::geometry_type::MULTI_POLYGON);
		geom->set_z(has_z);

		auto num_polygons = yyjson_arr_size(coord_array);
		if (num_polygons == 0) {
			// Empty, do nothing
			return;
		}

		size_t idx, max;
		yyjson_val *polygon_val;
		yyjson_arr_foreach(coord_array, idx, max, polygon_val) {
			if (!yyjson_is_arr(polygon_val)) {
				throw InvalidInputException("GeoJSON input coordinates field is not an array of arrays: %s",
				                            raw.GetString());
			}
			const auto mem = arena.AllocateAligned(sizeof(sgl::geometry));
			const auto polygon = new (mem) sgl::geometry(sgl::geometry_type::POLYGON, has_z, false);
			PolygonFromGeoJSON(polygon, polygon_val, arena, raw, has_z);

			geom->append_part(polygon);
		}
	}

	static void GeometryCollectionFromGeoJSON(sgl::geometry *geom, yyjson_val *root, ArenaAllocator &arena,
	                                          const string_t &raw, bool &has_z) {

		geom->set_type(sgl::geometry_type::GEOMETRY_COLLECTION);
		geom->set_z(has_z);

		auto geometries_val = yyjson_obj_get(root, "geometries");
		if (!geometries_val) {
			throw InvalidInputException("GeoJSON input does not have a geometries field: %s", raw.GetString());
		}
		if (!yyjson_is_arr(geometries_val)) {
			throw InvalidInputException("GeoJSON input geometries field is not an array: %s", raw.GetString());
		}
		auto num_geometries = yyjson_arr_size(geometries_val);
		if (num_geometries == 0) {
			// Empty, do nothing
			return;
		}

		size_t idx, max;
		yyjson_val *geometry_val;
		yyjson_arr_foreach(geometries_val, idx, max, geometry_val) {
			const auto mem = arena.AllocateAligned(sizeof(sgl::geometry));
			const auto geometry = new (mem) sgl::geometry(sgl::geometry_type::INVALID, has_z, false);
			FromGeoJSON(geometry, geometry_val, arena, raw, has_z);

			geom->append_part(geometry);
		}
	}

	static void FromGeoJSON(sgl::geometry *geom, yyjson_val *root, ArenaAllocator &arena, const string_t &raw,
	                        bool &has_z) {
		auto type_val = yyjson_obj_get(root, "type");
		if (!type_val) {
			throw InvalidInputException("GeoJSON input does not have a type field: %s", raw.GetString());
		}
		auto type_str = yyjson_get_str(type_val);
		if (!type_str) {
			throw InvalidInputException("GeoJSON input type field is not a string: %s", raw.GetString());
		}

		if (StringUtil::Equals(type_str, "GeometryCollection")) {
			return GeometryCollectionFromGeoJSON(geom, root, arena, raw, has_z);
		}

		// Get the coordinates
		auto coord_array = yyjson_obj_get(root, "coordinates");
		if (!coord_array) {
			throw InvalidInputException("GeoJSON input does not have a coordinates field: %s", raw.GetString());
		}
		if (!yyjson_is_arr(coord_array)) {
			throw InvalidInputException("GeoJSON input coordinates field is not an array: %s", raw.GetString());
		}

		if (StringUtil::Equals(type_str, "Point")) {
			return PointFromGeoJSON(geom, coord_array, arena, raw, has_z);
		}
		if (StringUtil::Equals(type_str, "LineString")) {
			return LineStringFromGeoJSON(geom, coord_array, arena, raw, has_z);
		}
		if (StringUtil::Equals(type_str, "Polygon")) {
			return PolygonFromGeoJSON(geom, coord_array, arena, raw, has_z);
		}
		if (StringUtil::Equals(type_str, "MultiPoint")) {
			return MultiPointFromGeoJSON(geom, coord_array, arena, raw, has_z);
		}
		if (StringUtil::Equals(type_str, "MultiLineString")) {
			return MultiLineStringFromGeoJSON(geom, coord_array, arena, raw, has_z);
		}
		if (StringUtil::Equals(type_str, "MultiPolygon")) {
			return MultiPolygonFromGeoJSON(geom, coord_array, arena, raw, has_z);
		}
		throw InvalidInputException("GeoJSON input has invalid type field: %s", raw.GetString());
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto &input = args.data[0];
		auto count = args.size();

		auto &lstate = LocalState::ResetAndGet(state);
		auto &arena = lstate.GetArena();

		JSONAllocator json_allocator(arena);

		UnaryExecutor::Execute<string_t, string_t>(input, result, count, [&](const string_t &input) {
			yyjson_read_err err;
			auto doc = yyjson_read_opts(const_cast<char *>(input.GetDataUnsafe()), input.GetSize(),
			                            YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_ALLOW_COMMENTS,
			                            json_allocator.GetYYJSONAllocator(), &err);

			if (err.code) {
				throw InvalidInputException("Could not parse GeoJSON input: %s, (%s)", err.msg, input.GetString());
			}

			const auto root = yyjson_doc_get_root(doc);
			if (!yyjson_is_obj(root)) {
				throw InvalidInputException("Could not parse GeoJSON input: Not a valid JSON object, (%s)",
				                            input.GetString());
			}

			bool has_z = false;
			sgl::geometry geom;

			// Parse into the geometry
			FromGeoJSON(&geom, root, arena, input, has_z);

			if (has_z) {
				// Ensure the geometries has consistent Z values
				sgl::ops::force_zm(lstate.GetAllocator(), geom, has_z, false, 0, 0);
			}
			D_ASSERT(geom.get_type() != sgl::geometry_type::INVALID);

			return lstate.Serialize(result, geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	    Deserializes a GEOMETRY from a GeoJSON fragment.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_GeomFromGeoJSON('{"type": "Point", "coordinates": [1.0, 2.0]}');
		----
		POINT (1 2)
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_GeomFromGeoJSON", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geojson", LogicalType::JSON());
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geojson", LogicalType::VARCHAR);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_GeomFromText
//======================================================================================================================

struct ST_GeomFromText {

	//------------------------------------------------------------------------------------------------------------------
	// Binding
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Remove this, this doesnt make any sense here. Invalid geometries should be handled by TRY_CAST
	//
	struct BindData final : public FunctionData {
		explicit BindData(bool ignore_invalid) : ignore_invalid(ignore_invalid) {
		}

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindData>(ignore_invalid);
		}
		bool Equals(const FunctionData &other_p) const override {
			return true;
		}

		bool ignore_invalid = false;
	};

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &arguments = input.GetArguments();
		auto &context = input.GetClientContext();

		if (arguments.empty()) {
			throw InvalidInputException("ST_GeomFromText requires at least one argument");
		}
		const auto &input_type = arguments[0]->GetReturnType();
		if (input_type.id() != LogicalTypeId::VARCHAR) {
			throw InvalidInputException("ST_GeomFromText requires a string argument");
		}

		bool ignore_invalid = false;
		for (idx_t i = 1; i < arguments.size(); i++) {
			auto &arg = arguments[i];
			if (arg->HasParameter()) {
				throw InvalidInputException("Parameters are not supported in ST_GeomFromText optional arguments");
			}
			if (!arg->IsFoldable()) {
				throw InvalidInputException(
				    "Non-constant arguments are not supported in ST_GeomFromText optional arguments");
			}
			if (arg->GetAlias() == "ignore_invalid") {
				if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
					throw InvalidInputException("ST_GeomFromText optional argument 'ignore_invalid' must be a boolean");
				}
				ignore_invalid = BooleanValue::Get(ExpressionExecutor::EvaluateScalar(context, *arg));
			}
		}
		return make_uniq<BindData>(ignore_invalid);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		const auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &bind_data = func_expr.BindInfo()->Cast<BindData>();
		const auto ignore_invalid = bind_data.ignore_invalid;

		sgl::wkt_reader reader(alloc);

		UnaryExecutor::Execute<string_t, string_t>(
		    args.data[0], result, args.size(), [&](const string_t &wkt) -> optional<string_t> {
			    const auto wkt_ptr = wkt.GetDataUnsafe();
			    const auto wkt_len = wkt.GetSize();

			    sgl::geometry geom;

			    if (!reader.try_parse(geom, wkt_ptr, wkt_len)) {

				    if (ignore_invalid) {
					    return nullopt;
				    }
				    const auto error = reader.get_error_message();
				    throw InvalidInputException(error);
			    }

			    return lstate.Serialize(result, geom);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DOCUMENTATION = R"(
		Deserialize a GEOMETRY from a WKT encoded string
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_GeomFromText", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("wkt", LogicalType::VARCHAR);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("wkt", LogicalType::VARCHAR);
				variant.AddParameter("ignore_invalid", LogicalType::BOOLEAN);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(Bind);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetDescription(DOCUMENTATION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_GeomFromWKB
//======================================================================================================================

struct ST_GeomFromWKB {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		sgl::wkb_reader reader(alloc);
		reader.set_allow_mixed_zm(true);
		reader.set_nan_as_empty(true);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &wkb) {
			const auto wkb_buf = wkb.GetDataUnsafe();
			const auto wkb_len = wkb.GetSize();

			sgl::geometry geom;

			if (!reader.try_parse(geom, wkb_buf, wkb_len)) {
				const auto error = reader.get_error_message();
				auto msg = "Could not parse WKB input:" + string(error);
				if (reader.get_error() == sgl::wkb_reader_error::UNSUPPORTED_TYPE) {
					msg += "\n(You can use TRY_CAST instead to replace unsupported geometries with NULL)";
				}
				throw InvalidInputException(msg);
			}

			if (reader.parsed_mixed_zm()) {
				sgl::ops::force_zm(alloc, geom, reader.parsed_any_z(), reader.parsed_any_m(), 0, 0);
			}

			return lstate.Serialize(result, geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		auto count = args.size();
		auto &input = args.data[0];

		input.Flatten();

		auto &point_children = StructVector::GetEntries(result);
		const auto x_data = FlatVector::GetDataMutable<double>(point_children[0]);
		const auto y_data = FlatVector::GetDataMutable<double>(point_children[1]);

		sgl::wkb_reader reader(alloc);
		reader.set_allow_mixed_zm(true);
		reader.set_nan_as_empty(true);

		for (idx_t i = 0; i < count; i++) {
			const auto &wkb = FlatVector::GetDataMutable<string_t>(input)[i];

			const auto wkb_ptr = wkb.GetDataUnsafe();
			const auto wkb_len = wkb.GetSize();

			sgl::geometry geom;
			if (!reader.try_parse(geom, wkb_ptr, wkb_len)) {
				const auto error = reader.get_error_message();
				throw InvalidInputException("Could not parse WKB input: %s", error);
			}

			if (geom.get_type() != sgl::geometry_type::POINT) {
				throw InvalidInputException("ST_Point2DFromWKB: WKB is not a POINT");
			}

			const auto vertex = geom.get_vertex_xy(0);

			x_data[i] = vertex.x;
			y_data[i] = vertex.y;
		}

		if (args.AllConstant() || args.size() == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		D_ASSERT(args.data.size() == 1);
		const auto count = args.size();
		auto &wkb_blobs = args.data[0];
		wkb_blobs.Flatten();

		auto &inner = ListVector::GetChildMutable(result);
		const auto lines = FlatVector::GetDataMutable<list_entry_t>(result);
		const auto wkb_data = FlatVector::GetDataMutable<string_t>(wkb_blobs);

		idx_t total_size = 0;

		sgl::wkb_reader reader(alloc);
		reader.set_allow_mixed_zm(true);
		reader.set_nan_as_empty(true);

		for (idx_t i = 0; i < count; i++) {
			auto wkb = wkb_data[i];

			const auto wkb_ptr = wkb.GetDataUnsafe();
			const auto wkb_len = wkb.GetSize();

			sgl::geometry geom;
			if (!reader.try_parse(geom, wkb_ptr, wkb_len)) {
				const auto error = reader.get_error_message();
				throw InvalidInputException("Could not parse WKB input: %s", error);
			}

			if (geom.get_type() != sgl::geometry_type::LINESTRING) {
				throw InvalidInputException("ST_LineString2DFromWKB: WKB is not a LINESTRING");
			}

			const auto line_size = geom.get_vertex_count();

			lines[i].offset = total_size;
			lines[i].length = line_size;

			ListVector::Reserve(result, total_size + line_size);

			// Since ListVector::Reserve potentially reallocates, we need to re-fetch the inner vector pointers
			auto &children = StructVector::GetEntries(inner);
			auto &x_child = children[0];
			auto &y_child = children[1];
			auto x_data = FlatVector::GetDataMutable<double>(x_child);
			auto y_data = FlatVector::GetDataMutable<double>(y_child);

			for (idx_t j = 0; j < line_size; j++) {
				const auto vertex = geom.get_vertex_xy(j);
				x_data[total_size + j] = vertex.x;
				y_data[total_size + j] = vertex.y;
			}

			total_size += line_size;
		}

		ListVector::SetListSize(result, total_size);

		if (args.AllConstant() || args.size() == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		D_ASSERT(args.data.size() == 1);
		auto count = args.size();

		// Set up input data
		auto &wkb_blobs = args.data[0];
		wkb_blobs.Flatten();
		auto wkb_data = FlatVector::GetDataMutable<string_t>(wkb_blobs);

		// Set up output data
		auto &ring_vec = ListVector::GetChildMutable(result);
		auto polygons = FlatVector::GetDataMutable<list_entry_t>(result);

		idx_t total_ring_count = 0;
		idx_t total_point_count = 0;

		sgl::wkb_reader reader(alloc);
		reader.set_allow_mixed_zm(true);
		reader.set_nan_as_empty(true);

		for (idx_t i = 0; i < count; i++) {
			auto wkb = wkb_data[i];

			const auto wkb_ptr = wkb.GetDataUnsafe();
			const auto wkb_len = wkb.GetSize();

			sgl::geometry geom;
			if (!reader.try_parse(geom, wkb_ptr, wkb_len)) {
				const auto error = reader.get_error_message();
				throw InvalidInputException("Could not parse WKB input: %s", error);
			}

			if (geom.get_type() != sgl::geometry_type::POLYGON) {
				throw InvalidInputException("ST_Polygon2DFromWKB: WKB is not a POLYGON");
			}

			const auto ring_count = geom.get_part_count();

			polygons[i].offset = total_ring_count;
			polygons[i].length = ring_count;

			ListVector::Reserve(result, total_ring_count + ring_count);
			// Since ListVector::Reserve potentially reallocates, we need to re-fetch the inner vector pointers

			const auto tail = geom.get_last_part();
			auto ring = tail;
			if (ring) {
				int j = 0;
				do {
					ring = ring->get_next();
					const auto point_count = ring->get_vertex_count();

					ListVector::Reserve(ring_vec, total_point_count + point_count);
					auto ring_entries = FlatVector::GetDataMutable<list_entry_t>(ring_vec);
					auto &inner = ListVector::GetChildMutable(ring_vec);

					auto &children = StructVector::GetEntries(inner);
					auto &x_child = children[0];
					auto &y_child = children[1];
					auto x_data = FlatVector::GetDataMutable<double>(x_child);
					auto y_data = FlatVector::GetDataMutable<double>(y_child);

					for (idx_t k = 0; k < point_count; k++) {
						const auto vertex = ring->get_vertex_xy(k);
						x_data[total_point_count + k] = vertex.x;
						y_data[total_point_count + k] = vertex.y;
					}

					ring_entries[total_ring_count + j].offset = total_point_count;
					ring_entries[total_ring_count + j].length = point_count;

					total_point_count += point_count;

					j++;

				} while (ring != tail);
			}

			total_ring_count += ring_count;
		}

		ListVector::SetListSize(result, total_ring_count);
		ListVector::SetListSize(ring_vec, total_point_count);

		if (args.AllConstant() || args.size() == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// TODO

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Point2DFromWKB", [](ScalarFunctionBuilder &builder) {
			builder.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("blob", LogicalType::BLOB);
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecutePoint);
				variant.CanThrowErrors();
			});

			builder.SetDescription("Deserialize a POINT_2D from a WKB encoded blob");
			builder.SetExample("");
			builder.SetTag("ext", "spatial");
			builder.SetTag("category", "conversion");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_LineString2DFromWKB", [](ScalarFunctionBuilder &builder) {
			builder.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("blob", LogicalType::BLOB);
				variant.SetReturnType(GeoTypes::LINESTRING_2D());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteLineString);
				variant.CanThrowErrors();
			});

			builder.SetDescription("Deserialize a LINESTRING_2D from a WKB encoded blob");
			builder.SetExample("");
			builder.SetTag("ext", "spatial");
			builder.SetTag("category", "conversion");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_Polygon2DFromWKB", [](ScalarFunctionBuilder &builder) {
			builder.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("blob", LogicalType::BLOB);
				variant.SetReturnType(GeoTypes::POLYGON_2D());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecutePolygon);
				variant.CanThrowErrors();
			});

			builder.SetDescription("Deserialize a POLYGON_2D from a WKB encoded blob");
			builder.SetExample("");
			builder.SetTag("ext", "spatial");
			builder.SetTag("category", "conversion");
		});

		/*
		FunctionBuilder::RegisterScalar(loader, "ST_GeomFromWKB", [](ScalarFunctionBuilder &builder) {
		    builder.AddVariant([](ScalarFunctionVariantBuilder &variant) {
		        variant.AddParameter("blob", LogicalType::BLOB);
		        variant.SetReturnType(LogicalType::GEOMETRY());

		        variant.SetInit(LocalState::Init);
		        variant.SetFunction(ExecuteGeometry);
		    });

		    builder.SetDescription(DESCRIPTION);
		    builder.SetExample(EXAMPLE);
		    builder.SetTag("ext", "spatial");
		    builder.SetTag("category", "conversion");
		});
		*/
	}
};

//======================================================================================================================
// ST_HasZ
//======================================================================================================================

struct ST_HasZ {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](const string_t &blob) {
			// TODO: Peek without deserializing!
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			return geom.has_z();
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = "Check if the input geometry has Z values.";

	static constexpr auto EXAMPLE = R"(
	-- HasZ for a 2D geometry
	SELECT ST_HasZ(ST_GeomFromText('POINT(1 1)'));
	----
	false

	-- HasZ for a 3DZ geometry
	SELECT ST_HasZ(ST_GeomFromText('POINT Z(1 1 1)'));
	----
	true

	-- HasZ for a 3DM geometry
	SELECT ST_HasZ(ST_GeomFromText('POINT M(1 1 1)'));
	----
	false

	-- HasZ for a 4D geometry
	SELECT ST_HasZ(ST_GeomFromText('POINT ZM(1 1 1 1)'));
	----
	true
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_HasZ", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_HasM
//======================================================================================================================

struct ST_HasM {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](const string_t &blob) {
			// TODO: Peek without deserializing!
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			return geom.has_m();
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = "Check if the input geometry has M values.";

	static constexpr auto EXAMPLE = R"(
	-- HasM for a 2D geometry
	SELECT ST_HasM(ST_GeomFromText('POINT(1 1)'));
	----
	false

	-- HasM for a 3DZ geometry
	SELECT ST_HasM(ST_GeomFromText('POINT Z(1 1 1)'));
	----
	false

	-- HasM for a 3DM geometry
	SELECT ST_HasM(ST_GeomFromText('POINT M(1 1 1)'));
	----
	true

	-- HasM for a 4D geometry
	SELECT ST_HasM(ST_GeomFromText('POINT ZM(1 1 1 1)'));
	----
	true
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_HasM", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_LineInterpolatePoint
//======================================================================================================================

struct ST_LineInterpolatePoint {
	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, double, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &blob, const double fraction) {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::LINESTRING) {
				    throw InvalidInputException("ST_LineInterpolatePoint: input is not a LINESTRING");
			    }

			    sgl::vertex_xyzm out_vertex = {0, 0, 0, 0};
			    if (sgl::linestring::interpolate(geom, fraction, out_vertex)) {
				    sgl::geometry point(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
				    point.set_vertex_array(&out_vertex, 1);
				    return lstate.Serialize(result, point);
			    }

			    sgl::geometry empty(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
			    return lstate.Serialize(result, empty);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns a point interpolated along a line at a fraction of total 2D length.
	)";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_LineInterpolatePoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("fraction", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "referencing");
		});
	}
};

//======================================================================================================================
// ST_LineInterpolatePoints
//======================================================================================================================

struct ST_LineInterpolatePoints {
	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		TernaryExecutor::Execute<string_t, double, bool, string_t>(
		    args.data[0], args.data[1], args.data[2], result,
		    [&](const string_t &blob, const double fraction, const bool repeat) {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::LINESTRING) {
				    throw InvalidInputException("ST_LineInterpolatePoints: input is not a LINESTRING");
			    }

			    // equivalent to ST_LineInterpolatePoint
			    if (!repeat || fraction > 0.5) {
				    sgl::vertex_xyzm out_vertex = {0, 0, 0, 0};

				    if (sgl::linestring::interpolate(geom, fraction, out_vertex)) {
					    sgl::geometry point(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
					    point.set_vertex_array(&out_vertex, 1);
					    return lstate.Serialize(result, point);
				    }

				    sgl::geometry empty(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
				    return lstate.Serialize(result, empty);
			    }

			    sgl::geometry mpoint;
			    sgl::linestring::interpolate_points(alloc, geom, fraction, mpoint);
			    return lstate.Serialize(result, mpoint);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns a multi-point interpolated along a line at a fraction of total 2D length.

		if repeat is false, the result is a single point, (and equivalent to ST_LineInterpolatePoint),
		otherwise, the result is a multi-point with points repeated at the fraction interval.
	)";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_LineInterpolatePoints", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("fraction", LogicalType::DOUBLE);
				variant.AddParameter("repeat", LogicalType::BOOLEAN);
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetFunction(ExecuteGeometry);
				variant.SetInit(LocalState::Init);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "referencing");
		});
	}
};

//======================================================================================================================
// ST_LineLocatePoint
//======================================================================================================================

struct ST_LineLocatePoint {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, double>(
		    args.data[0], args.data[1], result, args.size(),
		    [&](const string_t &line_blob, const string_t &point_blob) {
			    sgl::geometry line;
			    lstate.Deserialize(line_blob, line);

			    if (line.get_type() != sgl::geometry_type::LINESTRING) {
				    throw InvalidInputException("ST_LineLocatePoint: input is not a LINESTRING");
			    }
			    if (line.is_empty()) {
				    throw InvalidInputException("ST_LineLocatePoint: input LINESTRING is empty");
			    }

			    sgl::geometry point;
			    lstate.Deserialize(point_blob, point);

			    if (point.get_type() != sgl::geometry_type::POINT) {
				    throw InvalidInputException("ST_LineLocatePoint: input point is not a POINT");
			    }

			    if (point.is_empty()) {
				    throw InvalidInputException("ST_LineLocatePoint: input point is empty");
			    }

			    return sgl::linestring::line_locate_point(line, point);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the location on a line closest to a point as a fraction of the total 2D length of the line.
	)";
	static constexpr auto EXAMPLE = R"()";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_LineLocatePoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("point", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "referencing");
		});
	}
};

//======================================================================================================================
// ST_LineSubstring
//======================================================================================================================

struct ST_LineSubstring {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		TernaryExecutor::Execute<string_t, double, double, string_t>(
		    args.data[0], args.data[1], args.data[2], result,
		    [&](const string_t &blob, const double start_fraction, const double end_fraction) {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::LINESTRING) {
				    throw InvalidInputException("ST_LineSubstring: input is not a LINESTRING");
			    }

			    sgl::geometry sline;
			    sgl::linestring::substring(alloc, geom, start_fraction, end_fraction, sline);
			    return lstate.Serialize(result, sline);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns a substring of a line between two fractions of total 2D length.
	)";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_LineSubstring", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("start_fraction", LogicalType::DOUBLE);
				variant.AddParameter("end_fraction", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetFunction(ExecuteGeometry);
				variant.SetInit(LocalState::Init);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "referencing");
		});
	}
};

//======================================================================================================================
// ST_LocateAlong
//======================================================================================================================

struct ST_LocateAlong {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &arguments = input.GetArguments();

		if (arguments.size() == 2) {
			// Push back offset constant
			arguments.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
			input.GetBoundFunction().GetArguments().push_back(LogicalType::DOUBLE);
		}
		return nullptr; // No additional data needed
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		TernaryExecutor::Execute<string_t, double, double, string_t>(
		    args.data[0], args.data[1], args.data[2], result,
		    [&](const string_t &blob, const double measure, const double offset) {
			    // Reset after each execution, because this can be quite memory hungry
			    lstate.GetArena().Reset();

			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (!geom.has_m()) {
				    throw InvalidInputException("ST_LocateAlong: input geometry does not have M dimension");
			    }

			    sgl::geometry points(sgl::geometry_type::MULTI_POINT, geom.has_z(), geom.has_m());
			    sgl::ops::locate_along(alloc, geom, measure, offset, points);

			    if (points.get_part_count() == 1) {
				    const auto part = points.get_first_part();
				    return lstate.Serialize(result, *part);
			    }

			    return lstate.Serialize(result, points);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns a point or multi-point, containing the point(s) at the geometry with the given measure
		
		For a LINESTRING, or MULTILINESTRING, the location is determined by interpolating between M values
		For a POINT and MULTIPOINT, the point is returned if the measure matches the M value of the vertex, otherwise an empty geometry is returned
		For a POLYGON, only the exterior ring is considered, and treated as a LINESTRING

		If offset is provided, the resulting point(s) is offset by the given amount perpendicular to the line direction.
	)";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_LocateAlong", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("measure", LogicalType::DOUBLE);
				variant.AddParameter("offset", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS<Bind>);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("measure", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS<Bind>);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "referencing");
		});
	}
};

//======================================================================================================================
// ST_LocateBetween
//======================================================================================================================

struct ST_LocateBetween {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &arguments = input.GetArguments();

		if (arguments.size() == 3) {
			// Push back offset constant
			arguments.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
			input.GetBoundFunction().GetArguments().push_back(LogicalType::DOUBLE);
		}
		return nullptr; // No additional data needed
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &alloc = lstate.GetAllocator();

		// There is no quaternary executor
		const auto row_count = args.size();

		UnifiedVectorFormat geom_format;
		UnifiedVectorFormat lower_format;
		UnifiedVectorFormat upper_format;
		UnifiedVectorFormat offset_format;

		args.data[0].ToUnifiedFormat(geom_format);
		args.data[1].ToUnifiedFormat(lower_format);
		args.data[2].ToUnifiedFormat(upper_format);
		args.data[3].ToUnifiedFormat(offset_format);

		const auto geom_data = UnifiedVectorFormat::GetData<string_t>(geom_format);
		const auto lower_data = UnifiedVectorFormat::GetData<double>(lower_format);
		const auto upper_data = UnifiedVectorFormat::GetData<double>(upper_format);
		const auto offset_data = UnifiedVectorFormat::GetData<double>(offset_format);

		const auto result_data = FlatVector::GetDataMutable<string_t>(result);

		for (idx_t out_idx = 0; out_idx < row_count; out_idx++) {
			const auto geom_idx = geom_format.sel->get_index(out_idx);
			const auto lower_idx = lower_format.sel->get_index(out_idx);
			const auto upper_idx = upper_format.sel->get_index(out_idx);
			const auto offset_idx = offset_format.sel->get_index(out_idx);

			if (!geom_format.validity.RowIsValid(geom_idx) || !lower_format.validity.RowIsValid(lower_idx) ||
			    !upper_format.validity.RowIsValid(upper_idx) || !offset_format.validity.RowIsValid(offset_idx)) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			// Reset after each execution, because this can be quite memory hungry
			lstate.GetArena().Reset();

			sgl::geometry geom;
			lstate.Deserialize(geom_data[geom_idx], geom);

			if (!geom.has_m()) {
				throw InvalidInputException("ST_LocateBetween: input geometry does not have M dimension");
			}

			sgl::geometry collection(sgl::geometry_type::GEOMETRY_COLLECTION, geom.has_z(), geom.has_m());
			sgl::ops::locate_between(alloc, geom, lower_data[lower_idx], upper_data[upper_idx], offset_data[offset_idx],
			                         collection);

			if (collection.get_part_count() == 1) {
				const auto part = collection.get_first_part();
				result_data[out_idx] = lstate.Serialize(result, *part);
			} else {
				result_data[out_idx] = lstate.Serialize(result, collection);
			}
		}

		if (row_count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns a geometry or geometry collection created by filtering and interpolating vertices within a range of "M" values

		Creates a geometry or geometry collection, containing the parts formed by vertices that have an "M" value within the "start_measure" and "end_measure" range

		For LINESTRING or MULTILINESTRING, if a line segment would cross either the upper or lower bound, a vertex is added by interpolating the coordinates at the "intersection"
		For a POINT and MULTIPOINT, the point is added to the collection if its vertex has an "M" value within the range, otherwise it is skipped
		For a POLYGON, only the exterior ring is considered, and treated like a LINESTRING

		If offset is provided, the resulting vertices are offset by the given amount perpendicular to the line direction.
	)";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_LocateBetween", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("start_measure", LogicalType::DOUBLE);
				variant.AddParameter("end_measure", LogicalType::DOUBLE);
				variant.AddParameter("offset", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS<Bind>);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("start_measure", LogicalType::DOUBLE);
				variant.AddParameter("end_measure", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS<Bind>);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);
			func.SetTag("ext", "spatial");
			func.SetTag("category", "referencing");
		});
	}
};

//======================================================================================================================
// ST_ZMFlag
//======================================================================================================================

struct ST_ZMFlag {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, uint8_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);
			const auto has_z = geom.has_z();
			const auto has_m = geom.has_m();

			if (has_z && has_m) {
				return 3;
			}
			if (has_z) {
				return 2;
			}
			if (has_m) {
				return 1;
			}
			return 0;
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	Returns a flag indicating the presence of Z and M values in the input geometry.
	0 = No Z or M values
	1 = M values only
	2 = Z values only
	3 = Z and M values
	)";

	static constexpr auto EXAMPLE = R"(
	-- ZMFlag for a 2D geometry
	SELECT ST_ZMFlag(ST_GeomFromText('POINT(1 1)'));
	----
	0

	-- ZMFlag for a 3DZ geometry
	SELECT ST_ZMFlag(ST_GeomFromText('POINT Z(1 1 1)'));
	----
	2

	-- ZMFlag for a 3DM geometry
	SELECT ST_ZMFlag(ST_GeomFromText('POINT M(1 1 1)'));
	----
	1

	-- ZMFlag for a 4D geometry
	SELECT ST_ZMFlag(ST_GeomFromText('POINT ZM(1 1 1 1)'));
	----
	3
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_ZMFlag", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::UTINYINT);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Distance_Sphere
//======================================================================================================================

struct ST_Distance_Sphere {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : public FunctionData {

		bool always_xy = false;

		unique_ptr<FunctionData> Copy() const override {
			auto copy = make_uniq<BindData>();
			copy->always_xy = always_xy;
			return copy;
		}
		bool Equals(const FunctionData &other) const override {
			auto &other_bind = other.Cast<BindData>();
			return always_xy == other_bind.always_xy;
		}
	};

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		auto &func = input.GetBoundFunction();

		auto bind_data = make_uniq<BindData>();

		bool is_set = false;
		bind_data->always_xy = SpatialSettings::AlwaysXY(context, is_set);

		if (!is_set) {
			constexpr auto raw_message =
			    "The '%s' function is sensitive to the coordinate axis order of the input geometry.\n"
			    "The current default for this function is to assume [LATITUDE, LONGITUDE] axis order.\n"
			    "This is expected to change to [LONGITUDE, LATITUDE] in the future.\n "
			    "Please explicitly set the 'geometry_always_xy' setting to avoid unexpected changes in behavior.\n"
			    " * 'SET geometry_always_xy = true' to make this function assume all geometries are (LONGITUDE, "
			    "LATITUDE)\n"
			    " * 'SET geometry_always_xy = false' to keep the current behavior and make this warning go away.";

			auto &logger = Logger::Get(context);
			logger.WriteLog("Spatial", LogLevel::LOG_WARNING, StringUtil::Format(raw_message, func.GetName().c_str()));
		}

		return std::move(bind_data);
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	template <bool ALWAYS_XY>
	static double ComputeDistance(const sgl::vertex_xy &v1, const sgl::vertex_xy &v2) {
		if (ALWAYS_XY) {
			return sgl::math::haversine_distance(v1.y, v1.x, v2.y, v2.x);
		} else {
			return sgl::math::haversine_distance(v1.x, v1.y, v2.x, v2.y);
		}
	}

	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<BindData>();

		// Depending on the axis order setting, switch the order of coordinates for the haversine distance calculation
		const auto compute = bdata.always_xy ? ComputeDistance<true> : ComputeDistance<false>;

		BinaryExecutor::Execute<string_t, string_t, double>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &l_blob, const string_t &r_blob) {
			    sgl::geometry lhs;
			    sgl::geometry rhs;

			    lstate.Deserialize(l_blob, lhs);
			    lstate.Deserialize(r_blob, rhs);

			    if (lhs.get_type() != sgl::geometry_type::POINT || rhs.get_type() != sgl::geometry_type::POINT) {
				    throw InvalidInputException("ST_Distance_Sphere only accepts POINT geometries");
			    }

			    if (lhs.is_empty() || rhs.is_empty()) {
				    throw InvalidInputException("ST_Distance_Sphere does not accept empty geometries");
			    }

			    const auto lv = lhs.get_vertex_xy(0);
			    const auto rv = rhs.get_vertex_xy(0);

			    return compute(lv, rv);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 2);

		const auto &bdata = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<BindData>();

		auto &left = args.data[0];
		auto &right = args.data[1];
		auto count = args.size();

		using POINT_TYPE = StructTypeBinary<double, double>;
		using DISTANCE_TYPE = PrimitiveType<double>;

		if (bdata.always_xy) {
			GenericExecutor::ExecuteBinary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE>(
			    left, right, result, count, [&](const POINT_TYPE &left, const POINT_TYPE &right) {
				    return sgl::math::haversine_distance(left.b_val, left.a_val, right.b_val, right.a_val);
			    });
		} else {
			GenericExecutor::ExecuteBinary<POINT_TYPE, POINT_TYPE, DISTANCE_TYPE>(
			    left, right, result, count, [&](const POINT_TYPE &left, const POINT_TYPE &right) {
				    return sgl::math::haversine_distance(left.a_val, left.b_val, right.a_val, right.b_val);
			    });
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the haversine (great circle) distance between two geometries.

	    - Only supports POINT geometries.
	    - Returns the distance in meters.
	    - The input is expected to be in WGS84 (EPSG:4326) coordinates, using a [latitude, longitude] axis order.
	)";

	// TODO: Example
	static constexpr auto EXAMPLE = R"()";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Distance_Sphere", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", LogicalType::GEOMETRY());
				variant.AddParameter("geom2", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetBind(GeoTypes::PropagateCRS<Bind>);
				variant.SetFunction(ExecuteGeometry);

				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point1", GeoTypes::POINT_2D());
				variant.AddParameter("point2", GeoTypes::POINT_2D());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetBind(Bind);
				variant.SetFunction(ExecutePoint);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Hilbert
//======================================================================================================================
struct ST_Hilbert {

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D / BOX_2F
	//------------------------------------------------------------------------------------------------------------------
	template <class T>
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input_vec = args.data[0];
		auto &bounds_vec = args.data[1];
		auto count = args.size();

		constexpr auto max_hilbert = std::numeric_limits<uint16_t>::max();

		using BOX_TYPE = StructTypeQuaternary<T, T, T, T>;
		using UINT32_TYPE = PrimitiveType<uint32_t>;

		GenericExecutor::ExecuteBinary<BOX_TYPE, BOX_TYPE, UINT32_TYPE>(
		    input_vec, bounds_vec, result, count, [&](BOX_TYPE &box, BOX_TYPE &bounds) {
			    const auto x = box.a_val + (box.c_val - box.a_val) / static_cast<T>(2);
			    const auto y = box.b_val + (box.d_val - box.b_val) / static_cast<T>(2);

			    const auto hilbert_width = max_hilbert / (bounds.c_val - bounds.a_val);
			    const auto hilbert_height = max_hilbert / (bounds.d_val - bounds.b_val);

			    // TODO: Check for overflow
			    const auto hilbert_x = static_cast<uint32_t>((x - bounds.a_val) * hilbert_width);
			    const auto hilbert_y = static_cast<uint32_t>((y - bounds.b_val) * hilbert_height);
			    const auto h = sgl::math::hilbert_encode(16, hilbert_x, hilbert_y);
			    return UINT32_TYPE {h};
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// LON/LAT
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLonlat(DataChunk &args, ExpressionState &state, Vector &result) {
		using DOUBLE_TYPE = PrimitiveType<double>;
		using UINT32_TYPE = PrimitiveType<uint32_t>;
		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;

		auto constexpr max_hilbert = std::numeric_limits<uint16_t>::max();

		GenericExecutor::ExecuteTernary<DOUBLE_TYPE, DOUBLE_TYPE, BOX_TYPE, UINT32_TYPE>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
		    [&](DOUBLE_TYPE x, DOUBLE_TYPE y, BOX_TYPE &box) {
			    const auto hilbert_width = max_hilbert / (box.c_val - box.a_val);
			    const auto hilbert_height = max_hilbert / (box.d_val - box.b_val);

			    // TODO: Check for overflow
			    const auto hilbert_x = static_cast<uint32_t>((x.val - box.a_val) * hilbert_width);
			    const auto hilbert_y = static_cast<uint32_t>((y.val - box.b_val) * hilbert_height);
			    const auto h = sgl::math::hilbert_encode(16, hilbert_x, hilbert_y);
			    return UINT32_TYPE {h};
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, uint32_t>(
		    args.data[0], result, args.size(), [&](const string_t &geom) -> optional<uint32_t> {
			    // TODO: This is shit, dont rely on cached bounds
			    Box2D<float> bounds;
			    if (!Serde::TryGetBounds(geom, bounds)) {
				    return nullopt;
			    }

			    const auto dx = bounds.min.x + (bounds.max.x - bounds.min.x) / 2;
			    const auto dy = bounds.min.y + (bounds.max.y - bounds.min.y) / 2;

			    const auto hx = sgl::math::hilbert_f32_to_u32(dx);
			    const auto hy = sgl::math::hilbert_f32_to_u32(dy);

			    return sgl::math::hilbert_encode(16, hx, hy);
		    });
	}

	static void ExecuteGeometryWithBounds(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto constexpr max_hilbert = std::numeric_limits<uint16_t>::max();

		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
		using GEOM_TYPE = PrimitiveType<string_t>;
		using UINT32_TYPE = PrimitiveType<uint32_t>;

		GenericExecutor::ExecuteBinary<GEOM_TYPE, BOX_TYPE, UINT32_TYPE>(
		    args.data[0], args.data[1], result, args.size(), [&](const GEOM_TYPE &geom_type, const BOX_TYPE &bounds) {
			    const auto blob = geom_type.val;

			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    // TODO: Dont deserialize, just get the bounds from blob instead.
			    sgl::extent_xy geom_bounds = sgl::extent_xy::smallest();

			    if (sgl::ops::get_total_extent_xy(geom, geom_bounds) == 0) {
				    throw InvalidInputException("ST_Hilbert(geom, bounds) does not support empty geometries");
			    }

			    const auto dx = geom_bounds.min.x + (geom_bounds.max.x - geom_bounds.min.x) / 2;
			    const auto dy = geom_bounds.min.y + (geom_bounds.max.y - geom_bounds.min.y) / 2;

			    const auto hilbert_width = max_hilbert / (bounds.c_val - bounds.a_val);
			    const auto hilbert_height = max_hilbert / (bounds.d_val - bounds.b_val);
			    // TODO: Check for overflow
			    const auto hilbert_x = static_cast<uint32_t>((dx - bounds.a_val) * hilbert_width);
			    const auto hilbert_y = static_cast<uint32_t>((dy - bounds.b_val) * hilbert_height);

			    const auto h = sgl::math::hilbert_encode(16, hilbert_x, hilbert_y);
			    return UINT32_TYPE {h};
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Encodes the X and Y values as the hilbert curve index for a curve covering the given bounding box.
		If a geometry is provided, the center of the approximate bounding box is used as the point to encode.
		If no bounding box is provided, the hilbert curve index is mapped to the full range of a single-precision float.
		For the BOX_2D and BOX_2DF variants, the center of the box is used as the point to encode.
	)";

	// TODO: example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// TODO: All of these needs examples and docs

		FunctionBuilder::RegisterScalar(loader, "ST_Hilbert", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.AddParameter("bounds", GeoTypes::BOX_2D());
				variant.SetReturnType(LogicalType::UINTEGER);

				variant.SetFunction(ExecuteLonlat);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("bounds", GeoTypes::BOX_2D());
				variant.SetReturnType(LogicalType::UINTEGER);

				variant.SetFunction(ExecuteGeometryWithBounds);
				variant.SetInit(LocalState::Init);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::UINTEGER);

				variant.SetFunction(ExecuteGeometry);
				variant.SetInit(LocalState::Init);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.AddParameter("bounds", GeoTypes::BOX_2D());
				variant.SetReturnType(LogicalType::UINTEGER);

				variant.SetFunction(ExecuteBox<double>);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2DF());
				variant.AddParameter("bounds", GeoTypes::BOX_2DF());
				variant.SetReturnType(LogicalType::UINTEGER);

				variant.SetFunction(ExecuteBox<float>);
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);
		});
	}
};

//======================================================================================================================
// ST_InteriorRingN
//======================================================================================================================

struct ST_InteriorRingN {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, int64_t, string_t>(
		    args.data[0], args.data[1], result, args.size(),
		    [&](const string_t &blob, const int64_t &n) -> optional<string_t> {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    // ---- validate geometry ----
			    if (geom.get_type() != sgl::geometry_type::POLYGON) {
				    return nullopt;
			    }

			    if (geom.is_empty()) {
				    // empty polygon → NULL because ring index must be always out of bounds then
				    return nullopt;
			    }

			    if (n < 1) {
				    // invalid index → NULL
				    return nullopt;
			    }

			    const idx_t num_parts = geom.get_part_count(); // includes shell
			    // parts: 0 = exterior, 1..n = interior rings
			    const idx_t num_interior = num_parts > 0 ? num_parts - 1 : 0;

			    if (static_cast<idx_t>(n) > num_interior) {
				    // ring doesn't exist → NULL
				    return nullopt;
			    }

			    // interior ring N = part N (because part 0 = shell)
			    const auto *ring = geom.get_first_part();
			    for (idx_t i = 0; i < (idx_t)n; i++) {
				    ring = ring->get_next();
			    }

			    D_ASSERT(ring != nullptr);
			    return lstate.Serialize(result, *ring);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 2);
		auto &poly_vec = args.data[0];
		auto &n_vec = args.data[1];

		// same layout as ST_ExteriorRing::ExecutePolygon
		auto poly_entries = FlatVector::GetData<list_entry_t>(poly_vec);
		auto &ring_vec = ListVector::GetChildMutable(poly_vec);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &vertex_vec = ListVector::GetChildMutable(ring_vec);
		auto &vertex_vec_children = StructVector::GetEntries(vertex_vec);
		auto poly_x_data = FlatVector::GetDataMutable<double>(vertex_vec_children[0]);
		auto poly_y_data = FlatVector::GetDataMutable<double>(vertex_vec_children[1]);

		auto count = args.size();
		UnifiedVectorFormat poly_format;
		poly_vec.ToUnifiedFormat(poly_format);

		// We'll need to build the result list length: sum of selected interior ring lengths
		idx_t total_vertex_count = 0;

		// To inspect n per-row, extract unified format for n (it might be constant)
		UnifiedVectorFormat n_format;
		n_vec.ToUnifiedFormat(n_format);
		auto n_data = FlatVector::GetDataMutable<int64_t>(n_vec);

		for (idx_t i = 0; i < count; i++) {
			auto row_idx = poly_format.sel->get_index(i);
			if (!poly_format.validity.RowIsValid(row_idx)) {
				continue;
			}
			auto poly = poly_entries[row_idx];
			if (poly.length == 0) {
				// empty polygon -> nothing to add
				continue;
			}

			// read requested n for this row
			int64_t nr = 0;
			// handle constant / flat
			if (n_format.validity.RowIsValid(n_format.sel->get_index(i))) {
				nr = n_data[n_format.sel->get_index(i)];
			} else {
				// n is null -> will produce NULL result later
				continue;
			}

			// polygon has poly.length rings: first is exterior, rest are interior
			const idx_t ring_count = poly.length; // >=1 normally
			const idx_t interior_count = (ring_count > 0 ? ring_count - 1 : 0);

			if (nr < 1 || nr > static_cast<int64_t>(interior_count)) {
				// out of range or invalid -> no vertices added (result will be NULL or empty)
				continue;
			}

			// interior ring index in ring_entries: poly.offset + nr (since 1 -> first interior at offset+1)
			auto &ring = ring_entries[poly.offset + static_cast<idx_t>(nr)];
			total_vertex_count += ring.length;
		}

		// Allocate result
		auto &line_vec = result;
		ListVector::Reserve(line_vec, total_vertex_count);
		ListVector::SetListSize(line_vec, total_vertex_count);

		auto line_entries = FlatVector::GetDataMutable<list_entry_t>(line_vec);
		auto &line_coord_vec = StructVector::GetEntries(ListVector::GetChildMutable(line_vec));
		auto line_data_x = FlatVector::GetDataMutable<double>(line_coord_vec[0]);
		auto line_data_y = FlatVector::GetDataMutable<double>(line_coord_vec[1]);

		// Fill results
		idx_t line_data_offset = 0;
		for (idx_t i = 0; i < count; i++) {
			auto row_idx = poly_format.sel->get_index(i);
			if (!poly_format.validity.RowIsValid(row_idx)) {
				FlatVector::SetNull(line_vec, i, true);
				continue;
			}

			auto poly = poly_entries[row_idx];

			// read requested n for this row
			const auto n_idx = n_format.sel->get_index(i);
			if (!n_format.validity.RowIsValid(n_idx)) {
				FlatVector::SetNull(line_vec, i, true);
				continue;
			}

			const auto nr = n_data[n_idx];

			const idx_t ring_count = poly.length;
			const idx_t interior_count = (ring_count > 0 ? ring_count - 1 : 0);

			if (nr < 1 || nr > static_cast<int64_t>(interior_count)) {
				// out of range -> NULL result
				FlatVector::SetNull(line_vec, i, true);
				continue;
			}

			auto &ring = ring_entries[poly.offset + static_cast<idx_t>(nr)]; // offset + 1..N -> interior rings
			auto &line_entry = line_entries[i];
			line_entry.offset = line_data_offset;
			line_entry.length = ring.length;

			for (idx_t coord_idx = 0; coord_idx < ring.length; coord_idx++) {
				line_data_x[line_entry.offset + coord_idx] = poly_x_data[ring.offset + coord_idx];
				line_data_y[line_entry.offset + coord_idx] = poly_y_data[ring.offset + coord_idx];
			}

			line_data_offset += ring.length;
		}

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = "Returns the N-th interior ring (hole) of a POLYGON as a LINESTRING. Indexing "
	                                    "is 1-based  (n = 1 returns the first interior ring). "
	                                    "Returns NULL if the polygon is empty or has fewer than N interior rings.";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_AsText(ST_InteriorRingN(ST_GeomFromText('POLYGON((0 0,10 0,10 10,0 10,0 0),(2 2,4 2,4 4,2 4,2 2))'), 1));
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_InteriorRingN", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("n", LogicalType::BIGINT);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.AddParameter("n", LogicalType::BIGINT);
				variant.SetReturnType(GeoTypes::LINESTRING_2D());

				variant.SetFunction(ExecutePolygon);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_InterpolatePoint
//======================================================================================================================

struct ST_InterpolatePoint {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, double>(
		    args.data[0], args.data[1], result, args.size(),
		    [&](const string_t &line_blob, const string_t &point_blob) {
			    sgl::geometry line_geom;
			    sgl::geometry point_geom;

			    lstate.Deserialize(line_blob, line_geom);
			    lstate.Deserialize(point_blob, point_geom);

			    if (line_geom.get_type() != sgl::geometry_type::LINESTRING) {
				    throw InvalidInputException("ST_InterpolatePoint: input is not a LINESTRING");
			    }
			    if (point_geom.get_type() != sgl::geometry_type::POINT) {
				    throw InvalidInputException("ST_InterpolatePoint: input is not a POINT");
			    }
			    if (!line_geom.has_m()) {
				    throw InvalidInputException("ST_InterpolatePoint: input LINESTRING must have M values");
			    }
			    if (line_geom.is_empty()) {
				    throw InvalidInputException("ST_InterpolatePoint: input LINESTRING must not be empty");
			    }
			    if (point_geom.is_empty()) {
				    throw InvalidInputException("ST_InterpolatePoint: input POINT must not be empty");
			    }

			    double result = 0.0;
			    if (!sgl::linestring::interpolate_point(line_geom, point_geom, result)) {
				    throw InvalidInputException("ST_InterpolatePoint: unknown error");
			    }
			    return result;
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Computes the closest point on a LINESTRING to a given POINT and returns the interpolated M value of that point.

		First argument must be a linestring and must have a M dimension. The second argument must be a point. 
		Neither argument can be empty.
	)";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_InterpolatePoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", LogicalType::GEOMETRY());
				variant.AddParameter("point", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "referencing");
		});
	}
};

//======================================================================================================================
// ST_Intersects
//======================================================================================================================

struct ST_Intersects {

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {
		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
		using BOOL_TYPE = PrimitiveType<bool>;

		GenericExecutor::ExecuteBinary<BOX_TYPE, BOX_TYPE, BOOL_TYPE>(
		    args.data[0], args.data[1], result, args.size(), [&](BOX_TYPE &left, BOX_TYPE &right) {
			    return !(left.a_val > right.c_val || left.c_val < right.a_val || left.b_val > right.d_val ||
			             left.d_val < right.b_val);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// TODO: Add docs
	static constexpr auto DESCRIPTION = "";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Intersects", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box1", GeoTypes::BOX_2D());
				variant.AddParameter("box2", GeoTypes::BOX_2D());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetFunction(ExecuteBox);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

//======================================================================================================================
// ST_Intersects_Extent
//======================================================================================================================

struct ST_Intersects_Extent {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, bool>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &lhs_blob, const string_t &rhs_blob) {
			    // TODO: In the future we should store if the geom is
			    // empty/vertex count in the blob
			    sgl::geometry lhs_geom;
			    lstate.Deserialize(lhs_blob, lhs_geom);

			    sgl::extent_xy lhs_ext = sgl::extent_xy::smallest();
			    if (sgl::ops::get_total_extent_xy(lhs_geom, lhs_ext) == 0) {
				    return false;
			    }

			    sgl::geometry rhs_geom;
			    lstate.Deserialize(rhs_blob, rhs_geom);

			    sgl::extent_xy rhs_ext = sgl::extent_xy::smallest();
			    if (sgl::ops::get_total_extent_xy(rhs_geom, rhs_ext) == 0) {
				    return false;
			    }

			    return lhs_ext.intersects(rhs_ext);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
	    Returns true if the extent of two geometries intersects
	)";

	// TODO: Add examples
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Intersects_Extent", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", LogicalType::GEOMETRY());
				variant.AddParameter("geom2", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetBind(GeoTypes::PropagateCRS);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

//======================================================================================================================
// ST_IsClosed
//======================================================================================================================

struct ST_IsClosed {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			switch (geom.get_type()) {
			case sgl::geometry_type::LINESTRING:
				return sgl::linestring::is_closed(geom);
			case sgl::geometry_type::MULTI_LINESTRING:
				return sgl::multi_linestring::is_closed(geom);
			default:
				// TODO: We should support more than just LINESTRING and MULTILINESTRING (like PostGIS does)
				throw InvalidInputException("ST_IsClosed only accepts LINESTRING and MULTILINESTRING geometries");
			}
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = "Check if a geometry is 'closed'";
	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_IsClosed", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_IsEmpty
//======================================================================================================================

struct ST_IsEmpty {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			return sgl::ops::get_total_vertex_count(geom) == 0;
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLinestring(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<list_entry_t, bool>(args.data[0], result, args.size(),
		                                           [&](const list_entry_t &line) { return line.length == 0; });
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<list_entry_t, bool>(args.data[0], result, args.size(),
		                                           [&](const list_entry_t &poly) { return poly.length == 0; });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns true if the geometry is "empty".
	)";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_IsEmpty", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetFunction(ExecuteLinestring);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetFunction(ExecutePolygon);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Length
//======================================================================================================================

struct ST_Length {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			return sgl::ops::get_length(geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLinestring(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &line_vec = args.data[0];
		auto count = args.size();

		auto &coord_vec = ListVector::GetChildMutable(line_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetDataMutable<double>(coord_vec_children[0]);
		auto y_data = FlatVector::GetDataMutable<double>(coord_vec_children[1]);

		UnaryExecutor::Execute<list_entry_t, double>(line_vec, result, count, [&](const list_entry_t &line) {
			auto offset = line.offset;
			auto length = line.length;
			double sum = 0;
			// Loop over the segments
			for (idx_t j = offset; j < offset + length - 1; j++) {
				auto x1 = x_data[j];
				auto y1 = y_data[j];
				auto x2 = x_data[j + 1];
				auto y2 = y_data[j + 1];
				sum += std::sqrt(std::pow(x1 - x2, 2) + std::pow(y1 - y2, 2));
			}
			return sum;
		});

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the length of the input line geometry
	)";

	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Length", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetFunction(ExecuteLinestring);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_MakeEnvelope
//======================================================================================================================

struct ST_MakeEnvelope {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto &min_x_vec = args.data[0];
		auto &min_y_vec = args.data[1];
		auto &max_x_vec = args.data[2];
		auto &max_y_vec = args.data[3];

		using DOUBLE_TYPE = PrimitiveType<double>;
		using STRING_TYPE = PrimitiveType<string_t>;

		GenericExecutor::ExecuteQuaternary<DOUBLE_TYPE, DOUBLE_TYPE, DOUBLE_TYPE, DOUBLE_TYPE, STRING_TYPE>(
		    min_x_vec, min_y_vec, max_x_vec, max_y_vec, result, args.size(),
		    [&](const DOUBLE_TYPE vmin_x, const DOUBLE_TYPE vmin_y, const DOUBLE_TYPE vmax_x,
		        const DOUBLE_TYPE vmax_y) {
			    const auto min_x = vmin_x.val;
			    const auto min_y = vmin_y.val;
			    const auto max_x = vmax_x.val;
			    const auto max_y = vmax_y.val;

			    // This is pretty cool, we dont even need to allocate anything
			    const double buffer[10] = {min_x, min_y, min_x, max_y, max_x, max_y, max_x, min_y, min_x, min_y};

			    sgl::geometry ring(sgl::geometry_type::LINESTRING, false, false);
			    ring.set_vertex_array(buffer, 5);

			    sgl::geometry poly(sgl::geometry_type::POLYGON, false, false);
			    poly.append_part(&ring);

			    return lstate.Serialize(result, poly);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Create a rectangular polygon from min/max coordinates
	)";
	static constexpr auto EXAMPLE = ""; // todo: example

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_MakeEnvelope", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("min_x", LogicalType::DOUBLE);
				variant.AddParameter("min_y", LogicalType::DOUBLE);
				variant.AddParameter("max_x", LogicalType::DOUBLE);
				variant.AddParameter("max_y", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_MakeLine
//======================================================================================================================

struct ST_MakeLine {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (LIST)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteList(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto &child_vec = ListVector::GetChild(args.data[0]);

		UnifiedVectorFormat format;
		child_vec.ToUnifiedFormat(format);

		UnaryExecutor::Execute<list_entry_t, string_t>(
		    args.data[0], result, args.size(), [&](const list_entry_t &entry) {
			    const auto offset = entry.offset;
			    const auto length = entry.length;

			    uint32_t line_length = 0;
			    // First pass, filter types, count non-null entries

			    for (idx_t i = offset; i < offset + length; i++) {
				    const auto mapped_idx = format.sel->get_index(i);
				    if (!format.validity.RowIsValid(mapped_idx)) {
					    continue;
				    }
				    auto &blob = UnifiedVectorFormat::GetData<string_t>(format)[mapped_idx];

				    // TODO: Peek without deserializing
				    sgl::geometry geom;
				    lstate.Deserialize(blob, geom);

				    if (geom.get_type() != sgl::geometry_type::POINT) {
					    throw InvalidInputException("ST_MakeLine only accepts POINT geometries");
				    }

				    // TODO: Support Z and M
				    if (geom.has_z() || geom.has_m()) {
					    throw InvalidInputException(
					        "ST_MakeLine from list does not accept POINT geometries with Z or M values");
				    }

				    if (geom.is_empty()) {
					    continue;
				    }

				    line_length++;
			    }

			    if (line_length == 0) {
				    // Empty line
				    sgl::geometry empty(sgl::geometry_type::LINESTRING, false, false);
				    return lstate.Serialize(result, empty);
			    }

			    if (line_length == 1) {
				    throw InvalidInputException("ST_MakeLine requires zero or two or more POINT geometries");
			    }

			    const auto line_data = lstate.GetArena().AllocateAligned(line_length * 2 * sizeof(double));

			    // Second pass, copy over the vertex data
			    uint32_t vertex_idx = 0;
			    for (idx_t i = offset; i < offset + length; i++) {
				    D_ASSERT(vertex_idx < line_length);

				    const auto mapped_idx = format.sel->get_index(i);
				    if (!format.validity.RowIsValid(mapped_idx)) {
					    continue;
				    }
				    auto &blob = UnifiedVectorFormat::GetData<string_t>(format)[mapped_idx];

				    sgl::geometry point;
				    lstate.Deserialize(blob, point);

				    const auto point_data = point.get_vertex_array();

				    memcpy(line_data + vertex_idx * 2 * sizeof(double), point_data, 2 * sizeof(double));
				    vertex_idx++;
			    }

			    D_ASSERT(vertex_idx == line_length);

			    sgl::geometry line(sgl::geometry_type::LINESTRING, false, false);
			    line.set_vertex_array(line_data, line_length);

			    return lstate.Serialize(result, line);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY, GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteBinary(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &l_blob, const string_t &r_blob) {
			    sgl::geometry l_geom;
			    sgl::geometry r_geom;

			    lstate.Deserialize(l_blob, l_geom);
			    lstate.Deserialize(r_blob, r_geom);

			    if (l_geom.get_type() != sgl::geometry_type::POINT || r_geom.get_type() != sgl::geometry_type::POINT) {
				    throw InvalidInputException("ST_MakeLine only accepts POINT geometries");
			    }

			    if (l_geom.is_empty() && r_geom.is_empty()) {
				    sgl::geometry empty(sgl::geometry_type::LINESTRING, false, false);
				    return lstate.Serialize(result, empty);
			    }

			    if (l_geom.is_empty() || r_geom.is_empty()) {
				    throw InvalidInputException("ST_MakeLine requires zero or two or more POINT geometries");
			    }

			    const auto has_z = l_geom.has_z() || r_geom.has_z();
			    const auto has_m = l_geom.has_m() || r_geom.has_m();

			    sgl::geometry linestring(sgl::geometry_type::LINESTRING, has_z, has_m);

			    // Create a buffer large enough to store two vertices
			    double buffer[8] = {0};

			    const auto v1 = l_geom.get_vertex_xyzm(0);
			    const auto v2 = r_geom.get_vertex_xyzm(0);

			    // TODO: this is a bit ugly, add proper append method to sgl instead
			    idx_t idx = 0;
			    buffer[idx++] = v1.x;
			    buffer[idx++] = v1.y;
			    if (has_z) {
				    buffer[idx++] = l_geom.has_z() ? v1.z : 0;
			    }
			    if (has_m) {
				    buffer[idx++] = l_geom.has_m() ? l_geom.has_z() ? v1.m : v1.z : 0;
			    }
			    buffer[idx++] = v2.x;
			    buffer[idx++] = v2.y;
			    if (has_z) {
				    buffer[idx++] = r_geom.has_z() ? v2.z : 0;
			    }
			    if (has_m) {
				    buffer[idx++] = r_geom.has_m() ? r_geom.has_z() ? v2.m : v2.z : 0;
			    }

			    linestring.set_vertex_array(reinterpret_cast<const char *>(buffer), 2);

			    return lstate.Serialize(result, linestring);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION_LIST = R"(
		Create a LINESTRING from a list of POINT geometries
	)";
	static constexpr auto EXAMPLE_LIST = R"(
		SELECT ST_MakeLine([ST_Point(0, 0), ST_Point(1, 1)]);
		----
		LINESTRING(0 0, 1 1)
	)";

	static constexpr auto DESCRIPTION_BINARY = R"(
		Create a LINESTRING from two POINT geometries
	)";
	static constexpr auto EXAMPLE_BINARY = R"(
		SELECT ST_MakeLine(ST_Point(0, 0), ST_Point(1, 1));
		----
		LINESTRING(0 0, 1 1)
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_MakeLine", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geoms", LogicalType::LIST(LogicalType::GEOMETRY()));
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteList);
				variant.CanThrowErrors();

				variant.SetDescription(DESCRIPTION_LIST);
				variant.SetExample(EXAMPLE_LIST);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("start", LogicalType::GEOMETRY());
				variant.AddParameter("end", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteBinary);
				variant.CanThrowErrors();

				variant.SetDescription(DESCRIPTION_BINARY);
				variant.SetExample(EXAMPLE_BINARY);
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_MakePolygon
//======================================================================================================================

struct ST_MakePolygon {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (LINESTRING)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteFromShell(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry line;
			lstate.Deserialize(blob, line);

			if (line.get_type() != sgl::geometry_type::LINESTRING) {
				throw InvalidInputException("ST_MakePolygon only accepts LINESTRING geometries");
			}

			if (line.get_vertex_count() < 4) {
				throw InvalidInputException("ST_MakePolygon shell requires at least 4 vertices");
			}

			if (!sgl::linestring::is_closed(line)) {
				throw std::runtime_error("ST_MakePolygon shell must be closed (first and last vertex must be equal)");
			}

			sgl::geometry polygon(sgl::geometry_type::POLYGON, line.has_z(), line.has_m());
			polygon.append_part(&line);

			return lstate.Serialize(result, polygon);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (LINESTRING, LIST)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteFromRings(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto &child_vec = ListVector::GetChild(args.data[1]);

		UnifiedVectorFormat child_format;
		child_vec.ToUnifiedFormat(child_format);

		BinaryExecutor::Execute<string_t, list_entry_t, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &blob, const list_entry_t &hole_list) {
			    // First, setup shell

			    sgl::geometry shell;
			    lstate.Deserialize(blob, shell);

			    if (shell.get_type() != sgl::geometry_type::LINESTRING) {
				    throw InvalidInputException("ST_MakePolygon only accepts LINESTRING geometries");
			    }
			    // TODO: Support Z and M
			    if (shell.has_z() || shell.has_m()) {
				    throw InvalidInputException("ST_MakePolygon from list does not support Z or M values");
			    }
			    if (shell.get_vertex_count() < 4) {
				    throw InvalidInputException("ST_MakePolygon shell requires at least 4 vertices");
			    }
			    if (!sgl::linestring::is_closed(shell)) {
				    throw InvalidInputException(
				        "ST_MakePolygon shell must be closed (first and last vertex must be equal)");
			    }

			    // Make a polygon!
			    sgl::geometry polygon(sgl::geometry_type::POLYGON, false, false);

			    // Append the shell
			    polygon.append_part(&shell);

			    // Now setup the rings
			    const auto holes_offset = hole_list.offset;
			    const auto holes_length = hole_list.length;

			    for (idx_t hole_idx = 0; hole_idx < holes_length; hole_idx++) {
				    const auto mapped_idx = child_format.sel->get_index(holes_offset + hole_idx);
				    if (!child_format.validity.RowIsValid(mapped_idx)) {
					    continue;
				    }

				    const auto &hole_blob = UnifiedVectorFormat::GetData<string_t>(child_format)[mapped_idx];

				    // Allocate a new hole and deserialize into the memory
				    auto hole = lstate.DeserializeToHeap(hole_blob);

				    if (hole->get_type() != sgl::geometry_type::LINESTRING) {
					    throw InvalidInputException("ST_MakePolygon hole #%lu is not a LINESTRING geometry",
					                                hole_idx + 1);
				    }
				    if (hole->has_z() || hole->has_m()) {
					    throw InvalidInputException("ST_MakePolygon hole #%lu has Z or M values", hole_idx + 1);
				    }
				    if (hole->get_vertex_count() < 4) {
					    throw InvalidInputException("ST_MakePolygon hole #%lu requires at least 4 vertices",
					                                hole_idx + 1);
				    }
				    if (!sgl::linestring::is_closed(*hole)) {
					    throw InvalidInputException(
					        "ST_MakePolygon hole #%lu must be closed (first and last vertex must be equal)",
					        hole_idx + 1);
				    }

				    // Add the hole to the polygon
				    polygon.append_part(hole);
			    }

			    // Now serialize the polygon
			    return lstate.Serialize(result, polygon);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_MakePolygon", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("shell", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteFromShell);
				variant.CanThrowErrors();

				// TODO: Set example & docs
				variant.SetDescription("Create a POLYGON from a LINESTRING shell");
				variant.SetExample("SELECT ST_MakePolygon(ST_LineString([ST_Point(0, 0), ST_Point(1, 0), ST_Point(1, "
				                   "1), ST_Point(0, 0)]));");
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("shell", LogicalType::GEOMETRY());
				variant.AddParameter("holes", LogicalType::LIST(LogicalType::GEOMETRY()));
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteFromRings);
				variant.CanThrowErrors();

				// TODO: Set example & docs
				variant.SetDescription("Create a POLYGON from a LINESTRING shell and a list of LINESTRING holes");
				variant.SetExample("SELECT ST_MakePolygon(ST_LineString([ST_Point(0, 0), ST_Point(1, 0), ST_Point(1, "
				                   "1), ST_Point(0, 0)]), [ST_LineString([ST_Point(0.25, 0.25), ST_Point(0.75, 0.25), "
				                   "ST_Point(0.75, 0.75), ST_Point(0.25, 0.25)])]);");
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_MakeBox2D
//======================================================================================================================

struct ST_MakeBox2D {
	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY, GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteBinary(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto &bbox_vec = StructVector::GetEntries(result);
		const auto min_x_data = FlatVector::GetDataMutable<double>(bbox_vec[0]);
		const auto min_y_data = FlatVector::GetDataMutable<double>(bbox_vec[1]);
		const auto max_x_data = FlatVector::GetDataMutable<double>(bbox_vec[2]);
		const auto max_y_data = FlatVector::GetDataMutable<double>(bbox_vec[3]);

		UnifiedVectorFormat input_vdata1;
		UnifiedVectorFormat input_vdata2;
		args.data[0].ToUnifiedFormat(input_vdata1);
		args.data[1].ToUnifiedFormat(input_vdata2);
		const auto input_data1 = UnifiedVectorFormat::GetData<string_t>(input_vdata1);
		const auto input_data2 = UnifiedVectorFormat::GetData<string_t>(input_vdata2);

		const auto count = args.size();

		for (idx_t out_idx = 0; out_idx < count; out_idx++) {
			const auto row_idx1 = input_vdata1.sel->get_index(out_idx);
			const auto row_idx2 = input_vdata2.sel->get_index(out_idx);
			if (!input_vdata1.validity.RowIsValid(row_idx1) || !input_vdata2.validity.RowIsValid(row_idx2)) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			const auto &blob1 = input_data1[row_idx1];
			const auto &blob2 = input_data2[row_idx2];
			sgl::geometry geom1;
			sgl::geometry geom2;
			lstate.Deserialize(blob1, geom1);
			lstate.Deserialize(blob2, geom2);

			if (geom1.get_type() != sgl::geometry_type::POINT || geom2.get_type() != sgl::geometry_type::POINT) {
				throw InvalidInputException("ST_MakeBox2D only accepts POINT geometries");
			}

			if (geom1.is_empty() || geom2.is_empty()) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			const auto v1 = geom1.get_vertex_xy(0);
			const auto v2 = geom2.get_vertex_xy(0);

			min_x_data[out_idx] = std::min(v1.x, v2.x);
			min_y_data[out_idx] = std::min(v1.y, v2.y);
			max_x_data[out_idx] = std::max(v1.x, v2.x);
			max_y_data[out_idx] = std::max(v1.y, v2.y);
		}

		if (args.AllConstant()) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION_BINARY = R"(
		Create a BOX2D from two POINT geometries
	)";
	static constexpr auto EXAMPLE_BINARY = R"(
		SELECT ST_MakeBox2D(ST_Point(0, 0), ST_Point(1, 1));
		----
		BOX(0 0, 1 1)
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_MakeBox2D", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point1", LogicalType::GEOMETRY());
				variant.AddParameter("point2", LogicalType::GEOMETRY());
				variant.SetReturnType(GeoTypes::BOX_2D());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteBinary);
				variant.CanThrowErrors();

				variant.SetDescription(DESCRIPTION_BINARY);
				variant.SetExample(EXAMPLE_BINARY);
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_Multi
//======================================================================================================================

struct ST_Multi {

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			const auto has_z = geom.has_z();
			const auto has_m = geom.has_m();

			switch (geom.get_type()) {
			case sgl::geometry_type::POINT: {
				sgl::geometry mpoint(sgl::geometry_type::MULTI_POINT, has_z, has_m);
				mpoint.append_part(&geom);
				return lstate.Serialize(result, mpoint);
			}
			case sgl::geometry_type::LINESTRING: {
				sgl::geometry mline(sgl::geometry_type::MULTI_LINESTRING, has_z, has_m);
				mline.append_part(&geom);
				return lstate.Serialize(result, mline);
			}
			case sgl::geometry_type::POLYGON: {
				sgl::geometry mpoly(sgl::geometry_type::MULTI_POLYGON, has_z, has_m);
				mpoly.append_part(&geom);
				return lstate.Serialize(result, mpoly);
			}
			default:
				// Just return the original geometry
				return blob;
			}
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Turns a single geometry into a multi geometry.

		If the geometry is already a multi geometry, it is returned as is.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_Multi(ST_GeomFromText('POINT(1 2)'));
		----
		MULTIPOINT (1 2)

		SELECT ST_Multi(ST_GeomFromText('LINESTRING(1 1, 2 2)'));
		----
		MULTILINESTRING ((1 1, 2 2))

		SELECT ST_Multi(ST_GeomFromText('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'));
		----
		MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)))
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Multi", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_NGeometries / ST_NumGeometries
//======================================================================================================================

struct ST_NGeometries {

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			switch (geom.get_type()) {
			case sgl::geometry_type::POINT:
			case sgl::geometry_type::LINESTRING:
			case sgl::geometry_type::POLYGON:
				return geom.is_empty() ? 0 : 1;
			case sgl::geometry_type::MULTI_POINT:
			case sgl::geometry_type::MULTI_LINESTRING:
			case sgl::geometry_type::MULTI_POLYGON:
			case sgl::geometry_type::GEOMETRY_COLLECTION:
				return static_cast<int32_t>(geom.get_part_count());
			default:
				D_ASSERT(false);
				return 0;
			}
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the number of component geometries in a collection geometry.
	    If the input geometry is not a collection, this function returns 0 or 1 depending on if the geometry is empty or not.
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = R"(

	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// TODO: Maybe make a macro for the aliases
		for (auto &alias : {"ST_NumGeometries", "ST_NGeometries"}) {
			FunctionBuilder::RegisterScalar(loader, alias, [](ScalarFunctionBuilder &func) {
				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("geom", LogicalType::GEOMETRY());
					variant.SetReturnType(LogicalType::INTEGER);

					variant.SetInit(LocalState::Init);
					variant.SetFunction(Execute);
				});

				func.SetDescription(DESCRIPTION);
				func.SetExample(EXAMPLE);

				func.SetTag("ext", "spatial");
				func.SetTag("category", "property");
			});
		}
	}
};

//======================================================================================================================
// ST_NumInteriorRings / ST_NInteriorRings
//======================================================================================================================

struct ST_NInteriorRings {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, int32_t>(
		    args.data[0], result, args.size(), [&](const string_t &blob) -> optional<int32_t> {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::POLYGON) {
				    return nullopt;
			    }

			    const auto n_rings = static_cast<int32_t>(geom.get_part_count());
			    return n_rings == 0 ? 0 : n_rings - 1;
		    });
	}

	//------------------------------------------------------------------------------
	// Execute (POLYGON_2D)
	//------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<list_entry_t, int32_t>(
		    args.data[0], result, args.size(), [&](const list_entry_t &polygon) {
			    const auto rings = polygon.length;
			    return rings == 0 ? rings : static_cast<int32_t>(polygon.length) - 1; // -1 for the exterior ring
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the number of interior rings of a polygon
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// TODO: maybe make a macro for the aliases
		for (auto &alias : {"ST_NumInteriorRings", "ST_NInteriorRings"}) {
			FunctionBuilder::RegisterScalar(loader, alias, [](ScalarFunctionBuilder &func) {
				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("geom", LogicalType::GEOMETRY());
					variant.SetReturnType(LogicalType::INTEGER);

					variant.SetInit(LocalState::Init);
					variant.SetFunction(Execute);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
					variant.SetReturnType(LogicalType::INTEGER);

					variant.SetFunction(ExecutePolygon);
				});

				func.SetDescription(DESCRIPTION);
				func.SetExample(EXAMPLE);

				func.SetTag("ext", "spatial");
				func.SetTag("category", "property");
			});
		}
	}
};

//======================================================================================================================
// ST_NPoints
//======================================================================================================================

struct ST_NPoints {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (POINT_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using COUNT_TYPE = PrimitiveType<idx_t>;

		GenericExecutor::ExecuteUnary<POINT_TYPE, COUNT_TYPE>(args.data[0], result, args.size(),
		                                                      [](POINT_TYPE) { return 1; });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (LINESTRING_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		UnaryExecutor::Execute<list_entry_t, idx_t>(input, result, args.size(),
		                                            [](list_entry_t input) { return input.length; });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (POLYGON_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &input = args.data[0];
		auto count = args.size();
		auto &ring_vec = ListVector::GetChildMutable(input);
		auto ring_entries = FlatVector::GetDataMutable<list_entry_t>(ring_vec);

		UnaryExecutor::Execute<list_entry_t, idx_t>(input, result, count, [&](list_entry_t polygon) {
			auto polygon_offset = polygon.offset;
			auto polygon_length = polygon.length;
			idx_t npoints = 0;
			for (idx_t ring_idx = polygon_offset; ring_idx < polygon_offset + polygon_length; ring_idx++) {
				auto ring = ring_entries[ring_idx];
				npoints += ring.length;
			}
			return npoints;
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (BOX_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {

		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
		using COUNT_TYPE = PrimitiveType<idx_t>;

		GenericExecutor::ExecuteUnary<BOX_TYPE, COUNT_TYPE>(args.data[0], result, args.size(),
		                                                    [](BOX_TYPE) { return 4; });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, uint32_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);
			return sgl::ops::get_total_vertex_count(geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the number of vertices within a geometry
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {

		for (const auto &alias : {"ST_NumPoints", "ST_NPoints"}) {
			FunctionBuilder::RegisterScalar(loader, alias, [](ScalarFunctionBuilder &func) {
				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("geom", LogicalType::GEOMETRY());
					variant.SetReturnType(LogicalType::UINTEGER);

					variant.SetInit(LocalState::Init);
					variant.SetFunction(ExecuteGeometry);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("point", GeoTypes::POINT_2D());
					variant.SetReturnType(LogicalType::UBIGINT);
					variant.SetFunction(ExecutePoint);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
					variant.SetReturnType(LogicalType::UBIGINT);
					variant.SetFunction(ExecuteLineString);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
					variant.SetReturnType(LogicalType::UBIGINT);
					variant.SetFunction(ExecutePolygon);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("box", GeoTypes::BOX_2D());
					variant.SetReturnType(LogicalType::UBIGINT);
					variant.SetFunction(ExecuteBox);
				});

				func.SetDescription(DESCRIPTION);
				func.SetExample(EXAMPLE);

				func.SetTag("ext", "spatial");
				func.SetTag("category", "property");
			});
		}
	}
};

//======================================================================================================================
// ST_Perimeter
//======================================================================================================================

struct ST_Perimeter {

	//------------------------------------------------------------------------------
	// Execute (POLYGON_2D)
	//------------------------------------------------------------------------------
	static void ExecutePolygon(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &input = args.data[0];
		auto count = args.size();

		auto &ring_vec = ListVector::GetChildMutable(input);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		auto &coord_vec = ListVector::GetChildMutable(ring_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		auto x_data = FlatVector::GetDataMutable<double>(coord_vec_children[0]);
		auto y_data = FlatVector::GetDataMutable<double>(coord_vec_children[1]);

		UnaryExecutor::Execute<list_entry_t, double>(input, result, count, [&](list_entry_t polygon) {
			auto polygon_offset = polygon.offset;
			auto polygon_length = polygon.length;
			double perimeter = 0;
			for (idx_t ring_idx = polygon_offset; ring_idx < polygon_offset + polygon_length; ring_idx++) {
				auto ring = ring_entries[ring_idx];
				auto ring_offset = ring.offset;
				auto ring_length = ring.length;

				for (idx_t coord_idx = ring_offset; coord_idx < ring_offset + ring_length - 1; coord_idx++) {
					auto x1 = x_data[coord_idx];
					auto y1 = y_data[coord_idx];
					auto x2 = x_data[coord_idx + 1];
					auto y2 = y_data[coord_idx + 1];
					perimeter += std::sqrt(std::pow(x1 - x2, 2) + std::pow(y1 - y2, 2));
				}
			}
			return perimeter;
		});

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------
	// Execute (BOX_2D)
	//------------------------------------------------------------------------------
	static void ExecuteBox(DataChunk &args, ExpressionState &state, Vector &result) {
		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
		using PERIMETER_TYPE = PrimitiveType<double>;

		GenericExecutor::ExecuteUnary<BOX_TYPE, PERIMETER_TYPE>(args.data[0], result, args.size(), [&](BOX_TYPE &box) {
			auto minx = box.a_val;
			auto miny = box.b_val;
			auto maxx = box.c_val;
			auto maxy = box.d_val;
			return 2 * (maxx - minx + maxy - miny);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [&](const string_t &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);
			return sgl::ops::get_perimeter(geom);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the length of the perimeter of the geometry
	)";

	// TODO: Add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Perimeter", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecutePolygon);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("box", GeoTypes::BOX_2D());
				variant.SetReturnType(LogicalType::DOUBLE);
				variant.SetFunction(ExecuteBox);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Point
//======================================================================================================================

struct ST_Point {

	//------------------------------------------------------------------------------
	// POINT_2D
	//------------------------------------------------------------------------------
	static void ExecutePoint2D(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 2);
		auto count = args.size();

		auto &x = args.data[0];
		auto &y = args.data[1];

		x.Flatten();
		y.Flatten();

		auto &children = StructVector::GetEntries(result);
		auto &x_child = children[0];
		auto &y_child = children[1];

		x_child.Reference(x);
		y_child.Reference(y);

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------
	// POINT_3D
	//------------------------------------------------------------------------------
	static void ExecutePoint3D(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 3);
		auto count = args.size();

		auto &x = args.data[0];
		auto &y = args.data[1];
		auto &z = args.data[2];

		x.Flatten();
		y.Flatten();
		z.Flatten();

		auto &children = StructVector::GetEntries(result);
		auto &x_child = children[0];
		auto &y_child = children[1];
		auto &z_child = children[2];

		x_child.Reference(x);
		y_child.Reference(y);
		z_child.Reference(z);

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------
	// POINT_4D
	//------------------------------------------------------------------------------
	static void ExecutePoint4D(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 4);
		auto count = args.size();

		auto &x = args.data[0];
		auto &y = args.data[1];
		auto &z = args.data[2];
		auto &m = args.data[3];

		x.Flatten();
		y.Flatten();
		z.Flatten();
		m.Flatten();

		auto &children = StructVector::GetEntries(result);
		auto &x_child = children[0];
		auto &y_child = children[1];
		auto &z_child = children[2];
		auto &m_child = children[3];

		x_child.Reference(x);
		y_child.Reference(y);
		z_child.Reference(z);
		m_child.Reference(m);

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<double, double, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                  [&](const double x, const double y) {
			                                                  const double buffer[2] = {x, y};

			                                                  sgl::geometry geometry;
			                                                  geometry.set_type(sgl::geometry_type::POINT);
			                                                  geometry.set_vertex_array(buffer, 1);

			                                                  return lstate.Serialize(result, geometry);
		                                                  });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Creates a GEOMETRY point
	)";

	static constexpr auto DESCRIPTION_MAKE_POINT = R"(
		Creates a GEOMETRY point from an pair of floating point numbers.

		For geodetic coordinate systems, x is typically the longitude value and y is the latitude value.

		Note that ST_Point is equivalent. ST_MakePoint is provided for PostGIS compatibility.
	)";

	// TODO: example
	static constexpr auto EXAMPLE = "";

	// TODO: example
	static constexpr auto EXAMPLE_MAKE_POINT = R"(
	SELECT ST_AsText(ST_MakePoint(143.3, -24.2));
	----
	POINT (143.3 -24.2)
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Point", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::GEOMETRY());

				variant.SetFunction(ExecuteGeometry);
				variant.SetInit(LocalState::Init);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_Point2D", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::POINT_2D());
				variant.SetFunction(ExecutePoint2D);

				variant.SetDescription("Creates a POINT_2D");
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_MakePoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetFunction(ExecutePoint2D);
				variant.SetInit(LocalState::Init);
			});

			func.SetDescription(DESCRIPTION_MAKE_POINT);
			func.SetExample(EXAMPLE_MAKE_POINT);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_Point3D", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.AddParameter("z", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::POINT_3D());
				variant.SetFunction(ExecutePoint3D);

				variant.SetDescription("Creates a POINT_3D");
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_MakePoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.AddParameter("z", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::POINT_3D());
				variant.SetFunction(ExecutePoint3D);

				variant.SetDescription("Creates a POINT_3D.");
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_Point4D", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.AddParameter("z", LogicalType::DOUBLE);
				variant.AddParameter("m", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::POINT_4D());
				variant.SetFunction(ExecutePoint4D);

				variant.SetDescription("Creates a POINT_4D");
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});

		FunctionBuilder::RegisterScalar(loader, "ST_MakePoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("x", LogicalType::DOUBLE);
				variant.AddParameter("y", LogicalType::DOUBLE);
				variant.AddParameter("z", LogicalType::DOUBLE);
				variant.AddParameter("m", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::POINT_4D());
				variant.SetFunction(ExecutePoint4D);

				variant.SetDescription("Creates a POINT_4D.");
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_PointN
//======================================================================================================================

struct ST_PointN {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, int32_t, string_t>(
		    args.data[0], args.data[1], result, args.size(),
		    [&](const string_t &blob, const int32_t index) -> optional<string_t> {
			    // TODO: peek type without deserializing

			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::LINESTRING) {
				    return nullopt;
			    }

			    const auto point_count = geom.get_vertex_count();

			    const auto is_empty = point_count == 0;
			    const auto is_under = index == 0 || index < -static_cast<int64_t>(point_count);
			    const auto is_above = index > static_cast<int64_t>(point_count);

			    if (is_empty || is_under || is_above) {
				    return nullopt;
			    }

			    const auto vertex_elem = index < 0 ? point_count + index : index - 1;
			    const auto vertex_size = geom.get_vertex_width();
			    const auto vertex_data = geom.get_vertex_array();

			    // Reference the existing vertex data
			    sgl::geometry point(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
			    point.set_vertex_array(vertex_data + vertex_elem * vertex_size, 1);

			    return lstate.Serialize(result, point);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (LINESTRING_2D)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &geom_vec = args.data[0];
		auto &index_vec = args.data[1];
		auto count = args.size();
		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(geom_format);
		UnifiedVectorFormat index_format;
		index_vec.ToUnifiedFormat(index_format);

		auto line_vertex_entries = FlatVector::GetData<list_entry_t>(geom_vec);
		auto &line_vertex_vec = ListVector::GetChildMutable(geom_vec);
		auto &line_vertex_vec_children = StructVector::GetEntries(line_vertex_vec);
		auto line_x_data = FlatVector::GetDataMutable<double>(line_vertex_vec_children[0]);
		auto line_y_data = FlatVector::GetDataMutable<double>(line_vertex_vec_children[1]);

		auto &point_vertex_children = StructVector::GetEntries(result);
		auto point_x_data = FlatVector::GetDataMutable<double>(point_vertex_children[0]);
		auto point_y_data = FlatVector::GetDataMutable<double>(point_vertex_children[1]);

		auto index_data = FlatVector::GetDataMutable<int32_t>(index_vec);

		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {

			auto in_row_idx = geom_format.sel->get_index(out_row_idx);
			auto in_idx_idx = index_format.sel->get_index(out_row_idx);
			if (geom_format.validity.RowIsValid(in_row_idx) && index_format.validity.RowIsValid(in_idx_idx)) {
				auto line = line_vertex_entries[in_row_idx];
				auto line_offset = line.offset;
				auto line_length = line.length;
				auto index = index_data[in_idx_idx];

				if (line_length == 0 || index == 0 || index < -static_cast<int64_t>(line_length) ||
				    index > static_cast<int64_t>(line_length)) {
					FlatVector::SetNull(result, out_row_idx, true);
					continue;
				}
				auto actual_index = index < 0 ? line_length + index : index - 1;
				point_x_data[out_row_idx] = line_x_data[line_offset + actual_index];
				point_y_data[out_row_idx] = line_y_data[line_offset + actual_index];
			} else {
				FlatVector::SetNull(result, out_row_idx, true);
			}
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the n'th vertex from the input geometry as a point geometry
	)";

	// TODO: add example
	static constexpr auto EXAMPLe = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_PointN", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.AddParameter("index", LogicalType::INTEGER);
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("linestring", GeoTypes::LINESTRING_2D());
				variant.AddParameter("index", LogicalType::INTEGER);
				variant.SetReturnType(GeoTypes::POINT_2D());
				variant.SetFunction(ExecuteLineString);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLe);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_Points
//======================================================================================================================

struct ST_Points {

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &blob) {
			// Deserialize the geometry
			sgl::geometry geom;
			lstate.Deserialize(blob, geom);

			// Collect all vertices into a multipoint
			sgl::geometry mpoint;
			sgl::ops::collect_vertices(lstate.GetAllocator(), geom, mpoint);

			// Serialize the multipoint
			return lstate.Serialize(result, mpoint);
		});
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Collects all the vertices in the geometry into a MULTIPOINT
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_Points('LINESTRING(1 1, 2 2)'::GEOMETRY);
		----
		MULTIPOINT (1 1, 2 2)

		SELECT ST_Points('MULTIPOLYGON Z EMPTY'::GEOMETRY);
		----
		MULTIPOINT Z EMPTY
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Points", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_QuadKey
//======================================================================================================================

struct ST_QuadKey {

	//------------------------------------------------------------------------------------------------------------------
	// Helpers
	//------------------------------------------------------------------------------------------------------------------
	static void GetQuadKey(double lon, double lat, int32_t level, char *buffer) {

		lat = std::max(-85.05112878, std::min(85.05112878, lat));
		lon = std::max(-180.0, std::min(180.0, lon));

		const auto lat_rad = lat * PI / 180.0;
		const auto x = static_cast<int32_t>((lon + 180.0) / 360.0 * (1 << level));
		const auto y = static_cast<int32_t>((1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / PI) / 2.0 *
		                                    (1 << level));

		for (int i = level; i > 0; --i) {
			char digit = '0';
			const int32_t mask = 1 << (i - 1);
			if ((x & mask) != 0) {
				digit += 1;
			}
			if ((y & mask) != 0) {
				digit += 2;
			}
			buffer[level - i] = digit;
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (GEOMETRY)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		auto &point_in = args.data[0];
		auto &level_in = args.data[1];

		BinaryExecutor::Execute<string_t, int32_t, string_t>(
		    point_in, level_in, result, args.size(), [&](const string_t &blob, const int32_t level) {
			    if (level < 1 || level > 23) {
				    throw InvalidInputException("ST_QuadKey: Level must be between 1 and 23");
			    }

			    sgl::geometry point;
			    lstate.Deserialize(blob, point);

			    if (point.get_type() != sgl::geometry_type::POINT) {
				    throw InvalidInputException("ST_QuadKey: Only POINT geometries are supported");
			    }

			    if (point.is_empty()) {
				    throw InvalidInputException("ST_QuadKey: Empty geometries are not supported");
			    }

			    const auto vertex = point.get_vertex_xy(0);

			    char buffer[64];
			    GetQuadKey(vertex.x, vertex.y, level, buffer);
			    return StringVector::AddString(result, buffer, level);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute (LON/LAT)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLonLat(DataChunk &args, ExpressionState &state, Vector &result) {

		auto &lon_in = args.data[0];
		auto &lat_in = args.data[1];
		auto &lev_in = args.data[2];

		TernaryExecutor::Execute<double, double, int32_t, string_t>(
		    lon_in, lat_in, lev_in, result, [&](const double lon, const double lat, const int32_t level) {
			    if (level < 1 || level > 23) {
				    throw InvalidInputException("ST_QuadKey: Level must be between 1 and 23");
			    }
			    char buffer[64];
			    GetQuadKey(lon, lat, level, buffer);
			    return StringVector::AddString(result, buffer, level);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Compute the [quadkey](https://learn.microsoft.com/en-us/bingmaps/articles/bing-maps-tile-system) for a given lon/lat point at a given level.
		Note that the parameter order is __longitude__, __latitude__.

		`level` has to be between 1 and 23, inclusive.

		The input coordinates will be clamped to the lon/lat bounds of the earth (longitude between -180 and 180, latitude between -85.05112878 and 85.05112878).

		The geometry overload throws an error if the input geometry is not a `POINT`
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT ST_QuadKey(ST_Point(11.08, 49.45), 10);
		----
		1333203202
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_QuadKey", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("longitude", LogicalType::DOUBLE);
				variant.AddParameter("latitude", LogicalType::DOUBLE);
				variant.AddParameter("level", LogicalType::INTEGER);
				variant.SetReturnType(LogicalType::VARCHAR);
				variant.SetFunction(ExecuteLonLat);
				variant.CanThrowErrors();
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("point", LogicalType::GEOMETRY());
				variant.AddParameter("level", LogicalType::INTEGER);
				variant.SetReturnType(LogicalType::VARCHAR);
				variant.SetFunction(ExecuteGeometry);
				variant.SetInit(LocalState::Init);
				variant.CanThrowErrors();
			});

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);
		});
	}
};

//======================================================================================================================
// ST_RemoveRepeatedPoints
//======================================================================================================================

struct ST_RemoveRepeatedPoints {

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto count = args.size();
		UnifiedVectorFormat format;
		input.ToUnifiedFormat(format);

		auto in_line_entries = FlatVector::GetData<list_entry_t>(input);
		auto &in_line_vertex_vec = StructVector::GetEntries(ListVector::GetChildMutable(input));
		auto in_x_data = FlatVector::GetDataMutable<double>(in_line_vertex_vec[0]);
		auto in_y_data = FlatVector::GetDataMutable<double>(in_line_vertex_vec[1]);

		auto out_line_entries = FlatVector::GetDataMutable<list_entry_t>(result);
		auto &out_line_vertex_vec = StructVector::GetEntries(ListVector::GetChildMutable(result));

		idx_t out_offset = 0;
		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {

			auto in_row_idx = format.sel->get_index(out_row_idx);
			if (!format.validity.RowIsValid(in_row_idx)) {
				FlatVector::SetNull(result, out_row_idx, true);
				continue;
			}
			auto in = in_line_entries[in_row_idx];
			auto in_offset = in.offset;
			auto in_length = in.length;

			// Special case: if the line has less than 3 points, we can't remove any points
			if (in_length < 3) {

				ListVector::Reserve(result, out_offset + in_length);
				auto out_x_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[0]);
				auto out_y_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[1]);

				// If the line has less than 3 points, we can't remove any points
				// so we just copy the line
				out_line_entries[out_row_idx] = list_entry_t {out_offset, in_length};
				for (idx_t coord_idx = 0; coord_idx < in_length; coord_idx++) {
					out_x_data[out_offset + coord_idx] = in_x_data[in_offset + coord_idx];
					out_y_data[out_offset + coord_idx] = in_y_data[in_offset + coord_idx];
				}
				out_offset += in_length;
				continue;
			}

			// First pass, calculate how many points we need to keep
			// We always keep the first and last point, so we start at 2
			uint32_t points_to_keep = 0;

			auto last_x = in_x_data[in_offset];
			auto last_y = in_y_data[in_offset];
			points_to_keep++;

			for (idx_t i = 1; i < in_length; i++) {
				auto curr_x = in_x_data[in_offset + i];
				auto curr_y = in_y_data[in_offset + i];

				if (curr_x != last_x || curr_y != last_y) {
					points_to_keep++;
					last_x = curr_x;
					last_y = curr_y;
				}
			}

			// Special case: there is only 1 unique point in the line, so just keep
			// the start and end points
			if (points_to_keep == 1) {
				out_line_entries[out_row_idx] = list_entry_t {out_offset, 2};
				ListVector::Reserve(result, out_offset + 2);
				auto out_x_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[0]);
				auto out_y_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[1]);
				out_x_data[out_offset] = in_x_data[in_offset];
				out_y_data[out_offset] = in_y_data[in_offset];
				out_x_data[out_offset + 1] = in_x_data[in_offset + in_length - 1];
				out_y_data[out_offset + 1] = in_y_data[in_offset + in_length - 1];
				out_offset += 2;
				continue;
			}

			// Set the list entry
			out_line_entries[out_row_idx] = list_entry_t {out_offset, points_to_keep};

			// Second pass, copy the points we need to keep
			ListVector::Reserve(result, out_offset + points_to_keep);
			auto out_x_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[0]);
			auto out_y_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[1]);

			// Copy the first point
			out_x_data[out_offset] = in_x_data[in_offset];
			out_y_data[out_offset] = in_y_data[in_offset];
			out_offset++;

			// Copy the middle points (skip the last one, we'll copy it at the end)
			last_x = in_x_data[in_offset];
			last_y = in_y_data[in_offset];

			for (idx_t i = 1; i < in_length; i++) {
				auto curr_x = in_x_data[in_offset + i];
				auto curr_y = in_y_data[in_offset + i];

				if (curr_x != last_x || curr_y != last_y) {
					out_x_data[out_offset] = curr_x;
					out_y_data[out_offset] = curr_y;
					last_x = curr_x;
					last_y = curr_y;
					out_offset++;
				}
			}
		}
		ListVector::SetListSize(result, out_offset);

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D (With Tolerance)
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineStringWithTolerance(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &input = args.data[0];
		auto &tolerance = args.data[1];
		auto count = args.size();
		UnifiedVectorFormat format;
		input.ToUnifiedFormat(format);

		UnifiedVectorFormat tolerance_format;
		tolerance.ToUnifiedFormat(tolerance_format);

		auto in_line_entries = FlatVector::GetData<list_entry_t>(input);
		auto &in_line_vertex_vec = StructVector::GetEntries(ListVector::GetChildMutable(input));
		auto in_x_data = FlatVector::GetDataMutable<double>(in_line_vertex_vec[0]);
		auto in_y_data = FlatVector::GetDataMutable<double>(in_line_vertex_vec[1]);

		auto out_line_entries = FlatVector::GetDataMutable<list_entry_t>(result);
		auto &out_line_vertex_vec = StructVector::GetEntries(ListVector::GetChildMutable(result));

		idx_t out_offset = 0;

		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {
			auto in_row_idx = format.sel->get_index(out_row_idx);
			auto in_tol_idx = tolerance_format.sel->get_index(out_row_idx);
			if (!format.validity.RowIsValid(in_row_idx) || !tolerance_format.validity.RowIsValid(in_tol_idx)) {
				FlatVector::SetNull(result, out_row_idx, true);
				continue;
			}

			auto in = in_line_entries[in_row_idx];
			auto in_offset = in.offset;
			auto in_length = in.length;

			auto tolerance = Load<double>(tolerance_format.data + in_tol_idx);
			auto tolerance_squared = tolerance * tolerance;

			if (in_length < 3) {

				ListVector::Reserve(result, out_offset + in_length);
				auto out_x_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[0]);
				auto out_y_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[1]);

				// If the line has less than 3 points, we can't remove any points
				// so we just copy the line
				out_line_entries[out_row_idx] = list_entry_t {out_offset, in_length};
				for (idx_t coord_idx = 0; coord_idx < in_length; coord_idx++) {
					out_x_data[out_offset + coord_idx] = in_x_data[in_offset + coord_idx];
					out_y_data[out_offset + coord_idx] = in_y_data[in_offset + coord_idx];
				}
				out_offset += in_length;
				continue;
			}

			// First pass, calculate how many points we need to keep
			uint32_t points_to_keep = 0;

			auto last_x = in_x_data[in_offset];
			auto last_y = in_y_data[in_offset];
			points_to_keep++;

			for (idx_t i = 1; i < in_length; i++) {
				auto curr_x = in_x_data[in_offset + i];
				auto curr_y = in_y_data[in_offset + i];

				auto dist_squared = (curr_x - last_x) * (curr_x - last_x) + (curr_y - last_y) * (curr_y - last_y);

				if (dist_squared > tolerance_squared) {
					last_x = curr_x;
					last_y = curr_y;
					points_to_keep++;
				}
			}

			// Special case: there is only 1 unique point in the line, so just keep
			// the start and end points
			if (points_to_keep == 1) {
				out_line_entries[out_row_idx] = list_entry_t {out_offset, 2};
				ListVector::Reserve(result, out_offset + 2);
				auto out_x_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[0]);
				auto out_y_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[1]);
				out_x_data[out_offset] = in_x_data[in_offset];
				out_y_data[out_offset] = in_y_data[in_offset];
				out_x_data[out_offset + 1] = in_x_data[in_offset + in_length - 1];
				out_y_data[out_offset + 1] = in_y_data[in_offset + in_length - 1];
				out_offset += 2;
				continue;
			}

			// Set the list entry
			out_line_entries[out_row_idx] = list_entry_t {out_offset, points_to_keep};

			// Second pass, copy the points we need to keep
			ListVector::Reserve(result, out_offset + points_to_keep);
			auto out_x_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[0]);
			auto out_y_data = FlatVector::GetDataMutable<double>(out_line_vertex_vec[1]);

			// Copy the first point
			out_x_data[out_offset] = in_x_data[in_offset];
			out_y_data[out_offset] = in_y_data[in_offset];
			out_offset++;

			// With tolerance its different, we always keep the first and last point
			// regardless of distance to the previous point
			// Copy the middle points
			last_x = in_x_data[in_offset];
			last_y = in_y_data[in_offset];

			for (idx_t i = 1; i < in_length - 1; i++) {

				auto curr_x = in_x_data[in_offset + i];
				auto curr_y = in_y_data[in_offset + i];

				auto dist_squared = (curr_x - last_x) * (curr_x - last_x) + (curr_y - last_y) * (curr_y - last_y);
				if (dist_squared > tolerance_squared) {
					out_x_data[out_offset] = curr_x;
					out_y_data[out_offset] = curr_y;
					last_x = curr_x;
					last_y = curr_y;
					out_offset++;
				}
			}

			// Copy the last point
			out_x_data[points_to_keep - 1] = in_x_data[in_offset + in_length - 1];
			out_y_data[points_to_keep - 1] = in_y_data[in_offset + in_length - 1];
			out_offset++;
		}
		ListVector::SetListSize(result, out_offset);

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Remove repeated points from a LINESTRING.
	)";

	// TODO: example
	static constexpr auto EXAMPLE = R"()";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_RemoveRepeatedPoints", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(GeoTypes::LINESTRING_2D());

				variant.SetFunction(ExecuteLineString);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", GeoTypes::LINESTRING_2D());
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::LINESTRING_2D());

				variant.SetFunction(ExecuteLineStringWithTolerance);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_StartPoint
//======================================================================================================================

struct ST_StartPoint {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, string_t>(
		    args.data[0], result, args.size(), [&](const string_t &blob) -> optional<string_t> {
			    // TODO: Peek without deserializing!
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::LINESTRING) {
				    return nullopt;
			    }

			    if (geom.is_empty()) {
				    return nullopt;
			    }

			    const auto vertex_array = geom.get_vertex_array();

			    sgl::geometry point(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
			    point.set_vertex_array(vertex_array, 1);

			    return lstate.Serialize(result, point);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &geom_vec = args.data[0];
		auto count = args.size();

		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(geom_format);

		auto line_vertex_entries = FlatVector::GetData<list_entry_t>(geom_vec);
		auto &line_vertex_vec = ListVector::GetChildMutable(geom_vec);
		auto &line_vertex_vec_children = StructVector::GetEntries(line_vertex_vec);
		auto line_x_data = FlatVector::GetDataMutable<double>(line_vertex_vec_children[0]);
		auto line_y_data = FlatVector::GetDataMutable<double>(line_vertex_vec_children[1]);

		auto &point_vertex_children = StructVector::GetEntries(result);
		auto point_x_data = FlatVector::GetDataMutable<double>(point_vertex_children[0]);
		auto point_y_data = FlatVector::GetDataMutable<double>(point_vertex_children[1]);

		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {
			auto in_row_idx = geom_format.sel->get_index(out_row_idx);

			if (!geom_format.validity.RowIsValid(in_row_idx)) {
				FlatVector::SetNull(result, out_row_idx, true);
				continue;
			}

			auto line = line_vertex_entries[in_row_idx];
			auto line_offset = line.offset;
			auto line_length = line.length;

			if (line_length == 0) {
				FlatVector::SetNull(result, out_row_idx, true);
				continue;
			}

			point_x_data[out_row_idx] = line_x_data[line_offset];
			point_y_data[out_row_idx] = line_y_data[line_offset];
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the start point of a LINESTRING.
	)";

	// todo: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_StartPoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetFunction(ExecuteLineString);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_EndPoint
//======================================================================================================================

struct ST_EndPoint {

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteGeometry(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, string_t>(
		    args.data[0], result, args.size(), [&](const string_t &blob) -> optional<string_t> {
			    // TODO: Peek without deserializing!
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::LINESTRING) {
				    return nullopt;
			    }

			    if (geom.is_empty()) {
				    return nullopt;
			    }

			    const auto vertex_count = geom.get_vertex_count();
			    const auto vertex_width = geom.get_vertex_width();
			    const auto vertex_array = geom.get_vertex_array();

			    const auto point_data = vertex_array + ((vertex_count - 1) * vertex_width);

			    sgl::geometry point(sgl::geometry_type::POINT, geom.has_z(), geom.has_m());
			    point.set_vertex_array(point_data, 1);

			    return lstate.Serialize(result, point);
		    });
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static void ExecuteLineString(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &geom_vec = args.data[0];
		auto count = args.size();

		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(geom_format);

		auto line_vertex_entries = FlatVector::GetData<list_entry_t>(geom_vec);
		auto &line_vertex_vec = ListVector::GetChildMutable(geom_vec);
		auto &line_vertex_vec_children = StructVector::GetEntries(line_vertex_vec);
		auto line_x_data = FlatVector::GetDataMutable<double>(line_vertex_vec_children[0]);
		auto line_y_data = FlatVector::GetDataMutable<double>(line_vertex_vec_children[1]);

		auto &point_vertex_children = StructVector::GetEntries(result);
		auto point_x_data = FlatVector::GetDataMutable<double>(point_vertex_children[0]);
		auto point_y_data = FlatVector::GetDataMutable<double>(point_vertex_children[1]);

		for (idx_t out_row_idx = 0; out_row_idx < count; out_row_idx++) {
			auto in_row_idx = geom_format.sel->get_index(out_row_idx);

			if (!geom_format.validity.RowIsValid(in_row_idx)) {
				FlatVector::SetNull(result, out_row_idx, true);
				continue;
			}

			auto line = line_vertex_entries[in_row_idx];
			auto line_offset = line.offset;
			auto line_length = line.length;

			if (line_length == 0) {
				FlatVector::SetNull(result, out_row_idx, true);
				continue;
			}

			point_x_data[out_row_idx] = line_x_data[line_offset + line_length - 1];
			point_y_data[out_row_idx] = line_y_data[line_offset + line_length - 1];
		}
		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Returns the end point of a LINESTRING.
	)";

	// TODO: add example
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_EndPoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::GEOMETRY());
				variant.SetBind(GeoTypes::PropagateCRS);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteGeometry);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("line", GeoTypes::LINESTRING_2D());
				variant.SetReturnType(GeoTypes::POINT_2D());

				variant.SetFunction(ExecuteLineString);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

//======================================================================================================================
// ST_Within
//======================================================================================================================

struct ST_Within {

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D -> POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &point_in = args.data[0];
		auto &polygon_in = args.data[1];

		// Just execute ST_Contains, but reversed
		ST_Contains::Operation(polygon_in, point_in, result, args.size());
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// TODO: add example
	static constexpr auto DESCRIPTION = "";
	static constexpr auto EXAMPLE = "";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// ST_Within is the inverse of ST_Contains
		FunctionBuilder::RegisterScalar(loader, "ST_Within", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::POINT_2D());
				variant.AddParameter("geom2", GeoTypes::POLYGON_2D());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

enum class VertexOrdinate { X, Y, Z, M };

template <class OP>
struct PointAccessFunctionBase {
	static size_t GetOrdinateOffset(const sgl::geometry &geom) {
		switch (OP::ORDINATE) {
		case VertexOrdinate::X:
			return 0;
		case VertexOrdinate::Y:
			return 1;
		case VertexOrdinate::Z:
			return 2;
		case VertexOrdinate::M:
			return geom.has_z() ? 3 : 2;
		default:
			return 0;
		}
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, double>(
		    args.data[0], result, args.size(), [&](const string_t &blob) -> optional<double> {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.get_type() != sgl::geometry_type::POINT) {
				    throw InvalidInputException("%s only supports POINT geometries", OP::NAME);
			    }

			    if (geom.is_empty()) {
				    return nullopt;
			    }

			    if (OP::ORDINATE == VertexOrdinate::Z && !geom.has_z()) {
				    return nullopt;
			    }

			    if (OP::ORDINATE == VertexOrdinate::M && !geom.has_m()) {
				    return nullopt;
			    }

			    const auto vertex_data = geom.get_vertex_array();
			    const auto offset = GetOrdinateOffset(geom);

			    double res = 0.0;
			    memcpy(&res, vertex_data + offset * sizeof(double), sizeof(double));
			    return res;
		    });
	}

	static void ExecutePoint(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		// Only defined for X and Y
		D_ASSERT(OP::ORDINATE == VertexOrdinate::X || OP::ORDINATE == VertexOrdinate::Y);

		auto &point = args.data[0];
		auto &point_children = StructVector::GetEntries(point);
		auto &n_child = point_children[OP::ORDINATE == VertexOrdinate::X ? 0 : 1];
		result.Reference(n_child);
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, OP::NAME, [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
				variant.CanThrowErrors();

				variant.SetDescription(OP::DESCRIPTION);
				variant.SetExample(OP::EXAMPLE);
			});
			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});

		if (OP::ORDINATE == VertexOrdinate::X || OP::ORDINATE == VertexOrdinate::Y) {
			FunctionBuilder::RegisterScalar(loader, OP::NAME, [](ScalarFunctionBuilder &func) {
				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("point", GeoTypes::POINT_2D());
					variant.SetReturnType(LogicalType::DOUBLE);

					variant.SetFunction(ExecutePoint);

					variant.SetDescription(OP::DESCRIPTION);
					variant.SetExample(OP::EXAMPLE);
				});
				func.SetTag("ext", "spatial");
				func.SetTag("category", "property");
			});
		}
	}
};

struct VertexMinAggOp {
	static constexpr auto MIN_NOT_MAX = true;

	static double Init() {
		return std::numeric_limits<double>::max();
	}
	static double Merge(const double a, const double b) {
		return std::min(a, b);
	}
};

struct VertexMaxAggOp {
	static constexpr auto MIN_NOT_MAX = false;

	static double Init() {
		return std::numeric_limits<double>::lowest();
	}
	static double Merge(const double a, const double b) {
		return std::max(a, b);
	}
};

template <class OP, class AGG>
struct VertexAggFunctionBase {
	static size_t GetOrdinateOffset(const sgl::geometry &geom) {
		switch (OP::ORDINATE) {
		case VertexOrdinate::X:
			return 0;
		case VertexOrdinate::Y:
			return 1;
		case VertexOrdinate::Z:
			return 2;
		case VertexOrdinate::M:
			return geom.has_z() ? 3 : 2;
		default:
			return 0;
		}
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, double>(
		    args.data[0], result, args.size(), [&](const string_t &blob) -> optional<double> {
			    sgl::geometry geom;
			    lstate.Deserialize(blob, geom);

			    if (geom.is_empty()) {
				    return nullopt;
			    }
			    if (OP::ORDINATE == VertexOrdinate::Z && !geom.has_z()) {
				    return nullopt;
			    }
			    if (OP::ORDINATE == VertexOrdinate::M && !geom.has_m()) {
				    return nullopt;
			    }

			    const auto offset = GetOrdinateOffset(geom);

			    double res = AGG::Init();

			    // Ugly. TODO: Fix this whole class.
			    struct state_t {
				    double &res;
				    size_t offset;
			    } arg = {res, offset};

			    sgl::ops::visit_vertices_xyzm(geom, &arg, [](void *arg_ptr, const sgl::vertex_xyzm &vertex) {
				    auto &state = *static_cast<state_t *>(arg_ptr);

				    double val = 0.0;
				    memcpy(&val, reinterpret_cast<const char *>(&vertex) + state.offset * sizeof(double),
				           sizeof(double));

				    state.res = AGG::Merge(state.res, val);
			    });

			    return res;
		    });
	}

	static void ExecutePoint(DataChunk &args, ExpressionState &, Vector &result) {
		D_ASSERT(args.data.size() == 1);
		auto &point = args.data[0];
		auto &point_children = StructVector::GetEntries(point);

		switch (OP::ORDINATE) {
		case VertexOrdinate::X:
			result.Reference(point_children[0]);
			break;
		case VertexOrdinate::Y:
			result.Reference(point_children[1]);
			break;
		default:
			D_ASSERT(false);
			break;
		}
	}

	static void ExecuteLineString(DataChunk &args, ExpressionState &, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &line_vec = args.data[0];
		auto &line_coords = ListVector::GetChildMutable(line_vec);
		auto &line_coords_vec = StructVector::GetEntries(line_coords);

		const auto axis = OP::ORDINATE == VertexOrdinate::X ? 0 : 1;
		auto ordinate_data = FlatVector::GetDataMutable<double>(line_coords_vec[axis]);

		UnaryExecutor::Execute<list_entry_t, double>(
		    line_vec, result, args.size(), [&](const list_entry_t &line) -> optional<double> {
			    // Empty line, return NULL
			    if (line.length == 0) {
				    return nullopt;
			    }

			    auto val = AGG::Init();
			    for (idx_t i = line.offset; i < line.offset + line.length; i++) {
				    auto ordinate = ordinate_data[i];
				    val = AGG::Merge(val, ordinate);
			    }
			    return val;
		    });

		if (line_vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static void ExecutePolygon(DataChunk &args, ExpressionState &, Vector &result) {
		D_ASSERT(args.data.size() == 1);

		auto &input = args.data[0];
		auto count = args.size();

		UnifiedVectorFormat format;
		input.ToUnifiedFormat(format);

		auto &ring_vec = ListVector::GetChildMutable(input);
		auto ring_entries = FlatVector::GetDataMutable<list_entry_t>(ring_vec);
		auto &vertex_vec = ListVector::GetChildMutable(ring_vec);
		auto &vertex_vec_children = StructVector::GetEntries(vertex_vec);
		const auto axis = OP::ORDINATE == VertexOrdinate::X ? 0 : 1;
		auto ordinate_data = FlatVector::GetDataMutable<double>(vertex_vec_children[axis]);

		UnaryExecutor::Execute<list_entry_t, double>(
		    input, result, count, [&](const list_entry_t &polygon) -> optional<double> {
			    auto polygon_offset = polygon.offset;

			    // Empty polygon, return NULL
			    if (polygon.length == 0) {
				    return nullopt;
			    }

			    // We only have to check the outer shell
			    auto shell_ring = ring_entries[polygon_offset];
			    auto ring_offset = shell_ring.offset;
			    auto ring_length = shell_ring.length;

			    // Polygon is invalid. This should never happen but just in case
			    if (ring_length == 0) {
				    return nullopt;
			    }

			    auto val = AGG::Init();
			    for (idx_t coord_idx = ring_offset; coord_idx < ring_offset + ring_length - 1; coord_idx++) {
				    auto ordinate = ordinate_data[coord_idx];
				    val = AGG::Merge(val, ordinate);
			    }
			    return val;
		    });
	}

	static void ExecuteBox(DataChunk &args, ExpressionState &, Vector &result) {
		auto &input = args.data[0];
		auto &box_vec = StructVector::GetEntries(input);

		switch (OP::ORDINATE) {
		case VertexOrdinate::X:
			if (AGG::MIN_NOT_MAX) {
				result.Reference(box_vec[0]);
			} else {
				result.Reference(box_vec[2]);
			}
			break;
		case VertexOrdinate::Y:
			if (AGG::MIN_NOT_MAX) {
				result.Reference(box_vec[1]);
			} else {
				result.Reference(box_vec[3]);
			}
			break;
		default:
			D_ASSERT(false);
			break;
		}
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, OP::NAME, [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", LogicalType::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			// These are only defined for X/Y variants
			if (OP::ORDINATE == VertexOrdinate::X || OP::ORDINATE == VertexOrdinate::Y) {
				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("point", GeoTypes::POINT_2D());
					variant.SetReturnType(LogicalType::DOUBLE);

					variant.SetFunction(ExecutePoint);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("line", GeoTypes::LINESTRING_2D());
					variant.SetReturnType(LogicalType::DOUBLE);

					variant.SetFunction(ExecuteLineString);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("polygon", GeoTypes::POLYGON_2D());
					variant.SetReturnType(LogicalType::DOUBLE);

					variant.SetFunction(ExecutePolygon);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("box", GeoTypes::BOX_2D());
					variant.SetReturnType(LogicalType::DOUBLE);

					variant.SetFunction(ExecuteBox);
				});

				func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
					variant.AddParameter("box", GeoTypes::BOX_2DF());
					variant.SetReturnType(LogicalType::FLOAT);

					variant.SetFunction(ExecuteBox);
				});
			}

			func.SetDescription(OP::DESCRIPTION);
			func.SetExample(OP::EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

struct ST_X : PointAccessFunctionBase<ST_X> {
	static constexpr auto NAME = "ST_X";
	static constexpr auto DESCRIPTION = "Returns the X coordinate of a point geometry";
	static constexpr auto EXAMPLE = "SELECT ST_X(ST_Point(1, 2))";
	static constexpr auto ORDINATE = VertexOrdinate::X;
};

struct ST_XMax : VertexAggFunctionBase<ST_XMax, VertexMaxAggOp> {
	static auto constexpr NAME = "ST_XMax";
	static auto constexpr DESCRIPTION = "Returns the maximum X coordinate of a geometry";
	static auto constexpr EXAMPLE = "SELECT ST_XMax(ST_Point(1, 2))";
	static auto constexpr ORDINATE = VertexOrdinate::X;
};

struct ST_XMin : VertexAggFunctionBase<ST_XMin, VertexMinAggOp> {
	static constexpr auto NAME = "ST_XMin";
	static constexpr auto DESCRIPTION = "Returns the minimum X coordinate of a geometry";
	static constexpr auto EXAMPLE = "SELECT ST_XMin(ST_Point(1, 2))";
	static constexpr auto ORDINATE = VertexOrdinate::X;
};

struct ST_Y : PointAccessFunctionBase<ST_Y> {
	static constexpr auto NAME = "ST_Y";
	static constexpr auto DESCRIPTION = "Returns the Y coordinate of a point geometry";
	static constexpr auto EXAMPLE = "SELECT ST_Y(ST_Point(1, 2))";
	static constexpr auto ORDINATE = VertexOrdinate::Y;
};

struct ST_YMax : VertexAggFunctionBase<ST_YMax, VertexMaxAggOp> {
	static constexpr auto NAME = "ST_YMax";
	static constexpr auto DESCRIPTION = "Returns the maximum Y coordinate of a geometry";
	static constexpr auto EXAMPLE = "SELECT ST_YMax(ST_Point(1, 2))";
	static constexpr auto ORDINATE = VertexOrdinate::Y;
};

struct ST_YMin : VertexAggFunctionBase<ST_YMin, VertexMinAggOp> {
	static constexpr auto NAME = "ST_YMin";
	static constexpr auto DESCRIPTION = "Returns the minimum Y coordinate of a geometry";
	static constexpr auto EXAMPLE = "SELECT ST_YMin(ST_Point(1, 2))";
	static constexpr auto ORDINATE = VertexOrdinate::Y;
};

struct ST_Z : PointAccessFunctionBase<ST_Z> {
	static constexpr auto NAME = "ST_Z";
	static constexpr auto DESCRIPTION = "Returns the Z coordinate of a point geometry";
	static constexpr auto EXAMPLE = "SELECT ST_Z(ST_Point(1, 2, 3))";
	static constexpr auto ORDINATE = VertexOrdinate::Z;
};

struct ST_ZMax : VertexAggFunctionBase<ST_ZMax, VertexMaxAggOp> {
	static auto constexpr NAME = "ST_ZMax";
	static auto constexpr DESCRIPTION = "Returns the maximum Z coordinate of a geometry";
	static auto constexpr EXAMPLE = "SELECT ST_ZMax(ST_Point(1, 2, 3))";
	static auto constexpr ORDINATE = VertexOrdinate::Z;
};

struct ST_ZMin : VertexAggFunctionBase<ST_ZMin, VertexMinAggOp> {
	static constexpr auto NAME = "ST_ZMin";
	static constexpr auto DESCRIPTION = "Returns the minimum Z coordinate of a geometry";
	static constexpr auto EXAMPLE = "SELECT ST_ZMin(ST_Point(1, 2, 3))";
	static constexpr auto ORDINATE = VertexOrdinate::Z;
};

struct ST_M : PointAccessFunctionBase<ST_M> {
	static constexpr auto NAME = "ST_M";
	static constexpr auto DESCRIPTION = "Returns the M coordinate of a point geometry";
	static constexpr auto EXAMPLE = "SELECT ST_M(ST_Point(1, 2, 3, 4))";
	static constexpr auto ORDINATE = VertexOrdinate::M;
};

struct ST_MMax : VertexAggFunctionBase<ST_MMax, VertexMaxAggOp> {
	static constexpr auto NAME = "ST_MMax";
	static constexpr auto DESCRIPTION = "Returns the maximum M coordinate of a geometry";
	static constexpr auto EXAMPLE = "SELECT ST_MMax(ST_Point(1, 2, 3, 4))";
	static constexpr auto ORDINATE = VertexOrdinate::M;
};

struct ST_MMin : VertexAggFunctionBase<ST_MMin, VertexMinAggOp> {
	static constexpr auto NAME = "ST_MMin";
	static constexpr auto DESCRIPTION = "Returns the minimum M coordinate of a geometry";
	static constexpr auto EXAMPLE = "SELECT ST_MMin(ST_Point(1, 2, 3, 4))";
	static constexpr auto ORDINATE = VertexOrdinate::M;
};

constexpr const char *ST_M::NAME;
constexpr const char *ST_X::NAME;
constexpr const char *ST_Y::NAME;
constexpr const char *ST_Z::NAME;

} // namespace

// Helper to access the constant distance from the bind data
bool ST_DWithinHelper::TryGetConstDistance(const unique_ptr<FunctionData> &bind_data, double &result) {
	if (bind_data) {
		const auto &data = bind_data->Cast<ST_DistanceWithin::BindData>();
		if (data.is_constant) {
			result = data.distance;
			return true;
		}
	}
	return false;
}

//######################################################################################################################
// Register
//######################################################################################################################

void RegisterSpatialScalarFunctions(ExtensionLoader &loader) {
	ST_Affine::Register(loader);
	ST_Area::Register(loader);
	ST_AsGeoJSON::Register(loader);
	ST_AsText::Register(loader);
	// ST_AsWKB::Register(loader);
	ST_AsHEXWKB::Register(loader);
	ST_AsSVG::Register(loader);
	ST_Azimuth::Register(loader);
	ST_Centroid::Register(loader);
	ST_Collect::Register(loader);
	ST_CollectionExtract::Register(loader);
	ST_Contains::Register(loader);
	ST_Dimension::Register(loader);
	ST_Distance::Register(loader);
	ST_DistanceWithin::Register(loader);
	ST_Dump::Register(loader);
	ST_EndPoint::Register(loader);
	ST_Expand::Register(loader);
	ST_Extent::Register(loader);
	ST_Extent_Approx::Register(loader);
	// Op_IntersectApprox::Register(loader);
	ST_ExteriorRing::Register(loader);
	ST_FlipCoordinates::Register(loader);
	ST_Force2D::Register(loader);
	ST_Force3DZ::Register(loader);
	ST_Force3DM::Register(loader);
	ST_Force4D::Register(loader);
	ST_GeometryType::Register(loader);
	ST_GeomFromHEXWKB::Register(loader);
	ST_GeomFromGeoJSON::Register(loader);
	ST_GeomFromText::Register(loader);
	ST_GeomFromWKB::Register(loader);
	ST_HasZ::Register(loader);
	ST_HasM::Register(loader);
	ST_LineInterpolatePoint::Register(loader);
	ST_LineInterpolatePoints::Register(loader);
	ST_LineLocatePoint::Register(loader);
	ST_LineSubstring::Register(loader);
	ST_LocateAlong::Register(loader);
	ST_LocateBetween::Register(loader);
	ST_ZMFlag::Register(loader);
	ST_Distance_Sphere::Register(loader);
	ST_Hilbert::Register(loader);
	ST_InteriorRingN::Register(loader);
	ST_InterpolatePoint::Register(loader);
	ST_Intersects::Register(loader);
	ST_Intersects_Extent::Register(loader);
	ST_IsClosed::Register(loader);
	ST_IsEmpty::Register(loader);
	ST_Length::Register(loader);
	ST_MakeEnvelope::Register(loader);
	ST_MakeLine::Register(loader);
	ST_MakePolygon::Register(loader);
	ST_MakeBox2D::Register(loader);
	ST_Multi::Register(loader);
	ST_NGeometries::Register(loader);
	ST_NInteriorRings::Register(loader);
	ST_NPoints::Register(loader);
	ST_Perimeter::Register(loader);
	ST_Point::Register(loader);
	ST_PointN::Register(loader);
	ST_Points::Register(loader);
	ST_QuadKey::Register(loader);
	ST_RemoveRepeatedPoints::Register(loader);
	ST_StartPoint::Register(loader);
	ST_Within::Register(loader);
	ST_X::Register(loader);
	ST_XMax::Register(loader);
	ST_XMin::Register(loader);
	ST_Y::Register(loader);
	ST_YMax::Register(loader);
	ST_YMin::Register(loader);
	ST_Z::Register(loader);
	ST_ZMax::Register(loader);
	ST_ZMin::Register(loader);
	ST_M::Register(loader);
	ST_MMax::Register(loader);
	ST_MMin::Register(loader);
}

} // namespace duckdb
