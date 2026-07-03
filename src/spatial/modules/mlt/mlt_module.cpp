// MapLibre Tile (MLT) implementation

#include "spatial/modules/mlt/mlt_module.hpp"

#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/binary_reader.hpp"
#include "spatial/util/function_builder.hpp"

#include <mlt/encoder.hpp>
#include <mlt/decoder.hpp>
#include <mlt/tile.hpp>
#include <mlt/layer.hpp>
#include <mlt/feature.hpp>
#include <mlt/geometry.hpp>
#include <mlt/properties.hpp>

#include <optional>

namespace duckdb {

namespace {

//======================================================================================================================
// Geometry Conversion: DuckDB sgl blob → mlt::Encoder::Geometry
//======================================================================================================================

static int32_t CastDouble(double d) {
	if (d < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
	    d > static_cast<double>(std::numeric_limits<int32_t>::max())) {
		throw InvalidInputException("ST_AsMLT: coordinate out of range for int32_t");
	}
	return static_cast<int32_t>(d);
}

static mlt::Encoder::Geometry ConvertGeometry(const string_t &geom_blob) {
	using Vertex = mlt::Encoder::Vertex;
	using GeometryType = mlt::Encoder::GeometryType;

	mlt::Encoder::Geometry result;

	BinaryReader cursor(geom_blob.GetData(), geom_blob.GetSize());
	const auto le = cursor.Read<uint8_t>();
	if (le != 1) {
		throw InvalidInputException("ST_AsMLT: unsupported geometry endian-ness");
	}
	const auto meta = cursor.Read<uint32_t>();
	const auto type = static_cast<sgl::geometry_type>((meta & 0x0000FFFF) % 1000);
	const auto flag = (meta & 0x0000FFFF) / 1000;
	const auto has_z = (flag & 0x01) != 0;
	const auto has_m = (flag & 0x02) != 0;

	const auto vertex_width = (2 + (has_z ? 1 : 0) + (has_m ? 1 : 0)) * sizeof(double);
	const auto vertex_space = vertex_width - (2 * sizeof(double));

	auto read_vertex = [&]() -> Vertex {
		auto x_double = cursor.Read<double>();
		auto y_double = cursor.Read<double>();
		cursor.Skip(vertex_space);
		if (std::isnan(x_double) && std::isnan(y_double)) {
			throw InvalidInputException("ST_AsMLT: POINT geometry can't be empty");
		}
		return {CastDouble(x_double), CastDouble(y_double)};
	};

	auto read_vertices = [&](uint32_t count) -> std::vector<Vertex> {
		std::vector<Vertex> verts;
		verts.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			verts.push_back(read_vertex());
		}
		return verts;
	};

