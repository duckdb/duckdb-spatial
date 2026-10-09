#include "spatial/modules/main/spatial_functions.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/math.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "spatial/util/binary_reader.hpp"
#include "spatial/util/binary_writer.hpp"

namespace duckdb {

namespace {

//######################################################################################################################
// Util
//######################################################################################################################

//======================================================================================================================
// Local State
//======================================================================================================================

class LocalState final : public FunctionLocalState {
public:
	explicit LocalState(ClientContext &context) : arena(BufferAllocator::Get(context)), allocator(arena) {
	}

	static unique_ptr<FunctionLocalState> InitCast(CastLocalStateParameters &params);
	static LocalState &ResetAndGet(CastParameters &params);

	// De/Serialize geometries
	void Deserialize(const string_t &blob, sgl::geometry &geom);
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

unique_ptr<FunctionLocalState> LocalState::InitCast(CastLocalStateParameters &parameters) {
	return make_uniq<LocalState>(*parameters.context);
}

LocalState &LocalState::ResetAndGet(CastParameters &state) {
	auto &local_state = state.local_state->Cast<LocalState>();
	local_state.arena.Reset();
	return local_state;
}

void LocalState::Deserialize(const string_t &blob, sgl::geometry &geom) {
	Serde::Deserialize(geom, arena, blob.GetDataUnsafe(), blob.GetSize());
}

string_t LocalState::Serialize(Vector &vector, const sgl::geometry &geom) {
	const auto size = Serde::GetRequiredSize(geom);
	auto blob = StringVector::EmptyString(vector, size);
	Serde::Serialize(geom, blob.GetDataWriteable(), size);
	blob.Finalize();
	return blob;
}

//######################################################################################################################
// Cast Functions
//######################################################################################################################

//======================================================================================================================
// GEOMETRY Casts
//======================================================================================================================

struct GeometryCasts {

	//------------------------------------------------------------------------------------------------------------------
	// WKB_BLOB -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool FromWKBCast(Vector &source, Vector &result, idx_t count, CastParameters &params) {
		auto &lstate = LocalState::ResetAndGet(params);
		auto &alloc = lstate.GetAllocator();

		sgl::wkb_reader reader(alloc);
		reader.set_nan_as_empty(true);

		bool success = true;

		UnaryExecutor::Execute<string_t, string_t>(
		    source, result, count, [&](const string_t &wkb) -> optional<string_t> {
			    const auto wkb_ptr = wkb.GetDataUnsafe();
			    const auto wkb_len = wkb.GetSize();

			    sgl::geometry geom;

			    // Try parse, if it fails, assign error message and return NULL
			    if (!reader.try_parse(geom, wkb_ptr, wkb_len)) {
				    const auto error = reader.get_error_message();
				    if (success) {
					    success = false;
					    HandleCastError::AssignError(error, params.error_message);
				    }
				    return nullopt;
			    }

			    return lstate.Serialize(result, geom);
		    });

		return success;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		const auto geom_type = LogicalType::GEOMETRY();

		// Geometry -> BLOB is explicitly castable
		loader.RegisterCastFunction(geom_type, LogicalType::BLOB, DefaultCasts::ReinterpretCast);
	}
};

//======================================================================================================================
// POINT_2D Casts
//======================================================================================================================

struct PointCasts {

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		CoreVectorOperations::Point2DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_3D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		CoreVectorOperations::Point3DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_2D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(parameters);

		GenericExecutor::ExecuteUnary<POINT_TYPE, GEOMETRY_TYPE>(source, result, count, [&](const POINT_TYPE &point) {
			const double buffer[2] = {point.a_val, point.b_val};
			sgl::geometry geom(sgl::geometry_type::POINT, false, false);
			geom.set_vertex_array(buffer, 1);

			return lstate.Serialize(result, geom);
		});
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_3D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		using POINT_TYPE = StructTypeTernary<double, double, double>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(parameters);

		GenericExecutor::ExecuteUnary<POINT_TYPE, GEOMETRY_TYPE>(source, result, count, [&](const POINT_TYPE &point) {
			const double buffer[3] = {point.a_val, point.b_val, point.c_val};
			sgl::geometry geom(sgl::geometry_type::POINT, true, false);
			geom.set_vertex_array(buffer, 1);

			return lstate.Serialize(result, geom);
		});
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static bool FromGeometryCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		using POINT_TYPE = StructTypeBinary<double, double>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(parameters);

