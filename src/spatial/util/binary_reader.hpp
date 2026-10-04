#pragma once

#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "duckdb/common/exception.hpp"

namespace duckdb {

class BinaryReader {
public:
	BinaryReader() = default;
	BinaryReader(const char *ptr, const char *end) : beg(ptr), end(end), ptr(ptr) {
	}
	BinaryReader(const char *buffer, const size_t size) : BinaryReader(buffer, buffer + size) {
	}

	bool IsAtEnd() const {
		return ptr >= end;
	}

	void Reset() {
		ptr = beg;
	}

	template <class T>
	T Read() {
		static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
		CheckSize(sizeof(T));
		T value;
		memcpy(&value, ptr, sizeof(T));
		ptr += sizeof(T);
		return value;
	}

	template <class T>
	T ReadBE() {
		static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
		CheckSize(sizeof(T));

		uint8_t in[sizeof(T)];
		uint8_t out[sizeof(T)];
		memcpy(in, ptr, sizeof(T));
		ptr += sizeof(T);

		for (size_t i = 0; i < sizeof(T); i++) {
			out[i] = in[sizeof(T) - i - 1];
		}

		T swapped = 0;
		memcpy(&swapped, out, sizeof(T));
		return swapped;
	}

	const char *Reserve(const size_t size) {
		CheckSize(size);
		const char *result = ptr;
		ptr += size;
		return result;
	}

	void Skip(const size_t size) {
		CheckSize(size);
		ptr += size;
	}

	const char *GetStart() const {
		return beg;
	}

	const char *GetEnd() const {
		return end;
	}

private:
	void CheckSize(const size_t size) const {
		if (size > static_cast<size_t>(end - ptr)) {
			throw InvalidInputException("Unexpected end of binary data at position %zu",
			                            static_cast<size_t>(ptr - beg));
		}
	}

	const char *beg;
	const char *end;
	const char *ptr;
};

} // namespace duckdb
