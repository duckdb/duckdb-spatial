#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include "spatial/geometry/spatial_index_interface.hpp"
#include "spatial/geometry/cluster_types.hpp"
#include "spatial/geometry/flat_rtree.hpp"
#include "spatial/geometry/dbscan_engine.hpp"

using namespace duckdb::spatial;

struct TaxiRecord {
    int64_t rowid;
    double x_ft;
    double y_ft;
    double lat;
    double lon;
};

int main() {
    std::cout << "[RUNNING] Taxi Pickup Clustering on 5,000 real NYC taxi trips..." << std::endl;

    std::ifstream file("/tmp/taxi_projected_5000.csv");
    if (!file.is_open()) {
        std::cerr << "Could not open /tmp/taxi_projected_5000.csv" << std::endl;
        return 1;
    }

    std::string line;
    std::getline(file, line); // header: rowid,pickup_point,x_ft,y_ft,lat,lon

    std::vector<TaxiRecord> records;
    std::vector<Point2D> pts;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string token;

        // format: rowid,pickup_point,x_ft,y_ft,lat,lon
        // Note: pickup_point might be quoted if it contains commas, e.g. "POINT (...)"
        int64_t rowid = 0;
        double x_ft = 0.0, y_ft = 0.0, lat = 0.0, lon = 0.0;

        if (std::getline(ss, token, ',')) {
            rowid = std::stoll(token);
        }
        // skip pickup_point (may be quoted)
        if (ss.peek() == '"') {
            char c;
            ss >> c; // read opening quote
            std::getline(ss, token, '"');
            if (ss.peek() == ',') ss >> c; // consume comma
        } else {
            std::getline(ss, token, ',');
        }

        if (std::getline(ss, token, ',')) x_ft = std::stod(token);
        if (std::getline(ss, token, ',')) y_ft = std::stod(token);
        if (std::getline(ss, token, ',')) lat = std::stod(token);
        if (std::getline(ss, token, ',')) lon = std::stod(token);

        records.push_back({rowid, x_ft, y_ft, lat, lon});
        pts.emplace_back(x_ft, y_ft);
    }

    std::cout << "  Loaded " << pts.size() << " taxi records." << std::endl;

    FlatRTree2D rtree(32);
    rtree.Build(ArrayView<Point2D>(pts));

    // eps = 200.0 feet, min_points = 5
    DBSCANParams params(200.0, 5);
    DBSCANResult result = DBSCANEngine::Cluster2D(ArrayView<Point2D>(pts), rtree, params);

    std::cout << "  DBSCAN completed: " << result.NumClusters() << " clusters, "
              << result.NumNoise() << " noise points." << std::endl;

    // Write out results: rowid,lat,lon,cluster_id
    std::ofstream out("/tmp/taxi_clustered_5000.csv");
    out << "rowid,lat,lon,cluster_id\n";
    for (size_t i = 0; i < records.size(); ++i) {
        out << records[i].rowid << ","
            << std::fixed << std::setprecision(6) << records[i].lat << ","
            << records[i].lon << ",";
        if (result.IsNoise(i)) {
            out << "\n"; // NULL
        } else {
            out << result.GetClusterId(i) << "\n";
        }
    }

    std::cout << "  Exported results to /tmp/taxi_clustered_5000.csv." << std::endl;
    return 0;
}
