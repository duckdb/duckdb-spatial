// Mapbox Vector Tiles (MVT) implementation

#include "spatial/modules/mvt/mvt_module.hpp"

#include "duckdb/common/types/list_segment.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"

#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/binary_reader.hpp"
#include "spatial/util/function_builder.hpp"

#include "protozero/pbf_writer.hpp"

namespace duckdb {

namespace {
// ######################################################################################################################
// Util
// ######################################################################################################################

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

} // namespace

namespace {

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
		if ((tile_x < 0) || ((uint32_t)tile_x >= zoom_extent)) {
			throw InvalidInputException("ST_TileEnvelope: tile_x is out of range for specified tile_zoom");
		}
		if ((tile_y < 0) || ((uint32_t)tile_y >= zoom_extent)) {
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
	static void Register(DatabaseInstance &db) {
		FunctionBuilder::RegisterScalar(db, "ST_TileEnvelope", [](ScalarFunctionBuilder &func) {
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

} // namespace

//======================================================================================================================
// ST_AsMVT
//======================================================================================================================
namespace {

struct ST_AsMVT {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : FunctionData {
		string layer_name = "layer";
		int32_t extent = 4096;
		idx_t geom_idx = 0;
		string feature_id_name = "feature_id";
		vector<string> property_names;
		vector<LogicalType> property_types;

		unique_ptr<FunctionData> Copy() const override {
			auto copy = make_uniq<BindData>();
			copy->layer_name = layer_name;
			copy->extent = extent;
			copy->geom_idx = geom_idx;
			copy->feature_id_name = feature_id_name;
			copy->property_names = property_names;
			copy->property_types = property_types;
			return std::move(copy);
		}

		bool Equals(const FunctionData &other) const override {
			auto &other_data = other.Cast<BindData>();
			return layer_name == other_data.layer_name && extent == other_data.extent &&
			       geom_idx == other_data.geom_idx && feature_id_name == other_data.feature_id_name &&
			       property_names == other_data.property_names && property_types == other_data.property_types;
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, AggregateFunction &function,
	                                     vector<unique_ptr<Expression>> &arguments) {
		auto result = make_uniq<BindData>();

		// Validate and set optional arguments
		if (arguments.size() > 1) {
			const auto &name_arg = arguments[1];
			if (name_arg->return_type.id() != LogicalTypeId::VARCHAR) {
				throw InvalidInputException("ST_AsMVT: second argument must be a VARCHAR");
			}
			if (!name_arg->IsFoldable()) {
				throw InvalidInputException("ST_AsMVT: second argument must be a constant string");
			}
			result->layer_name = ExpressionExecutor::EvaluateScalar(context, *name_arg).GetValue<string>();
			Function::EraseArgument(function, arguments, 1);
		}

		if (arguments.size() > 2) {
			const auto &extent_arg = arguments[2];
			if (extent_arg->return_type.id() != LogicalTypeId::INTEGER) {
				throw InvalidInputException("ST_AsMVT: third argument must be an INTEGER");
			}
			if (!extent_arg->IsFoldable()) {
				throw InvalidInputException("ST_AsMVT: third argument must be a constant integer");
			}
			result->extent = ExpressionExecutor::EvaluateScalar(context, *extent_arg).GetValue<int32_t>();
			Function::EraseArgument(function, arguments, 2);
		}

		string geom_name;
		if (arguments.size() > 3) {
			const auto &geom_name_arg = arguments[3];
			if (geom_name_arg->return_type.id() != LogicalTypeId::VARCHAR) {
				throw InvalidInputException("ST_AsMVT: fourth argument must be a VARCHAR");
			}
			if (!geom_name_arg->IsFoldable()) {
				throw InvalidInputException("ST_AsMVT: fourth argument must be a constant string");
			}
			geom_name = ExpressionExecutor::EvaluateScalar(context, *geom_name_arg).GetValue<string>();
			Function::EraseArgument(function, arguments, 3);
		}

		if (arguments.size() > 4) {
			const auto &feature_id_arg = arguments[4];
			if (feature_id_arg->return_type.id() != LogicalTypeId::VARCHAR) {
				throw InvalidInputException("ST_AsMVT: fifth argument must be a VARCHAR");
			}
			if (!feature_id_arg->IsFoldable()) {
				throw InvalidInputException("ST_AsMVT: fifth argument must be a constant string");
			}
			result->feature_id_name = ExpressionExecutor::EvaluateScalar(context, *feature_id_arg).GetValue<string>();
			Function::EraseArgument(function, arguments, 4);
		}

		// Find the geometry column index in the row
		const auto &row = arguments[0];
		const auto &row_type = row->return_type;
		if (row_type.id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException("ST_AsMVT: first argument must be a STRUCT");
		}

		auto geom_idx = optional_idx::Invalid();

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

		result->geom_idx = geom_idx.GetIndex();

		// TODO: Cast properties to supported types
		for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
			if (i == result->geom_idx) {
				continue; // Skip the geometry column
			}
			auto &child_name = StructType::GetChildName(row_type, i);
			auto &child_type = StructType::GetChildType(row_type, i);
			result->property_names.push_back(child_name);
			result->property_types.push_back(child_type);
		}

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// State
	//------------------------------------------------------------------------------------------------------------------
	struct Feature {
		vector<uint32_t> geometry;
		vector<pair<uint32_t, uint32_t>> tags;
		uint32_t type;
	};

	struct Layer {
		vector<Feature> features;

		vector<string_t> vals;
		unordered_map<string_t, uint32_t> val_map;

		void Combine(ArenaAllocator &arena, const Layer &other) {
			// Copy the features over
			for (auto feature : other.features) {
				// Check if we need to add new keys
				for (auto &tag : feature.tags) {

					// Map the old to the new value
					auto &old_val = other.vals[tag.second];
					auto val_it = val_map.find(old_val);
					if (val_it == val_map.end()) {
						// This is a new value, add it to the value map
						const auto new_val_idx = vals.size();

						if (old_val.IsInlined()) {
							vals.push_back(old_val);
							val_map[old_val] = new_val_idx;
						} else {
							const auto mem = arena.Allocate(old_val.GetSize());
							memcpy(mem, old_val.GetData(), old_val.GetSize());
							auto new_val = string_t(const_char_ptr_cast(mem), old_val.GetSize());

							vals.push_back(new_val);
							val_map[new_val] = new_val_idx;
						}
						tag.second = new_val_idx;
					} else {
						// This value already exists, replace it in the feature
						tag.second = val_it->second;
					}
				}
				features.push_back(std::move(feature));
			}
		}

		uint32_t AddValue(ArenaAllocator &arena, int32_t value) {

			// Encode as protobuf int64_t
			string encoded_value;
			{
				protozero::pbf_writer writer(encoded_value);
				writer.add_int64(4, value);
			}
			if (encoded_value.size() < string_t::INLINE_BYTES) {
				return Intern(string_t(encoded_value));
			}
			// If the encoded value is too large, we need to store it as a blob
			const auto mem = arena.Allocate(encoded_value.size());
			memcpy(mem, encoded_value.data(), encoded_value.size());
			return Intern(string_t(const_char_ptr_cast(mem), encoded_value.size()));
		}

		uint32_t AddValue(ArenaAllocator &arena, double value) {
			string encoded_value;
			{
				protozero::pbf_writer writer(encoded_value);
				writer.add_double(3, value);
			}
			if (encoded_value.size() < string_t::INLINE_BYTES) {
				return Intern(string_t(encoded_value));
			}
			// If the encoded value is too large, we need to store it as a blob
			const auto mem = arena.Allocate(encoded_value.size());
			memcpy(mem, encoded_value.data(), encoded_value.size());
			return Intern(string_t(const_char_ptr_cast(mem), encoded_value.size()));
		}

		uint32_t AddValue(ArenaAllocator &arena, const string_t &value) {
			string encoded_value;
			{
				protozero::pbf_writer writer(encoded_value);
				writer.add_string(1, value.GetData(), value.GetSize());
			}
			if (encoded_value.size() < string_t::INLINE_BYTES) {
				return Intern(string_t(encoded_value));
			}
			// If the value is too large, we need to store it as a blob
			const auto mem = arena.Allocate(encoded_value.size());
			memcpy(mem, encoded_value.data(), encoded_value.size());
			return Intern(string_t(const_char_ptr_cast(mem), encoded_value.size()));
		}

	private:
		uint32_t Intern(const string_t &value) {
			const auto it = val_map.find(value);
			if (it != val_map.end()) {
				return it->second;
			}
			// This is a new value, add it to the value map
			const auto new_val_idx = vals.size();
			vals.push_back(value);
			val_map[value] = new_val_idx;
			return new_val_idx;
		}
	};

	struct State {
		Layer layer;
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
	static int32_t CastDouble(double d) {
		if (d < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
		    d > static_cast<double>(std::numeric_limits<int32_t>::max())) {
			throw InvalidInputException("ST_AsMVT: coordinate out of range for int32_t");
		}
		return static_cast<int32_t>(d);
	}

	static void Update(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &state_vec,
	                   idx_t count) {

		const auto &bdata = aggr_input_data.bind_data->Cast<BindData>();
		const auto &row_cols = StructVector::GetEntries(inputs[0]);

		UnifiedVectorFormat state_format;
		UnifiedVectorFormat geom_format;
		vector<UnifiedVectorFormat> property_formats;

		state_vec.ToUnifiedFormat(count, state_format);

		for (idx_t col_idx = 0; col_idx < row_cols.size(); col_idx++) {
			if (col_idx == bdata.geom_idx) {
				row_cols[col_idx]->ToUnifiedFormat(count, geom_format);
			} else {
				property_formats.emplace_back();
				row_cols[col_idx]->ToUnifiedFormat(count, property_formats.back());
			}
		}

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			auto &state = *UnifiedVectorFormat::GetData<State *>(state_format)[state_format.sel->get_index(row_idx)];

			const auto geom_row_idx = geom_format.sel->get_index(row_idx);
			if (!geom_format.validity.RowIsValid(geom_row_idx)) {
				// Skip if geometry is NULL
				continue;
			}

			Feature feature;

			auto &geom_blob = UnifiedVectorFormat::GetData<string_t>(geom_format)[geom_row_idx];
			// Deserialize the geometry

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
				feature.type = 1; // MVT_POINT

				// Read the point geometry
				const auto vertex_count = cursor.Read<uint32_t>();
				if (vertex_count == 0) {
					// No vertices, skip
					throw InvalidInputException("ST_AsMVT: POINT geometry cant be empty");
				}
				const auto x = CastDouble(cursor.Read<double>());
				const auto y = CastDouble(cursor.Read<double>());
				cursor.Skip(vertex_space); // Skip z and m if present

				feature.geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
				feature.geometry.push_back(protozero::encode_zigzag32(x));
				feature.geometry.push_back(protozero::encode_zigzag32(y));

			} break;
			case sgl::geometry_type::LINESTRING: {
				feature.type = 2; // MVT_LINESTRING

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
						feature.geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
						feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
						feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
						feature.geometry.push_back((2 & 0x7) | ((vertex_count - 1) << 3)); // LineTo, part count
					} else {
						feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
						feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
					}

					cursor_x = x;
					cursor_y = y;
				}
			} break;
			case sgl::geometry_type::POLYGON: {
				feature.type = 3; // MVT_POLYGON

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
							feature.geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
							feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
							feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
							feature.geometry.push_back((2 & 0x7) | ((vertex_count - 2) << 3));

							cursor_x = x;
							cursor_y = y;

						} else if (vertex_idx == vertex_count - 1) {
							// Close the ring
							feature.geometry.push_back((7 & 0x7) | (1 << 3)); // ClosePath
						} else {
							// Add the vertex
							feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
							feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));

							cursor_x = x;
							cursor_y = y;
						}
					}
				}
			} break;
			case sgl::geometry_type::MULTI_POINT: {
				feature.type = 1; // MVT_POINT

				const auto part_count = cursor.Read<uint32_t>();
				if (part_count == 0) {
					throw InvalidInputException("ST_AsMVT: MULTI_POINT geometry cant be empty");
				}

				int32_t cursor_x = 0;
				int32_t cursor_y = 0;

				feature.geometry.push_back((1 & 0x7) | (part_count << 3)); // MoveTo, part count

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

					feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
					feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));