	switch (type) {
	case sgl::geometry_type::POINT: {
		result.type = GeometryType::POINT;
		result.coordinates.push_back(read_vertex());
	} break;

	case sgl::geometry_type::LINESTRING: {
		result.type = GeometryType::LINESTRING;
		auto vertex_count = cursor.Read<uint32_t>();
		if (vertex_count < 2) {
			throw InvalidInputException("ST_AsMLT: LINESTRING must have at least 2 vertices");
		}
		result.coordinates = read_vertices(vertex_count);
	} break;

	case sgl::geometry_type::POLYGON: {
		result.type = GeometryType::POLYGON;
		auto ring_count = cursor.Read<uint32_t>();
		if (ring_count == 0) {
			throw InvalidInputException("ST_AsMLT: POLYGON can't be empty");
		}

		// Encoder expects coordinates = all ring vertices concatenated,
		// ringSizes = vertex count per ring.
		for (uint32_t i = 0; i < ring_count; i++) {
			auto vertex_count = cursor.Read<uint32_t>();
			auto ring_verts = read_vertices(vertex_count);
			result.coordinates.insert(result.coordinates.end(), ring_verts.begin(), ring_verts.end());
			result.ringSizes.push_back(vertex_count);
		}
	} break;

	case sgl::geometry_type::MULTI_POINT: {
		result.type = GeometryType::MULTIPOINT;
		auto part_count = cursor.Read<uint32_t>();
		if (part_count == 0) {
			throw InvalidInputException("ST_AsMLT: MULTIPOINT can't be empty");
		}
		for (uint32_t i = 0; i < part_count; i++) {
			cursor.Skip(sizeof(uint32_t) + sizeof(uint8_t)); // part size and type
			result.coordinates.push_back(read_vertex());
		}
	} break;

	case sgl::geometry_type::MULTI_LINESTRING: {
		result.type = GeometryType::MULTILINESTRING;
		auto part_count = cursor.Read<uint32_t>();
		if (part_count == 0) {
			throw InvalidInputException("ST_AsMLT: MULTILINESTRING can't be empty");
		}
		for (uint32_t i = 0; i < part_count; i++) {
			cursor.Skip(sizeof(uint32_t) + sizeof(uint8_t)); // part size and type
			auto vertex_count = cursor.Read<uint32_t>();
			if (vertex_count < 2) {
				throw InvalidInputException("ST_AsMLT: LINESTRING in MULTILINESTRING must have at least 2 vertices");
			}
			result.parts.push_back(read_vertices(vertex_count));
		}
	} break;

	case sgl::geometry_type::MULTI_POLYGON: {
		result.type = GeometryType::MULTIPOLYGON;
		auto poly_count = cursor.Read<uint32_t>();
		if (poly_count == 0) {
			throw InvalidInputException("ST_AsMLT: MULTIPOLYGON can't be empty");
		}

		for (uint32_t poly_idx = 0; poly_idx < poly_count; poly_idx++) {
			cursor.Skip(sizeof(uint32_t) + sizeof(uint8_t)); // part size and type
			auto ring_count = cursor.Read<uint32_t>();
			if (ring_count == 0) {
				throw InvalidInputException("ST_AsMLT: POLYGON in MULTIPOLYGON can't be empty");
			}

			// Encoder expects parts[p] = all vertices for polygon p (rings concatenated),
			// with partRingSizes[p] listing the vertex count of each ring.
			std::vector<Vertex> poly_verts;
			std::vector<uint32_t> ring_sizes;
			for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
				auto vertex_count = cursor.Read<uint32_t>();
				auto ring_verts = read_vertices(vertex_count);
				poly_verts.insert(poly_verts.end(), ring_verts.begin(), ring_verts.end());
				ring_sizes.push_back(vertex_count);
			}
			result.parts.push_back(std::move(poly_verts));
			result.partRingSizes.push_back(std::move(ring_sizes));
		}
	} break;

	case sgl::geometry_type::GEOMETRY_COLLECTION:
		throw InvalidInputException("ST_AsMLT: GEOMETRYCOLLECTION is not supported");
	default:
		throw InvalidInputException("ST_AsMLT: unsupported geometry type %d", static_cast<int>(type));
	}

	return result;
}

//======================================================================================================================
// ST_AsMLT — Aggregate Function
//======================================================================================================================

struct ST_AsMLT {

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