		GenericExecutor::ExecuteUnary<GEOMETRY_TYPE, POINT_TYPE>(source, result, count, [&](const GEOMETRY_TYPE &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob.val, geom);

			if (geom.get_type() != sgl::geometry_type::POINT) {
				throw ConversionException("Cannot cast non-point GEOMETRY to POINT_2D");
			}
			if (geom.is_empty()) {
				// TODO: Maybe make this return NULL instead
				throw ConversionException("Cannot cast empty point GEOMETRY to POINT_2D");
			}
			const auto vertex = geom.get_vertex_xy(0);
			return POINT_TYPE {vertex.x, vertex.y};
		});

		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> POINT_3D
	//------------------------------------------------------------------------------------------------------------------
	static bool FromGeometryCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		using POINT_TYPE = StructTypeTernary<double, double, double>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(parameters);

		GenericExecutor::ExecuteUnary<GEOMETRY_TYPE, POINT_TYPE>(source, result, count, [&](const GEOMETRY_TYPE &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob.val, geom);

			if (geom.get_type() != sgl::geometry_type::POINT) {
				throw ConversionException("Cannot cast non-point GEOMETRY to POINT_3D");
			}
			if (geom.is_empty()) {
				// TODO: Maybe make this return NULL instead
				throw ConversionException("Cannot cast empty point GEOMETRY to POINT_3D");
			}
			const auto vertex = geom.get_vertex_xyzm(0);
			return POINT_TYPE {vertex.x, vertex.y, vertex.z};
		});

		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT(N) -> POINT_2D
	//------------------------------------------------------------------------------------------------------------------
	static bool ToPoint2DCast(Vector &source, Vector &result, idx_t count, CastParameters &) {
		auto &children = StructVector::GetEntries(source);
		auto &x_child = children[0];
		auto &y_child = children[1];

		auto &result_children = StructVector::GetEntries(result);
		auto &result_x_child = result_children[0];
		auto &result_y_child = result_children[1];

		result_x_child.Reference(x_child);
		result_y_child.Reference(y_child);

		if (count == 1) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_4D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast4D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		CoreVectorOperations::Point4DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POINT_4D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast4D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		using POINT_TYPE = StructTypeQuaternary<double, double, double, double>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(parameters);

		GenericExecutor::ExecuteUnary<POINT_TYPE, GEOMETRY_TYPE>(source, result, count, [&](const POINT_TYPE &point) {
			const double buffer[4] = {point.a_val, point.b_val, point.c_val, point.d_val};
			sgl::geometry geom(sgl::geometry_type::POINT, true, true);
			geom.set_vertex_array(buffer, 1);

			return lstate.Serialize(result, geom);
		});
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> POINT_4D
	//------------------------------------------------------------------------------------------------------------------
	static bool FromGeometryCast4D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		using POINT_TYPE = StructTypeQuaternary<double, double, double, double>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;

		auto &lstate = LocalState::ResetAndGet(parameters);

		GenericExecutor::ExecuteUnary<GEOMETRY_TYPE, POINT_TYPE>(source, result, count, [&](const GEOMETRY_TYPE &blob) {
			sgl::geometry geom;
			lstate.Deserialize(blob.val, geom);

			if (geom.get_type() != sgl::geometry_type::POINT) {
				throw ConversionException("Cannot cast non-point GEOMETRY to POINT_4D");
			}
			if (geom.is_empty()) {
				// TODO: Maybe make this return NULL instead
				throw ConversionException("Cannot cast empty point GEOMETRY to POINT_4D");
			}
			const auto vertex = geom.get_vertex_xyzm(0);
			return POINT_TYPE {vertex.x, vertex.y, vertex.z, vertex.m};
		});

		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// POINT_2D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::POINT_2D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast), 1);
		// POINT_3D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::POINT_3D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast3D), 1);
		// POINT_2D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::POINT_2D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast, nullptr, LocalState::InitCast), 1);
		// POINT_3D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::POINT_3D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast3D, nullptr, LocalState::InitCast), 1);
		// GEOMETRY -> POINT_2D
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), GeoTypes::POINT_2D(),
		                            BoundCastInfo(FromGeometryCast, nullptr, LocalState::InitCast), 1);
		// GEOMETRY -> POINT_3D
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), GeoTypes::POINT_3D(),
		                            BoundCastInfo(FromGeometryCast3D, nullptr, LocalState::InitCast), 1);
		// POINT_3D -> POINT_2D
		loader.RegisterCastFunction(GeoTypes::POINT_3D(), GeoTypes::POINT_2D(), ToPoint2DCast, 1);

		// POINT_4D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::POINT_4D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast4D), 1);
		// POINT_4D -> POINT_2D
		loader.RegisterCastFunction(GeoTypes::POINT_4D(), GeoTypes::POINT_2D(), ToPoint2DCast, 1);
		// POINT_4D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::POINT_4D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast4D, nullptr, LocalState::InitCast), 1);
		// GEOMETRY -> POINT_4D
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), GeoTypes::POINT_4D(),
		                            BoundCastInfo(FromGeometryCast4D, nullptr, LocalState::InitCast), 1);
	}
};

