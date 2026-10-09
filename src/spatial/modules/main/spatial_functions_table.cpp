#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "spatial/geometry/bbox.hpp"
#include "spatial/modules/main/spatial_functions.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/function_builder.hpp"
#include "duckdb/common/random_engine.hpp"

namespace duckdb {

namespace {

//######################################################################################################################
// Table Functions
//######################################################################################################################

//======================================================================================================================
// ST_GeneratePoints
//======================================================================================================================

struct ST_GeneratePoints {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct GeneratePointsBindData final : TableFunctionData {
		idx_t count = 0;
		int64_t seed = -1;
		Box2D<double> bbox;
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
		auto result = make_uniq<GeneratePointsBindData>();

		return_types.push_back(GeoTypes::POINT_2D());
		names.push_back("point");

		// Extract the bounding box
		const auto &box_value = input.inputs[0];
		auto &box_components = StructValue::GetChildren(box_value);
		result->bbox.min.x = box_components[0].GetValue<double>();
		result->bbox.min.y = box_components[1].GetValue<double>();
		result->bbox.max.x = box_components[2].GetValue<double>();
		result->bbox.max.y = box_components[3].GetValue<double>();

		// Extract the count
		const auto &count_value = input.inputs[1];
		const auto count = count_value.GetValue<int64_t>();
		if (count < 0) {
			throw BinderException("Count must be a non-negative integer");
		}
		result->count = UnsafeNumericCast<idx_t>(count);

		// Extract the seed (optional)
		if (input.inputs.size() == 3) {
			result->seed = input.inputs[2].GetValue<int64_t>();
		}

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init
	//------------------------------------------------------------------------------------------------------------------
	struct GeneratePointsState final : GlobalTableFunctionState {
		RandomEngine rng;
		idx_t current_idx;

		explicit GeneratePointsState(const int64_t seed) : rng(seed), current_idx(0) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		auto &bind_data = input.bind_data->Cast<GeneratePointsBindData>();
		auto result = make_uniq<GeneratePointsState>(bind_data.seed);
		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
		auto &bind_data = data_p.bind_data->Cast<GeneratePointsBindData>();
		auto &state = data_p.global_state->Cast<GeneratePointsState>();

		auto &point_vec = StructVector::GetEntries(output.data[0]);
		auto x_data = FlatVector::GetDataMutable<double>(point_vec[0]);
		auto y_data = FlatVector::GetDataMutable<double>(point_vec[1]);

		const auto chunk_size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.count - state.current_idx);
		for (idx_t i = 0; i < chunk_size; i++) {

			x_data[i] = state.rng.NextRandom32(bind_data.bbox.min.x, bind_data.bbox.max.x);
			y_data[i] = state.rng.NextRandom32(bind_data.bbox.min.y, bind_data.bbox.max.y);

			state.current_idx++;
		}
		FlatVector::SetSize(output.data[0], count_t(chunk_size));
		output.SetCardinality(chunk_size);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Cardinality
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<NodeStatistics> Cardinality(ClientContext &context, const FunctionData *bind_data_p) {
		auto &bind_data = bind_data_p->Cast<GeneratePointsBindData>();
		return make_uniq<NodeStatistics>(bind_data.count, bind_data.count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// DOCUMENTATION
	//------------------------------------------------------------------------------------------------------------------
	static constexpr auto DESCRIPTION = R"(
		Generates a set of random points within the specified bounding box.

		Takes a bounding box (min_x, min_y, max_x, max_y), a count of points to generate, and optionally a seed for the random number generator.
	)";
	static constexpr auto EXAMPLE =
	    "SELECT * FROM ST_GeneratePoints({min_x: 0, min_y:0, max_x:10, max_y:10}::BOX_2D, 5, 42);";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		// TODO: Dont overload, make seed named parameter instead
		TableFunctionSet set("ST_GeneratePoints");

		TableFunction generate_points({GeoTypes::BOX_2D(), LogicalType::BIGINT}, Execute, Bind, Init);
		generate_points.cardinality = Cardinality;

		// Overload without seed
		set.AddFunction(generate_points);

		// Overload with seed
		generate_points.GetArguments() = {GeoTypes::BOX_2D(), LogicalType::BIGINT, LogicalType::BIGINT};
		set.AddFunction(generate_points);
		loader.RegisterFunction(set);

		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "spatial");
		FunctionBuilder::AddTableFunctionDocs(loader, "ST_GeneratePoints", DESCRIPTION, EXAMPLE, tags);
	}
};

} // namespace

//######################################################################################################################
// Register
//######################################################################################################################
void RegisterSpatialTableFunctions(ExtensionLoader &loader) {
	ST_GeneratePoints::Register(loader);
}

} // namespace duckdb