		const auto &row_type = arguments[0]->return_type;
		if (row_type.id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException("ST_AsMLT: first argument must be a STRUCT (i.e. a row type)");
		}

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
						throw InvalidInputException("ST_AsMLT: layer name cannot be empty");
					}
				}
				folded_layer = true;
			} else {
				throw InvalidInputException("ST_AsMLT: layer name must be a constant string");
			}
		}

		if (arguments.size() >= 3) {
			auto &extent_expr = arguments[2];
			if (extent_expr->IsFoldable()) {
				auto extent_val = ExpressionExecutor::EvaluateScalar(context, *extent_expr);
				if (extent_val.IsNull()) {
					throw InvalidInputException("ST_AsMLT: extent cannot be NULL");
				}
				result->extent = IntegerValue::Get(extent_val);
				if (result->extent == 0) {
					throw InvalidInputException("ST_AsMLT: extent must be greater than zero");
				}
				folded_extent = true;
			} else {
				throw InvalidInputException("ST_AsMLT: extent must be a constant integer");
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
						throw InvalidInputException("ST_AsMLT: geometry column name cannot be empty");
					}
				}
				folded_geom = true;
			} else {
				throw InvalidInputException("ST_AsMLT: geometry column name must be a constant string");
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
						throw InvalidInputException("ST_AsMLT: feature id column name cannot be empty");
					}
				}
				folded_feature = true;
			} else {
				throw InvalidInputException("ST_AsMLT: feature id column name must be a constant string");
			}
		}

		// Find the geometry column
		optional_idx geom_idx = optional_idx::Invalid();
		if (geom_name.empty()) {
			for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
				auto &child = StructType::GetChildType(row_type, i);
				if (child == LogicalType::GEOMETRY()) {
					if (geom_idx != optional_idx::Invalid()) {
						throw InvalidInputException("ST_AsMLT: only one geometry column is allowed in the input row");
					}
					geom_idx = i;
				}
			}
		} else {
			for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
				auto &child = StructType::GetChildType(row_type, i);
				auto &child_name = StructType::GetChildName(row_type, i);
				if (child == LogicalType::GEOMETRY() && child_name == geom_name) {
					if (geom_idx != optional_idx::Invalid()) {
						throw InvalidInputException("ST_AsMLT: only one geometry column is allowed in the input row");
					}
					geom_idx = i;
				}
			}
		}
		if (!geom_idx.IsValid()) {
			throw InvalidInputException("ST_AsMLT: input row must contain a geometry column");
		}
		result->geometry_column_idx = geom_idx.GetIndex();

		// Find the feature id column
		if (!feature_id_name.empty()) {
			for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
				auto &child_name = StructType::GetChildName(row_type, i);
				if (child_name == feature_id_name) {
					if (result->feature_id_column_idx.IsValid()) {
						throw InvalidInputException("ST_AsMLT: only one feature id column is allowed in the input row");
					}
					auto &child_type = StructType::GetChildType(row_type, i);
					if (child_type != LogicalTypeId::INTEGER && child_type != LogicalTypeId::BIGINT) {
						throw InvalidInputException("ST_AsMLT: feature id column must be of type INTEGER or BIGINT");
					}
					result->feature_id_column_idx = i;
				}
			}
			if (!result->feature_id_column_idx.IsValid()) {
				throw InvalidInputException("ST_AsMLT: feature id column not found in input row");
			}
		}

		unordered_set<LogicalTypeId> valid_property_types = {LogicalTypeId::VARCHAR, LogicalTypeId::FLOAT,
		                                                     LogicalTypeId::DOUBLE,  LogicalTypeId::INTEGER,
		                                                     LogicalTypeId::BIGINT,  LogicalTypeId::BOOLEAN};

		for (idx_t i = 0; i < StructType::GetChildCount(row_type); i++) {
			if (i != result->geometry_column_idx &&
			    (!result->feature_id_column_idx.IsValid() || i != result->feature_id_column_idx.GetIndex())) {
				auto &name = StructType::GetChildName(row_type, i);
				auto &type = StructType::GetChildType(row_type, i);
				if (valid_property_types.find(type.id()) == valid_property_types.end()) {
					throw InvalidInputException("ST_AsMLT: property column \"%s\" has unsupported type \"%s\"\n"
					                            "Only the following property types are supported: VARCHAR, FLOAT, "
					                            "DOUBLE, INTEGER, BIGINT, BOOLEAN",
					                            name.c_str(), type.ToString().c_str());
				}
				result->tag_names.push_back(name);
			}
		}

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
	// State
	//------------------------------------------------------------------------------------------------------------------
	struct State {
		std::vector<mlt::Encoder::Feature> features;
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

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			const auto state_idx = state_format.sel->get_index(row_idx);
			auto &features = UnifiedVectorFormat::GetData<State *>(state_format)[state_idx]->features;

			const auto geom_idx = geom_format.sel->get_index(row_idx);
			if (!geom_format.validity.RowIsValid(geom_idx)) {
				continue;
			}
			auto &geom_blob = UnifiedVectorFormat::GetData<string_t>(geom_format)[geom_idx];

			mlt::Encoder::Feature feature;
			feature.geometry = ConvertGeometry(geom_blob);

			// Feature ID
			if (bdata.feature_id_column_idx.IsValid()) {
				const auto fid_idx = fid_format.sel->get_index(row_idx);
				if (fid_format.validity.RowIsValid(fid_idx)) {
					switch (fid_type.id()) {
					case LogicalTypeId::INTEGER: {
						auto val = UnifiedVectorFormat::GetData<int32_t>(fid_format)[fid_idx];
						feature.id = static_cast<uint64_t>(val);
					} break;
					case LogicalTypeId::BIGINT: {
						auto val = UnifiedVectorFormat::GetData<int64_t>(fid_format)[fid_idx];
						feature.id = static_cast<uint64_t>(val);
					} break;
					default:
						break;
					}
				} else {
					feature.id = std::nullopt;
				}
			} else {
				feature.id = std::nullopt;
			}

			// Properties
			for (idx_t prop_vec_idx = 0; prop_vec_idx < property_formats.size(); prop_vec_idx++) {
				const auto &prop_format = property_formats[prop_vec_idx];
				const auto prop_row_idx = prop_format.sel->get_index(row_idx);
				if (!prop_format.validity.RowIsValid(prop_row_idx)) {
					continue;
				}

				auto &prop_name = bdata.tag_names[prop_vec_idx];
				auto &prop_type = property_types[prop_vec_idx];

				switch (prop_type.id()) {
				case LogicalTypeId::VARCHAR: {
					auto &val = UnifiedVectorFormat::GetData<string_t>(prop_format)[prop_row_idx];
					feature.properties[prop_name] = std::string(val.GetData(), val.GetSize());
				} break;
				case LogicalTypeId::FLOAT: {
					auto val = UnifiedVectorFormat::GetData<float>(prop_format)[prop_row_idx];
					feature.properties[prop_name] = val;
				} break;
				case LogicalTypeId::DOUBLE: {
					auto val = UnifiedVectorFormat::GetData<double>(prop_format)[prop_row_idx];
					feature.properties[prop_name] = val;
				} break;
				case LogicalTypeId::INTEGER: {
					auto val = UnifiedVectorFormat::GetData<int32_t>(prop_format)[prop_row_idx];
					feature.properties[prop_name] = val;
				} break;
				case LogicalTypeId::BIGINT: {
					auto val = UnifiedVectorFormat::GetData<int64_t>(prop_format)[prop_row_idx];
					feature.properties[prop_name] = val;
				} break;
				case LogicalTypeId::BOOLEAN: {
					auto val = UnifiedVectorFormat::GetData<bool>(prop_format)[prop_row_idx];
					feature.properties[prop_name] = val;
				} break;
				default:
					break;
				}
			}

			features.push_back(std::move(feature));
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

			target.features.insert(target.features.end(), std::make_move_iterator(source.features.begin()),
			                       std::make_move_iterator(source.features.end()));
			source.features.clear();
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

		mlt::Encoder encoder;
		mlt::EncoderConfig config;
		config.includeIds = bdata.feature_id_column_idx.IsValid();

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			auto &state = *state_ptr[state_format.sel->get_index(raw_idx)];
			const auto out_idx = raw_idx + offset;

			mlt::Encoder::Layer layer;
			layer.name = bdata.layer_name;
			layer.extent = static_cast<uint32_t>(bdata.extent);
			layer.features = std::move(state.features);

			auto encoded = encoder.encode({layer}, config);

			const auto result_data = FlatVector::GetData<string_t>(result);
			result_data[out_idx] =
			    StringVector::AddStringOrBlob(result, reinterpret_cast<const char *>(encoded.data()), encoded.size());
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Docs
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Encode a set of geometries and properties into a MapLibre Tile (MLT).

		The function takes as input a row type (STRUCT) containing a geometry column and any number of property columns.
		It returns a single binary BLOB containing the MapLibre Tile.

		`ST_AsMLT(row STRUCT, layer_name VARCHAR DEFAULT 'layer', extent INTEGER DEFAULT 4096, geom_column_name VARCHAR DEFAULT NULL, feature_id_column_name VARCHAR DEFAULT NULL) -> BLOB`

		- The first argument is a struct containing the geometry and properties.
		- The second argument is the name of the layer. Optional, defaults to 'layer'.
		- The third argument is the tile extent. Optional, defaults to 4096.
		- The fourth argument is the name of the geometry column. Optional, auto-detected if omitted.
		- The fifth argument is the name of the feature id column. Optional.

		The input struct must contain exactly one geometry column of type GEOMETRY. Property columns
		may be of types VARCHAR, FLOAT, DOUBLE, INTEGER, BIGINT, or BOOLEAN.

		MLT is a next-generation vector tile format with better compression and faster decoding than MVT.
		Use ST_AsMVTGeom to transform geometries to tile coordinates before encoding.

		```sql
		SELECT ST_AsMLT({'geom': geom, 'id': id, 'name': name}, 'cities', 4096, 'geom', 'id') AS tile
		FROM cities;
		```
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterAggregate(loader, "ST_AsMLT", [&](AggregateFunctionBuilder &func) {
			const auto optional_args = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
			                            LogicalType::VARCHAR};
			AggregateFunction agg({LogicalTypeId::ANY}, LogicalType::BLOB, StateSize, Initialize, Update, Combine,
			                      Finalize, nullptr, Bind);

			func.SetFunction(agg);
			for (auto &arg_type : optional_args) {
				agg.arguments.push_back(arg_type);
				func.SetFunction(agg);
			}

			func.SetDescription(DESCRIPTION);
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
			func.CanThrowErrors();
		});
	}
};