//======================================================================================================================
// LINESTRING_{2D,3D} Casts
//======================================================================================================================

struct LinestringCasts {

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &) {
		CoreVectorOperations::LineString2DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_3D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast3D(Vector &source, Vector &result, idx_t count, CastParameters &) {
		CoreVectorOperations::LineString3DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_{2D,3D} -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	template <bool HAS_Z>
	static bool ToGeometryCastTemplate(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &lstate = LocalState::ResetAndGet(parameters);
		auto &arena = lstate.GetArena();

		auto &coord_vec = ListVector::GetChild(source);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		const auto x_data = FlatVector::GetData<double>(coord_vec_children[0]);
		const auto y_data = FlatVector::GetData<double>(coord_vec_children[1]);
		const auto z_data = HAS_Z ? FlatVector::GetData<double>(coord_vec_children[2]) : nullptr;

		const auto coord_size = HAS_Z ? 3 : 2;

		UnaryExecutor::Execute<list_entry_t, string_t>(source, result, count, [&](const list_entry_t &line) {
			const auto vertex_data_mem = arena.AllocateAligned(sizeof(double) * coord_size * line.length);
			const auto vertex_data_ptr = reinterpret_cast<double *>(vertex_data_mem);

			if (HAS_Z) {
				for (idx_t i = 0; i < line.length; i++) {
					vertex_data_ptr[i * 3] = x_data[line.offset + i];
					vertex_data_ptr[i * 3 + 1] = y_data[line.offset + i];
					vertex_data_ptr[i * 3 + 2] = z_data[line.offset + i];
				}
			} else {
				for (idx_t i = 0; i < line.length; i++) {
					vertex_data_ptr[i * 2] = x_data[line.offset + i];
					vertex_data_ptr[i * 2 + 1] = y_data[line.offset + i];
				}
			}

			sgl::geometry geom(sgl::geometry_type::LINESTRING, HAS_Z, false);
			geom.set_vertex_array(vertex_data_mem, line.length);

			return lstate.Serialize(result, geom);
		});
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_2D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return ToGeometryCastTemplate<false>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_3D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return ToGeometryCastTemplate<true>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> LINESTRING_{2D,3D}
	//------------------------------------------------------------------------------------------------------------------
	template <bool HAS_Z>
	static bool FromGeometryCastTemplate(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &lstate = LocalState::ResetAndGet(parameters);

		idx_t total_coords = 0;

		UnaryExecutor::Execute<string_t, list_entry_t>(source, result, count, [&](const string_t &blob) {
			sgl::geometry line;
			lstate.Deserialize(blob, line);

			if (line.get_type() != sgl::geometry_type::LINESTRING) {
				// TODO: Dont throw here, return NULL instead to allow TRY_CAST
				throw ConversionException(
				    StringUtil::Format("Cannot cast non-linestring GEOMETRY to LINESTRING_%s", HAS_Z ? "3D" : "2D"));
			}

			const auto line_size = line.get_vertex_count();

			const auto entry = list_entry_t(total_coords, line_size);
			total_coords += line_size;
			ListVector::Reserve(result, total_coords);

			// Re-fetch the coord vector children, as the ListVector::Reserve() call may have invalidated the pointers
			auto &coord_vec = ListVector::GetChildMutable(result);
			auto &coord_vec_children = StructVector::GetEntries(coord_vec);

			auto x_data = FlatVector::GetDataMutable<double>(coord_vec_children[0]);
			auto y_data = FlatVector::GetDataMutable<double>(coord_vec_children[1]);

			if (HAS_Z) {
				auto z_data = FlatVector::GetDataMutable<double>(coord_vec_children[2]);
				for (idx_t i = 0; i < line_size; i++) {
					const auto vertex = line.get_vertex_xyzm(i);
					x_data[entry.offset + i] = vertex.x;
					y_data[entry.offset + i] = vertex.y;
					z_data[entry.offset + i] = vertex.z;
				}
			} else {
				for (idx_t i = 0; i < line_size; i++) {
					const auto vertex = line.get_vertex_xy(i);
					x_data[entry.offset + i] = vertex.x;
					y_data[entry.offset + i] = vertex.y;
				}
			}
			return entry;
		});
		ListVector::SetListSize(result, total_coords);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static bool FromGeometryCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return FromGeometryCastTemplate<false>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> LINESTRING_3D
	//------------------------------------------------------------------------------------------------------------------
	static bool FromGeometryCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return FromGeometryCastTemplate<true>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// LINESTRING_3D -> LINESTRING_2D
	//------------------------------------------------------------------------------------------------------------------
	static bool ToLine2DCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &coord_src = ListVector::GetChild(source);
		auto &coord_src_children = StructVector::GetEntries(coord_src);
		const auto x_src = FlatVector::GetData<double>(coord_src_children[0]);
		const auto y_src = FlatVector::GetData<double>(coord_src_children[1]);

		idx_t total_coords = 0;

		UnaryExecutor::Execute<list_entry_t, list_entry_t>(source, result, count, [&](const list_entry_t &line) {
			const auto line_size = line.length;

			const auto entry = list_entry_t(total_coords, line_size);
			total_coords += line_size;
			ListVector::Reserve(result, total_coords);

			// Re-fetch the coord vector children, as the ListVector::Reserve() call may have invalidated the pointers
			auto &coord_dst = ListVector::GetChildMutable(result);
			auto &coord_dst_children = StructVector::GetEntries(coord_dst);

			auto x_dst = FlatVector::GetDataMutable<double>(coord_dst_children[0]);
			auto y_dst = FlatVector::GetDataMutable<double>(coord_dst_children[1]);

			for (idx_t i = 0; i < line.length; i++) {
				x_dst[entry.offset + i] = x_src[line.offset + i];
				y_dst[entry.offset + i] = y_src[line.offset + i];
			}
			return entry;
		});
		ListVector::SetListSize(result, total_coords);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// LINESTRING_2D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::LINESTRING_2D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast), 1);
		// LINESTRING_3D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::LINESTRING_3D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast3D), 1);
		// LINESTRING_2D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::LINESTRING_2D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast, nullptr, LocalState::InitCast), 1);
		// LINESTRING_3D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::LINESTRING_3D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast3D, nullptr, LocalState::InitCast), 1);
		// GEOMETRY -> LINESTRING_2D
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), GeoTypes::LINESTRING_2D(),
		                            BoundCastInfo(FromGeometryCast, nullptr, LocalState::InitCast), 1);
		// GEOMETRY -> LINESTRING_3D
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), GeoTypes::LINESTRING_3D(),
		                            BoundCastInfo(FromGeometryCast3D, nullptr, LocalState::InitCast), 1);
		// LINESTRING_3D -> LINESTRING_2D
		loader.RegisterCastFunction(GeoTypes::LINESTRING_3D(), GeoTypes::LINESTRING_2D(), ToLine2DCast, 1);
	}
};

