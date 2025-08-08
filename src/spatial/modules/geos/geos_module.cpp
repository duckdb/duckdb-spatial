#include "spatial/modules/geos/geos_module.hpp"
#include "spatial/modules/geos/geos_geometry.hpp"
#include "spatial/modules/geos/geos_serde.hpp"
#include "spatial/spatial_types.hpp"
#include "spatial/util/function_builder.hpp"

#include "duckdb/common/vector_operations/senary_executor.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// Local State
//------------------------------------------------------------------------------

namespace {

class LocalState final : public FunctionLocalState {
public:
	static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
	                                           FunctionData *bind_data) {
		return make_uniq<LocalState>(state.GetContext());
	}

	static LocalState &ResetAndGet(ExpressionState &state) {
		auto &local_state = ExecuteFunctionState::GetFunctionState(state)->Cast<LocalState>();
		return local_state;
	}

	GEOSContextHandle_t GetContext() const {
		return ctx;
	}

	GeosGeometry Deserialize(const string_t &blob) const;
	string_t Serialize(Vector &result, const GeosGeometry &geom) const;

	// Most GEOS functions do not use an arena, so just use the default allocator
	explicit LocalState(ClientContext &context) {
		ctx = GEOS_init_r();

		GEOSContext_setErrorMessageHandler_r(
		    ctx, [](const char *message, void *) { throw InvalidInputException(message); }, nullptr);
	}

	~LocalState() override {
		GEOS_finish_r(ctx);
	}

private:
	GEOSContextHandle_t ctx;
};

string_t LocalState::Serialize(Vector &result, const GeosGeometry &geom) const {
	// Get the size of the serialized geometry
	const auto raw = geom.get_raw();
	const auto size = GeosSerde::GetRequiredSize(ctx, raw);

	// Allocate a blob of the correct size
	auto blob = StringVector::EmptyString(result, size);
	const auto ptr = blob.GetDataWriteable();

	// Serialize the geometry into the blob
	GeosSerde::Serialize(ctx, raw, ptr, size);

	// Finalize and return the blob
	blob.Finalize();
	return blob;
}

GeosGeometry LocalState::Deserialize(const string_t &blob) const {
	const auto blob_ptr = blob.GetData();
	const auto blob_len = blob.GetSize();

	const auto geom = GeosSerde::Deserialize(ctx, blob_ptr, blob_len);

	if (geom == nullptr) {
		throw InvalidInputException("Could not deserialize geometry");
	}

	return GeosGeometry(ctx, geom);
}

} // namespace

//------------------------------------------------------------------------------
// Base Functions
//------------------------------------------------------------------------------

namespace {

template <class IMPL, class RETURN_TYPE = bool>
class SymmetricPreparedBinaryFunction {
public:
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		auto &lhs_vec = args.data[0];
		auto &rhs_vec = args.data[1];

		const auto lhs_is_const =
		    lhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(lhs_vec);
		const auto rhs_is_const =
		    rhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(rhs_vec);

		if (lhs_is_const && rhs_is_const) {
			// Both are const, just execute once
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			const auto &lhs_blob = ConstantVector::GetData<string_t>(lhs_vec)[0];
			const auto &rhs_blob = ConstantVector::GetData<string_t>(rhs_vec)[0];
			const auto lhs_geom = lstate.Deserialize(lhs_blob);
			const auto rhs_geom = lstate.Deserialize(rhs_blob);
			ConstantVector::GetData<RETURN_TYPE>(result)[0] = IMPL::ExecutePredicateNormal(lhs_geom, rhs_geom);

		} else if (lhs_is_const != rhs_is_const) {
			// One of the two is const, prepare the const one and execute on the non-const one
			auto &const_vec = lhs_is_const ? lhs_vec : rhs_vec;
			auto &probe_vec = lhs_is_const ? rhs_vec : lhs_vec;

			const auto &const_blob = ConstantVector::GetData<string_t>(const_vec)[0];
			const auto const_geom = lstate.Deserialize(const_blob);
			const auto const_prep = const_geom.get_prepared();

			UnaryExecutor::Execute<string_t, RETURN_TYPE>(
			    probe_vec, result, args.size(), [&](const string_t &probe_blob) {
				    const auto probe_geom = lstate.Deserialize(probe_blob);
				    return IMPL::ExecutePredicatePrepared(const_prep, probe_geom);
			    });
		} else {
			// Both are non-const, just execute normally
			BinaryExecutor::Execute<string_t, string_t, RETURN_TYPE>(
			    lhs_vec, rhs_vec, result, args.size(), [&](const string_t &lhs_blob, const string_t &rhs_blob) {
				    const auto lhs = lstate.Deserialize(lhs_blob);
				    const auto rhs = lstate.Deserialize(rhs_blob);
				    return IMPL::ExecutePredicateNormal(lhs, rhs);
			    });
		}
	}
};

template <class IMPL, class RETURN_TYPE = bool>
class AsymmetricPreparedBinaryFunction {
public:
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		auto &lhs_vec = args.data[0];
		auto &rhs_vec = args.data[1];

		const auto lhs_is_const =
		    lhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(lhs_vec);
		const auto rhs_is_const =
		    rhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(rhs_vec);

		if (lhs_is_const && rhs_is_const) {
			// Both are const, just execute once
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			const auto &lhs_blob = ConstantVector::GetData<string_t>(lhs_vec)[0];
			const auto &rhs_blob = ConstantVector::GetData<string_t>(rhs_vec)[0];
			const auto lhs_geom = lstate.Deserialize(lhs_blob);
			const auto rhs_geom = lstate.Deserialize(rhs_blob);
			ConstantVector::GetData<RETURN_TYPE>(result)[0] = IMPL::ExecutePredicateNormal(lhs_geom, rhs_geom);

		} else if (lhs_is_const) {
			// Prepare the left const and run on the non-const right
			// Because this predicate is not symmetric, we can't just swap the two, so we only prepare the left
			const auto lhs_blob = ConstantVector::GetData<string_t>(lhs_vec)[0];
			const auto lhs_geom = lstate.Deserialize(lhs_blob);
			const auto lhs_prep = lhs_geom.get_prepared();

			UnaryExecutor::Execute<string_t, RETURN_TYPE>(rhs_vec, result, args.size(), [&](const string_t &rhs_blob) {
				const auto rhs_geom = lstate.Deserialize(rhs_blob);
				return IMPL::ExecutePredicatePrepared(lhs_prep, rhs_geom);
			});
		} else {
			// Both are non-const, just execute normally
			BinaryExecutor::Execute<string_t, string_t, RETURN_TYPE>(
			    lhs_vec, rhs_vec, result, args.size(), [&](const string_t &lhs_blob, const string_t &rhs_blob) {
				    const auto lhs = lstate.Deserialize(lhs_blob);
				    const auto rhs = lstate.Deserialize(rhs_blob);
				    return IMPL::ExecutePredicateNormal(lhs, rhs);
			    });
		}
	}
};

} // namespace

//------------------------------------------------------------------------------
// Functions
//------------------------------------------------------------------------------

