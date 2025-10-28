// Mapbox Vector Tiles (MVT) implementation

#include "spatial/modules/mvt/mvt_module.hpp"

#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/function_builder.hpp"

#include "protozero/buffer_vector.hpp"
#include "protozero/basic_pbf_writer.hpp"
#include "spatial/util/binary_reader.hpp"

namespace duckdb {

namespace {

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

	string_t Serialize(Vector &vector, const sgl::geometry &geom);

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

string_t LocalState::Serialize(Vector &vector, const sgl::geometry &geom) {
	const auto size = Serde::GetRequiredSize(geom);
	auto blob = StringVector::EmptyString(vector, size);
	Serde::Serialize(geom, blob.GetDataWriteable(), size);
	blob.Finalize();
	return blob;
}

//======================================================================================================================
// ST_TileEnvelope
//======================================================================================================================

struct ST_TileEnvelope {
	static constexpr double RADIUS = 6378137.0;
	static constexpr double PI = 3.141592653589793;
	static constexpr double CIRCUMFERENCE = 2 * PI * RADIUS;

	static void ExecuteWebMercator(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);

		TernaryExecutor::Execute<int32_t, int32_t, int32_t, string_t>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
		    [&](int32_t tile_zoom, int32_t tile_x, int32_t tile_y) {
			    validate_tile_zoom_argument(tile_zoom);
			    uint32_t zoom_extent = 1u << tile_zoom;
			    validate_tile_index_arguments(zoom_extent, tile_x, tile_y);
			    sgl::geometry bbox;
			    get_tile_bbox(lstate.GetAllocator(), zoom_extent, tile_x, tile_y, bbox);
			    return lstate.Serialize(result, bbox);
		    });
	}

	static void validate_tile_zoom_argument(int32_t tile_zoom) {
		if ((tile_zoom < 0) || (tile_zoom > 30)) {
			throw InvalidInputException("ST_TileEnvelope: tile_zoom must be in the range [0,30]");
		}
	}

	static void validate_tile_index_arguments(uint32_t zoom_extent, int32_t tile_x, int32_t tile_y) {
		if ((tile_x < 0) || (static_cast<uint32_t>(tile_x) >= zoom_extent)) {
			throw InvalidInputException("ST_TileEnvelope: tile_x is out of range for specified tile_zoom");
		}
		if ((tile_y < 0) || (static_cast<uint32_t>(tile_y) >= zoom_extent)) {
			throw InvalidInputException("ST_TileEnvelope: tile_y is out of range for specified tile_zoom");
		}
	}

	static void get_tile_bbox(GeometryAllocator &allocator, uint32_t zoom_extent, int32_t tile_x, int32_t tile_y,
	                          sgl::geometry &bbox) {
		double single_tile_width = CIRCUMFERENCE / zoom_extent;
		double single_tile_height = CIRCUMFERENCE / zoom_extent;
		double tile_left = get_tile_left(tile_x, single_tile_width);
		double tile_right = tile_left + single_tile_width;
		double tile_top = get_tile_top(tile_y, single_tile_height);
		double tile_bottom = tile_top - single_tile_height;

		sgl::polygon::init_from_bbox(allocator, tile_left, tile_bottom, tile_right, tile_top, bbox);
	}

	static double get_tile_left(uint32_t tile_x, double single_tile_width) {
		return -0.5 * CIRCUMFERENCE + (tile_x * single_tile_width);
	}

	static double get_tile_top(uint32_t tile_y, double single_tile_height) {
		return 0.5 * CIRCUMFERENCE - (tile_y * single_tile_height);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
        The `ST_TileEnvelope` scalar function generates tile envelope rectangular polygons from specified zoom level and tile indices.

        This is used in MVT generation to select the features corresponding to the tile extent. The envelope is in the Web Mercator
        coordinate reference system (EPSG:3857). The tile pyramid starts at zoom level 0, corresponding to a single tile for the
        world. Each zoom level doubles the number of tiles in each direction, such that zoom level 1 is 2 tiles wide by 2 tiles high,
        zoom level 2 is 4 tiles wide by 4 tiles high, and so on. Tile indices start at `[x=0, y=0]` at the top left, and increase
        down and right. For example, at zoom level 2, the top right tile is `[x=3, y=0]`, the bottom left tile is `[x=0, y=3]`, and
        the bottom right is `[x=3, y=3]`.

        ```sql
        SELECT ST_TileEnvelope(2, 3, 1);
        ```
    )";
	static constexpr auto EXAMPLE = R"(
        SELECT ST_TileEnvelope(2, 3, 1);
        ┌───────────────────────────────────────────────────────────────────────────────────────────────────────────┐
        │                                         st_tileenvelope(2, 3, 1)                                          │
        │                                                 geometry                                                  │
        ├───────────────────────────────────────────────────────────────────────────────────────────────────────────┤
        │ POLYGON ((1.00188E+07 0, 1.00188E+07 1.00188E+07, 2.00375E+07 1.00188E+07, 2.00375E+07 0, 1.00188E+07 0)) │
        └───────────────────────────────────────────────────────────────────────────────────────────────────────────┘
    )";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_TileEnvelope", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("tile_zoom", LogicalType::INTEGER);
				variant.AddParameter("tile_x", LogicalType::INTEGER);
				variant.AddParameter("tile_y", LogicalType::INTEGER);
				variant.SetReturnType(GeoTypes::GEOMETRY());
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteWebMercator);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "conversion");
		});
	}
};

//======================================================================================================================
// ST_AsMVT
//======================================================================================================================
enum class MVTValueType : uint32_t {
	STRING = 1,
	FLOAT = 2,
	DOUBLE = 3,
	INT = 4,
	BOOL = 7,
};