//======================================================================================================================
// POLYGON_{2D,3D} Casts
//======================================================================================================================

struct PolygonCasts {

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		CoreVectorOperations::Polygon2DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_3D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		CoreVectorOperations::Polygon3DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_{2D,3D} -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	template <bool HAS_Z>
	static bool ToGeometryCastTemplate(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &lstate = LocalState::ResetAndGet(parameters);
		auto &arena = lstate.GetArena();

		auto &ring_vec = ListVector::GetChild(source);
		const auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vec);
		const auto &coord_vec = ListVector::GetChild(ring_vec);
		auto &coord_vec_children = StructVector::GetEntries(coord_vec);
		const auto x_data = FlatVector::GetData<double>(coord_vec_children[0]);
		const auto y_data = FlatVector::GetData<double>(coord_vec_children[1]);
		const auto z_data = HAS_Z ? FlatVector::GetData<double>(coord_vec_children[2]) : nullptr;


		const auto coord_size = HAS_Z ? 3 : 2;

		UnaryExecutor::Execute<list_entry_t, string_t>(source, result, count, [&](const list_entry_t &poly) {
			sgl::geometry geom(sgl::geometry_type::POLYGON, HAS_Z, false);

			for (idx_t i = 0; i < poly.length; i++) {
				const auto ring_entry = ring_entries[poly.offset + i];

				// Allocate part
				const auto ring_mem = arena.AllocateAligned(sizeof(sgl::geometry));
				const auto ring_ptr = new (ring_mem) sgl::geometry(sgl::geometry_type::LINESTRING, HAS_Z, false);

				// Allocate data
				const auto ring_data_mem = arena.AllocateAligned(sizeof(double) * coord_size * ring_entry.length);
				const auto ring_data_ptr = reinterpret_cast<double *>(ring_data_mem);

				if (HAS_Z) {
					for (idx_t j = 0; j < ring_entry.length; j++) {
						ring_data_ptr[j * 3] = x_data[ring_entry.offset + j];
						ring_data_ptr[j * 3 + 1] = y_data[ring_entry.offset + j];
						ring_data_ptr[j * 3 + 2] = z_data[ring_entry.offset + j];
					}
				} else {
					for (idx_t j = 0; j < ring_entry.length; j++) {
						ring_data_ptr[j * 2] = x_data[ring_entry.offset + j];
						ring_data_ptr[j * 2 + 1] = y_data[ring_entry.offset + j];
					}
				}

				ring_ptr->set_vertex_array(ring_data_mem, ring_entry.length);

				// Append part
				geom.append_part(ring_ptr);
			}

			return lstate.Serialize(result, geom);
		});
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_2D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return ToGeometryCastTemplate<false>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_3D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return ToGeometryCastTemplate<true>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> POLYGON_{2D,3D}
	//------------------------------------------------------------------------------------------------------------------
	template <bool HAS_Z>
	static bool FromGeometryCastTemplate(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &lstate = LocalState::ResetAndGet(parameters);
		auto &ring_vec = ListVector::GetChildMutable(result);

		idx_t total_rings = 0;
		idx_t total_coords = 0;

		UnaryExecutor::Execute<string_t, list_entry_t>(source, result, count, [&](const string_t &blob) {
			sgl::geometry poly;
			lstate.Deserialize(blob, poly);

			// TODO: Dont throw here, return NULL instead to allow TRY_CAST
			if (poly.get_type() != sgl::geometry_type::POLYGON) {
				throw ConversionException(
				    StringUtil::Format("Cannot cast non-polygon GEOMETRY to POLYGON_%s", HAS_Z ? "3D" : "2D"));
			}

			const auto poly_size = poly.get_part_count();
			const auto poly_entry = list_entry_t(total_rings, poly_size);

			ListVector::Reserve(result, total_rings + poly_size);

			const auto tail = poly.get_last_part();
			auto head = tail;

			if (head) {
				idx_t ring_idx = 0;
				do {
					D_ASSERT(ring_idx < poly_size);
					head = head->get_next();

					const auto ring_size = head->get_vertex_count();
					const auto ring_entry = list_entry_t(total_coords, ring_size);

					ListVector::Reserve(ring_vec, total_coords + ring_size);

					const auto ring_entries = FlatVector::GetDataMutable<list_entry_t>(ring_vec);
					auto &coord_vec = ListVector::GetChildMutable(ring_vec);
					auto &coord_vec_children = StructVector::GetEntries(coord_vec);
					auto x_data = FlatVector::GetDataMutable<double>(coord_vec_children[0]);
					auto y_data = FlatVector::GetDataMutable<double>(coord_vec_children[1]);

					ring_entries[total_rings + ring_idx] = ring_entry;

					if (HAS_Z) {
						auto z_data = FlatVector::GetDataMutable<double>(coord_vec_children[2]);
						for (idx_t j = 0; j < ring_size; j++) {
							const auto vertext = head->get_vertex_xyzm(j);
							x_data[ring_entry.offset + j] = vertext.x;
							y_data[ring_entry.offset + j] = vertext.y;
							z_data[ring_entry.offset + j] = vertext.z;
						}
					} else {
						for (idx_t j = 0; j < ring_size; j++) {
							const auto vertext = head->get_vertex_xy(j);
							x_data[ring_entry.offset + j] = vertext.x;
							y_data[ring_entry.offset + j] = vertext.y;
						}
					}
					total_coords += ring_size;

					ring_idx++;
				} while (head != tail);
			}

			total_rings += poly_size;

			return poly_entry;
		});

		ListVector::SetListSize(result, total_rings);
		ListVector::SetListSize(ring_vec, total_coords);

		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static bool FromGeometryCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return FromGeometryCastTemplate<false>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// GEOMETRY -> POLYGON_3D
	//------------------------------------------------------------------------------------------------------------------
	static bool FromGeometryCast3D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return FromGeometryCastTemplate<true>(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// POLYGON_3D -> POLYGON_2D
	//------------------------------------------------------------------------------------------------------------------
	static bool ToPolygon2DCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &ring_dst = ListVector::GetChildMutable(result);

		auto &ring_src = ListVector::GetChild(source);
		const auto ring_entries_src = FlatVector::GetData<list_entry_t>(ring_src);
		const auto &coord_src = ListVector::GetChild(ring_src);
		auto &coord_src_children = StructVector::GetEntries(coord_src);
		const auto x_src = FlatVector::GetData<double>(coord_src_children[0]);
		const auto y_src = FlatVector::GetData<double>(coord_src_children[1]);

		idx_t total_rings = 0;
		idx_t total_coords = 0;

		UnaryExecutor::Execute<list_entry_t, list_entry_t>(source, result, count, [&](const list_entry_t &poly) {
			const auto poly_size = poly.length;
			const auto poly_entry = list_entry_t(total_rings, poly_size);

			for (idx_t i = 0; i < poly.length; i++) {
				const auto ring_entry_src = ring_entries_src[poly.offset + i];
				const auto ring_size = ring_entry_src.length;
				const auto ring_entry_dst = list_entry_t(total_coords, ring_size);

				ListVector::Reserve(ring_dst, total_coords + ring_size);

				const auto ring_entries_dst = FlatVector::GetDataMutable<list_entry_t>(ring_dst);
				auto &coord_dst = ListVector::GetChildMutable(ring_dst);
				auto &coord_dst_children = StructVector::GetEntries(coord_dst);
				auto x_dst = FlatVector::GetDataMutable<double>(coord_dst_children[0]);
				auto y_dst = FlatVector::GetDataMutable<double>(coord_dst_children[1]);

				ring_entries_dst[total_rings + i] = ring_entry_dst;

				for (idx_t j = 0; j < ring_size; j++) {
					x_dst[ring_entry_dst.offset + j] = x_src[ring_entry_src.offset + j];
					y_dst[ring_entry_dst.offset + j] = y_src[ring_entry_src.offset + j];
				}
				total_coords += ring_size;
			}

			total_rings += poly_size;

			return poly_entry;
		});

		ListVector::SetListSize(result, total_rings);
		ListVector::SetListSize(ring_dst, total_coords);

		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// POLYGON_2D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::POLYGON_2D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast), 1);
		// POLYGON_3D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::POLYGON_3D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast3D), 1);
		// POLYGON_2D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::POLYGON_2D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast, nullptr, LocalState::InitCast), 1);
		// POLYGON_3D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::POLYGON_3D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast3D, nullptr, LocalState::InitCast), 1);
		// GEOMETRY -> POLYGON_2D
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), GeoTypes::POLYGON_2D(),
		                            BoundCastInfo(FromGeometryCast, nullptr, LocalState::InitCast), 1);
		// GEOMETRY -> POLYGON_3D
		loader.RegisterCastFunction(LogicalType::GEOMETRY(), GeoTypes::POLYGON_3D(),
		                            BoundCastInfo(FromGeometryCast3D, nullptr, LocalState::InitCast), 1);
		// POLYGON_3D -> POLYGON_2D
		loader.RegisterCastFunction(GeoTypes::POLYGON_3D(), GeoTypes::POLYGON_2D(), ToPolygon2DCast, 1);
	}
};