namespace {

struct ST_Boundary {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
		    args.data[0], result, args.size(), [&](const string_t &geom_blob, ValidityMask &mask, idx_t row_idx) {
			    const auto geom = lstate.Deserialize(geom_blob);
			    if (geom.type() == GEOS_GEOMETRYCOLLECTION) {
				    mask.SetInvalid(row_idx);
				    return string_t {};
			    }
			    const auto boundary = geom.get_boundary();

			    return lstate.Serialize(result, boundary);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Boundary", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the \"boundary\" of a geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Buffer {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, double, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                    [&](const string_t &blob, double radius) {
			                                                    const auto geom = lstate.Deserialize(blob);
			                                                    const auto buffer = geom.get_buffer(radius, 8);
			                                                    return lstate.Serialize(result, buffer);
		                                                    });
	}

	static void ExecuteWithSegments(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		TernaryExecutor::Execute<string_t, double, int32_t, string_t>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
		    [&](const string_t &blob, double radius, int32_t segments) {
			    const auto geom = lstate.Deserialize(blob);
			    const auto buffer = geom.get_buffer(radius, segments);
			    return lstate.Serialize(result, buffer);
		    });
	}

	template <class T>
	static T TryParseStringArgument(const char *name, const vector<string> &keys, const vector<T> &values,
	                                const string_t &arg) {
		D_ASSERT(keys.size() == values.size());
		for (idx_t i = 0; i < keys.size(); i++) {
			if (StringUtil::CIEquals(keys[i], arg.GetString())) {
				return values[i];
			}
		}

		auto candidates = StringUtil::Join(keys, ", ");
		throw InvalidInputException("Unknown %s: '%s', accepted inputs: %s", name, arg.GetString().c_str(),
		                            candidates.c_str());
	}

	static void ExecuteWithStyle(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		SenaryExecutor::Execute<string_t, double, int32_t, string_t, string_t, double, string_t>(
		    args, result,
		    [&](const string_t &blob, double radius, int32_t segments, const string_t &cap_style_str,
		        const string_t &join_style_str, double mitre_limit) {
			    const auto geom = lstate.Deserialize(blob);
			    const auto cap_style = TryParseStringArgument<GEOSBufCapStyles>(
			        "cap style", {"CAP_ROUND", "CAP_FLAT", "CAP_SQUARE"},
			        {GEOSBUF_CAP_ROUND, GEOSBUF_CAP_FLAT, GEOSBUF_CAP_SQUARE}, cap_style_str);

			    const auto join_style = TryParseStringArgument<GEOSBufJoinStyles>(
			        "join style", {"JOIN_ROUND", "JOIN_MITRE", "JOIN_BEVEL"},
			        {GEOSBUF_JOIN_ROUND, GEOSBUF_JOIN_MITRE, GEOSBUF_JOIN_BEVEL}, join_style_str);

			    const auto buffer = geom.get_buffer_style(radius, segments, cap_style, join_style, mitre_limit);
			    return lstate.Serialize(result, buffer);
		    });
	}

	static constexpr auto DESCRIPTION = R"(
	    Returns a buffer around the input geometry at the target distance

	    `geom` is the input geometry.

	    `distance` is the target distance for the buffer, using the same units as the input geometry.

	    `num_triangles` represents how many triangles that will be produced to approximate a quarter circle. The larger the number, the smoother the resulting geometry. The default value is 8.

		`cap_style` must be one of "CAP_ROUND", "CAP_FLAT", "CAP_SQUARE". This parameter is case-insensitive.

	    `join_style` must be one of "JOIN_ROUND", "JOIN_MITRE", "JOIN_BEVEL". This parameter is case-insensitive.

	    `mitre_limit` only applies when `join_style` is "JOIN_MITRE". It is the ratio of the distance from the corner to the mitre point to the corner radius. The default value is 1.0.

	    This is a planar operation and will not take into account the curvature of the earth.
	)";
	static constexpr auto EXAMPLE = "";

	static void Register(ExtensionLoader &loader) {

		FunctionBuilder::RegisterScalar(loader, "ST_Buffer", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.AddParameter("num_triangles", LogicalType::INTEGER);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteWithSegments);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.AddParameter("num_triangles", LogicalType::INTEGER);
				variant.AddParameter("cap_style", LogicalType::VARCHAR);
				variant.AddParameter("join_style", LogicalType::VARCHAR);
				variant.AddParameter("mitre_limit", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteWithStyle);
			});

			func.SetDescription(DESCRIPTION);
			func.SetExample(EXAMPLE);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_BuildArea {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto area = geom.get_built_area();
			return lstate.Serialize(result, area);
		});
	}

	static constexpr auto DESCRIPTION = R"(
		Creates a polygonal geometry by attemtping to "fill in" the input geometry.

		Unlike ST_Polygonize, this function does not fill in holes.)";

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_BuildArea", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(DESCRIPTION);
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Contains : AsymmetricPreparedBinaryFunction<ST_Contains> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.contains(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.contains(rhs);
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Contains", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Returns true if the first geometry contains the second geometry

				In contrast to `ST_ContainsProperly`, this function will also return true if `geom2` is contained strictly on the boundary of `geom1`.
				A geometry always `ST_Contains` itself, but does not `ST_ContainsProperly` itself.
			)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_ContainsProperly : AsymmetricPreparedBinaryFunction<ST_ContainsProperly> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		// We have no choice but to prepare the left geometry
		const auto lhs_prep = lhs.get_prepared();
		return lhs_prep.contains_properly(rhs);
	}

	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.contains_properly(rhs);
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_ContainsProperly", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Returns true if the first geometry \"properly\" contains the second geometry

				In contrast to `ST_Contains`, this function does not return true if `geom2` is contained strictly on the boundary of `geom1`.
				A geometry always `ST_Contains` itself, but does not `ST_ContainsProperly` itself.
			)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_WithinProperly : AsymmetricPreparedBinaryFunction<ST_WithinProperly> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		// We have no choice but to prepare the right geometry
		const auto rhs_prep = rhs.get_prepared();
		return rhs_prep.contains_properly(lhs);
	}

	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.contains_properly(rhs);
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_WithinProperly", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Returns true if the first geometry \"properly\" is contained by the second geometry

				This function functions the same as `ST_ContainsProperly`, but the arguments are swapped.
			)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_ConcaveHull {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		TernaryExecutor::Execute<string_t, double, bool, string_t>(
		    args.data[0], args.data[1], args.data[2], result, args.size(),
		    [&](const string_t &geom_blob, const double ratio, const bool allowHoles) {
			    const auto geom = lstate.Deserialize(geom_blob);
			    const auto hull = geom.get_concave_hull(ratio, allowHoles);
			    return lstate.Serialize(result, hull);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_ConcaveHull", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("ratio", LogicalType::DOUBLE);
				variant.AddParameter("allowHoles", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(
			    "Returns the 'concave' hull of the input geometry, containing all of the source input's points, and "
			    "which can be used to create polygons from points. The ratio parameter dictates the level of "
			    "concavity; 1.0 returns the convex hull; and 0 indicates to return the most concave hull possible. Set "
			    "allowHoles to a non-zero value to allow output containing holes.");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_ConvexHull {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto hull = geom.get_convex_hull();
			return lstate.Serialize(result, hull);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_ConvexHull", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the convex hull enclosing the geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_CoverageInvalidEdges {

	static unique_ptr<FunctionData> Bind(ClientContext &context, ScalarFunction &bound_function,
	                                     vector<unique_ptr<Expression>> &arguments) {

		// Set the default value for the tolerance parameter
		if (arguments.size() == 1) {
			arguments.push_back(make_uniq_base<Expression, BoundConstantExpression>(Value::DOUBLE(0)));
		}
		return nullptr;
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnifiedVectorFormat format;

		auto &list_vec = args.data[0];
		auto &item_vec = ListVector::GetEntry(list_vec);
		item_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), format);

		// Collection to hold the working set of geometries
		GeosCollection collection(lstate.GetContext());

		BinaryExecutor::ExecuteWithNulls<list_entry_t, double, string_t>(
		    list_vec, args.data[1], result, args.size(),
		    [&](const list_entry_t &list, double tolerance, ValidityMask &mask, idx_t row_idx) {
			    // Reset the collection
			    collection.clear();
			    collection.reserve(list.length);

			    const auto offset = list.offset;
			    const auto length = list.length;

			    // Collect all geometries in the list into the collection
			    for (idx_t i = offset; i < offset + length; i++) {
				    const auto mapped_idx = format.sel->get_index(i);

				    if (!format.validity.RowIsValid(mapped_idx)) {
					    continue;
				    }

				    const auto &geom_blob = UnifiedVectorFormat::GetData<string_t>(format)[mapped_idx];

				    auto geom = lstate.Deserialize(geom_blob);
				    collection.add(std::move(geom));
			    }

			    // Now make a geometrycollection and get the invalid edges
			    const auto geometry_col = collection.get_collection();
			    const auto invalid = geometry_col.get_coverage_invalid_edges(tolerance);

			    if (invalid.is_empty()) {
				    mask.SetInvalid(row_idx);
				    return string_t {};
			    }

			    return lstate.Serialize(result, invalid);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_CoverageInvalidEdges", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geoms", LogicalType::LIST(GeoTypes::GEOMETRY()));
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geoms", LogicalType::LIST(GeoTypes::GEOMETRY()));
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Returns the invalid edges in a polygonal coverage, which are edges that are not shared by two polygons.
				Returns NULL if the input is not a polygonal coverage, or if the input is valid.
				Tolerance is 0 by default.
			)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_CoverageSimplify {

	static unique_ptr<FunctionData> Bind(ClientContext &context, ScalarFunction &bound_function,
	                                     vector<unique_ptr<Expression>> &arguments) {
		// Set the default value for the simplify_boundary parameter
		if (arguments.size() == 2) {
			arguments.push_back(make_uniq_base<Expression, BoundConstantExpression>(Value::BOOLEAN(true)));
		}
		return nullptr;
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnifiedVectorFormat format;

		auto &list_vec = args.data[0];
		auto &item_vec = ListVector::GetEntry(list_vec);
		item_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), format);

		// Collection to hold the working set of geometries
		GeosCollection collection(lstate.GetContext());

		TernaryExecutor::Execute<list_entry_t, double, bool, string_t>(
		    list_vec, args.data[1], args.data[2], result, args.size(),
		    [&](const list_entry_t &list, double tolerance, bool simplify_boundary) {
			    // Reset the collection
			    collection.clear();
			    collection.reserve(list.length);

			    const auto offset = list.offset;
			    const auto length = list.length;

			    // Collect all geometries in the list into the collection
			    for (idx_t i = offset; i < offset + length; i++) {
				    const auto mapped_idx = format.sel->get_index(i);

				    if (!format.validity.RowIsValid(mapped_idx)) {
					    continue;
				    }

				    const auto &geom_blob = UnifiedVectorFormat::GetData<string_t>(format)[mapped_idx];

				    auto geom = lstate.Deserialize(geom_blob);
				    collection.add(std::move(geom));
			    }

			    // Now make a geometrycollection and simplify
			    const auto geometry_col = collection.get_collection();
			    const auto simplified = geometry_col.get_coverage_simplified(tolerance, !simplify_boundary);
			    return lstate.Serialize(result, simplified);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_CoverageSimplify", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geoms", LogicalType::LIST(GeoTypes::GEOMETRY()));
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.AddParameter("simplify_boundary", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geoms", LogicalType::LIST(GeoTypes::GEOMETRY()));
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetBind(Bind);
				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Simplify the edges in a polygonal coverage, preserving the coverange by ensuring that the there are no seams between the resulting simplified polygons.

				By default, the boundary of the coverage is also simplified, but this can be controlled with the optional third 'simplify_boundary' parameter.
			)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_CoverageUnion {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnifiedVectorFormat format;

		auto &list_vec = args.data[0];
		auto &item_vec = ListVector::GetEntry(list_vec);
		item_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), format);

		// Collection to hold the working set of geometries
		GeosCollection collection(lstate.GetContext());

		UnaryExecutor::Execute<list_entry_t, string_t>(list_vec, result, args.size(), [&](const list_entry_t &list) {
			// Reset the collection
			collection.clear();
			collection.reserve(list.length);

			const auto offset = list.offset;
			const auto length = list.length;

			// Collect all geometries in the list into the collection
			for (idx_t i = offset; i < offset + length; i++) {
				const auto mapped_idx = format.sel->get_index(i);

				if (!format.validity.RowIsValid(mapped_idx)) {
					continue;
				}

				const auto &geom_blob = UnifiedVectorFormat::GetData<string_t>(format)[mapped_idx];

				auto geom = lstate.Deserialize(geom_blob);
				collection.add(std::move(geom));
			}

			// Now make a geometrycollection and simplify
			const auto geometry_col = collection.get_collection();
			const auto unioned = geometry_col.get_coverage_union();
			return lstate.Serialize(result, unioned);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_CoverageUnion", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geoms", LogicalType::LIST(GeoTypes::GEOMETRY()));
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Union all geometries in a polygonal coverage into a single geometry.
				This may be faster than using `ST_Union`, but may use more memory.
			)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_CoveredBy : AsymmetricPreparedBinaryFunction<ST_CoveredBy> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.covered_by(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.covered_by(rhs);
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_CoveredBy", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if geom1 is \"covered by\" geom2");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_Covers : AsymmetricPreparedBinaryFunction<ST_Covers> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.covers(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.covers(rhs);
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Covers", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geom1 \"covers\" geom2");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_Crosses : SymmetricPreparedBinaryFunction<ST_Crosses> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.crosses(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.crosses(rhs);
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Crosses", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if geom1 \"crosses\" geom2");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_Difference {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                      [&](const string_t &lhs_blob, const string_t &rhs_blob) {
			                                                      const auto lhs = lstate.Deserialize(lhs_blob);
			                                                      const auto rhs = lstate.Deserialize(rhs_blob);
			                                                      const auto difference = lhs.get_difference(rhs);
			                                                      return lstate.Serialize(result, difference);
		                                                      });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Difference", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the \"difference\" between two geometries");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Disjoint : SymmetricPreparedBinaryFunction<ST_Disjoint> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.disjoint(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.disjoint(rhs);
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Disjoint", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometries are disjoint");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_Distance : SymmetricPreparedBinaryFunction<ST_Distance, double> {
	static double ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.distance_to(rhs);
	}
	static double ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.distance_to(rhs);
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Distance_GEOS", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::DOUBLE);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the planar distance between two geometries");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "measurement");
		});
	}
};

