#include "spatial/geometry/spatial_index_interface.hpp"
#include <iostream>
#include <cassert>

using namespace duckdb::spatial;

// Concrete mock implementation of SpatialIndex2D to test the abstract interface
class NaiveLinearSpatialIndex2D : public SpatialIndex2D {
public:
	void Build(const ArrayView<Point2D> &points) override {
		points_.clear();
		for (size_t i = 0; i < points.size(); ++i) {
			points_.push_back(points[i]);
		}
	}

	void RadiusSearch(const Point2D &center, double eps, std::vector<size_t> &matches) const override {
		matches.clear();
		const double eps_sq = eps * eps;
		for (size_t i = 0; i < points_.size(); ++i) {
			if (center.DistanceSquared(points_[i]) <= eps_sq) {
				matches.push_back(i);
			}
		}
	}

	size_t Count() const override {
		return points_.size();
	}

private:
	std::vector<Point2D> points_;
};

int main() {
	std::cout << "[RUNNING] test_step1_abstractions...\n";

	// 1. Test ArrayView
	std::vector<int> test_vec = {10, 20, 30};
	ArrayView<int> view(test_vec);
	assert(view.size() == 3);
	assert(view[0] == 10 && view[1] == 20 && view[2] == 30);

	// 2. Test Point2D and Distance
	Point2D p1(0.0, 0.0);
	Point2D p2(3.0, 4.0);
	assert(std::abs(p1.Distance(p2) - 5.0) < 1e-9);
	assert(std::abs(p1.DistanceSquared(p2) - 25.0) < 1e-9);

	// 3. Test Point3D and Distance
	Point3D p3d_1(1.0, 2.0, 3.0);
	Point3D p3d_2(4.0, 6.0, 3.0);
	assert(std::abs(p3d_1.Distance(p3d_2) - 5.0) < 1e-9);

	// 4. Test BoundingBox2D
	BoundingBox2D bbox = BoundingBox2D::FromPointWithRadius(Point2D(5.0, 5.0), 2.0);
	assert(bbox.min_x == 3.0 && bbox.max_x == 7.0);
	assert(bbox.min_y == 3.0 && bbox.max_y == 7.0);
	assert(bbox.Contains(Point2D(5.0, 6.0)));
	assert(!bbox.Contains(Point2D(5.0, 8.0)));

	BoundingBox2D bbox2(6.0, 6.0, 10.0, 10.0);
	assert(bbox.Intersects(bbox2));

	BoundingBox2D bbox3(20.0, 20.0, 30.0, 30.0);
	assert(!bbox.Intersects(bbox3));

	// 5. Test Abstract SpatialIndex2D Polymorphism
	std::vector<Point2D> pts = {
		Point2D(0.0, 0.0),
		Point2D(1.0, 0.0),
		Point2D(0.0, 1.0),
		Point2D(10.0, 10.0)
	};

	SpatialIndex2D *index = new NaiveLinearSpatialIndex2D();
	index->Build(ArrayView<Point2D>(pts));
	assert(index->Count() == 4);

	std::vector<size_t> matches;
	index->RadiusSearch(Point2D(0.0, 0.0), 1.5, matches);
	assert(matches.size() == 3);
	assert(matches[0] == 0 && matches[1] == 1 && matches[2] == 2);

	delete index;

	std::cout << "[PASSED] test_step1_abstractions completed successfully!\n";
	return 0;
}