//======================================================================================================================
// BOX_2D Casts
//======================================================================================================================

struct BoxCasts {

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D -> VARCHAR
	//------------------------------------------------------------------------------------------------------------------
	static bool ToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		CoreVectorOperations::Box2DToVarchar(source, result, count);
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2D -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast2D(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &lstate = LocalState::ResetAndGet(parameters);
		auto &alloc = lstate.GetAllocator();

		using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;
		GenericExecutor::ExecuteUnary<BOX_TYPE, GEOMETRY_TYPE>(source, result, count, [&](const BOX_TYPE &box) {
			const auto minx = box.a_val;
			const auto miny = box.b_val;
			const auto maxx = box.c_val;
			const auto maxy = box.d_val;

			sgl::geometry poly;
			sgl::polygon::init_from_bbox(alloc, minx, miny, maxx, maxy, poly);

			return lstate.Serialize(result, poly);
		});
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// BOX_2DF -> GEOMETRY
	//------------------------------------------------------------------------------------------------------------------
	static bool ToGeometryCast2F(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &lstate = LocalState::ResetAndGet(parameters);
		auto &alloc = lstate.GetAllocator();
		using BOX_TYPE = StructTypeQuaternary<float, float, float, float>;
		using GEOMETRY_TYPE = PrimitiveType<string_t>;
		GenericExecutor::ExecuteUnary<BOX_TYPE, GEOMETRY_TYPE>(source, result, count, [&](const BOX_TYPE &box) {
			const auto minx = box.a_val;
			const auto miny = box.b_val;
			const auto maxx = box.c_val;
			const auto maxy = box.d_val;

			sgl::geometry poly;
			sgl::polygon::init_from_bbox(alloc, minx, miny, maxx, maxy, poly);

			return lstate.Serialize(result, poly);
		});
		return true;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// BOX_2D -> VARCHAR
		loader.RegisterCastFunction(GeoTypes::BOX_2D(), LogicalType::VARCHAR, BoundCastInfo(ToVarcharCast), 1);

		// BOX_2D -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::BOX_2D(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast2D, nullptr, LocalState::InitCast), 1);

		// BOX_2F -> GEOMETRY
		loader.RegisterCastFunction(GeoTypes::BOX_2DF(), LogicalType::GEOMETRY(),
		                            BoundCastInfo(ToGeometryCast2F, nullptr, LocalState::InitCast), 1);
	}
};

} // namespace