struct ST_DistanceWithin {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		// Because this takes an extra argument, we cant reuse the SymmetricPreparedBinary...

		const auto &lstate = LocalState::ResetAndGet(state);

		auto &lhs_vec = args.data[0];
		auto &rhs_vec = args.data[1];
		auto &arg_vec = args.data[2];

		const auto lhs_is_const =
		    lhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(lhs_vec);
		const auto rhs_is_const =
		    rhs_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(rhs_vec);
		const auto arg_is_const =
		    arg_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(arg_vec);

		if (lhs_is_const && rhs_is_const && arg_is_const) {
			// Both geometries (and the argument) are constant, so only execute it once
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			const auto &lhs_blob = ConstantVector::GetData<string_t>(lhs_vec)[0];
			const auto &rhs_blob = ConstantVector::GetData<string_t>(rhs_vec)[0];
			const auto &arg_dist = ConstantVector::GetData<double>(arg_vec)[0];
			const auto lhs_geom = lstate.Deserialize(lhs_blob);
			const auto rhs_geom = lstate.Deserialize(rhs_blob);

			ConstantVector::GetData<bool>(result)[0] = lhs_geom.distance_within(rhs_geom, arg_dist);
		} else if (lhs_is_const && rhs_is_const && !arg_is_const) {
			// The geometries are constant, but the distance is not, prepare the larger one and execute unary

			const auto &lhs_blob = ConstantVector::GetData<string_t>(lhs_vec)[0];
			const auto &rhs_blob = ConstantVector::GetData<string_t>(rhs_vec)[0];

			const auto rhs_bigger = rhs_blob.GetSize() > lhs_blob.GetSize();

			const auto large_geom = rhs_bigger ? lstate.Deserialize(rhs_blob) : lstate.Deserialize(lhs_blob);
			const auto probe_geom = rhs_bigger ? lstate.Deserialize(lhs_blob) : lstate.Deserialize(rhs_blob);

			// PreparedDistanceWithin only works if one is prepared. so just choose the larger one
			const auto prep_geom = large_geom.get_prepared();

			UnaryExecutor::Execute<double, bool>(arg_vec, result, args.size(), [&](const double arg_dist) {
				return prep_geom.distance_within(probe_geom, arg_dist);
			});

		} else if (lhs_is_const != rhs_is_const) {
			// One of the two is const, prepare the const one and execute on the non-const one
			auto &const_vec = lhs_is_const ? lhs_vec : rhs_vec;
			auto &probe_vec = lhs_is_const ? rhs_vec : lhs_vec;

			const auto &const_blob = ConstantVector::GetData<string_t>(const_vec)[0];
			const auto const_geom = lstate.Deserialize(const_blob);
			const auto const_prep = const_geom.get_prepared();

			BinaryExecutor::Execute<string_t, double, bool>(probe_vec, arg_vec, result, args.size(),
			                                                [&](const string_t &probe_blob, double distance) {
				                                                const auto probe_geom = lstate.Deserialize(probe_blob);
				                                                return const_prep.distance_within(probe_geom, distance);
			                                                });
		} else {
			// Both are non-const, just execute normally
			TernaryExecutor::Execute<string_t, string_t, double, bool>(
			    lhs_vec, rhs_vec, arg_vec, result, args.size(),
			    [&](const string_t &lhs_blob, const string_t &rhs_blob, double distance) {
				    const auto lhs = lstate.Deserialize(lhs_blob);
				    const auto rhs = lstate.Deserialize(rhs_blob);
				    return lhs.distance_within(rhs, distance);
			    });
		}
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_DWithin_GEOS", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.AddParameter("distance", LogicalType::DOUBLE);
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription(R"(
				Returns if two geometries are within a target distance of each-other
			)");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_Equals {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, bool>(args.data[0], args.data[1], result, args.size(),
		                                                  [&](const string_t &lhs_blob, const string_t &rhs_blob) {
			                                                  const auto lhs = lstate.Deserialize(lhs_blob);
			                                                  const auto rhs = lstate.Deserialize(rhs_blob);
			                                                  return lhs.equals(rhs);
		                                                  });
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Equals", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometries are \"equal\"");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_Envelope {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto envelope = geom.get_envelope();
			return lstate.Serialize(result, envelope);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Envelope", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the minimum bounding rectangle of a geometry as a polygon geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Intersection {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                      [&](const string_t &lhs_blob, const string_t &rhs_blob) {
			                                                      const auto lhs = lstate.Deserialize(lhs_blob);
			                                                      const auto rhs = lstate.Deserialize(rhs_blob);
			                                                      const auto intersection = lhs.get_intersection(rhs);
			                                                      return lstate.Serialize(result, intersection);
		                                                      });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Intersection", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the intersection of two geometries");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Intersects : SymmetricPreparedBinaryFunction<ST_Intersects> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.intersects(rhs);
	}

	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.intersects(rhs);
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Intersects", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometries intersect");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_IsRing {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			return geom.is_ring();
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_IsRing", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometry is a ring (both ST_IsClosed and ST_IsSimple).");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