//======================================================================================================================
// Geometry Conversion: mlt::geometry → DuckDB sgl blob
//======================================================================================================================

static void ConvertCoords(GeometryAllocator &alloc, const mlt::CoordVec &coords, sgl::geometry &geom) {
	auto vertex_count = static_cast<uint32_t>(coords.size());
	auto *data = static_cast<double *>(alloc.alloc(sizeof(double) * 2 * vertex_count));
	for (uint32_t i = 0; i < vertex_count; i++) {
		data[i * 2] = static_cast<double>(coords[i].x);
		data[i * 2 + 1] = static_cast<double>(coords[i].y);
	}
	geom.set_vertex_array(data, vertex_count);
}

static void ConvertMLTGeometry(GeometryAllocator &alloc, const mlt::geometry::Geometry &mlt_geom,
                               sgl::geometry &result) {
	using mlt::geometry::LineString;
	using mlt::geometry::MultiLineString;
	using mlt::geometry::MultiPoint;
	using mlt::geometry::MultiPolygon;
	using mlt::geometry::Point;
	using mlt::geometry::Polygon;
	using GT = mlt::metadata::tileset::GeometryType;

	switch (mlt_geom.type) {
	case GT::POINT: {
		const auto &point = static_cast<const Point &>(mlt_geom);
		const auto &coord = point.getCoordinate();
		auto *data = static_cast<double *>(alloc.alloc(sizeof(double) * 2));
		data[0] = static_cast<double>(coord.x);
		data[1] = static_cast<double>(coord.y);
		result.set_type(sgl::geometry_type::POINT);
		result.set_vertex_array(data, 1);
	} break;

	case GT::LINESTRING: {
		const auto &line = static_cast<const LineString &>(mlt_geom);
		result.set_type(sgl::geometry_type::LINESTRING);
		ConvertCoords(alloc, line.getCoordinates(), result);
	} break;

	case GT::POLYGON: {
		const auto &poly = static_cast<const Polygon &>(mlt_geom);
		result.set_type(sgl::geometry_type::POLYGON);
		for (const auto &ring : poly.getRings()) {
			auto *ring_geom = static_cast<sgl::geometry *>(alloc.alloc(sizeof(sgl::geometry)));
			new (ring_geom) sgl::geometry(sgl::geometry_type::LINESTRING, false, false);
			ConvertCoords(alloc, ring, *ring_geom);
			result.append_part(ring_geom);
		}
	} break;

	case GT::MULTIPOINT: {
		const auto &mp = static_cast<const MultiPoint &>(mlt_geom);
		result.set_type(sgl::geometry_type::MULTI_POINT);
		for (const auto &coord : mp.getCoordinates()) {
			auto *pt = static_cast<sgl::geometry *>(alloc.alloc(sizeof(sgl::geometry)));
			new (pt) sgl::geometry(sgl::geometry_type::POINT, false, false);
			auto *data = static_cast<double *>(alloc.alloc(sizeof(double) * 2));
			data[0] = static_cast<double>(coord.x);
			data[1] = static_cast<double>(coord.y);
			pt->set_vertex_array(data, 1);
			result.append_part(pt);
		}
	} break;

	case GT::MULTILINESTRING: {
		const auto &mls = static_cast<const MultiLineString &>(mlt_geom);
		result.set_type(sgl::geometry_type::MULTI_LINESTRING);
		for (const auto &line_coords : mls.getLineStrings()) {
			auto *line_geom = static_cast<sgl::geometry *>(alloc.alloc(sizeof(sgl::geometry)));
			new (line_geom) sgl::geometry(sgl::geometry_type::LINESTRING, false, false);
			ConvertCoords(alloc, line_coords, *line_geom);
			result.append_part(line_geom);
		}
	} break;

	case GT::MULTIPOLYGON: {
		const auto &mpoly = static_cast<const MultiPolygon &>(mlt_geom);
		result.set_type(sgl::geometry_type::MULTI_POLYGON);
		for (const auto &poly_rings : mpoly.getPolygons()) {
			auto *poly_geom = static_cast<sgl::geometry *>(alloc.alloc(sizeof(sgl::geometry)));
			new (poly_geom) sgl::geometry(sgl::geometry_type::POLYGON, false, false);
			for (const auto &ring : poly_rings) {
				auto *ring_geom = static_cast<sgl::geometry *>(alloc.alloc(sizeof(sgl::geometry)));
				new (ring_geom) sgl::geometry(sgl::geometry_type::LINESTRING, false, false);
				ConvertCoords(alloc, ring, *ring_geom);
				poly_geom->append_part(ring_geom);
			}
			result.append_part(poly_geom);
		}
	} break;

	default:
		throw InvalidInputException("ST_ReadMLT: unsupported geometry type");
	}
}

