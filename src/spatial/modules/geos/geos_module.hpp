#pragma once

#include "duckdb/common/types/string_type.hpp"

namespace duckdb {

class ExtensionLoader;

void RegisterGEOSModule(ExtensionLoader &loader);

class GeosOperations {
public:
    static string_t clip_to_rect(Vector &result, const string_t &blob, double x_min, double y_min, double x_max, double y_max);
};

} // namespace duckdb