struct ST_IsSimple {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			return geom.is_simple();
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_IsSimple", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometry is simple");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

struct ST_IsValid {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			// GEOS can only construct geometries with a valid amount of vertices.
			// So if deserialization fails, it cant be valid
			try {
				const auto geom = lstate.Deserialize(geom_blob);
				return geom.is_valid();
			} catch (...) {
				return false;
			}
		});
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_IsValid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometry is valid");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "property");
		});
	}
};

struct ST_LineMerge {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(),
		                                           [&](const string_t &geometry_blob) {
			                                           const auto geometry = lstate.Deserialize(geometry_blob);
			                                           const auto merged = geometry.get_linemerged(false);
			                                           return lstate.Serialize(result, merged);
		                                           });
	}

	static void ExecuteWithDirection(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		BinaryExecutor::Execute<string_t, bool, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                  [&](const string_t &geometry_blob, bool preserve_direction) {
			                                                  const auto geometry = lstate.Deserialize(geometry_blob);
			                                                  const auto merged =
			                                                      geometry.get_linemerged(preserve_direction);
			                                                  return lstate.Serialize(result, merged);
		                                                  });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_LineMerge", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());
				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("preserve_direction", LogicalType::BOOLEAN);
				variant.SetReturnType(GeoTypes::GEOMETRY());
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteWithDirection);
			});

			func.SetDescription(R"("Merges" the input line geometry, optionally taking direction into account.)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_MakeValid {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto valid = geom.get_made_valid();
			return lstate.Serialize(result, valid);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_MakeValid", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns a valid representation of the geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_MaximumInscribedCircle {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		auto &struct_vecs = StructVector::GetEntries(result);
		auto &center_vec = *struct_vecs[0];
		auto &nearest_vec = *struct_vecs[1];

		using STRING_TYPE = PrimitiveType<string_t>;
		using RESULT_TYPE = StructTypeTernary<string_t, string_t, double>;

		GenericExecutor::ExecuteUnary<STRING_TYPE, RESULT_TYPE>(
		    args.data[0], result, args.size(), [&](const STRING_TYPE &geom_blob_val) {
			    const auto &geom_blob = geom_blob_val.val;

			    const auto geom = lstate.Deserialize(geom_blob);
			    const auto circle = geom.get_maximum_inscribed_circle();

			    const auto center = circle.get_point_n(0);
			    const auto nearest = circle.get_point_n(1);
			    const auto radius = center.distance_to(nearest);

			    const auto center_blob = lstate.Serialize(center_vec, center);
			    const auto nearest_blob = lstate.Serialize(nearest_vec, nearest);

			    return RESULT_TYPE {center_blob, nearest_blob, radius};
		    });
	}

	static void ExecuteWithTolerance(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		auto &struct_vecs = StructVector::GetEntries(result);
		auto &center_vec = *struct_vecs[0];
		auto &nearest_vec = *struct_vecs[1];

		using STRING_TYPE = PrimitiveType<string_t>;
		using DOUBLE_TYPE = PrimitiveType<double>;
		using RESULT_TYPE = StructTypeTernary<string_t, string_t, double>;

		GenericExecutor::ExecuteBinary<STRING_TYPE, DOUBLE_TYPE, RESULT_TYPE>(
		    args.data[0], args.data[1], result, args.size(),
		    [&](const STRING_TYPE &geom_blob_val, const DOUBLE_TYPE &tolerance_val) {
			    const auto &geom_blob = geom_blob_val.val;
			    const auto &tolerance = tolerance_val.val;

			    const auto geom = lstate.Deserialize(geom_blob);
			    const auto circle = geom.get_maximum_inscribed_circle(tolerance);

			    const auto center = circle.get_point_n(0);
			    const auto nearest = circle.get_point_n(1);
			    const auto radius = center.distance_to(nearest);

			    const auto center_blob = lstate.Serialize(center_vec, center);
			    const auto nearest_blob = lstate.Serialize(nearest_vec, nearest);

			    return RESULT_TYPE {center_blob, nearest_blob, radius};
		    });
	}

	static void Register(ExtensionLoader &loader) {

		const auto result_type = LogicalType::STRUCT(
		    {{"center", GeoTypes::GEOMETRY()}, {"nearest", GeoTypes::GEOMETRY()}, {"radius", LogicalType::DOUBLE}});

		FunctionBuilder::RegisterScalar(loader, "ST_MaximumInscribedCircle", [&](ScalarFunctionBuilder &func) {
			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(result_type);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([&](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.SetReturnType(result_type);
				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteWithTolerance);
			});

			func.SetDescription(R"(
				Returns the maximum inscribed circle of the input geometry, optionally with a tolerance.

				By default, the tolerance is computed as `max(width, height) / 1000`.
				The return value is a struct with the center of the circle, the nearest point to the center on the boundary of the geometry, and the radius of the circle.
			)");

			func.SetExample(R"(
				-- Find the maximum inscribed circle of a square
				SELECT ST_MaximumInscribedCircle(
					ST_GeomFromText('POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))')
				);
				----
				{'center': POINT (5 5), 'nearest': POINT (5 0), 'radius': 5.0}
			)");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_MinimumRotatedRectangle {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto mrr = geom.get_minimum_rotated_rectangle();
			return lstate.Serialize(result, mrr);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_MinimumRotatedRectangle", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the minimum rotated rectangle that bounds the input geometry, finding the "
			                    "surrounding box that has the lowest area by using a rotated rectangle, rather than "
			                    "taking the lowest and highest coordinate values as per ST_Envelope().");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Node {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto node = geom.get_noded();
			return lstate.Serialize(result, node);
		});
	}

	static constexpr auto DESCRIPTION = R"(
		Returns a "noded" MultiLinestring, produced by combining a collection of input linestrings and adding additional vertices where they intersect.
	)";
	static constexpr auto EXAMPLE = R"(
		-- Create a noded multilinestring from two intersecting lines
		SELECT ST_Node(
			ST_GeomFromText('MULTILINESTRING((0 0, 2 2), (0 2, 2 0))')
		);
		----
		MULTILINESTRING ((0 0, 1 1), (1 1, 2 2), (0 2, 1 1), (1 1, 2 0))
	)";

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Node", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

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