static string_t SerializeMLTGeometry(GeometryAllocator &alloc, const mlt::geometry::Geometry &mlt_geom,
                                     Vector &result_vec) {
	sgl::geometry geom;
	ConvertMLTGeometry(alloc, mlt_geom, geom);
	const auto size = Serde::GetRequiredSize(geom);
	auto blob = StringVector::EmptyString(result_vec, size);
	Serde::Serialize(geom, blob.GetDataWriteable(), size);
	blob.Finalize();
	return blob;
}

//======================================================================================================================
// Property Conversion: mlt::Property → DuckDB value
//======================================================================================================================

static LogicalType MLTScalarTypeToDuckDB(mlt::metadata::tileset::ScalarType type) {
	using ST = mlt::metadata::tileset::ScalarType;
	switch (type) {
	case ST::BOOLEAN:
		return LogicalType::BOOLEAN;
	case ST::INT_8:
		return LogicalType::TINYINT;
	case ST::UINT_8:
		return LogicalType::UTINYINT;
	case ST::INT_32:
		return LogicalType::INTEGER;
	case ST::UINT_32:
		return LogicalType::UINTEGER;
	case ST::INT_64:
		return LogicalType::BIGINT;
	case ST::UINT_64:
		return LogicalType::UBIGINT;
	case ST::FLOAT:
		return LogicalType::FLOAT;
	case ST::DOUBLE:
		return LogicalType::DOUBLE;
	case ST::STRING:
		return LogicalType::VARCHAR;
	default:
		return LogicalType::VARCHAR;
	}
}

