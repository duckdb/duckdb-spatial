#pragma once

#include <mlt/metadata/tileset.hpp>
#include <mlt/util/packed_bitset.hpp>
#include <mlt/util/noncopyable.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mlt {

/// A block of data and a collection of strings views on it
class StringDictViews : util::noncopyable {
public:
    StringDictViews() = default;
    StringDictViews(std::vector<std::uint8_t>&& data_, std::vector<std::string_view> views_) noexcept
        : data(std::move(data_)),
          views(std::move(views_)) {}
    StringDictViews(std::shared_ptr<std::vector<std::uint8_t>> data_, std::vector<std::string_view> views_) noexcept
        : sharedData(std::move(data_)),
          views(std::move(views_)) {}
    StringDictViews(StringDictViews&&) noexcept = default;
    StringDictViews& operator=(StringDictViews&&) = default;

    const auto& getStrings() const noexcept { return views; }

private:
    std::vector<std::uint8_t> data;
    std::shared_ptr<std::vector<std::uint8_t>> sharedData;
    std::vector<std::string_view> views;
};

/// A single feature property.
/// String properties reference the source property vector and must not outlive it.
using Property = std::variant<std::nullptr_t,
                              bool,
                              std::optional<bool>,
                              std::int32_t,
                              std::optional<std::int32_t>,
                              std::int64_t,
                              std::optional<std::int64_t>,
                              std::uint32_t,
                              std::optional<std::uint32_t>,
                              std::uint64_t,
                              std::optional<std::uint64_t>,
                              float,
                              std::optional<float>,
                              double,
                              std::optional<double>,
                              std::string_view>;

/// Map of properties for a single feature
using PropertyMap = std::unordered_map<std::string, Property>;

/// A single property for a column, with one value per feature
using PropertyVec = std::variant<std::vector<std::uint8_t>,
                                 std::vector<std::uint32_t>,
                                 std::vector<std::uint64_t>,
                                 std::vector<std::int32_t>,
                                 std::vector<std::int64_t>,
                                 std::vector<float>,
                                 std::vector<double>,
                                 StringDictViews>;

namespace detail {
struct PropertyCounter {
    const bool byteIsBoolean;
    template <typename T>
    std::size_t operator()(const std::vector<T>& vec) const noexcept {
        return vec.size();
    }
    std::size_t operator()(const std::vector<std::uint8_t>& vec) const noexcept {
        // For boolean columns, each bit is a property
        return vec.size() * (byteIsBoolean ? 8 : 1);
    }
    std::size_t operator()(const StringDictViews& views) const noexcept { return views.getStrings().size(); }
};
} // namespace detail
static inline std::size_t propertyCount(const PropertyVec& vec, bool byteIsBoolean) {
    return std::visit(detail::PropertyCounter{byteIsBoolean}, vec);
}

/// A column of properties and the present bits for each feature
class PresentProperties : public util::noncopyable {
public:
    using ScalarType = metadata::tileset::ScalarType;

    PresentProperties() = delete;
    PresentProperties(ScalarType type_, PropertyVec properties_, const PackedBitset& present) noexcept;

    ScalarType getType() const noexcept { return type; }
    bool isBoolean() const noexcept { return type == ScalarType::BOOLEAN; }
    const PropertyVec& getProperties() const noexcept { return properties; }

    std::size_t getPropertyCount() const { return propertyCount(properties, isBoolean()); }

    std::optional<Property> getProperty(std::uint32_t logicalIndex) const;

private:
    ScalarType type;
    PropertyVec properties;

    using ByteIndexVec = std::vector<std::uint8_t>;
    using ShortIndexVec = std::vector<std::uint16_t>;
    using IntIndexVec = std::vector<std::uint32_t>;
    std::variant<std::monostate, ByteIndexVec, ShortIndexVec, IntIndexVec> physicalIndexes;
};

/// All the property columns for a layer
using PropertyVecMap = std::unordered_map<std::string, PresentProperties>;

} // namespace mlt