struct ST_Normalize {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			geom.normalize_in_place();
			return lstate.Serialize(result, geom);
		});
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Normalize", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the \"normalized\" representation of the geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Overlaps : SymmetricPreparedBinaryFunction<ST_Overlaps> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.overlaps(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.overlaps(rhs);
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Overlaps", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometries overlap");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_PointOnSurface {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto point = geom.get_point_on_surface();
			return lstate.Serialize(result, point);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_PointOnSurface", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns a point guaranteed to lie on the surface of the geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Polygonize {

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnifiedVectorFormat format;

		auto &list_vec = args.data[0];
		auto &item_vec = ListVector::GetEntry(list_vec);
		item_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), format);

		// Collection to hold the working set of geometries
		GeosCollection collection(lstate.GetContext());

		UnaryExecutor::Execute<list_entry_t, string_t>(list_vec, result, args.size(), [&](const list_entry_t &list) {
			// Reset the collection
			collection.clear();
			collection.reserve(list.length);

			const auto offset = list.offset;
			const auto length = list.length;

			// Collect all geometries in the list into the collection
			for (idx_t i = offset; i < offset + length; i++) {
				const auto mapped_idx = format.sel->get_index(i);

				if (!format.validity.RowIsValid(mapped_idx)) {
					continue;
				}

				const auto &geom_blob = UnifiedVectorFormat::GetData<string_t>(format)[mapped_idx];

				auto geom = lstate.Deserialize(geom_blob);
				collection.add(std::move(geom));
			}

			// Now polygonize the collection
			const auto polygon = collection.get_polygonized();
			return lstate.Serialize(result, polygon);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Polygonize", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geometries", LogicalType::LIST(GeoTypes::GEOMETRY()));
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns a polygonized representation of the input geometries");
			func.SetExample(R"(
				-- Create a polygon from a closed linestring ring
				SELECT ST_Polygonize([
					ST_GeomFromText('LINESTRING(0 0, 0 10, 10 10, 10 0, 0 0)')
				]);
				---
				GEOMETRYCOLLECTION (POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0)))
			)");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_ReducePrecision {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, double, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &geom_blob, double precision) {
			    const auto geom = lstate.Deserialize(geom_blob);
			    const auto reduced = geom.get_reduced_precision(precision);
			    return lstate.Serialize(result, reduced);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_ReducePrecision", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("precision", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the geometry with all vertices reduced to the given precision");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_RemoveRepeatedPoints {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto reduced = geom.get_without_repeated_points(0);
			return lstate.Serialize(result, reduced);
		});
	}

	static void ExecuteWithTolerance(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, double, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &geom_blob, double tolerance) {
			    const auto geom = lstate.Deserialize(geom_blob);
			    const auto reduced = geom.get_without_repeated_points(tolerance);
			    return lstate.Serialize(result, reduced);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_RemoveRepeatedPoints", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(ExecuteWithTolerance);
			});

			func.SetDescription("Returns the geometry with repeated points removed");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Reverse {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto reversed = geom.get_reversed();
			return lstate.Serialize(result, reversed);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Reverse", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the geometry with the order of its vertices reversed");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_ShortestLine {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		BinaryExecutor::Execute<string_t, string_t, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                      [&](const string_t &lhs_blob, const string_t &rhs_blob) {
			                                                      const auto lhs = lstate.Deserialize(lhs_blob);
			                                                      const auto rhs = lstate.Deserialize(rhs_blob);
			                                                      const auto line = lhs.get_shortest_line(rhs);
			                                                      return lstate.Serialize(result, line);
		                                                      });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_ShortestLine", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the shortest line between two geometries");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "measurement");
		});
	}
};

struct ST_Simplify {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		BinaryExecutor::Execute<string_t, double, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                    [&](const string_t &geom_blob, double tolerance) {
			                                                    const auto geom = lstate.Deserialize(geom_blob);
			                                                    const auto simplified = geom.get_simplified(tolerance);
			                                                    return lstate.Serialize(result, simplified);
		                                                    });
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Simplify", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns a simplified version of the geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_SimplifyPreserveTopology {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);
		BinaryExecutor::Execute<string_t, double, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](const string_t &geom_blob, double tolerance) {
			    const auto geom = lstate.Deserialize(geom_blob);
			    const auto simplified = geom.get_simplified_topo(tolerance);
			    return lstate.Serialize(result, simplified);
		    });
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_SimplifyPreserveTopology", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.AddParameter("tolerance", LogicalType::DOUBLE);
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns a simplified version of the geometry that preserves topology");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Touches : SymmetricPreparedBinaryFunction<ST_Touches> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.touches(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.touches(rhs);
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Touches", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the geometries touch");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

struct ST_Union {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		BinaryExecutor::Execute<string_t, string_t, string_t>(args.data[0], args.data[1], result, args.size(),
		                                                      [&](const string_t &lhs_blob, const string_t &rhs_blob) {
			                                                      const auto lhs = lstate.Deserialize(lhs_blob);
			                                                      const auto rhs = lstate.Deserialize(rhs_blob);
			                                                      const auto unioned = lhs.get_union(rhs);
			                                                      return lstate.Serialize(result, unioned);
		                                                      });
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Union", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the union of two geometries");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_VoronoiDiagram {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &lstate = LocalState::ResetAndGet(state);

		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &geom_blob) {
			const auto geom = lstate.Deserialize(geom_blob);
			const auto mrr = geom.get_voronoi_diagram();
			return lstate.Serialize(result, mrr);
		});
	}

	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_VoronoiDiagram", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom", GeoTypes::GEOMETRY());
				variant.SetReturnType(GeoTypes::GEOMETRY());

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns the Voronoi diagram of the supplied MultiPoint geometry");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

struct ST_Within : AsymmetricPreparedBinaryFunction<ST_Within> {
	static bool ExecutePredicateNormal(const GeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.within(rhs);
	}
	static bool ExecutePredicatePrepared(const PreparedGeosGeometry &lhs, const GeosGeometry &rhs) {
		return lhs.within(rhs);
	}
	static void Register(ExtensionLoader &loader) {
		FunctionBuilder::RegisterScalar(loader, "ST_Within", [](ScalarFunctionBuilder &func) {
			func.AddVariant([](ScalarFunctionVariantBuilder &variant) {
				variant.AddParameter("geom1", GeoTypes::GEOMETRY());
				variant.AddParameter("geom2", GeoTypes::GEOMETRY());
				variant.SetReturnType(LogicalType::BOOLEAN);

				variant.SetInit(LocalState::Init);
				variant.SetFunction(Execute);
			});

			func.SetDescription("Returns true if the first geometry is within the second");
			func.SetTag("ext", "spatial");
			func.SetTag("category", "relation");
		});
	}
};

//######################################################################################################################
// Aggregate Functions
//######################################################################################################################

//======================================================================================================================
// Base GEOS-based unary aggregate
//======================================================================================================================
struct GeosUnaryAggState {
	GEOSGeometry *geom = nullptr;
	GEOSContextHandle_t context = nullptr;
};

struct GeosUnaryAggFunction {

	// Serialize a GEOS geometry
	static string_t Serialize(const GEOSContextHandle_t context, Vector &result, const GEOSGeometry *geom) {
		D_ASSERT(geom);
		const auto size = GeosSerde::GetRequiredSize(context, geom);
		auto blob = StringVector::EmptyString(result, size);
		const auto ptr = blob.GetDataWriteable();

		// Serialize the geometry
		GeosSerde::Serialize(context, geom, ptr, size);

		blob.Finalize();
		return blob;
	}

	// Deserialize a GEOS geometry
	static GEOSGeometry *Deserialize(const GEOSContextHandle_t context, const string_t &blob) {
		const auto ptr = blob.GetData();
		const auto size = blob.GetSize();

		return GeosSerde::Deserialize(context, ptr, size);
	}