struct MVTValue {
	MVTValueType type;
	uint32_t size;
	union {
		const char *string_value;
		float float_value;
		double double_value;
		int64_t int_value;
		bool bool_value;
	};
};

struct MVTValueEq {
	bool operator()(const MVTValue &a, const MVTValue &b) const {
		if (a.type != b.type) {
			return false;
		}
		switch (a.type) {
		case MVTValueType::STRING:
			return (a.size == b.size) && (strncmp(a.string_value, b.string_value, a.size) == 0);
		case MVTValueType::FLOAT:
			return a.float_value == b.float_value;
		case MVTValueType::DOUBLE:
			return a.double_value == b.double_value;
		case MVTValueType::INT:
			return a.int_value == b.int_value;
		case MVTValueType::BOOL:
			return a.bool_value == b.bool_value;
		}
		return false; // Should not reach here
	}
};

struct MVTValueHash {
	size_t operator()(const MVTValue &val) const {
		// Use duckdb::Hash
		size_t h1 = duckdb::Hash(static_cast<uint32_t>(val.type));
		size_t h2 = 0;
		switch (val.type) {
		case MVTValueType::STRING:
			h2 = duckdb::Hash(val.string_value, val.size);
			break;
		case MVTValueType::FLOAT:
			h2 = duckdb::Hash(val.float_value);
			break;
		case MVTValueType::DOUBLE:
			h2 = duckdb::Hash(val.double_value);
			break;
		case MVTValueType::INT:
			h2 = duckdb::Hash(val.int_value);
			break;
		case MVTValueType::BOOL:
			h2 = duckdb::Hash(val.bool_value);
			break;
		}
		return h1 ^ (h2 << 1); // Combine the two hashes
	}
};

class MVTValueSet {
public:
	void Clear() {
		map.clear();
		vec.clear();
	}
	uint32_t Insert(const MVTValue &val) {
		const auto it = map.insert(make_pair(val, static_cast<uint32_t>(map.size())));
		if (it.second) {
			// New entry, add it to the order vector
			vec.emplace_back(it.first->first);
		}
		return it.first->second;
	}

	vector<reference<const MVTValue>> &GetOrderedValues() {
		return vec;
	}

private:
	// Unordered map is pointer-stable, so we can store references in the order vector
	unordered_map<MVTValue, uint32_t, MVTValueHash, MVTValueEq> map;
	vector<reference<const MVTValue>> vec;
};

struct MVTFeature {
	MVTFeature *next;
	int32_t id; // Optional feature id, -1 if not set
	uint32_t type;
	uint32_t geom_array_size;
	uint32_t tags_array_size;
	uint32_t *geom_array_data;
	uint32_t *tags_array_keys;
	MVTValue *tags_array_vals;
};

struct MVTLayer {
	MVTFeature *features_head = nullptr;
	MVTFeature *features_tail = nullptr;

	void Absorb(MVTLayer &other) {
		// Append other's features to this layer
		if (other.features_head) {
			if (features_tail) {
				features_tail->next = other.features_head;
				features_tail = other.features_tail;
			} else {
				features_head = other.features_head;
				features_tail = other.features_tail;
			}
			other.features_head = nullptr;
			other.features_tail = nullptr;
		}
	}

	void Combine(ArenaAllocator &allocator, const MVTLayer &other) {
		// Copy the features from the other into this, but reference the same values
		auto other_feature = other.features_head;
		while (other_feature) {
			const auto new_feature_mem = allocator.AllocateAligned(sizeof(MVTFeature));
			const auto new_feature = new (new_feature_mem) MVTFeature();

			// Copy the feature data
			*new_feature = *other_feature;

			new_feature->next = nullptr;
			if (features_tail) {
				features_tail->next = new_feature;
				features_tail = new_feature;
			} else {
				features_head = new_feature;
				features_tail = new_feature;
			}

			other_feature = other_feature->next;
		}
	}

	// Write the layer to the buffer
	void Finalize(const uint32_t extent, const vector<string> &tag_names, const string &layer_name,
	              vector<char> &buffer, MVTValueSet &tag_dict) {

		protozero::basic_pbf_writer<std::vector<char>> tile_writer {buffer};
		protozero::basic_pbf_writer<std::vector<char>> layer_writer {tile_writer, 3}; // layers = 3

		// Add version
		layer_writer.add_uint32(15, 2);

		// Layer name = 1
		layer_writer.add_string(1, layer_name);

		auto feature = features_head;
		while (feature) {

			protozero::basic_pbf_writer<std::vector<char>> feature_writer {layer_writer, 2}; // features = 2

			// Id = 1
			if (feature->id >= 0) {
				// Only write if the id is set (not negative)
				feature_writer.add_uint64(1, feature->id);
			}

			// Tags = 2
			{
				protozero::detail::packed_field_varint<std::vector<char>, uint32_t> tags_writer(feature_writer, 2);
				for (uint32_t tag_idx = 0; tag_idx < feature->tags_array_size; tag_idx++) {
					const auto &key_idx = feature->tags_array_keys[tag_idx];
					const auto &val = feature->tags_array_vals[tag_idx];

					// Try to find the value in the dictionary
					// If it exists, we use the existing index
					// If it does not exist, we add it to the dictionary and use the newly added index
					const auto val_idx = tag_dict.Insert(val);

					tags_writer.add_element(key_idx);
					tags_writer.add_element(val_idx);
				}
			}

			// Type = 3
			feature_writer.add_uint32(3, feature->type);

			// Geometry = 4
			feature_writer.add_packed_uint32(4, feature->geom_array_data,
			                                 feature->geom_array_data + feature->geom_array_size);

			feature = feature->next;
		}

		// Tag Keys = 3
		for (auto &key : tag_names) {
			layer_writer.add_string(3, key);
		}

		for (const auto &tag : tag_dict.GetOrderedValues()) {
			auto &val = tag.get();
			protozero::basic_pbf_writer<std::vector<char>> val_writer {layer_writer, 4}; // values = 4
			switch (val.type) {
			case MVTValueType::STRING:
				val_writer.add_string(1, val.string_value, val.size);
				break;
			case MVTValueType::FLOAT:
				val_writer.add_float(2, val.float_value);
				break;
			case MVTValueType::DOUBLE:
				val_writer.add_double(3, val.double_value);
				break;
			case MVTValueType::INT:
				val_writer.add_int64(4, val.int_value);
				break;
			case MVTValueType::BOOL:
				val_writer.add_bool(7, val.bool_value);
				break;
			default:
				throw InternalException("ST_AsMVT: Unsupported MVT value type");
			}
		}

		// Extent = 5
		layer_writer.add_uint32(5, extent);
	}
};