//======================================================================================================================
// ST_ReadMLT — Table Function
//======================================================================================================================

struct ST_ReadMLT {

	struct ReadBindData final : TableFunctionData {
		string file_name;

		struct PropertyColumn {
			string name;
			LogicalType type;
		};
		vector<PropertyColumn> property_columns;

		explicit ReadBindData(string file_name_p) : file_name(std::move(file_name_p)) {
		}
	};

	struct ReadGlobalState final : GlobalTableFunctionState {
		mlt::MapLibreTile tile;
		idx_t current_layer_idx = 0;
		idx_t current_feature_idx = 0;

		explicit ReadGlobalState(mlt::MapLibreTile tile_p) : tile(std::move(tile_p)) {
		}

		idx_t MaxThreads() const override {
			return 1;
		}
	};

	struct ReadLocalState final : LocalTableFunctionState {
		ArenaAllocator arena;
		GeometryAllocator alloc;

		explicit ReadLocalState(ClientContext &context) : arena(BufferAllocator::Get(context)), alloc(arena) {
		}
	};

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names) {
		auto file_name = StringValue::Get(input.inputs[0]);
		auto &fs = FileSystem::GetFileSystem(context);

		auto handle = fs.OpenFile(file_name, FileFlags::FILE_FLAGS_READ);
		auto file_size = handle->GetFileSize();

		string buffer;
		buffer.resize(file_size);
		handle->Read(const_cast<char *>(buffer.data()), file_size, 0);

		mlt::Decoder decoder(true);
		auto tile = decoder.decode(mlt::DataView(buffer.data(), buffer.size()));

		auto result = make_uniq<ReadBindData>(file_name);

		// Fixed columns
		names.push_back("layer_name");
		return_types.push_back(LogicalType::VARCHAR);

		names.push_back("id");
		return_types.push_back(LogicalType::UBIGINT);

		names.push_back("geometry");
		return_types.push_back(LogicalType::GEOMETRY());

		// Collect property columns across all layers
		unordered_map<string, LogicalType> seen_props;
		for (auto &layer : tile.getLayers()) {
			for (auto &[prop_name, prop_col] : layer.getProperties()) {
				if (seen_props.find(prop_name) == seen_props.end()) {
					auto duckdb_type = MLTScalarTypeToDuckDB(prop_col.getType());
					seen_props[prop_name] = duckdb_type;
					result->property_columns.push_back({prop_name, duckdb_type});
					names.push_back(prop_name);
					return_types.push_back(duckdb_type);
				}
			}
		}

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input) {
		auto &bind_data = input.bind_data->Cast<ReadBindData>();
		auto &fs = FileSystem::GetFileSystem(context);

		auto handle = fs.OpenFile(bind_data.file_name, FileFlags::FILE_FLAGS_READ);
		auto file_size = handle->GetFileSize();

		string buffer;
		buffer.resize(file_size);
		handle->Read(const_cast<char *>(buffer.data()), file_size, 0);

		mlt::Decoder decoder(true);
		auto tile = decoder.decode(mlt::DataView(buffer.data(), buffer.size()));

		return make_uniq<ReadGlobalState>(std::move(tile));
	}

	static unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &,
	                                                     GlobalTableFunctionState *) {
		return make_uniq<ReadLocalState>(context.client);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Scan
	//------------------------------------------------------------------------------------------------------------------
	static void Scan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		auto &gstate = input.global_state->Cast<ReadGlobalState>();
		auto &lstate = input.local_state->Cast<ReadLocalState>();
		auto &bind_data = input.bind_data->Cast<ReadBindData>();

		auto &layers = gstate.tile.getLayers();
		idx_t output_idx = 0;
		const idx_t batch_size = STANDARD_VECTOR_SIZE;

		while (output_idx < batch_size && gstate.current_layer_idx < layers.size()) {
			auto &layer = layers[gstate.current_layer_idx];
			auto &features = layer.getFeatures();

			while (output_idx < batch_size && gstate.current_feature_idx < features.size()) {
				auto &feature = features[gstate.current_feature_idx];

				lstate.arena.Reset();

				// layer_name
				output.data[0].SetValue(output_idx, Value(layer.getName()));

				// id
				auto feature_id = feature.getID();
				if (feature_id.has_value()) {
					output.data[1].SetValue(output_idx, Value::UBIGINT(feature_id.value()));
				} else {
					FlatVector::SetNull(output.data[1], output_idx, true);
				}

				// geometry
				auto geom_blob = SerializeMLTGeometry(lstate.alloc, feature.getGeometry(), output.data[2]);
				FlatVector::GetData<string_t>(output.data[2])[output_idx] = geom_blob;

				// properties
				for (idx_t prop_idx = 0; prop_idx < bind_data.property_columns.size(); prop_idx++) {
					auto &prop_col = bind_data.property_columns[prop_idx];
					auto col_idx = 3 + prop_idx;

					auto prop = feature.getProperty(prop_col.name, layer);
					if (!prop.has_value()) {
						FlatVector::SetNull(output.data[col_idx], output_idx, true);
						continue;
					}

					auto &val = prop.value();
					std::visit(
					    [&](auto &&v) {
						    using T = std::decay_t<decltype(v)>;
						    if constexpr (std::is_same_v<T, std::nullptr_t>) {
							    FlatVector::SetNull(output.data[col_idx], output_idx, true);
						    } else if constexpr (std::is_same_v<T, bool>) {
							    output.data[col_idx].SetValue(output_idx, Value::BOOLEAN(v));
						    } else if constexpr (std::is_same_v<T, std::int32_t>) {
							    output.data[col_idx].SetValue(output_idx, Value::INTEGER(v));
						    } else if constexpr (std::is_same_v<T, std::int64_t>) {
							    output.data[col_idx].SetValue(output_idx, Value::BIGINT(v));
						    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
							    output.data[col_idx].SetValue(output_idx, Value::UINTEGER(v));
						    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
							    output.data[col_idx].SetValue(output_idx, Value::UBIGINT(v));
						    } else if constexpr (std::is_same_v<T, float>) {
							    output.data[col_idx].SetValue(output_idx, Value::FLOAT(v));
						    } else if constexpr (std::is_same_v<T, double>) {
							    output.data[col_idx].SetValue(output_idx, Value::DOUBLE(v));
						    } else if constexpr (std::is_same_v<T, std::string_view>) {
							    output.data[col_idx].SetValue(output_idx, Value(string(v.data(), v.size())));
						    } else {
							    // optional<T> variants — extract or null
							    if constexpr (requires { v.has_value(); }) {
								    if (v.has_value()) {
									    output.data[col_idx].SetValue(output_idx, Value::CreateValue(*v));
								    } else {
									    FlatVector::SetNull(output.data[col_idx], output_idx, true);
								    }
							    } else {
								    FlatVector::SetNull(output.data[col_idx], output_idx, true);
							    }
						    }
					    },
					    val);
				}

				output_idx++;
				gstate.current_feature_idx++;
			}

			if (gstate.current_feature_idx >= features.size()) {
				gstate.current_layer_idx++;
				gstate.current_feature_idx = 0;
			}
		}

		output.SetCardinality(output_idx);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Docs
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Read a MapLibre Tile (MLT) file and return its features as rows.

		Returns one row per feature with columns: layer_name, id, geometry, and one column per property
		found across all layers in the tile.

		```sql
		SELECT * FROM ST_ReadMLT('path/to/tile.mlt');
		```
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		TableFunction read("ST_ReadMLT", {LogicalType::VARCHAR}, Scan, Bind, InitGlobal, InitLocal);
		loader.RegisterFunction(read);

		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "spatial");
		tags.insert("category", "conversion");
		FunctionBuilder::AddTableFunctionDocs(loader, "ST_ReadMLT", DESCRIPTION, "", tags);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Replacement Scan
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<TableRef> ReplacementScan(ClientContext &context, ReplacementScanInput &input,
	                                            optional_ptr<ReplacementScanData>) {
		auto &table_name = input.table_name;
		if (!StringUtil::EndsWith(StringUtil::Lower(table_name), ".mlt")) {
			return nullptr;
		}

		auto table_function = make_uniq<TableFunctionRef>();
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
		table_function->function = make_uniq<FunctionExpression>("ST_ReadMLT", std::move(children));
		return std::move(table_function);
	}
};

} // namespace

//======================================================================================================================
// Register
//======================================================================================================================
void RegisterMLTModule(ExtensionLoader &loader) {
	ST_AsMLT::Register(loader);
	ST_ReadMLT::Register(loader);
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(ST_ReadMLT::ReplacementScan);
}

} // namespace duckdb
