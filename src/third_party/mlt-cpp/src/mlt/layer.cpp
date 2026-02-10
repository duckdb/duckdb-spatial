#include <mlt/layer.hpp>
#include <mlt/geometry_vector.hpp>

namespace mlt {

Layer::Layer(std::string name_,
             extent_t extent_,
             std::unique_ptr<geometry::GeometryVector>&& geometryVector_,
             std::vector<Feature> features_,
             PropertyVecMap properties_) noexcept
    : name(std::move(name_)),
      extent(extent_),
      geometryVector(std::move(geometryVector_)),
      features(std::move(features_)),
      properties(std::move(properties_)) {}

Layer::~Layer() = default;

} // namespace mlt