//======================================================================================================================
// Vector Operations
//======================================================================================================================
//  TODO: Move/inline this. This is a relic from the original implementation, but being able to access it from outside
//  is not really important anymore (there are other ways to work around it).

//------------------------------------------------------------------------------
// POINT_2D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::Point2DToVarchar(Vector &source, Vector &result, idx_t count) {
	using POINT_TYPE = StructTypeBinary<double, double>;
	using VARCHAR_TYPE = PrimitiveType<string_t>;

	GenericExecutor::ExecuteUnary<POINT_TYPE, VARCHAR_TYPE>(source, result, count, [&](POINT_TYPE &point) {
		auto x = point.a_val;
		auto y = point.b_val;

		if (std::isnan(x) || std::isnan(y)) {
			return StringVector::AddString(result, "POINT EMPTY");
		}

		return StringVector::AddString(result, StringUtil::Format("POINT (%s)", MathUtil::format_coord(x, y)));
	});
}

//------------------------------------------------------------------------------
// POINT_3D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::Point3DToVarchar(Vector &source, Vector &result, idx_t count) {
	using POINT_TYPE = StructTypeTernary<double, double, double>;
	using VARCHAR_TYPE = PrimitiveType<string_t>;

	GenericExecutor::ExecuteUnary<POINT_TYPE, VARCHAR_TYPE>(source, result, count, [&](POINT_TYPE &point) {
		auto x = point.a_val;
		auto y = point.b_val;
		auto z = point.c_val;

		if (std::isnan(x) || std::isnan(y) || std::isnan(z)) {
			return StringVector::AddString(result, "POINT EMPTY");
		}

		return StringVector::AddString(result, StringUtil::Format("POINT Z (%s)", MathUtil::format_coord(x, y, z)));
	});
}