class MVTFeatureBuilder {
public:
	void Reset() {
		id = -1;
		geometry_type = 0;
		geometry.clear();
		tags.clear();
	}

	void SetId(int32_t value) {
		id = value;
	}

	void SetGeometry(const string_t &geom_blob) {

		BinaryReader cursor(geom_blob.GetData(), geom_blob.GetSize());
		const auto type = static_cast<sgl::geometry_type>(cursor.Read<uint8_t>() + 1);
		const auto flags = cursor.Read<uint8_t>();
		cursor.Skip(sizeof(uint16_t));
		cursor.Skip(sizeof(uint32_t)); // padding

		// Parse flags
		const auto has_z = (flags & 0x01) != 0;
		const auto has_m = (flags & 0x02) != 0;
		const auto has_bbox = (flags & 0x04) != 0;

		const auto format_v1 = (flags & 0x40) != 0;
		const auto format_v0 = (flags & 0x80) != 0;

		if (format_v1 || format_v0) {
			// Unsupported version, throw an error
			throw NotImplementedException(
			    "This geometry seems to be written with a newer version of the DuckDB spatial library that is not "
			    "compatible with this version. Please upgrade your DuckDB installation.");
		}

		if (has_bbox) {
			// Skip past bbox if present
			cursor.Skip(sizeof(float) * 2 * (2 + has_z + has_m));
		}

		// Read the first type
		cursor.Skip(sizeof(uint32_t));

		const auto vertex_width = (2 + (has_z ? 1 : 0) + (has_m ? 1 : 0)) * sizeof(double);
		const auto vertex_space = vertex_width - (2 * sizeof(double)); // Space for x and y

		switch (type) {
		case sgl::geometry_type::POINT: {
			geometry_type = 1; // MVT_POINT

			// Read the point geometry
			const auto vertex_count = cursor.Read<uint32_t>();
			if (vertex_count == 0) {
				// No vertices, skip
				throw InvalidInputException("ST_AsMVT: POINT geometry cant be empty");
			}
			const auto x = CastDouble(cursor.Read<double>());
			const auto y = CastDouble(cursor.Read<double>());
			cursor.Skip(vertex_space); // Skip z and m if present

			geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
			geometry.push_back(protozero::encode_zigzag32(x));
			geometry.push_back(protozero::encode_zigzag32(y));

		} break;
		case sgl::geometry_type::LINESTRING: {
			geometry_type = 2; // MVT_LINESTRING

			const auto vertex_count = cursor.Read<uint32_t>();
			if (vertex_count < 2) {
				// Invalid linestring, skip
				throw InvalidInputException("ST_AsMVT: LINESTRING geometry cant contain less than 2 vertices");
			}
			// Read the vertices
			int32_t cursor_x = 0;
			int32_t cursor_y = 0;

			for (uint32_t vertex_idx = 0; vertex_idx < vertex_count; vertex_idx++) {

				const auto x = CastDouble(cursor.Read<double>());
				const auto y = CastDouble(cursor.Read<double>());
				cursor.Skip(vertex_space); // Skip z and m if present

				if (vertex_idx == 0) {
					geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
					geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
					geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
					geometry.push_back((2 & 0x7) | ((vertex_count - 1) << 3)); // LineTo, part count
				} else {
					geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
					geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
				}

				cursor_x = x;
				cursor_y = y;
			}
		} break;
		case sgl::geometry_type::POLYGON: {
			geometry_type = 3; // MVT_POLYGON

			const auto part_count = cursor.Read<uint32_t>();
			if (part_count == 0) {
				// No parts, invalid
				throw InvalidInputException("ST_AsMVT: POLYGON geometry cant be empty");
			}

			int32_t cursor_x = 0;
			int32_t cursor_y = 0;

			auto ring_cursor = cursor;
			cursor.Skip((part_count * 4) + (part_count % 2 == 1 ? 4 : 0)); // Skip part types and padding
			for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
				const auto vertex_count = ring_cursor.Read<uint32_t>();
				if (vertex_count < 3) {
					// Invalid polygon, skip
					throw InvalidInputException("ST_AsMVT: POLYGON ring cant contain less than 3 vertices");
				}

				for (uint32_t vertex_idx = 0; vertex_idx < vertex_count; vertex_idx++) {
					const auto x = CastDouble(cursor.Read<double>());
					const auto y = CastDouble(cursor.Read<double>());
					cursor.Skip(vertex_space); // Skip z and m if present

					if (vertex_idx == 0) {
						geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
						geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
						geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
						geometry.push_back((2 & 0x7) | ((vertex_count - 2) << 3));

						cursor_x = x;
						cursor_y = y;

					} else if (vertex_idx == vertex_count - 1) {
						// Close the ring
						geometry.push_back((7 & 0x7) | (1 << 3)); // ClosePath
					} else {
						// Add the vertex
						geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
						geometry.push_back(protozero::encode_zigzag32(y - cursor_y));

						cursor_x = x;
						cursor_y = y;
					}
				}
			}
		} break;
		case sgl::geometry_type::MULTI_POINT: {
			geometry_type = 1; // MVT_POINT

			const auto part_count = cursor.Read<uint32_t>();
			if (part_count == 0) {
				throw InvalidInputException("ST_AsMVT: MULTI_POINT geometry cant be empty");
			}

			int32_t cursor_x = 0;
			int32_t cursor_y = 0;

			geometry.push_back((1 & 0x7) | (part_count << 3)); // MoveTo, part count

			// Read the parts
			for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
				cursor.Skip(sizeof(uint32_t)); // Skip part type
				const auto vertex_count = cursor.Read<uint32_t>();
				if (vertex_count == 0) {
					// No vertices, skip
					throw InvalidInputException("ST_AsMVT: POINT geometry cant be empty");
				}

				const auto x = CastDouble(cursor.Read<double>());
				const auto y = CastDouble(cursor.Read<double>());
				cursor.Skip(vertex_space); // Skip z and m if present

				geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
				geometry.push_back(protozero::encode_zigzag32(y - cursor_y));

				cursor_x = x;
				cursor_y = y;
			}
		} break;
		case sgl::geometry_type::MULTI_LINESTRING: {
			geometry_type = 2; // MVT_LINESTRING

			// Read the multi-linestring geometry
			const auto part_count = cursor.Read<uint32_t>();
			if (part_count == 0) {
				// No parts, invalid
				throw InvalidInputException("ST_AsMVT: MULTI_LINESTRING geometry cant be empty");
			}
			int32_t cursor_x = 0;
			int32_t cursor_y = 0;

			for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
				cursor.Skip(sizeof(uint32_t)); // Skip part type
				const auto vertex_count = cursor.Read<uint32_t>();

				if (vertex_count < 2) {
					// Invalid linestring, skip
					throw InvalidInputException("ST_AsMVT: LINESTRING geometry cant contain less than 2 vertices");
				}

				for (uint32_t vertex_idx = 0; vertex_idx < vertex_count; vertex_idx++) {

					const auto x = CastDouble(cursor.Read<double>());
					const auto y = CastDouble(cursor.Read<double>());
					cursor.Skip(vertex_space); // Skip z and m if present

				if (vertex_idx == 0) {
					geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
					geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
					geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
					geometry.push_back((2 & 0x7) | ((vertex_count - 1) << 3)); // LineTo, part count
				} else {
					geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
					geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
				}

					cursor_x = x;
					cursor_y = y;
				}
			}
		} break;
		case sgl::geometry_type::MULTI_POLYGON: {
			geometry_type = 3; // MVT_POLYGON

			// Read the multi-linestring geometry
			const auto poly_count = cursor.Read<uint32_t>();
			if (poly_count == 0) {
				// No parts, invalid
				throw InvalidInputException("ST_AsMVT: MULTI_POLYGON geometry cant be empty");
			}

			int32_t cursor_x = 0;
			int32_t cursor_y = 0;

			for (uint32_t poly_idx = 0; poly_idx < poly_count; poly_idx++) {
				cursor.Skip(sizeof(uint32_t)); // Skip part type
				const auto part_count = cursor.Read<uint32_t>();
				if (part_count == 0) {
					// No parts, invalid
					throw InvalidInputException("ST_AsMVT: POLYGON geometry cant be empty");
				}

				auto ring_cursor = cursor;
				cursor.Skip((part_count * 4) + (part_count % 2 == 1 ? 4 : 0)); // Skip part types and padding

				for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
					const auto vertex_count = ring_cursor.Read<uint32_t>();
					if (vertex_count < 3) {
						// Invalid polygon, skip
						throw InvalidInputException("ST_AsMVT: POLYGON ring cant contain less than 3 vertices");
					}

					for (uint32_t vertex_idx = 0; vertex_idx < vertex_count; vertex_idx++) {
						const auto x = CastDouble(cursor.Read<double>());
						const auto y = CastDouble(cursor.Read<double>());
						cursor.Skip(vertex_space); // Skip z and m if present

						if (vertex_idx == 0) {
							geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
							geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
							geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
							geometry.push_back((2 & 0x7) | ((vertex_count - 2) << 3));

							cursor_x = x;
							cursor_y = y;

						} else if (vertex_idx == vertex_count - 1) {
							// Close the ring
							geometry.push_back((7 & 0x7) | (1 << 3)); // ClosePath
						} else {
							// Add the vertex
							geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
							geometry.push_back(protozero::encode_zigzag32(y - cursor_y));

							cursor_x = x;
							cursor_y = y;
						}
					}
				}
			}
		} break;
		case sgl::geometry_type::GEOMETRY_COLLECTION: {
			throw InvalidInputException("ST_AsMVT: Geometries of type \"GEOMETRYCOLLECTION\" are not supported");
		} break;
		default:
			throw InvalidInputException("ST_AsMVT: unsupported geometry type %d", static_cast<int>(type));
		}
	}

	void AddProperty(idx_t key, const string_t &value, ArenaAllocator &allocator) {
		// We need to copy the string into the arena, as the input string might be temporary

		MVTValue v;
		v.type = MVTValueType::STRING;
		v.size = static_cast<uint32_t>(value.GetSize());

		if (value.GetSize() != 0) {
			const auto str_mem = allocator.Allocate(value.GetSize());
			memcpy(str_mem, value.GetData(), value.GetSize());
			v.string_value = const_char_ptr_cast(str_mem);
		}

		tags.emplace_back(static_cast<uint32_t>(key), v);
	}

	void AddProperty(uint32_t key, float value) {
		MVTValue v;
		v.type = MVTValueType::FLOAT;
		v.float_value = value;

		tags.emplace_back(key, v);
	}

	void AddProperty(uint32_t key, double value) {
		MVTValue v;
		v.type = MVTValueType::DOUBLE;
		v.double_value = value;
		tags.emplace_back(key, v);
	}

	void AddProperty(uint32_t key, bool value) {
		MVTValue v;
		v.type = MVTValueType::BOOL;
		v.bool_value = value;

		tags.emplace_back(key, v);
	}

	void AddProperty(uint32_t key, int64_t value) {
		MVTValue v;
		v.type = MVTValueType::INT;
		v.int_value = value;

		tags.emplace_back(key, v);
	}

	void AddProperty(uint32_t key, int32_t value) {
		AddProperty(key, static_cast<int64_t>(value));
	}

	bool IsEmpty() const {
		return geometry.empty();
	}

	void Finalize(ArenaAllocator &arena, MVTLayer &layer) {
		if (geometry.empty()) {
			// No geometry, skip
			return;
		}

		const auto feature_mem = arena.AllocateAligned(sizeof(MVTFeature));
		const auto feature_ptr = new (feature_mem) MVTFeature();

		feature_ptr->next = nullptr;
		feature_ptr->id = id;
		feature_ptr->type = geometry_type;

		// Copy over the geometry data
		feature_ptr->geom_array_data =
		    reinterpret_cast<uint32_t *>(arena.AllocateAligned(geometry.size() * sizeof(uint32_t)));
		feature_ptr->geom_array_size = static_cast<uint32_t>(geometry.size());
		memcpy(feature_ptr->geom_array_data, geometry.data(), geometry.size() * sizeof(uint32_t));

		// Copy over the tags
		feature_ptr->tags_array_size = static_cast<uint32_t>(tags.size());
		if (feature_ptr->tags_array_size != 0) {

			feature_ptr->tags_array_keys =
			    reinterpret_cast<uint32_t *>(arena.AllocateAligned(feature_ptr->tags_array_size * sizeof(uint32_t)));
			feature_ptr->tags_array_vals =
			    reinterpret_cast<MVTValue *>(arena.AllocateAligned(feature_ptr->tags_array_size * sizeof(MVTValue)));

			for (idx_t i = 0; i < tags.size(); i++) {
				feature_ptr->tags_array_keys[i] = tags[i].first;
				feature_ptr->tags_array_vals[i] = tags[i].second;
			}
		}

		// Append to the layer
		if (layer.features_tail) {
			layer.features_tail->next = feature_ptr;
			layer.features_tail = feature_ptr;
		} else {
			layer.features_head = feature_ptr;
			layer.features_tail = feature_ptr;
		}
	}

