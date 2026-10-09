#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/util/binary_reader.hpp"
#include "spatial/util/binary_writer.hpp"
#include "spatial/util/math.hpp"
#include "spatial/geometry/sgl.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/arena_allocator.hpp"

namespace duckdb {

size_t Serde::GetRequiredSize(const sgl::geometry &geom) {

	const auto root = geom.get_parent();
	auto part = &geom;

	size_t total_size = 0;

	while (true) {
		total_size += sizeof(uint8_t);  // LE/BE byte
		total_size += sizeof(uint32_t); // type id
		switch (part->get_type()) {
		case sgl::geometry_type::POINT: {
			total_size += part->get_vertex_width();
		} break;
		case sgl::geometry_type::LINESTRING: {
			total_size += sizeof(uint32_t) + (part->get_vertex_width() * part->get_vertex_count());
		} break;
		case sgl::geometry_type::POLYGON: {
			total_size += sizeof(uint32_t); // ring count
			const auto tail = part->get_last_part();
			if (tail) {
				auto ring = tail;
				do {
					ring = ring->get_next();
					total_size += sizeof(uint32_t) + (ring->get_vertex_width() * ring->get_vertex_count());
				} while (ring != tail);
			}
		} break;
		case sgl::geometry_type::MULTI_POINT:
		case sgl::geometry_type::MULTI_LINESTRING:
		case sgl::geometry_type::MULTI_POLYGON:
		case sgl::geometry_type::GEOMETRY_COLLECTION: {
			total_size += sizeof(uint32_t); // part count
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		}
		default: {
			throw InvalidInputException("Cannot serialize geometry of type %d", static_cast<int>(part->get_type()));
		}
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return total_size;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

void Serde::Serialize(const sgl::geometry &geom, char *buffer, size_t buffer_size) {
	const auto root = geom.get_parent();
	auto part = &geom;

	BinaryWriter writer(buffer, buffer_size);

	while (true) {
		writer.Write<uint8_t>(1); // Little Endian

		// Also write type
		auto type_id = static_cast<uint32_t>(part->get_type());
		type_id += part->has_z() * 1000;
		type_id += part->has_m() * 2000;
		writer.Write<uint32_t>(type_id);

		switch (part->get_type()) {
		case sgl::geometry_type::POINT: {
			constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
			const auto vert_empty = sgl::vertex_xyzm {nan, nan, nan, nan};
			const auto vert_array =
			    part->is_empty() ? reinterpret_cast<const char *>(&vert_empty) : part->get_vertex_array();
			const auto vert_width = part->get_vertex_width();

			writer.Copy(vert_array, vert_width);
		} break;
		case sgl::geometry_type::LINESTRING: {

			const auto vert_array = part->get_vertex_array();
			const auto vert_width = part->get_vertex_width();
			const auto vert_count = part->get_vertex_count();

			writer.Write<uint32_t>(vert_count);
			writer.Copy(vert_array, vert_width * vert_count);
		} break;
		case sgl::geometry_type::POLYGON: {
			const auto ring_count = part->get_part_count();
			writer.Write<uint32_t>(ring_count);
			const auto tail = part->get_last_part();
			if (tail) {
				auto ring = tail;
				do {
					ring = ring->get_next();

					const auto vert_array = ring->get_vertex_array();
					const auto vert_width = ring->get_vertex_width();
					const auto vert_count = ring->get_vertex_count();

					writer.Write<uint32_t>(vert_count);
					writer.Copy(vert_array, vert_width * vert_count);

				} while (ring != tail);
			}
		} break;
		case sgl::geometry_type::MULTI_POINT:
		case sgl::geometry_type::MULTI_LINESTRING:
		case sgl::geometry_type::MULTI_POLYGON:
		case sgl::geometry_type::GEOMETRY_COLLECTION: {
			const auto part_count = part->get_part_count();
			writer.Write<uint32_t>(part_count);
			if (part->is_empty()) {
				break;
			}
			part = part->get_first_part();
			continue;
		}
		default: {
			throw InvalidInputException("Cannot serialize geometry of type %d", static_cast<int>(part->get_type()));
		}
		}

		while (true) {
			const auto parent = part->get_parent();
			if (parent == root) {
				return;
			}
			if (part != parent->get_last_part()) {
				part = part->get_next();
				break;
			}
			part = parent;
		}
	}
}

template <class GEOM_TYPE = sgl::geometry>
void Prepare(GEOM_TYPE &type, ArenaAllocator &allocator) {
}

// Specialize for prepared_geometry
template <>
void Prepare<sgl::prepared_geometry>(sgl::prepared_geometry &type, ArenaAllocator &allocator) {
	GeometryAllocator alloc(allocator);
	type.build(alloc);
	type.set_prepared(true);
}

template <class GEOM_TYPE = sgl::geometry>
static void DeserializeInternal(GEOM_TYPE &result, ArenaAllocator &arena, const char *buffer, size_t buffer_size) {
	if (buffer_size == 0) {
		throw InvalidInputException("Unexpected end of binary data at position 0");
	}
	BinaryReader reader(buffer, buffer_size);

	uint32_t stack[32];
	uint32_t depth = 0;

	sgl::geometry *geom = &result;

	while (true) {
		const auto le = reader.Read<uint8_t>() == 1;
		if (!le) {
			throw InvalidInputException("Only little-endian WKB is supported");
		}
		const auto meta = reader.Read<uint32_t>();
		const auto type = static_cast<sgl::geometry_type>((meta & 0x0000FFFF) % 1000);
		const auto flag = (meta & 0x0000FFFF) / 1000;
		const auto has_z = (flag & 0x01) != 0;
		const auto has_m = (flag & 0x02) != 0;

		geom->set_type(type);
		geom->set_z(has_z);
		geom->set_m(has_m);

		const auto vert_width = geom->get_vertex_width();
		switch (type) {
		case sgl::geometry_type::POINT: {
			constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
			const auto vert_array = reader.Reserve(vert_width);
			auto vert_empty = sgl::vertex_xyzm {nan, nan, nan, nan};
			memcpy(&vert_empty, vert_array, vert_width);
			if (vert_empty.all_nan()) {
				geom->set_vertex_array(nullptr, 0);
			} else {
				geom->set_vertex_array(vert_array, 1);
			}
		} break;
		case sgl::geometry_type::LINESTRING: {
			const auto vert_count = reader.Read<uint32_t>();
			const auto vert_array = reader.Reserve(vert_count * vert_width);
			geom->set_vertex_array(vert_array, vert_count);
		} break;
		case sgl::geometry_type::POLYGON: {
			const auto ring_count = reader.Read<uint32_t>();
			if (ring_count == 0) {
				break;
			}
			for (uint32_t i = 0; i < ring_count; i++) {
				auto ring_mem = arena.AllocateAligned(sizeof(GEOM_TYPE));
				const auto ring = new (ring_mem) GEOM_TYPE(sgl::geometry_type::LINESTRING, has_z, has_m);

				const auto vert_count = reader.Read<uint32_t>();
				const auto vert_array = reader.Reserve(vert_count * ring->get_vertex_width());
				ring->set_vertex_array(vert_array, vert_count);

				Prepare(*ring, arena);

				geom->append_part(ring);
			}
		} break;
		case sgl::geometry_type::MULTI_POINT:
		case sgl::geometry_type::MULTI_LINESTRING:
		case sgl::geometry_type::MULTI_POLYGON:
		case sgl::geometry_type::GEOMETRY_COLLECTION: {
			if (depth >= 32) {
				throw InvalidInputException("Geometry is too deeply nested to deserialize");
			}

			const auto part_count = reader.Read<uint32_t>();
			if (part_count == 0) {
				break;
			}

			stack[depth++] = part_count;

			// Make a new part
			const auto part_mem = arena.AllocateAligned(sizeof(GEOM_TYPE));
			const auto part_ptr = new (part_mem) GEOM_TYPE(sgl::geometry_type::INVALID, has_z, has_m);

			geom->append_part(part_ptr);
			geom = part_ptr;

			// Continue to next iteration
			continue;
		}
		default: {
			throw InvalidInputException("Cannot deserialize geometry of type %d", static_cast<int>(type));
		}
		}

		// Inner loop
		while (true) {
			const auto parent = geom->get_parent();

			if (depth == 0) {
				return;
			}

			stack[depth - 1]--;
			if (stack[depth - 1] > 0) {
				const auto part_mem = arena.AllocateAligned(sizeof(GEOM_TYPE));
				const auto part_ptr = new (part_mem) GEOM_TYPE(sgl::geometry_type::INVALID, has_z, has_m);

				parent->append_part(part_ptr);

				geom = part_ptr;
				break;
			}

			geom = parent;
			depth--;
		}
	}
}

void Serde::Deserialize(sgl::geometry &result, ArenaAllocator &arena, const char *buffer, size_t buffer_size) {
	DeserializeInternal<sgl::geometry>(result, arena, buffer, buffer_size);
}

void Serde::DeserializePrepared(sgl::prepared_geometry &result, ArenaAllocator &arena, const char *buffer,
                                size_t buffer_size) {
	DeserializeInternal<sgl::prepared_geometry>(result, arena, buffer, buffer_size);
}

uint32_t Serde::TryGetBounds(const string_t &blob, Box2D<float> &bbox) {
	GeometryExtent extent = GeometryExtent::Empty();
	const auto count = Geometry::GetExtent(blob, extent);
	if (count == 0) {
		return 0;
	}
	bbox.min.x = MathUtil::DoubleToFloatDown(extent.x_min);
	bbox.min.y = MathUtil::DoubleToFloatDown(extent.y_min);
	bbox.max.x = MathUtil::DoubleToFloatUp(extent.x_max);
	bbox.max.y = MathUtil::DoubleToFloatUp(extent.y_max);
	return count;
}

bool Serde::TryGetBounds(const Value &value, Box2D<float> &bbox) {
	if (value.IsNull()) {
		return false;
	}
	const auto &blob = StringValue::Get(value);
	return TryGetBounds(string_t(blob.data(), static_cast<uint32_t>(blob.size())), bbox) != 0;
}

} // namespace duckdb