//------------------------------------------------------------------------------
// POINT_4D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::Point4DToVarchar(Vector &source, Vector &result, idx_t count) {
	using POINT_TYPE = StructTypeQuaternary<double, double, double, double>;
	using VARCHAR_TYPE = PrimitiveType<string_t>;

	GenericExecutor::ExecuteUnary<POINT_TYPE, VARCHAR_TYPE>(source, result, count, [&](POINT_TYPE &point) {
		auto x = point.a_val;
		auto y = point.b_val;
		auto z = point.c_val;
		auto m = point.d_val;

		if (std::isnan(x) || std::isnan(y) || std::isnan(z) || std::isnan(m)) {
			return StringVector::AddString(result, "POINT EMPTY");
		}

		return StringVector::AddString(result, StringUtil::Format("POINT ZM (%s)", MathUtil::format_coord(x, y, z, m)));
	});
}

//------------------------------------------------------------------------------
// LINESTRING_2D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::LineString2DToVarchar(Vector &source, Vector &result, idx_t count) {
	auto &inner = ListVector::GetChild(source);
	auto &children = StructVector::GetEntries(inner);
	auto x_data = FlatVector::GetData<double>(children[0]);
	auto y_data = FlatVector::GetData<double>(children[1]);

	UnaryExecutor::Execute<list_entry_t, string_t>(source, result, count, [&](const list_entry_t &line) {
		auto offset = line.offset;
		auto length = line.length;

		if (length == 0) {
			return StringVector::AddString(result, "LINESTRING EMPTY");
		}

		string result_str = "LINESTRING (";
		for (idx_t i = offset; i < offset + length; i++) {
			result_str += MathUtil::format_coord(x_data[i], y_data[i]);
			if (i < offset + length - 1) {
				result_str += ", ";
			}
		}
		result_str += ")";
		return StringVector::AddString(result, result_str);
	});
}

//------------------------------------------------------------------------------
// LINESTRING_3D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::LineString3DToVarchar(Vector &source, Vector &result, idx_t count) {
	auto &inner = ListVector::GetChild(source);
	auto &children = StructVector::GetEntries(inner);
	auto x_data = FlatVector::GetData<double>(children[0]);
	auto y_data = FlatVector::GetData<double>(children[1]);
	auto z_data = FlatVector::GetData<double>(children[2]);

	UnaryExecutor::Execute<list_entry_t, string_t>(source, result, count, [&](const list_entry_t &line) {
		auto offset = line.offset;
		auto length = line.length;

		if (length == 0) {
			return StringVector::AddString(result, "LINESTRING EMPTY");
		}

		string result_str = "LINESTRING Z (";
		for (idx_t i = offset; i < offset + length; i++) {
			result_str += MathUtil::format_coord(x_data[i], y_data[i], z_data[i]);
			if (i < offset + length - 1) {
				result_str += ", ";
			}
		}
		result_str += ")";
		return StringVector::AddString(result, result_str);
	});
}

