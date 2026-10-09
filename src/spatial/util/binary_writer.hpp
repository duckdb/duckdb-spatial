#pragma once

#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "duckdb/common/exception.hpp"

namespace duckdb {

class BinaryWriter {
public:
	BinaryWriter(char *ptr, char *end) : beg(ptr), end(end), ptr(ptr) {
	}
	BinaryWriter(char *buffer, const size_t size) : BinaryWriter(buffer, buffer + size) {
	}

	template <class T>
	void Write(const T &value) {
		static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
		CheckSize(sizeof(T));
		memcpy(ptr, &value, sizeof(T));
		ptr += sizeof(T);
	}

	char *Reserve(const size_t size) {
		CheckSize(size);
		char *result = ptr;
		ptr += size;
		return result;
	}

	void Skip(const size_t size, const bool zero = false) {
		CheckSize(size);
		if (zero) {
			memset(ptr, 0, size);
		}
		ptr += size;
	}

	void Copy(const char *buffer, const size_t size) {
		if (buffer == nullptr) {
			D_ASSERT(size == 0);
			return;
		}
		if (size == 0) {
			return;
		}

		CheckSize(size);
		memcpy(ptr, buffer, size);
		ptr += size;
	}

	char *GetStart() const {
		return beg;
	}

	char *GetEnd() const {
		return end;
	}

private:
	void CheckSize(const size_t size) const {
		if (ptr + size > end) {
			throw InternalException("Buffer overflow");
		}
	}

	char *beg;
	char *end;
	char *ptr;
};

} // namespace duckdb
