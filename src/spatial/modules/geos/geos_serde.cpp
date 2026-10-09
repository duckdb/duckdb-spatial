#include "spatial/modules/geos/geos_serde.hpp"

#include "duckdb/common/typedefs.hpp"
#include "geos_c.h"
#include "duckdb/common/types/geometry.hpp"
#include "sgl/sgl.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "spatial/util/binary_writer.hpp"
#include "spatial/util/math.hpp"
#include "spatial/util/binary_reader.hpp"

namespace sgl {
enum class geometry_type : uint8_t;
}
namespace duckdb {

template <class T>
static T StorageTypeFromGEOS(int type) {
	switch (type) {
	case GEOS_POINT:
		return static_cast<T>(1);
	case GEOS_LINESTRING:
		return static_cast<T>(2);
	case GEOS_POLYGON:
		return static_cast<T>(3);
	case GEOS_MULTIPOINT:
		return static_cast<T>(4);
	case GEOS_MULTILINESTRING:
		return static_cast<T>(5);
	case GEOS_MULTIPOLYGON:
		return static_cast<T>(6);
	case GEOS_GEOMETRYCOLLECTION:
		return static_cast<T>(7);
	default:
		throw InvalidInputException("Unsupported GEOS geometry type %d", type);
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Get Required Size
//----------------------------------------------------------------------------------------------------------------------
static size_t GetCoordSeqLength(const GEOSContextHandle_t ctx, const GEOSCoordSequence *seq) {
	uint32_t len = 0;
	GEOSCoordSeq_getSize_r(ctx, seq, &len);
	return len;
}

size_t GeosSerde::GetRequiredSize(GEOSContextHandle_t ctx, const GEOSGeom_t *geom) {
	const auto type = GEOSGeomTypeId_r(ctx, geom);
	const bool has_z = GEOSHasZ_r(ctx, geom);
	const bool has_m = GEOSHasM_r(ctx, geom);

	const auto vert_width = sizeof(double) * (2 + has_z + has_m);

	size_t size = sizeof(uint8_t) + sizeof(uint32_t); // endian + type

	switch (type) {
	case GEOS_POINT: {
		size += vert_width;
	} break;
	case GEOS_LINESTRING: {
		const auto seq = GEOSGeom_getCoordSeq_r(ctx, geom);
		const auto len = GetCoordSeqLength(ctx, seq);
		size += sizeof(uint32_t) + (len * vert_width);
	} break;
	case GEOS_POLYGON: {

		size += sizeof(uint32_t); // num rings
		if (GEOSisEmpty_r(ctx, geom)) {
			break;
		}

		const auto exterior_ptr = GEOSGetExteriorRing_r(ctx, geom);
		const auto exterior_seq = GEOSGeom_getCoordSeq_r(ctx, exterior_ptr);
		const auto exterior_len = GetCoordSeqLength(ctx, exterior_seq);

		size += sizeof(uint32_t);          // num points in shell
		size += exterior_len * vert_width; // shell points

		const auto num_rings = GEOSGetNumInteriorRings_r(ctx, geom);
		for (auto i = 0; i < num_rings; i++) {
			const auto interior_ptr = GEOSGetInteriorRingN_r(ctx, geom, i);
			const auto interior_seq = GEOSGeom_getCoordSeq_r(ctx, interior_ptr);
			const auto interior_len = GetCoordSeqLength(ctx, interior_seq);
			size += sizeof(uint32_t);          // num points in hole
			size += interior_len * vert_width; // hole points
		}
	} break;
	case GEOS_MULTIPOINT:
	case GEOS_MULTILINESTRING:
	case GEOS_MULTIPOLYGON:
	case GEOS_GEOMETRYCOLLECTION: {
		size += sizeof(uint32_t); // num parts
		const auto num_items = GEOSGetNumGeometries_r(ctx, geom);
		for (auto i = 0; i < num_items; i++) {
			const auto item = GEOSGetGeometryN_r(ctx, geom, i);
			size += GetRequiredSize(ctx, item);
		}
	} break;
	default:
		break;
	}
	return size;
}

//----------------------------------------------------------------------------------------------------------------------
// Serialization
//----------------------------------------------------------------------------------------------------------------------

static void SerializeCoordSeq(const GEOSContextHandle_t ctx, const GEOSCoordSequence *seq, bool has_z, bool has_m,
                              size_t len, BinaryWriter &cursor) {
	const auto buffer = cursor.Reserve(len * sizeof(double) * (2 + has_z + has_m));
	GEOSCoordSeq_copyToBuffer_r(ctx, seq, reinterpret_cast<double *>(buffer), has_z, has_m);
}

static void SerializeInternal(const GEOSContextHandle_t ctx, const GEOSGeometry *geom, BinaryWriter &cursor) {
	const auto type = GEOSGeomTypeId_r(ctx, geom);
	const bool has_z = GEOSHasZ_r(ctx, geom);
	const bool has_m = GEOSHasM_r(ctx, geom);

	cursor.Write<uint8_t>(1); // Little Endian
	cursor.Write<uint32_t>(StorageTypeFromGEOS<uint32_t>(type) + (has_z * 1000) + (has_m * 2000));

	switch (type) {
	case GEOS_POINT: {
		if (GEOSisEmpty_r(ctx, geom)) {
			// Write NaNs for empty point
			constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
			constexpr VertexXYZM empty_point {nan, nan, nan, nan};
			cursor.Copy(reinterpret_cast<const char *>(&empty_point), sizeof(double) * (2 + has_z + has_m));
		} else {
			const auto seq = GEOSGeom_getCoordSeq_r(ctx, geom);
			SerializeCoordSeq(ctx, seq, has_z, has_m, 1, cursor);
		}
	} break;
	case GEOS_LINESTRING: {
		if (GEOSisEmpty_r(ctx, geom)) {
			cursor.Write<uint32_t>(0);
			break;
		}
		const auto seq = GEOSGeom_getCoordSeq_r(ctx, geom);
		const auto len = GetCoordSeqLength(ctx, seq);
		cursor.Write<uint32_t>(len);
		SerializeCoordSeq(ctx, seq, has_z, has_m, len, cursor);
	} break;
	case GEOS_POLYGON: {
		if (GEOSisEmpty_r(ctx, geom)) {
			cursor.Write<uint32_t>(0);
			break;
		}

		const auto num_rings = GEOSGetNumInteriorRings_r(ctx, geom);

		cursor.Write<uint32_t>(num_rings + 1);

		const auto exterior_ptr = GEOSGetExteriorRing_r(ctx, geom);
		const auto exterior_seq = GEOSGeom_getCoordSeq_r(ctx, exterior_ptr);
		const auto exterior_len = GetCoordSeqLength(ctx, exterior_seq);

		// Starting with the exterior ring
		cursor.Write<uint32_t>(exterior_len);
		SerializeCoordSeq(ctx, exterior_seq, has_z, has_m, exterior_len, cursor);

		// And for each interior ring
		for (auto i = 0; i < num_rings; i++) {
			const auto interior_ptr = GEOSGetInteriorRingN_r(ctx, geom, i);
			const auto interior_seq = GEOSGeom_getCoordSeq_r(ctx, interior_ptr);
			const auto interior_len = GetCoordSeqLength(ctx, interior_seq);
			cursor.Write<uint32_t>(interior_len);
			SerializeCoordSeq(ctx, interior_seq, has_z, has_m, interior_len, cursor);
		}
	} break;
	case GEOS_MULTIPOINT:
	case GEOS_MULTILINESTRING:
	case GEOS_MULTIPOLYGON:
	case GEOS_GEOMETRYCOLLECTION: {
		const auto num_items = GEOSGetNumGeometries_r(ctx, geom);
		cursor.Write<uint32_t>(num_items);
		for (auto i = 0; i < num_items; i++) {
			const auto item = GEOSGetGeometryN_r(ctx, geom, i);
			SerializeInternal(ctx, item, cursor);
		}
	} break;
	default:
		// Unsupported geometry type
		D_ASSERT(false);
		break;
	}
}

void GeosSerde::Serialize(GEOSContextHandle_t ctx, const GEOSGeom_t *geom, char *buffer, size_t buffer_size) {
	BinaryWriter cursor(buffer, buffer_size);

	const auto type = GEOSGeomTypeId_r(ctx, geom);
	if (type < GEOS_POINT || type > GEOS_GEOMETRYCOLLECTION) {
		// Unsupported geometry type
		throw InvalidInputException("Unsupported GEOS geometry type %d", type);
	}

	// Serialize the geometry
	SerializeInternal(ctx, geom, cursor);
}

//------------------------------------------------------------------------------
// Deserialize
//------------------------------------------------------------------------------
static GEOSGeom_t *DeserializeInternal(BinaryReader &reader, ArenaAllocator &arena, GEOSContextHandle_t ctx);

template <class T>
static T *MakeArray(ArenaAllocator &arena, size_t count) {
	return reinterpret_cast<T *>(arena.AllocateAligned(count * sizeof(T)));
}

template <class V = VertexXY>
static GEOSGeom_t *DeserializeTemplated(BinaryReader &reader, ArenaAllocator &arena, GEOSContextHandle_t ctx,
                                        sgl::geometry_type type) {
	constexpr auto VERTEX_SIZE = V::HAS_Z + V::HAS_M + 2;

	switch (type) {
	case sgl::geometry_type::POINT: {
		auto vert = reader.Read<V>();
		if (vert.AllNan()) {
			return GEOSGeom_createEmptyPoint_r(ctx);
		}
		auto seq = GEOSCoordSeq_copyFromBuffer_r(ctx, reinterpret_cast<const double *>(&vert), 1, V::HAS_Z, V::HAS_M);
		return GEOSGeom_createPoint_r(ctx, seq);
	}
	case sgl::geometry_type::LINESTRING: {
		const auto vert_count = reader.Read<uint32_t>();
		if (vert_count == 0) {
			return GEOSGeom_createEmptyLineString_r(ctx);
		}
		auto vert_array = MakeArray<double>(arena, vert_count * VERTEX_SIZE);
		auto ptr = reader.Reserve(vert_count * VERTEX_SIZE * sizeof(double));
		memcpy(vert_array, ptr, vert_count * VERTEX_SIZE * sizeof(double));
		auto seq = GEOSCoordSeq_copyFromBuffer_r(ctx, vert_array, vert_count, V::HAS_Z, V::HAS_M);
		return GEOSGeom_createLineString_r(ctx, seq);
	}
	case sgl::geometry_type::POLYGON: {
		const auto ring_count = reader.Read<uint32_t>();
		if (ring_count == 0) {
			return GEOSGeom_createEmptyPolygon_r(ctx);
		}
		auto ring_array = MakeArray<GEOSGeometry *>(arena, ring_count);
		for (uint32_t i = 0; i < ring_count; i++) {
			const auto vert_count = reader.Read<uint32_t>();
			auto vert_array = MakeArray<double>(arena, vert_count * VERTEX_SIZE);
			auto ptr = reader.Reserve(vert_count * VERTEX_SIZE * sizeof(double));
			memcpy(vert_array, ptr, vert_count * VERTEX_SIZE * sizeof(double));
			auto seq = GEOSCoordSeq_copyFromBuffer_r(ctx, vert_array, vert_count, V::HAS_Z, V::HAS_M);
			ring_array[i] = GEOSGeom_createLinearRing_r(ctx, seq);
		}
		return GEOSGeom_createPolygon_r(ctx, ring_array[0], ring_array + 1, ring_count - 1);
	}
	case sgl::geometry_type::MULTI_POINT: {
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			return GEOSGeom_createEmptyCollection_r(ctx, GEOS_MULTIPOINT);
		}
		const auto part_array = MakeArray<GEOSGeometry *>(arena, part_count);
		for (uint32_t i = 0; i < part_count; i++) {
			part_array[i] = DeserializeInternal(reader, arena, ctx);
		}
		return GEOSGeom_createCollection_r(ctx, GEOS_MULTIPOINT, part_array, part_count);
	}
	case sgl::geometry_type::MULTI_LINESTRING: {
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			return GEOSGeom_createEmptyCollection_r(ctx, GEOS_MULTILINESTRING);
		}
		const auto part_array = MakeArray<GEOSGeometry *>(arena, part_count);
		for (uint32_t i = 0; i < part_count; i++) {
			part_array[i] = DeserializeInternal(reader, arena, ctx);
		}
		return GEOSGeom_createCollection_r(ctx, GEOS_MULTILINESTRING, part_array, part_count);
	}
	case sgl::geometry_type::MULTI_POLYGON: {
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			return GEOSGeom_createEmptyCollection_r(ctx, GEOS_MULTIPOLYGON);
		}
		const auto part_array = MakeArray<GEOSGeometry *>(arena, part_count);
		for (uint32_t i = 0; i < part_count; i++) {
			part_array[i] = DeserializeInternal(reader, arena, ctx);
		}
		return GEOSGeom_createCollection_r(ctx, GEOS_MULTIPOLYGON, part_array, part_count);
	}
	case sgl::geometry_type::GEOMETRY_COLLECTION: {
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			return GEOSGeom_createEmptyCollection_r(ctx, GEOS_GEOMETRYCOLLECTION);
		}
		const auto part_array = MakeArray<GEOSGeometry *>(arena, part_count);
		for (uint32_t i = 0; i < part_count; i++) {
			part_array[i] = DeserializeInternal(reader, arena, ctx);
		}
		return GEOSGeom_createCollection_r(ctx, GEOS_GEOMETRYCOLLECTION, part_array, part_count);
	}
	default:
		throw InvalidInputException("Unsupported geometry type %d", static_cast<int>(type));
	}
}