	template <class STATE>
	static void Initialize(STATE &state) {
		state.geom = nullptr;
		state.context = GEOS_init_r();
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &data) {
		if (!source.geom) {
			return;
		}
		if (!target.geom) {
			target.geom = GEOSGeom_clone_r(target.context, source.geom);
			return;
		}
		auto curr = target.geom;
		target.geom = OP::Merge(target.context, curr, source.geom);
		GEOSGeom_destroy_r(target.context, curr);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		if (!state.geom) {
			state.geom = Deserialize(state.context, input);
		} else {
			auto next = Deserialize(state.context, input);
			auto curr = state.geom;
			state.geom = OP::Merge(state.context, curr, next);
			GEOSGeom_destroy_r(state.context, next);
			GEOSGeom_destroy_r(state.context, curr);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &, idx_t) {
		// There is no point in doing anything else, intersection and union is idempotent
		if (!state.geom) {
			state.geom = Deserialize(state.context, input);
		}
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.geom) {
			finalize_data.ReturnNull();
		} else {
			target = Serialize(state.context, finalize_data.result, state.geom);
		}
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &) {
		if (state.geom) {
			GEOSGeom_destroy_r(state.context, state.geom);
			state.geom = nullptr;
		}
		if (state.context) {
			GEOS_finish_r(state.context);
			state.context = nullptr;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

//======================================================================================================================
// ST_MemUnion_Agg
//======================================================================================================================

struct ST_MemUnion_Agg : GeosUnaryAggFunction {
	static GEOSGeometry *Merge(const GEOSContextHandle_t context, const GEOSGeometry *curr, const GEOSGeometry *next) {
		return GEOSUnion_r(context, curr, next);
	}

	static void Register(ExtensionLoader &loader) {
		const auto agg =
		    AggregateFunction::UnaryAggregateDestructor<GeosUnaryAggState, string_t, string_t, ST_MemUnion_Agg>(
		        GeoTypes::GEOMETRY(), GeoTypes::GEOMETRY());

		FunctionBuilder::RegisterAggregate(loader, "ST_MemUnion_Agg", [&](AggregateFunctionBuilder &func) {
			func.SetFunction(agg);
			func.SetDescription(R"(Computes the union of a set of input geometries.
				"Slower, but might be more memory efficient than ST_UnionAgg as each geometry is merged into the union individually rather than all at once.)");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_Intersection_Agg
//======================================================================================================================

struct ST_Intersection_Agg : GeosUnaryAggFunction {
	static GEOSGeometry *Merge(const GEOSContextHandle_t context, const GEOSGeometry *curr, const GEOSGeometry *next) {
		return GEOSIntersection_r(context, curr, next);
	}

	static void Register(ExtensionLoader &loader) {
		const auto agg =
		    AggregateFunction::UnaryAggregateDestructor<GeosUnaryAggState, string_t, string_t, ST_Intersection_Agg>(
		        GeoTypes::GEOMETRY(), GeoTypes::GEOMETRY());

		FunctionBuilder::RegisterAggregate(loader, "ST_Intersection_Agg", [&](AggregateFunctionBuilder &func) {
			func.SetFunction(agg);
			func.SetDescription("Computes the intersection of a set of geometries");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_Union_Agg
//======================================================================================================================
struct ST_Union_Agg {

	struct State {
		GEOSContextHandle_t context = nullptr;
		vector<GEOSGeometry *> geoms;
	};

	static idx_t StateSize(const AggregateFunction &) {
		return sizeof(State);
	}

	static string_t Serialize(const GEOSContextHandle_t context, Vector &result, const GEOSGeometry *geom) {
		D_ASSERT(geom);
		const auto size = GeosSerde::GetRequiredSize(context, geom);
		auto blob = StringVector::EmptyString(result, size);
		const auto ptr = blob.GetDataWriteable();

		// Serialize the geometry
		GeosSerde::Serialize(context, geom, ptr, size);

		blob.Finalize();
		return blob;
	}

	// Deserialize a GEOS geometry
	static GEOSGeometry *Deserialize(const GEOSContextHandle_t context, const string_t &blob) {
		const auto ptr = blob.GetData();
		const auto size = blob.GetSize();

		return GeosSerde::Deserialize(context, ptr, size);
	}

	static void Initialize(const AggregateFunction &, data_ptr_t state_mem) {
		const auto state_ptr = new (state_mem) State();
		auto &state = *state_ptr;
		state.context = GEOS_init_r();
	}

	static void Update(Vector inputs[], AggregateInputData &, idx_t, Vector &state_vec, idx_t count) {

		auto &geom_vec = inputs[0];

		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(count, geom_format);

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto geom_ptr = UnifiedVectorFormat::GetData<string_t>(geom_format);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			const auto geom_idx = geom_format.sel->get_index(raw_idx);

			if (!geom_format.validity.RowIsValid(geom_idx)) {
				continue;
			}

			// Now, deserialize the geometry and append it to the list in each state
			auto &state = *state_ptr[state_idx];
			const auto geom = Deserialize(state.context, geom_ptr[geom_idx]);
			state.geoms.push_back(geom);
		}
	}

	static void Absorb(Vector &state_vec, Vector &combined, AggregateInputData &aggr_input_data, idx_t count) {
		D_ASSERT(aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE);

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto combined_ptr = FlatVector::GetData<State *>(combined);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			auto &state = *state_ptr[state_idx];

			// Steal the list from the state and append it to the combined state
			auto &combined_state = *combined_ptr[raw_idx];
			combined_state.geoms.insert(combined_state.geoms.end(), state.geoms.begin(), state.geoms.end());
			state.geoms.clear();
		}
	}

	static void Combine(Vector &state_vec, Vector &combined, AggregateInputData &aggr_input_data, idx_t count) {

		//	Can we use destructive combining?
		if (aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE) {
			Absorb(state_vec, combined, aggr_input_data, count);
			return;
		}

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto combined_ptr = FlatVector::GetData<State *>(combined);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			const auto &state = *state_ptr[state_idx];

			// We can't steal the list, we need to clone and append all elements
			auto &combined_state = *combined_ptr[raw_idx];

			for (auto &geom : state.geoms) {
				combined_state.geoms.push_back(GEOSGeom_clone_r(combined_state.context, geom));
			}
		}
	}

	static void Finalize(Vector &state_vec, AggregateInputData &aggr_input_data, Vector &result, idx_t count,
	                     idx_t offset) {

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			auto &state = *state_ptr[state_format.sel->get_index(raw_idx)];
			const auto out_idx = raw_idx + offset;

			// Now create a geometry collection out of all geometries
			const auto collection = GEOSGeom_createCollection_r(state.context, GEOS_GEOMETRYCOLLECTION,
			                                                    state.geoms.data(), state.geoms.size());

			// If we manage to construct the collection, it takes ownership of the geometries
			// So we need to leak the list or risk a double free
			state.geoms.clear();

			try {
				// Compute the coverage
				const auto result_union = GEOSUnaryUnion_r(state.context, collection);

				// Serialize the result
				const auto result_ptr = FlatVector::GetData<string_t>(result);
				result_ptr[out_idx] = Serialize(state.context, result, result_union);

				// Destroy the unioned geometry
				GEOSGeom_destroy_r(state.context, result_union);

				// Destroy the collection
				GEOSGeom_destroy_r(state.context, collection);

			} catch (...) {
				// Destroy the collection, and rethrow the exception
				GEOSGeom_destroy_r(state.context, collection);
				throw;
			}
		}
	}

	static void Destroy(Vector &state_vec, AggregateInputData &aggr, idx_t count) {
		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto row_idx = state_format.sel->get_index(raw_idx);
			if (state_format.validity.RowIsValid(row_idx)) {
				auto &state = *state_ptr[row_idx];

				state.geoms.clear();

				if (state.context) {
					GEOS_finish_r(state.context);
					state.context = nullptr;
				}
			}
		}
	}

	static void Register(ExtensionLoader &loader) {
		AggregateFunction agg({GeoTypes::GEOMETRY()}, GeoTypes::GEOMETRY(), StateSize, Initialize, Update, Combine,
		                      Finalize, nullptr, nullptr, Destroy);

		FunctionBuilder::RegisterAggregate(loader, "ST_Union_Agg", [&](AggregateFunctionBuilder &func) {
			func.SetFunction(agg);
			func.SetDescription("Computes the union of a set of input geometries");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// Base GEOS-based coverage aggregate
//======================================================================================================================

struct GEOSCoverageAggFunction {

	struct State {
		GEOSContextHandle_t context = nullptr;
		// This used to be a nice linked list.
		// Unfortunately, there are issues when using custom destructors in combination with both window and aggregate
		// functions. So we use a vector instead.
		// We cant use a deque because we need to pass a contiguos array to GEOS.
		vector<GEOSGeometry *> geoms;

		double tolerance = 0;
		bool parameters_set = false;
		bool simplify_boundary = false;
	};

	// Serialize a GEOS geometry
	static string_t Serialize(const GEOSContextHandle_t context, Vector &result, const GEOSGeometry *geom) {
		D_ASSERT(geom);
		const auto size = GeosSerde::GetRequiredSize(context, geom);
		auto blob = StringVector::EmptyString(result, size);
		const auto ptr = blob.GetDataWriteable();

		// Serialize the geometry
		GeosSerde::Serialize(context, geom, ptr, size);

		blob.Finalize();
		return blob;
	}

	// Deserialize a GEOS geometry
	static GEOSGeometry *Deserialize(const GEOSContextHandle_t context, const string_t &blob) {
		const auto ptr = blob.GetData();
		const auto size = blob.GetSize();

		return GeosSerde::Deserialize(context, ptr, size);
	}

	static void Initialize(const AggregateFunction &, data_ptr_t state_mem) {
		const auto state_ptr = new (state_mem) State();
		auto &state = *state_ptr;
		state.context = GEOS_init_r();
	}

	static void Absorb(Vector &state_vec, Vector &combined, AggregateInputData &aggr_input_data, idx_t count) {
		D_ASSERT(aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE);

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto combined_ptr = FlatVector::GetData<State *>(combined);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			auto &state = *state_ptr[state_idx];

			// Steal the list from the state and append it to the combined state
			auto &combined_state = *combined_ptr[raw_idx];
			combined_state.geoms.insert(combined_state.geoms.end(), state.geoms.begin(), state.geoms.end());
			state.geoms.clear();

			// Copy params
			if (!combined_state.parameters_set && state.parameters_set) {
				combined_state.tolerance = state.tolerance;
				combined_state.simplify_boundary = state.simplify_boundary;
				combined_state.parameters_set = true;
			}
		}
	}

	static void Combine(Vector &state_vec, Vector &combined, AggregateInputData &aggr_input_data, idx_t count) {

		//	Can we use destructive combining?
		if (aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE) {
			Absorb(state_vec, combined, aggr_input_data, count);
			return;
		}

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto combined_ptr = FlatVector::GetData<State *>(combined);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			const auto &state = *state_ptr[state_idx];

			// We can't steal the list, we need to clone and append all elements
			auto &combined_state = *combined_ptr[raw_idx];

			for (auto &geom : state.geoms) {
				combined_state.geoms.push_back(GEOSGeom_clone_r(combined_state.context, geom));
			}

			// Copy params
			if (!combined_state.parameters_set && state.parameters_set) {
				combined_state.tolerance = state.tolerance;
				combined_state.simplify_boundary = state.simplify_boundary;
				combined_state.parameters_set = true;
			}
		}
	}

	static idx_t StateSize(const AggregateFunction &) {
		return sizeof(State);
	}

	template <class OP>
	static void Finalize(Vector &state_vec, AggregateInputData &aggr_input_data, Vector &result, idx_t count,
	                     idx_t offset) {

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);

		auto &mask = FlatVector::Validity(result);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			auto &state = *state_ptr[state_format.sel->get_index(raw_idx)];
			const auto out_idx = raw_idx + offset;

			// Now create a geometry collection out of all geometries
			const auto collection = GEOSGeom_createCollection_r(state.context, GEOS_GEOMETRYCOLLECTION,
			                                                    state.geoms.data(), state.geoms.size());

			// If we manage to construct the collection, it takes ownership of the geometries
			// So we need to leak the list or risk a double free
			state.geoms.clear();

			try {
				// And perform the operation with the result
				OP::Build(state, collection, result, mask, out_idx);

				// Destroy the collection
				GEOSGeom_destroy_r(state.context, collection);

			} catch (...) {
				// Destroy the collection, and rethrow the exception
				GEOSGeom_destroy_r(state.context, collection);
				throw;
			}
		}
	}

	static void Destroy(Vector &state_vec, AggregateInputData &aggr, idx_t count) {
		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto row_idx = state_format.sel->get_index(raw_idx);
			if (state_format.validity.RowIsValid(row_idx)) {
				auto &state = *state_ptr[row_idx];

				state.geoms.clear();

				if (state.context) {
					GEOS_finish_r(state.context);
					state.context = nullptr;
				}
			}
		}
	}
};

//======================================================================================================================
// ST_CoverageUnion_Agg
//======================================================================================================================

struct ST_CoverageSimplify_Agg : GEOSCoverageAggFunction {

	static unique_ptr<FunctionData> Bind(ClientContext &context, AggregateFunction &function,
	                                     vector<unique_ptr<Expression>> &arguments) {
		if (arguments.size() == 2) {
			arguments.push_back(make_uniq_base<Expression, BoundConstantExpression>(Value::BOOLEAN(true)));
			function.arguments.push_back(LogicalType::BOOLEAN);
		}
		return nullptr;
	}

	static void Update(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &state_vec,
	                   idx_t count) {

		auto &geom_vec = inputs[0];

		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(count, geom_format);

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		UnifiedVectorFormat tolerance_format;
		inputs[1].ToUnifiedFormat(count, tolerance_format);

		UnifiedVectorFormat simplify_boundary_format;
		inputs[2].ToUnifiedFormat(count, simplify_boundary_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto geom_ptr = UnifiedVectorFormat::GetData<string_t>(geom_format);
		const auto tolerance_ptr = UnifiedVectorFormat::GetData<double>(tolerance_format);
		const auto simplify_boundary_ptr = UnifiedVectorFormat::GetData<bool>(simplify_boundary_format);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			const auto geom_idx = geom_format.sel->get_index(raw_idx);
			const auto tolerance_idx = tolerance_format.sel->get_index(raw_idx);

			if (!geom_format.validity.RowIsValid(geom_idx)) {
				continue;
			}
			if (!tolerance_format.validity.RowIsValid(tolerance_idx)) {
				continue;
			}
			if (!simplify_boundary_format.validity.RowIsValid(raw_idx)) {
				continue;
			}

			// Now, deserialize the geometry and append it to the list in each state
			auto &state = *state_ptr[state_idx];
			const auto geom = Deserialize(state.context, geom_ptr[geom_idx]);
			state.geoms.push_back(geom);

			// Also set parameters
			if (!state.parameters_set) {
				state.tolerance = tolerance_ptr[tolerance_idx];
				state.simplify_boundary = simplify_boundary_ptr[raw_idx];
				state.parameters_set = true;
			}
		}
	}

	static void Build(const State &state, const GEOSGeometry *collection, Vector &result, ValidityMask &,
	                  idx_t out_idx) {
		// Compute the coverage
		const auto simplified =
		    GEOSCoverageSimplifyVW_r(state.context, collection, state.tolerance, !state.simplify_boundary);

		// Serialize the result
		const auto result_ptr = FlatVector::GetData<string_t>(result);
		result_ptr[out_idx] = Serialize(state.context, result, simplified);
		GEOSGeom_destroy_r(state.context, simplified);
	}

	static void Register(ExtensionLoader &loader) {
		using SELF = ST_CoverageSimplify_Agg;

		AggregateFunction agg({GeoTypes::GEOMETRY(), LogicalType::DOUBLE}, GeoTypes::GEOMETRY(), StateSize, Initialize,
		                      Update, Combine, Finalize<SELF>, nullptr, Bind, Destroy);

		FunctionBuilder::RegisterAggregate(loader, "ST_CoverageSimplify_Agg", [&](AggregateFunctionBuilder &func) {
			func.SetFunction(agg);
			func.SetDescription("Simplifies a set of geometries while maintaining coverage");

			// TODO: this is a hack
			agg.arguments.push_back(LogicalType::BOOLEAN);
			func.SetFunction(agg);

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_CoverageUnion_Agg
//======================================================================================================================

struct ST_CoverageUnion_Agg : GEOSCoverageAggFunction {

	static void Update(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &state_vec,
	                   idx_t count) {

		auto &geom_vec = inputs[0];

		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(count, geom_format);

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto geom_ptr = UnifiedVectorFormat::GetData<string_t>(geom_format);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			const auto geom_idx = geom_format.sel->get_index(raw_idx);

			if (!geom_format.validity.RowIsValid(geom_idx)) {
				continue;
			}

			// Now, deserialize the geometry and append it to the list in each state
			auto &state = *state_ptr[state_idx];
			const auto geom = Deserialize(state.context, geom_ptr[geom_idx]);
			state.geoms.push_back(geom);

			// Also set parameters
			if (!state.parameters_set) {
				state.parameters_set = true;
			}
		}
	}

	static void Build(const State &state, const GEOSGeometry *collection, Vector &result, ValidityMask &,
	                  idx_t out_idx) {
		// Compute the union
		const auto coverage = GEOSCoverageUnion_r(state.context, collection);

		// Serialize the result
		const auto result_ptr = FlatVector::GetData<string_t>(result);
		result_ptr[out_idx] = Serialize(state.context, result, coverage);
		GEOSGeom_destroy_r(state.context, coverage);
	}

	static void Register(ExtensionLoader &loader) {
		using SELF = ST_CoverageUnion_Agg;

		const AggregateFunction agg({GeoTypes::GEOMETRY()}, GeoTypes::GEOMETRY(), StateSize, Initialize, Update,
		                            Combine, Finalize<SELF>, nullptr, nullptr, Destroy);

		FunctionBuilder::RegisterAggregate(loader, "ST_CoverageUnion_Agg", [&](AggregateFunctionBuilder &func) {
			func.SetFunction(agg);
			func.SetDescription("Unions a set of geometries while maintaining coverage");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

//======================================================================================================================
// ST_CoverageInvalidEdges_Agg
//======================================================================================================================

struct ST_CoverageInvalidEdges_Agg : GEOSCoverageAggFunction {

	static unique_ptr<FunctionData> Bind(ClientContext &context, AggregateFunction &function,
	                                     vector<unique_ptr<Expression>> &arguments) {
		if (arguments.size() == 1) {
			arguments.push_back(make_uniq_base<Expression, BoundConstantExpression>(Value::DOUBLE(0.0)));
			function.arguments.push_back(LogicalType::DOUBLE);
		}
		return nullptr;
	}

	static void Update(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &state_vec,
	                   idx_t count) {

		auto &geom_vec = inputs[0];

		UnifiedVectorFormat geom_format;
		geom_vec.ToUnifiedFormat(count, geom_format);

		UnifiedVectorFormat state_format;
		state_vec.ToUnifiedFormat(count, state_format);

		UnifiedVectorFormat tolerance_format;
		inputs[1].ToUnifiedFormat(count, tolerance_format);

		const auto state_ptr = UnifiedVectorFormat::GetData<State *>(state_format);
		const auto geom_ptr = UnifiedVectorFormat::GetData<string_t>(geom_format);
		const auto tolerance_ptr = UnifiedVectorFormat::GetData<double>(tolerance_format);

		for (idx_t raw_idx = 0; raw_idx < count; raw_idx++) {
			const auto state_idx = state_format.sel->get_index(raw_idx);
			const auto geom_idx = geom_format.sel->get_index(raw_idx);
			const auto tolerance_idx = tolerance_format.sel->get_index(raw_idx);

			if (!geom_format.validity.RowIsValid(geom_idx)) {
				continue;
			}
			if (!tolerance_format.validity.RowIsValid(tolerance_idx)) {
				continue;
			}

			// Now, deserialize the geometry and append it to the list in each state
			auto &state = *state_ptr[state_idx];
			const auto geom = Deserialize(state.context, geom_ptr[geom_idx]);
			state.geoms.push_back(geom);

			// Also set parameters
			if (!state.parameters_set) {
				state.tolerance = tolerance_ptr[tolerance_idx];
				state.parameters_set = true;
			}
		}
	}

	static void Build(const State &state, const GEOSGeometry *collection, Vector &result, ValidityMask &mask,
	                  idx_t out_idx) {

		// Check if there are any invalid edges
		GEOSGeometry *edges;
		GEOSCoverageIsValid_r(state.context, collection, state.tolerance, &edges);
		if (GEOSisEmpty_r(state.context, edges)) {
			mask.SetInvalid(out_idx);
			return;
		}
		// Serialize the result
		const auto result_ptr = FlatVector::GetData<string_t>(result);
		result_ptr[out_idx] = Serialize(state.context, result, edges);
		GEOSGeom_destroy_r(state.context, edges);
	}

	static void Register(ExtensionLoader &loader) {
		using SELF = ST_CoverageInvalidEdges_Agg;

		AggregateFunction agg({GeoTypes::GEOMETRY()}, GeoTypes::GEOMETRY(), StateSize, Initialize, Update, Combine,
		                      Finalize<SELF>, nullptr, Bind, Destroy, nullptr);

		FunctionBuilder::RegisterAggregate(loader, "ST_CoverageInvalidEdges_Agg", [&](AggregateFunctionBuilder &func) {
			func.SetFunction(agg);

			// TODO: this is a hack
			agg.arguments.push_back(LogicalType::DOUBLE);
			func.SetFunction(agg);

			func.SetDescription("Returns the invalid edges of a coverage geometry");

			func.SetTag("ext", "spatial");
			func.SetTag("category", "construction");
		});
	}
};

} // namespace

//######################################################################################################################
// Register Module
//######################################################################################################################

void RegisterGEOSModule(ExtensionLoader &loader) {

	// Scalar Functions
	ST_Boundary::Register(loader);
	ST_Buffer::Register(loader);
	ST_BuildArea::Register(loader);
	ST_Contains::Register(loader);
	ST_ContainsProperly::Register(loader);
	ST_WithinProperly::Register(loader);
	ST_ConcaveHull::Register(loader);
	ST_ConvexHull::Register(loader);
	ST_CoverageInvalidEdges::Register(loader);
	ST_CoverageSimplify::Register(loader);
	ST_CoverageUnion::Register(loader);
	ST_CoveredBy::Register(loader);
	ST_Covers::Register(loader);
	ST_Crosses::Register(loader);
	ST_Difference::Register(loader);
	ST_Disjoint::Register(loader);
	ST_Distance::Register(loader);
	ST_DistanceWithin::Register(loader);
	ST_Equals::Register(loader);
	ST_Envelope::Register(loader);
	ST_Intersection::Register(loader);
	ST_Intersects::Register(loader);
	ST_IsRing::Register(loader);
	ST_IsSimple::Register(loader);
	ST_IsValid::Register(loader);
	ST_LineMerge::Register(loader);
	ST_MakeValid::Register(loader);
	ST_MaximumInscribedCircle::Register(loader);
	ST_MinimumRotatedRectangle::Register(loader);
	ST_Node::Register(loader);
	ST_Normalize::Register(loader);
	ST_Overlaps::Register(loader);
	ST_PointOnSurface::Register(loader);
	ST_Polygonize::Register(loader);
	ST_ReducePrecision::Register(loader);
	ST_RemoveRepeatedPoints::Register(loader);
	ST_Reverse::Register(loader);
	ST_ShortestLine::Register(loader);
	ST_Simplify::Register(loader);
	ST_SimplifyPreserveTopology::Register(loader);
	ST_Touches::Register(loader);
	ST_Union::Register(loader);
	ST_VoronoiDiagram::Register(loader);
	ST_Within::Register(loader);

	// Aggregate Functions
	ST_MemUnion_Agg::Register(loader);
	ST_Intersection_Agg::Register(loader);
	ST_Union_Agg::Register(loader);

	// Coverage Aggregate Functions
	ST_CoverageInvalidEdges_Agg::Register(loader);
	ST_CoverageUnion_Agg::Register(loader);
	ST_CoverageSimplify_Agg::Register(loader);
}

string_t GeosOperations::clip_to_rect(Vector &result, const string_t &blob, double x_min, double y_min, double x_max, double y_max) {
	auto ctx = GEOS_init_r();

	GEOSContext_setErrorMessageHandler_r(ctx, [](const char *message, void *) { throw InvalidInputException(message); }, nullptr);

	const auto geom = GeosSerde::Deserialize(ctx, blob.GetData(), blob.GetSize());
	if (geom == nullptr) {
		GEOS_finish_r(ctx);
		throw InvalidInputException("Could not deserialize geometry");
	}
	auto geos = GeosGeometry(ctx, geom);
	auto clipped = geos.clip_by_rect(x_min, y_min, x_max, y_max);
	const auto raw = clipped.get_raw();
	const auto size = GeosSerde::GetRequiredSize(ctx, raw);
	string_t serialized = StringVector::EmptyString(result, size);
	const auto ptr = serialized.GetDataWriteable();
	GeosSerde::Serialize(ctx, raw, ptr, size);
	serialized.Finalize();
	GEOS_finish_r(ctx);
	return serialized;
}

} // namespace duckdb
