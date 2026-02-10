#include <mlt/properties.hpp>

#include <mlt/util/packed_bitset.hpp>
#include <mlt/util/stl.hpp>

#include <cassert>
#include <limits>
#include <stdexcept>
#include <variant>

namespace mlt {

namespace {

// Visitor to extract a single `Property` from each type of `PropertyVec`
struct ExtractPropertyVisitor {
    ExtractPropertyVisitor(std::size_t i_, bool byteIsBooleans_)
        : i(i_),
          byteIsBooleans(byteIsBooleans_) {}

    template <typename T>
    std::optional<Property> operator()(const T& vec) const;

    template <typename T>
    std::optional<Property> operator()(const std::vector<T>& vec) const {
        assert(i < vec.size());
        return (i < vec.size()) ? std::optional<Property>{vec[i]} : std::nullopt;
    }

    std::optional<Property> operator()(const std::vector<std::uint8_t>& vec) const {
        if (byteIsBooleans) {
            assert(i < 8 * vec.size());
            return testBit(vec, i);
        }
        assert(i < vec.size());
        return (i < vec.size()) ? std::optional<Property>{static_cast<std::uint32_t>(vec[i])} : std::nullopt;
    }

private:
    const std::size_t i;
    const bool byteIsBooleans;
};

template <>
std::optional<Property> ExtractPropertyVisitor::operator()(const StringDictViews& views) const {
    const auto& strings = views.getStrings();
    assert(i < strings.size());
    return (i < strings.size()) ? std::optional<Property>{strings[i]} : std::nullopt;
}

auto getPropertyValue(const PropertyVec& layerProperties, std::size_t sourceIndex, bool isBoolean) {
    const auto value = std::visit(ExtractPropertyVisitor(sourceIndex, isBoolean), layerProperties);
    if (value) {
        return *value;
    } else {
        throw std::runtime_error("Missing property value");
    }
};

template <typename T>
std::vector<T> buildIndexVector(const PackedBitset& present) {
    std::vector<T> indexes;
    indexes.reserve(8 * present.size());
    T curPhysicalIndex = 0;
    constexpr auto nullIndex = std::numeric_limits<T>::max();
    for (const auto bits : present) {
        for (std::uint8_t i = 0; i < 8; ++i) {
            indexes.push_back((bits & (1 << i)) ? curPhysicalIndex++ : nullIndex);
        }
    }
    return indexes;
}
} // namespace

PresentProperties::PresentProperties(ScalarType type_, PropertyVec properties_, const PackedBitset& present) noexcept
    : type(type_),
      properties(std::move(properties_)) {
    if (!present.empty()) {
        if (8 * present.size() < std::numeric_limits<std::uint16_t>::max()) {
            physicalIndexes = buildIndexVector<std::uint16_t>(present);
        } else {
            physicalIndexes = buildIndexVector<std::uint32_t>(present);
        }
    }
}

std::optional<Property> PresentProperties::getProperty(std::uint32_t logicalIndex) const {
    using OptIndex = std::optional<std::uint32_t>;
    const auto index = std::visit(
        util::overloaded{[&](const std::monostate&) -> OptIndex {
                             // not an optional property, physical index is the same as
                             // logical index
                             return logicalIndex;
                         },
                         [&](const auto& indexes) {
                             if (logicalIndex < indexes.size()) {
                                 const auto idx = indexes[logicalIndex];
                                 return (idx < std::numeric_limits<decltype(idx)>::max()) ? OptIndex{idx} : OptIndex{};
                             }
                             return OptIndex{};
                         }},
        physicalIndexes);

    return index ? getPropertyValue(properties, *index, isBoolean()) : std::optional<Property>{};
}

} // namespace mlt
