#pragma once

namespace duckdb {

struct FunctionData;

struct ST_KNNHelper {
	static bool TryGetConstK(const unique_ptr<FunctionData> &bind_data, int32_t &result);
};

} // namespace duckdb