private:
	static int32_t CastDouble(double d) {
		if (d < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
		    d > static_cast<double>(std::numeric_limits<int32_t>::max())) {
			throw InvalidInputException("ST_AsMVT: coordinate out of range for int32_t");
		}
		return static_cast<int32_t>(d);
	}

	int32_t id = -1;
	uint32_t geometry_type = 0;
	vector<uint32_t> geometry;
	vector<pair<uint32_t, MVTValue>> tags;
};

struct ST_AsMVT {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : FunctionData {

		idx_t geometry_column_idx = 0;
		string layer_name = "layer";
		int32_t extent = 4096;
		vector<string> tag_names;
		optional_idx feature_id_column_idx = optional_idx::Invalid();

		unique_ptr<FunctionData> Copy() const override {
			auto result = make_uniq<BindData>();
			result->geometry_column_idx = geometry_column_idx;
			result->layer_name = layer_name;
			result->extent = extent;
			result->tag_names = tag_names;
			result->feature_id_column_idx = feature_id_column_idx;
			return std::move(result);
		}

		bool Equals(const FunctionData &other_p) const override {
			auto &other = other_p.Cast<BindData>();
			return geometry_column_idx == other.geometry_column_idx && layer_name == other.layer_name &&
			       extent == other.extent && tag_names == other.tag_names &&
			       feature_id_column_idx == other.feature_id_column_idx;
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, AggregateFunction &function,
	                                     vector<unique_ptr<Expression>> &arguments) {
		auto result = make_uniq<BindData>();

		// Figure part of the row is the geometry column
		const auto &row_type = arguments[0]->return_type;
		if (row_type.id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException("ST_AsMVT: first argument must be a STRUCT (i.e. a row type)");
		}

		// Fold all the other parameters
		auto folded_layer = false;
		auto folded_extent = false;
		auto folded_geom = false;
		auto folded_feature = false;

		if (arguments.size() >= 2) {
			auto &layer_expr = arguments[1];
			if (layer_expr->IsFoldable()) {
				auto layer_val = ExpressionExecutor::EvaluateScalar(context, *layer_expr);
				if (!layer_val.IsNull()) {
					result->layer_name = StringValue::Get(layer_val);
					if (result->layer_name.empty()) {
						throw InvalidInputException("ST_AsMVT: layer name cannot be empty");
					}
				}
				folded_layer = true;
			} else {
				throw InvalidInputException("ST_AsMVT: layer name must be a constant string");
			}
		}

		if (arguments.size() >= 3) {
			auto &extent_expr = arguments[2];
			if (extent_expr->IsFoldable()) {
				auto extent_val = ExpressionExecutor::EvaluateScalar(context, *extent_expr);
				if (extent_val.IsNull()) {
					throw InvalidInputException("ST_AsMVT: extent cannot be NULL");
				}
				result->extent = IntegerValue::Get(extent_val);
				if (result->extent == 0) {
					throw InvalidInputException("ST_AsMVT: extent must be greater than zero");
				}
				folded_extent = true;
			} else {
				throw InvalidInputException("ST_AsMVT: extent must be a constant integer");
			}
		}
		string geom_name;
		if (arguments.size() >= 4) {
			auto &geom_expr = arguments[3];
			if (geom_expr->IsFoldable()) {
				auto geom_val = ExpressionExecutor::EvaluateScalar(context, *geom_expr);
				if (!geom_val.IsNull()) {
					geom_name = StringValue::Get(geom_val);
					if (geom_name.empty()) {
						throw InvalidInputException("ST_AsMVT: geometry column name cannot be empty");
					}
				}
				folded_geom = true;
			} else {
				throw InvalidInputException("ST_AsMVT: geometry column name must be a constant string");
			}
		}

		string feature_id_name;
		if (arguments.size() >= 5) {
			auto &feature_expr = arguments[4];
			if (feature_expr->IsFoldable()) {
				auto feature_val = ExpressionExecutor::EvaluateScalar(context, *feature_expr);
				if (!feature_val.IsNull()) {
					feature_id_name = StringValue::Get(feature_val);
					if (feature_id_name.empty()) {
						throw InvalidInputException("ST_AsMVT: feature id column name cannot be empty");
					}
				}
				folded_feature = true;
			} else {
				throw InvalidInputException("ST_AsMVT: feature id column name must be a constant string");
			}
		}

		// Fetch the geometry column index, either based on name or on position
		optional_idx geom_idx = optional_idx::Invalid();
		if (geom_name.empty()) {
			// Look for the first geometry column
			for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
				auto &child = StructType::GetChildType(row_type, i);
				if (child == GeoTypes::GEOMETRY()) {
					if (geom_idx != optional_idx::Invalid()) {
						throw InvalidInputException("ST_AsMVT: only one geometry column is allowed in the input row");
					}
					geom_idx = i;
				}
			}
		} else {
			// Look for the geometry column by name
			for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
				auto &child = StructType::GetChildType(row_type, i);
				auto &child_name = StructType::GetChildName(row_type, i);
				if (child == GeoTypes::GEOMETRY() && child_name == geom_name) {
					if (geom_idx != optional_idx::Invalid()) {
						throw InvalidInputException("ST_AsMVT: only one geometry column is allowed in the input row");
					}
					geom_idx = i;
				}
			}
		}
		if (!geom_idx.IsValid()) {
			throw InvalidInputException("ST_AsMVT: input row must contain a geometry column");
		}

