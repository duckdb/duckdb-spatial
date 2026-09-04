#include "spatial/geometry/flat_rtree.hpp"
#include "spatial/geometry/dbscan_engine.hpp"
#include "spatial/geometry/cluster_types.hpp"

#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <iomanip>
#include <cassert>
#include <fstream>

using namespace duckdb::spatial;

// Helper to format memory usage
void PrintMemUsage() {
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line)) {
		if (line.rfind("VmRSS:", 0) == 0 || line.rfind("VmPeak:", 0) == 0) {
			std::cout << "    " << line << "\n";
		}
	}
}

// Generate synthetic spatial clusters with background noise
std::vector<Point2D> GenerateClusters(size_t n_points, size_t n_clusters, double cluster_std, double noise_ratio = 0.05, uint32_t seed = 42) {
	std::mt19937 gen(seed);
	std::uniform_real_distribution<double> center_dist(-500.0, 500.0);
	std::uniform_real_distribution<double> noise_dist(-600.0, 600.0);
	std::uniform_real_distribution<double> uniform01(0.0, 1.0);
	std::normal_distribution<double> norm(0.0, cluster_std);

	// Generate random cluster centers
	std::vector<Point2D> centers;
	centers.reserve(n_clusters);
	for (size_t c = 0; c < n_clusters; ++c) {
		centers.emplace_back(center_dist(gen), center_dist(gen));
	}

	std::vector<Point2D> points;
	points.reserve(n_points);

	for (size_t i = 0; i < n_points; ++i) {
		if (uniform01(gen) < noise_ratio) {
			// Random background noise
			points.emplace_back(noise_dist(gen), noise_dist(gen));
		} else {
			// Gaussian point around a random cluster center
			size_t c = i % n_clusters;
			points.emplace_back(centers[c].x + norm(gen), centers[c].y + norm(gen));
		}
	}

	return points;
}

void RunBenchmark(const std::string &name, size_t n_points, size_t n_clusters, double cluster_std, double eps, int64_t min_pts, const std::string &export_csv = "") {
	std::cout << "=================================================================\n";
	std::cout << " BENCHMARK: " << name << " (" << n_points << " points)\n";
	std::cout << " Parameters: eps = " << eps << ", min_points = " << min_pts << "\n";
	std::cout << "=================================================================\n";

	// 1. Generate points
	auto t_gen_start = std::chrono::high_resolution_clock::now();
	auto points = GenerateClusters(n_points, n_clusters, cluster_std);
	auto t_gen_end = std::chrono::high_resolution_clock::now();
	double gen_ms = std::chrono::duration<double, std::milli>(t_gen_end - t_gen_start).count();
	std::cout << "  [1/3] Generated " << points.size() << " points in " << std::fixed << std::setprecision(2) << gen_ms << " ms\n";

	// 2. Build Inbuilt FlatRTree
	FlatRTree2D rtree(64);
	auto t_tree_start = std::chrono::high_resolution_clock::now();
	rtree.Build(ArrayView<Point2D>(points));
	auto t_tree_end = std::chrono::high_resolution_clock::now();
	double tree_ms = std::chrono::duration<double, std::milli>(t_tree_end - t_tree_start).count();
	std::cout << "  [2/3] Built Inbuilt FlatRTree in " << tree_ms << " ms\n";

	// 3. Run DBSCAN
	DBSCANParams params(eps, min_pts);
	auto t_dbscan_start = std::chrono::high_resolution_clock::now();
	auto result = DBSCANEngine::Cluster2D(ArrayView<Point2D>(points), rtree, params);
	auto t_dbscan_end = std::chrono::high_resolution_clock::now();
	double dbscan_ms = std::chrono::duration<double, std::milli>(t_dbscan_end - t_dbscan_start).count();

	double total_ms = tree_ms + dbscan_ms;
	double throughput = (n_points / (total_ms / 1000.0));

	std::cout << "  [3/3] DBSCAN Clustering completed in " << dbscan_ms << " ms\n";
	std::cout << "  ---------------------------------------------------------------\n";
	std::cout << "  RESULTS:\n";
	std::cout << "    * Total Points:       " << n_points << "\n";
	std::cout << "    * Clusters Found:     " << result.NumClusters() << "\n";
	std::cout << "    * Noise Points:       " << result.NumNoise() << " (" 
	          << std::fixed << std::setprecision(1) << (100.0 * result.NumNoise() / n_points) << "%)\n";
	std::cout << "    * Total Index+Cluster: " << std::fixed << std::setprecision(2) << total_ms << " ms\n";
	std::cout << "    * Throughput:         " << static_cast<size_t>(throughput) << " points/sec\n";
	std::cout << "  MEMORY PROFILE:\n";
	PrintMemUsage();

	// Optionally export sample to CSV for DuckDB CLI verification
	if (!export_csv.empty() && n_points <= 1000) {
		std::ofstream out(export_csv);
		out << "id,x,y,cluster_id\n";
		for (size_t i = 0; i < points.size(); ++i) {
			out << i << "," << points[i].x << "," << points[i].y << ",";
			if (result.IsNoise(i)) {
				out << "\n"; // NULL in CSV
			} else {
				out << result.GetClusterId(i) << "\n";
			}
		}
		std::cout << "  Exported sample results to " << export_csv << "\n";
	}
	std::cout << "\n";
}

int main() {
	std::cout << "\n=================================================================\n";
	std::cout << " DUCKDB SPATIAL DBSCAN SCALE BENCHMARK (10, 1000, 1000000 points)\n";
	std::cout << " Inbuilt FlatRTree (Hilbert-packed static R-Tree)\n";
	std::cout << "=================================================================\n\n";

	// Scale 1: 10 points
	RunBenchmark("Small Scale", 10, 2, 0.2, 0.5, 3, "/tmp/clusters_10.csv");

	// Scale 2: 1,000 points
	RunBenchmark("Medium Scale", 1000, 10, 2.0, 4.0, 10, "/tmp/clusters_1000.csv");

	// Scale 3: 1,000,000 points (1 MILLION POINTS!)
	RunBenchmark("Massive Scale (1 Million Points)", 1000000, 50, 5.0, 6.0, 15);

	std::cout << "[SUCCESS] All scale tests (10, 1000, 1000000) completed successfully!\n";
	return 0;
}
