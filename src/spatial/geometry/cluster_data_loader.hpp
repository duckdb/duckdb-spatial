#pragma once

#include "spatial/geometry/spatial_index_interface.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <stdexcept>

namespace duckdb {
namespace spatial {

class ClusterDataLoader {
public:
	// Load 2D points from CSV file (e.g. sample2d.csv)
	static std::vector<Point2D> LoadPoints2DFromCSV(const std::string &filename) {
		std::ifstream file(filename);
		if (!file.is_open()) {
			throw std::runtime_error("Failed to open file: " + filename);
		}

		std::vector<Point2D> points;
		std::string line;
		size_t line_num = 0;

		while (std::getline(file, line)) {
			line_num++;
			// Trim carriage return if present
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty()) {
				continue;
			}

			// Parse x, y separated by comma or whitespace
			char *end_ptr = nullptr;
			const char *str = line.c_str();
			double x = std::strtod(str, &end_ptr);
			if (end_ptr == str) {
				throw std::runtime_error("Invalid coordinate for x at line " + std::to_string(line_num));
			}

			// Skip delimiter (comma or whitespace)
			while (*end_ptr == ',' || *end_ptr == ' ' || *end_ptr == '\t') {
				end_ptr++;
			}

			const char *y_str = end_ptr;
			double y = std::strtod(y_str, &end_ptr);
			if (end_ptr == y_str) {
				throw std::runtime_error("Invalid coordinate for y at line " + std::to_string(line_num));
			}

			points.emplace_back(x, y);
		}

		return points;
	}

	// Load 3D points from CSV file (e.g. sample3d.csv)
	static std::vector<Point3D> LoadPoints3DFromCSV(const std::string &filename) {
		std::ifstream file(filename);
		if (!file.is_open()) {
			throw std::runtime_error("Failed to open file: " + filename);
		}

		std::vector<Point3D> points;
		std::string line;
		size_t line_num = 0;

		while (std::getline(file, line)) {
			line_num++;
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty()) {
				continue;
			}

			char *end_ptr = nullptr;
			const char *str = line.c_str();
			double x = std::strtod(str, &end_ptr);
			if (end_ptr == str) {
				throw std::runtime_error("Invalid coordinate for x at line " + std::to_string(line_num));
			}

			while (*end_ptr == ',' || *end_ptr == ' ' || *end_ptr == '\t') {
				end_ptr++;
			}

			const char *y_str = end_ptr;
			double y = std::strtod(y_str, &end_ptr);
			if (end_ptr == y_str) {
				throw std::runtime_error("Invalid coordinate for y at line " + std::to_string(line_num));
			}

			while (*end_ptr == ',' || *end_ptr == ' ' || *end_ptr == '\t') {
				end_ptr++;
			}

			const char *z_str = end_ptr;
			double z = std::strtod(z_str, &end_ptr);
			if (end_ptr == z_str) {
				throw std::runtime_error("Invalid coordinate for z at line " + std::to_string(line_num));
			}

			points.emplace_back(x, y, z);
		}

		return points;
	}

	// Calculate 2D data bounding box
	static BoundingBox2D ComputeBounds(const std::vector<Point2D> &points) {
		BoundingBox2D bbox;
		for (size_t i = 0; i < points.size(); ++i) {
			bbox.ExpandToInclude(points[i]);
		}
		return bbox;
	}
};

} // namespace spatial
} // namespace duckdb