		result->geometry_column_idx = geom_idx.GetIndex();

		// Fetch the feature id column index, based on name if provided
		if (!feature_id_name.empty()) {
			// Look for the feature id column by name
			for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
				auto &child_name = StructType::GetChildName(row_type, i);
				if (child_name == feature_id_name) {
					if (result->feature_id_column_idx.IsValid()) {
						throw InvalidInputException("ST_AsMVT: only one feature id column is allowed in the input row");
					}
					auto &child_type = StructType::GetChildType(row_type, i);
					if (child_type != LogicalTypeId::INTEGER && child_type != LogicalTypeId::BIGINT) {
						throw InvalidInputException("ST_AsMVT: feature id column must be of type INTEGER or BIGINT");
					}
					result->feature_id_column_idx = i;
				}
			}
			if (!result->feature_id_column_idx.IsValid()) {
				throw InvalidInputException("ST_AsMVT: feature id column not found in input row");
			}
		}

		unordered_set<LogicalTypeId> valid_property_types = {LogicalTypeId::VARCHAR, LogicalTypeId::FLOAT,
		                                                     LogicalTypeId::DOUBLE,  LogicalTypeId::INTEGER,
		                                                     LogicalTypeId::BIGINT,  LogicalTypeId::BOOLEAN};

		// Collect tag names
		for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
			if (i != result->geometry_column_idx &&
			    (!result->feature_id_column_idx.IsValid() || i != result->feature_id_column_idx.GetIndex())) {
				auto &name = StructType::GetChildName(row_type, i);
				auto &type = StructType::GetChildType(row_type, i);

				if (valid_property_types.find(type.id()) == valid_property_types.end()) {
					auto type_name = type.ToString();
					throw InvalidInputException("ST_AsMVT: property column \"%s\" has unsupported type \"%s\"\n"
					                            "Only the following property types are supported: VARCHAR, FLOAT, "
					                            "DOUBLE, INTEGER, BIGINT, BOOLEAN",
					                            name.c_str(), type_name.c_str());
				}
				result->tag_names.push_back(name);
			}
		}

		// Erase arguments, back to front
		if (folded_feature) {
			Function::EraseArgument(function, arguments, 4);
		}
		if (folded_geom) {
			Function::EraseArgument(function, arguments, 3);
		}
		if (folded_extent) {
			Function::EraseArgument(function, arguments, 2);
		}
		if (folded_layer) {
			Function::EraseArgument(function, arguments, 1);
		}

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Initialize
	//------------------------------------------------------------------------------------------------------------------
	struct State {
		MVTLayer layer;
	};

	static idx_t StateSize(const AggregateFunction &) {
		return sizeof(State);
	}

	static void Initialize(const AggregateFunction &, data_ptr_t state_mem) {
		new (state_mem) State();
	}

	//------------------------------------------------------------------------------------------------------------------
	// Update
	//------------------------------------------------------------------------------------------------------------------
	static void Update(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &state_vec, idx_t count) {
		const auto &bdata = aggr.bind_data->Cast<BindData>();
		const auto &row_cols = StructVector::GetEntries(inputs[0]);

		UnifiedVectorFormat state_format;
		UnifiedVectorFormat geom_format;
		UnifiedVectorFormat fid_format;
		LogicalType fid_type;

		vector<UnifiedVectorFormat> property_formats;
		vector<LogicalType> property_types;

		state_vec.ToUnifiedFormat(count, state_format);

		for (idx_t col_idx = 0; col_idx < row_cols.size(); col_idx++) {
			if (col_idx == bdata.geometry_column_idx) {
				row_cols[col_idx]->ToUnifiedFormat(count, geom_format);
			} else if (bdata.feature_id_column_idx.IsValid() && col_idx == bdata.feature_id_column_idx.GetIndex()) {
				row_cols[col_idx]->ToUnifiedFormat(count, fid_format);
				fid_type = row_cols[col_idx]->GetType();
			} else {
				property_formats.emplace_back();
				row_cols[col_idx]->ToUnifiedFormat(count, property_formats.back());
				property_types.push_back(row_cols[col_idx]->GetType());
			}
		}

		// Reusable geometry buffer
		MVTFeatureBuilder feature;

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			const auto state_idx = state_format.sel->get_index(row_idx);
			auto &layer = UnifiedVectorFormat::GetData<State *>(state_format)[state_idx]->layer;

			const auto geom_idx = geom_format.sel->get_index(row_idx);
			if (!geom_format.validity.RowIsValid(geom_idx)) {
				// Skip if geometry is NULL
				continue;
			}

			auto &geom_blob = UnifiedVectorFormat::GetData<string_t>(geom_format)[geom_idx];

			// Reset the feature
			feature.Reset();

			// Set geometry
			feature.SetGeometry(geom_blob);

			if (feature.IsEmpty()) {
				// No geometry, skip
				continue;
			}

			// Do we have a feature id?
			if (bdata.feature_id_column_idx.IsValid()) {
				const auto fid_idx = fid_format.sel->get_index(row_idx);
				if (fid_format.validity.RowIsValid(fid_idx)) {
					// Set the feature id
					switch (fid_type.id()) {
					case LogicalTypeId::TINYINT: {
						auto &fid_val = UnifiedVectorFormat::GetData<int8_t>(fid_format)[fid_idx];
						feature.SetId(fid_val);
					} break;
					case LogicalTypeId::SMALLINT: {
						auto &fid_val = UnifiedVectorFormat::GetData<int16_t>(fid_format)[fid_idx];
						feature.SetId(fid_val);
					}
					case LogicalTypeId::INTEGER: {
						auto &fid_val = UnifiedVectorFormat::GetData<int32_t>(fid_format)[fid_idx];
						feature.SetId(fid_val);
					} break;
					case LogicalTypeId::BIGINT: {
						auto &fid_val = UnifiedVectorFormat::GetData<int64_t>(fid_format)[fid_idx];
						if (fid_val < std::numeric_limits<int32_t>::min() ||
						    fid_val > std::numeric_limits<int32_t>::max()) {
							throw InvalidInputException("ST_AsMVT: feature id out of range for int32");
						}
						feature.SetId(static_cast<int32_t>(fid_val));
					} break;
					default:
						throw InvalidInputException("ST_AsMVT: feature id column must be of type INTEGER or BIGINT");
					}
				}
			}

			// Add properties
			for (idx_t prop_vec_idx = 0; prop_vec_idx < property_formats.size(); prop_vec_idx++) {
				const auto &prop_format = property_formats[prop_vec_idx];
				const auto prop_row_idx = prop_format.sel->get_index(row_idx);
				if (!prop_format.validity.RowIsValid(prop_row_idx)) {
					// Skip if property is NULL
					continue;
				}

				// Switch on property type
				auto &prop_type = property_types[prop_vec_idx];
				switch (prop_type.id()) {
				case LogicalTypeId::VARCHAR: {
					auto &prop_val = UnifiedVectorFormat::GetData<string_t>(prop_format)[prop_row_idx];
					feature.AddProperty(prop_vec_idx, prop_val, aggr.allocator);
				} break;
				case LogicalTypeId::FLOAT: {
					auto &prop_val = UnifiedVectorFormat::GetData<float>(prop_format)[prop_row_idx];
					feature.AddProperty(prop_vec_idx, prop_val);
				} break;
				case LogicalTypeId::DOUBLE: {
					auto &prop_val = UnifiedVectorFormat::GetData<double>(prop_format)[prop_row_idx];
					feature.AddProperty(prop_vec_idx, prop_val);
				} break;
				case LogicalTypeId::INTEGER: {
					auto &prop_val = UnifiedVectorFormat::GetData<int32_t>(prop_format)[prop_row_idx];
					feature.AddProperty(prop_vec_idx, prop_val);
				} break;
				case LogicalTypeId::BIGINT: {
					auto &prop_val = UnifiedVectorFormat::GetData<int64_t>(prop_format)[prop_row_idx];
					feature.AddProperty(prop_vec_idx, prop_val);
				} break;
				case LogicalTypeId::BOOLEAN: {
					auto &prop_val = UnifiedVectorFormat::GetData<bool>(prop_format)[prop_row_idx];
					feature.AddProperty(prop_vec_idx, prop_val);
				} break;
				default:
					throw InvalidInputException("ST_AsMVT: unsupported property type: %s", prop_type.ToString());
				}
			}

			feature.Finalize(aggr.allocator, layer);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Combine
	//------------------------------------------------------------------------------------------------------------------
	static void Combine(Vector &source_vec, Vector &target_vec, AggregateInputData &aggr, idx_t count) {
		UnifiedVectorFormat source_format;
		source_vec.ToUnifiedFormat(count, source_format);

		const auto source_ptr = UnifiedVectorFormat::GetData<State *>(source_format);
		const auto target_ptr = FlatVector::GetData<State *>(target_vec);

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			auto &source = *source_ptr[source_format.sel->get_index(row_idx)];
			auto &target = *target_ptr[row_idx];

			if (aggr.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE) {
				// Absorb the feature data from source into target
				target.layer.Absorb(source.layer);
			} else {
				// Append the feature data from source to target
				target.layer.Combine(aggr.allocator, source.layer);
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Finalize
	//------------------------------------------------------------------------------------------------------------------
	static void Finalize(Vector &state_vec, AggregateInputData &aggr, Vector &result, idx_t count, idx_t offset) {
		const auto &bdata = aggr.bind_data->Cast<BindData>();

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);
		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);

		vector<char> buffer;
		MVTValueSet tag_dict;

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			auto &state = *state_ptr[state_format.sel->get_index(raw_idx)];
			const auto out_idx = raw_idx + offset;

			buffer.clear();
			tag_dict.Clear();

			state.layer.Finalize(bdata.extent, bdata.tag_names, bdata.layer_name, buffer, tag_dict);

			// Now we have the layer buffer, we can write it to the result vector
			const auto result_data = FlatVector::GetData<string_t>(result);
			result_data[out_idx] = StringVector::AddStringOrBlob(result, buffer.data(), buffer.size());
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Docs
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Make a Mapbox Vector Tile from a set of geometries and properties
		The function takes as input a row type (STRUCT) containing a geometry column and any number of property columns.
		It returns a single binary BLOB containing the Mapbox Vector Tile.

		The function has the following signature:

		`ST_AsMVT(row STRUCT, layer_name VARCHAR DEFAULT 'layer', extent INTEGER DEFAULT 4096, geom_column_name VARCHAR DEFAULT NULL, feature_id_column_name VARCHAR DEFAULT NULL) -> BLOB`

		- The first argument is a struct containing the geometry and properties.
		- The second argument is the name of the layer in the vector tile. This argument is optional and defaults to 'layer'.
		- The third argument is the extent of the tile. This argument is optional and defaults to 4096.
		- The fourth argument is the name of the geometry column in the input row. This argument is optional. If not provided, the first geometry column in the input row will be used. If multiple geometry columns are present, an error will be raised.
		- The fifth argument is the name of the feature id column in the input row. This argument is optional. If provided, the values in this column will be used as feature ids in the vector tile. The column must be of type INTEGER or BIGINT. If set to negative or NULL, a feature id will not be assigned to the corresponding feature.

		The input struct must contain exactly one geometry column of type GEOMETRY. It can contain any number of property columns of types VARCHAR, FLOAT, DOUBLE, INTEGER, BIGINT, or BOOLEAN.

		Example:
		```sql
		SELECT ST_AsMVT({'geom': geom, 'id': id, 'name': name}, 'cities', 4096, 'geom', 'id') AS tile
		FROM cities;
		 ```

		This example creates a vector tile named 'cities' with an extent of 4096 from the 'cities' table, using 'geom' as the geometry column and 'id' as the feature id column.

		However, you probably want to use the ST_AsMVTGeom function to first transform and clip your geometries to the tile extent.
		The following example assumes the geometry is in WebMercator ("EPSG:3857") coordinates.
		Replace `{z}`, `{x}`, and `{y}` with the appropriate tile coordinates, `{your table}` with your table name, and `{tile_path}` with the path to write the tile to.

		```sql
		COPY (
	        SELECT ST_AsMVT({{
	            "geometry": ST_AsMVTGeom(
	                geometry,
	                ST_Extent(ST_TileEnvelope({z}, {x}, {y})),
	                4096,
	                256,
	                false
	            )
	        }})
	        FROM {your table} WHERE ST_Intersects(geometry, ST_TileEnvelope({z}, {x}, {y}))
		) to {tile_path} (FORMAT 'BLOB');
		```
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {

		FunctionBuilder::RegisterAggregate(loader, "ST_AsMVT", [&](AggregateFunctionBuilder &func) {
			// name, extent, layer_name, feature_id_name
			const auto optional_args = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
			                            LogicalType::VARCHAR};
			AggregateFunction agg({LogicalTypeId::ANY}, LogicalType::BLOB, StateSize, Initialize, Update, Combine,
			                      Finalize, nullptr, Bind);

			// Push the variants∂
			func.SetFunction(agg);
			for (auto &arg_type : optional_args) {
				// Register all the variants with optional arguments
				agg.arguments.push_back(arg_type);
				func.SetFunction(agg);
			}

			func.SetDescription(DESCRIPTION);
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

} // namespace
//======================================================================================================================
//  Register
//======================================================================================================================
void RegisterMapboxVectorTileModule(ExtensionLoader &loader) {
	ST_TileEnvelope::Register(loader);
	ST_AsMVT::Register(loader);
}

} // namespace duckdb