//------------------------------------------------------------------------------
// POLYGON_2D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::Polygon2DToVarchar(Vector &source, Vector &result, idx_t count) {
	auto &poly_vector = source;
	auto &ring_vector = ListVector::GetChild(poly_vector);
	auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vector);
	auto &point_vector = ListVector::GetChild(ring_vector);
	auto &point_children = StructVector::GetEntries(point_vector);
	auto x_data = FlatVector::GetData<double>(point_children[0]);
	auto y_data = FlatVector::GetData<double>(point_children[1]);

	UnaryExecutor::Execute<list_entry_t, string_t>(poly_vector, result, count, [&](list_entry_t polygon_entry) {
		auto offset = polygon_entry.offset;
		auto length = polygon_entry.length;

		if (length == 0) {
			return StringVector::AddString(result, "POLYGON EMPTY");
		}

		string result_str = "POLYGON (";
		for (idx_t i = offset; i < offset + length; i++) {
			auto ring_entry = ring_entries[i];
			auto ring_offset = ring_entry.offset;
			auto ring_length = ring_entry.length;
			result_str += "(";
			for (idx_t j = ring_offset; j < ring_offset + ring_length; j++) {
				result_str += MathUtil::format_coord(x_data[j], y_data[j]);
				if (j < ring_offset + ring_length - 1) {
					result_str += ", ";
				}
			}
			result_str += ")";
			if (i < offset + length - 1) {
				result_str += ", ";
			}
		}
		result_str += ")";
		return StringVector::AddString(result, result_str);
	});
}

//------------------------------------------------------------------------------
// POLYGON_3D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::Polygon3DToVarchar(Vector &source, Vector &result, idx_t count) {
	auto &poly_vector = source;
	auto &ring_vector = ListVector::GetChild(poly_vector);
	auto ring_entries = FlatVector::GetData<list_entry_t>(ring_vector);
	auto &point_vector = ListVector::GetChild(ring_vector);
	auto &point_children = StructVector::GetEntries(point_vector);
	auto x_data = FlatVector::GetData<double>(point_children[0]);
	auto y_data = FlatVector::GetData<double>(point_children[1]);
	auto z_data = FlatVector::GetData<double>(point_children[2]);

	UnaryExecutor::Execute<list_entry_t, string_t>(poly_vector, result, count, [&](list_entry_t polygon_entry) {
		auto offset = polygon_entry.offset;
		auto length = polygon_entry.length;

		if (length == 0) {
			return StringVector::AddString(result, "POLYGON EMPTY");
		}

		string result_str = "POLYGON Z (";
		for (idx_t i = offset; i < offset + length; i++) {
			auto ring_entry = ring_entries[i];
			auto ring_offset = ring_entry.offset;
			auto ring_length = ring_entry.length;
			result_str += "(";
			for (idx_t j = ring_offset; j < ring_offset + ring_length; j++) {
				result_str += MathUtil::format_coord(x_data[j], y_data[j], z_data[j]);
				if (j < ring_offset + ring_length - 1) {
					result_str += ", ";
				}
			}
			result_str += ")";
			if (i < offset + length - 1) {
				result_str += ", ";
			}
		}
		result_str += ")";
		return StringVector::AddString(result, result_str);
	});
}

//------------------------------------------------------------------------------
// BOX_2D -> VARCHAR
//------------------------------------------------------------------------------
void CoreVectorOperations::Box2DToVarchar(Vector &source, Vector &result, idx_t count) {
	using BOX_TYPE = StructTypeQuaternary<double, double, double, double>;
	using VARCHAR_TYPE = PrimitiveType<string_t>;
	GenericExecutor::ExecuteUnary<BOX_TYPE, VARCHAR_TYPE>(source, result, count, [&](BOX_TYPE &box) {
		return StringVector::AddString(result,
		                               StringUtil::Format("BOX(%s, %s)", MathUtil::format_coord(box.a_val, box.b_val),
		                                                  MathUtil::format_coord(box.c_val, box.d_val)));
	});
}

//######################################################################################################################
// Register
//######################################################################################################################

void RegisterSpatialCastFunctions(ExtensionLoader &loader) {
	GeometryCasts::Register(loader);
	PointCasts::Register(loader);
	LinestringCasts::Register(loader);
	PolygonCasts::Register(loader);
	BoxCasts::Register(loader);
}

} // namespace duckdb