					cursor_x = x;
					cursor_y = y;
				}
			} break;
			case sgl::geometry_type::MULTI_LINESTRING: {
				feature.type = 2; // MVT_LINESTRING

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
							feature.geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
							feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
							feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
							feature.geometry.push_back((2 & 0x7) | ((vertex_count - 2) << 3)); // LineTo, part count
						} else {
							feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
							feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
						}

						cursor_x = x;
						cursor_y = y;
					}
				}

			} break;
			case sgl::geometry_type::MULTI_POLYGON: {
				feature.type = 3; // MVT_POLYGON

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
								feature.geometry.push_back((1 & 0x7) | (1 << 3)); // MoveTo, 1 part
								feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
								feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));
								feature.geometry.push_back((2 & 0x7) | ((vertex_count - 1) << 3));

								cursor_x = x;
								cursor_y = y;

							} else if (vertex_idx == vertex_count - 1) {
								// Close the ring
								feature.geometry.push_back((7 & 0x7) | (1 << 3)); // ClosePath
							} else {
								// Add the vertex
								feature.geometry.push_back(protozero::encode_zigzag32(x - cursor_x));
								feature.geometry.push_back(protozero::encode_zigzag32(y - cursor_y));

								cursor_x = x;
								cursor_y = y;
							}
						}
					}
				}
			} break;
			default:
				throw InvalidInputException("ST_AsMVT: unsupported geometry type");
			}

			// Write out the properties
			for (idx_t prop_idx = 0; prop_idx < property_formats.size(); prop_idx++) {
				auto &property_format = property_formats[prop_idx];
				const auto prop_row_idx = property_format.sel->get_index(row_idx);

				if (!property_format.validity.RowIsValid(prop_row_idx)) {
					continue;
				}

				uint32_t tag_idx = 0;

				auto &property_type = bdata.property_types[prop_idx];

				if (property_type.id() == LogicalTypeId::VARCHAR) {
					auto &property_value = UnifiedVectorFormat::GetData<string_t>(property_format)[prop_row_idx];
					tag_idx = state.layer.AddValue(aggr_input_data.allocator, property_value);
				} else if (property_type.id() == LogicalTypeId::INTEGER) {
					auto property_value = UnifiedVectorFormat::GetData<int32_t>(property_format)[prop_row_idx];
					tag_idx = state.layer.AddValue(aggr_input_data.allocator, property_value);
				} else if (property_type.id() == LogicalTypeId::DOUBLE) {
					auto property_value = UnifiedVectorFormat::GetData<double>(property_format)[prop_row_idx];
					tag_idx = state.layer.AddValue(aggr_input_data.allocator, property_value);
				} else {
					throw InvalidInputException("ST_AsMVT: unsupported property type: " + property_type.ToString());
				}

				// Add the tag to the feature
				feature.tags.emplace_back(prop_idx, tag_idx);
			}

			if (!feature.geometry.empty()) {
				state.layer.features.push_back(std::move(feature));
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Combine
	//------------------------------------------------------------------------------------------------------------------
	static void Combine(Vector &source_vec, Vector &target_vec, AggregateInputData &aggr_input_data, idx_t count) {
		// There is no point in doing destructive combining here. In the future if we have a linked list it might

		UnifiedVectorFormat source_format;
		source_vec.ToUnifiedFormat(count, source_format);

		const auto source_ptr = UnifiedVectorFormat::GetData<const State *>(source_format);
		const auto target_ptr = FlatVector::GetData<State *>(target_vec);

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			auto &source = *source_ptr[source_format.sel->get_index(row_idx)];
			auto &target = *target_ptr[row_idx];

			// Append the feature data from source to target
			target.layer.Combine(aggr_input_data.allocator, source.layer);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Finalize
	//------------------------------------------------------------------------------------------------------------------
	static void Finalize(Vector &state_vec, AggregateInputData &aggr_input_data, Vector &result, idx_t count,
	                     idx_t offset) {

		const auto &bdata = aggr_input_data.bind_data->Cast<BindData>();

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);
		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);

		// Layer and feature buffers
		string l_buffer;

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			auto &state = *state_ptr[state_format.sel->get_index(raw_idx)];
			const auto out_idx = raw_idx + offset;

			l_buffer.clear();
			protozero::pbf_writer tile_writer(l_buffer);
			protozero::pbf_writer layer_writer(tile_writer, 3);

			// Add version
			layer_writer.add_uint32(15, 2);

			// Add layer name
			layer_writer.add_string(1, bdata.layer_name);

			// Add features
			for (auto &feature : state.layer.features) {

				protozero::pbf_writer feature_writer(layer_writer, 2);

				// Id = 1
				feature_writer.add_uint64(1, 0);

				// Tags = 2
				protozero::packed_field_uint32 tags_writer(feature_writer, 2);
				for (const auto &tag : feature.tags) {
					tags_writer.add_element(tag.first);
					tags_writer.add_element(tag.second);
				}
				tags_writer.commit();

				// Type = 3
				feature_writer.add_enum(3, feature.type);

				// Geometry = 4
				feature_writer.add_packed_uint32(4, feature.geometry.begin(), feature.geometry.end());

				// Add to the layer
				feature_writer.commit();
			}

			// Now add keys and values
			for (auto &key : bdata.property_names) {
				layer_writer.add_string(3, key);
			}

			for (auto &val : state.layer.vals) {
				// Add val
				layer_writer.add_message(4, val.GetData(), val.GetSize());
			}

			// Add extent
			layer_writer.add_uint32(5, bdata.extent);

			// Commit the layer
			layer_writer.commit();

			// Now we have the layer buffer, we can write it to the result vector
			const auto result_data = FlatVector::GetData<string_t>(result);
			result_data[out_idx] = StringVector::AddStringOrBlob(result, l_buffer.data(), l_buffer.size());
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Destroy
	//------------------------------------------------------------------------------------------------------------------
	static void Destroy(Vector &state_vec, AggregateInputData &, idx_t count) {
		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto row_idx = state_format.sel->get_index(raw_idx);
			if (state_format.validity.RowIsValid(row_idx)) {
				auto &state = *state_ptr[row_idx];

				// Call destructor
				state.~State();
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(DatabaseInstance &db) {
		AggregateFunction agg({LogicalTypeId::ANY}, LogicalType::BLOB, StateSize, Initialize, Update, Combine, Finalize,
		                      nullptr, Bind, Destroy);

		FunctionBuilder::RegisterAggregate(db, "ST_AsMVT", [&](AggregateFunctionBuilder &func) {
			func.SetFunction(agg);
			func.SetDescription("Makes a vector tile from a set of geometries");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

} // namespace
//======================================================================================================================
//  Register
//======================================================================================================================
void RegisterMapboxVectorTileModule(DatabaseInstance &db) {
	ST_TileEnvelope::Register(db);
	ST_AsMVT::Register(db);
};

} // namespace duckdb