static GEOSGeom_t *DeserializeInternal(BinaryReader &reader, ArenaAllocator &arena, GEOSContextHandle_t ctx) {

	while (true) {
		const auto le = reader.Read<uint8_t>();
		if (!le) {
			throw InvalidInputException("Only little-endian WKB is supported");
		}

		const auto meta = reader.Read<uint32_t>();
		const auto type = static_cast<sgl::geometry_type>((meta & 0x0000FFFF) % 1000);
		const auto flag = (meta & 0x0000FFFF) / 1000;
		const auto has_z = (flag & 0x01) != 0;
		const auto has_m = (flag & 0x02) != 0;

		if (has_z && has_m) {
			return DeserializeTemplated<VertexXYZM>(reader, arena, ctx, type);
		}
		if (has_z) {
			return DeserializeTemplated<VertexXYZ>(reader, arena, ctx, type);
		}
		if (has_m) {
			return DeserializeTemplated<VertexXYM>(reader, arena, ctx, type);
		} else {
			return DeserializeTemplated<VertexXY>(reader, arena, ctx, type);
		}
	}
}

GEOSGeom_t *GeosSerde::Deserialize(GEOSContextHandle_t ctx, ArenaAllocator &arena, const char *buffer,
                                   size_t buffer_size) {
	// GEOS always does full copies of the data,
	// so reset the arena after each deserialization
	// TODO: Do this, for now we cant because some aggregates will reuse the arena for their own state
	// and then we cant reset it
	// arena.Reset();

	// Deserialize the geometry
	BinaryReader reader(buffer, buffer_size);
	return DeserializeInternal(reader, arena, ctx);
}

} // namespace duckdb
