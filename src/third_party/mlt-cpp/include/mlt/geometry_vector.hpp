#pragma once

#include <mlt/geometry.hpp>
#include <mlt/util/morton_curve.hpp>

#include <algorithm>
#include <cassert>

namespace mlt::geometry {

struct MortonSettings {
    unsigned numBits;
    unsigned coordinateShift;
};

enum class VertexBufferType : std::uint32_t {
    MORTON,
    VEC_2,
    VEC_3
};

struct TopologyVector : public util::noncopyable {
    TopologyVector(std::vector<std::uint32_t>&& geometryOffsets_,
                   std::vector<std::uint32_t>&& partOffsets_,
                   std::vector<std::uint32_t>&& ringOffsets_) noexcept
        : geometryOffsets(geometryOffsets_),
          partOffsets(partOffsets_),
          ringOffsets(ringOffsets_) {}

    const std::vector<std::uint32_t>& getGeometryOffsets() const noexcept { return geometryOffsets; }

    const std::vector<std::uint32_t>& getPartOffsets() const noexcept { return partOffsets; }

    const std::vector<std::uint32_t>& getRingOffsets() const noexcept { return ringOffsets; }

private:
    std::vector<std::uint32_t> geometryOffsets;
    std::vector<std::uint32_t> partOffsets;
    std::vector<std::uint32_t> ringOffsets;
};

struct GeometryVector : public util::noncopyable {
    using GeometryType = metadata::tileset::GeometryType;

    virtual ~GeometryVector() = default;

    std::uint32_t getNumGeometries() const noexcept { return numGeometries; }
    bool isSingleGeometryType() const noexcept { return singleType; }
    virtual GeometryType getGeometryType(std::size_t index) const noexcept = 0;
    virtual bool containsPolygonGeometry() const noexcept = 0;

    std::vector<std::unique_ptr<Geometry>> getGeometries(const GeometryFactory&) const;

protected:
    GeometryVector(std::uint32_t numGeometries_,
                   bool singleType_,

                   std::vector<std::uint32_t>&& indexBuffer_,
                   std::vector<std::int32_t>&& vertexBuffer_,
                   VertexBufferType vertexBufferType_,
                   std::vector<std::uint32_t>&& vertexOffsets_,
                   std::vector<std::uint32_t>&& triangleCounts_,
                   std::optional<TopologyVector>&& topologyVector_,
                   std::optional<MortonSettings> mortonSettings_ = {}) noexcept
        : numGeometries(numGeometries_),
          scale(1.0f),
          singleType(singleType_),
          indexBuffer(std::move(indexBuffer_)),
          vertexBuffer(std::move(vertexBuffer_)),
          vertexBufferType(vertexBufferType_),
          vertexOffsets(std::move(vertexOffsets_)),
          triangleCounts(std::move(triangleCounts_)),
          topologyVector(std::move(topologyVector_)),
          mortonSettings(mortonSettings_) {}

    void applyTriangles(Geometry&,
                        std::uint32_t& triangleOffset,
                        std::uint32_t& indexBufferOffset,
                        std::uint32_t totalVertices,
                        bool multiPolygon) const;

protected:
    std::uint32_t numGeometries;
    float scale;
    bool singleType;
    std::vector<std::uint32_t> indexBuffer;
    std::vector<std::int32_t> vertexBuffer;
    VertexBufferType vertexBufferType;
    std::vector<std::uint32_t> vertexOffsets;
    std::vector<std::uint32_t> triangleCounts;
    std::optional<TopologyVector> topologyVector;
    std::optional<MortonSettings> mortonSettings;
};

struct GpuVector : public GeometryVector {
    GpuVector(std::uint32_t numGeometries_,
              bool singleType_,

              std::vector<std::uint32_t>&& triangleCounts_,
              std::vector<std::uint32_t>&& indexBuffer_,
              std::vector<std::int32_t>&& vertexBuffer_,
              VertexBufferType vertexBufferType_,
              std::optional<TopologyVector>&& topologyVector_) noexcept
        : GeometryVector(numGeometries_,
                         singleType_,
                         std::move(indexBuffer_),
                         std::move(vertexBuffer_),
                         vertexBufferType_,
                         {},
                         std::move(triangleCounts_),
                         std::move(topologyVector_)) {}

private:
};

struct ConstGpuVector : public GpuVector {
    ConstGpuVector(std::uint32_t numGeometries_,

                   GeometryType geometryType_,
                   std::vector<std::uint32_t>&& triangleCounts_,
                   std::vector<std::uint32_t>&& indexBuffer_,
                   std::vector<std::int32_t>&& vertexBuffer_,
                   VertexBufferType vertexBufferType_,
                   std::optional<TopologyVector>&& topologyVector_) noexcept
        : GpuVector(numGeometries_,
                    /*singleType=*/true,
                    std::move(triangleCounts_),
                    std::move(indexBuffer_),
                    std::move(vertexBuffer_),
                    vertexBufferType_,
                    std::move(topologyVector_)),
          geometryType(geometryType_) {}

    GeometryType getGeometryType(std::size_t) const noexcept override { return geometryType; }

    bool containsPolygonGeometry() const noexcept override {
        return geometryType == GeometryType::POLYGON || geometryType == GeometryType::MULTIPOLYGON;
    }

private:
    GeometryType geometryType;
};

struct FlatGpuVector : public GpuVector {
    FlatGpuVector(std::vector<GeometryType>&& geometryTypes_,
                  std::vector<std::uint32_t>&& triangleCounts_,
                  std::vector<std::uint32_t>&& indexBuffer_,
                  std::vector<std::int32_t>&& vertexBuffer_,
                  std::optional<TopologyVector>&& topologyVector_ = {}) noexcept
        : GpuVector(static_cast<std::uint32_t>(geometryTypes_.size()),
                    /*singleType=*/false,
                    std::move(triangleCounts_),
                    std::move(indexBuffer_),
                    std::move(vertexBuffer_),
                    VertexBufferType::VEC_2,
                    std::move(topologyVector_)),
          geometryTypes(std::move(geometryTypes_)) {}

    GeometryType getGeometryType(std::size_t index) const noexcept override {
        assert(index < geometryTypes.size());
        return geometryTypes[index];
    }

    bool containsPolygonGeometry() const noexcept override {
        // TODO: cache?  types can't change
        return std::ranges::any_of(geometryTypes, [](const auto type) {
            return type == GeometryType::POLYGON || type == GeometryType::MULTIPOLYGON;
        });
    }

private:
    std::vector<GeometryType> geometryTypes;
};

struct CpuVector : public GeometryVector {
    CpuVector(std::uint32_t numGeometries_,
              bool singleType_,
              TopologyVector&& topologyVector_,
              std::vector<std::uint32_t>&& vertexOffsets_,
              std::vector<std::int32_t>&& vertexBuffer_,
              VertexBufferType vertexBufferType_,
              std::optional<MortonSettings> mortonSettings_) noexcept
        : GeometryVector(numGeometries_,
                         singleType_,
                         /*indexBuffer=*/{},
                         std::move(vertexBuffer_),
                         vertexBufferType_,
                         std::move(vertexOffsets_),
                         {},
                         std::move(topologyVector_),
                         mortonSettings_) {}

    Coordinate getVertex(std::size_t index) const {
        if (!vertexOffsets.empty() && !mortonSettings) {
            const auto vertexOffset = vertexOffsets[index] * 2;
            return {static_cast<float>(vertexBuffer[vertexOffset]), static_cast<float>(vertexBuffer[vertexOffset + 1])};
        }
        if (!vertexOffsets.empty() && mortonSettings) {
            // TODO: move decoding of the morton codes on the GPU in the vertex shader
            const auto vertexOffset = vertexOffsets[index];
            const auto mortonEncodedVertex = vertexBuffer[vertexOffset];
            // TODO: improve performance -> inline calculation and move to decoding of VertexBuffer
            return util::MortonCurve::decode(
                mortonEncodedVertex, mortonSettings->numBits, mortonSettings->coordinateShift);
        }

        return {static_cast<float>(vertexBuffer[2 * index]), static_cast<float>(vertexBuffer[(2 * index) + 1])};
    }
};

struct ConstGeometryVector : public CpuVector {
    ConstGeometryVector(std::uint32_t numGeometries_,
                        GeometryType geometryType_,
                        VertexBufferType vertexBufferType_,
                        TopologyVector&& topologyVector_,
                        std::vector<std::uint32_t>&& vertexOffsets_,
                        std::vector<std::int32_t>&& vertexBuffer_,
                        std::optional<MortonSettings> mortonSettings_) noexcept
        : CpuVector(numGeometries_,
                    /*singleType=*/true,
                    std::move(topologyVector_),
                    std::move(vertexOffsets_),
                    std::move(vertexBuffer_),
                    vertexBufferType_,
                    mortonSettings_),
          geometryType(geometryType_) {}

    GeometryType getGeometryType(std::size_t) const noexcept override { return geometryType; }

    bool containsPolygonGeometry() const noexcept override {
        return geometryType == GeometryType::POLYGON || geometryType == GeometryType::MULTIPOLYGON;
    }

private:
    GeometryType geometryType;
};

struct FlatGeometryVector : public CpuVector {
    FlatGeometryVector(std::vector<GeometryType>&& geometryTypes_,
                       TopologyVector&& topologyVector_,
                       std::vector<std::uint32_t>&& vertexOffsets_,
                       std::vector<std::int32_t>&& vertexBuffer_,
                       VertexBufferType vertexBufferType_,
                       std::optional<MortonSettings> mortonSettings_) noexcept
        : CpuVector(static_cast<std::uint32_t>(geometryTypes_.size()),
                    /*singleType=*/false,
                    std::move(topologyVector_),
                    std::move(vertexOffsets_),
                    std::move(vertexBuffer_),
                    vertexBufferType_,
                    mortonSettings_),
          geometryTypes(geometryTypes_) {}

    GeometryType getGeometryType(std::size_t index) const noexcept override {
        assert(index < geometryTypes.size());
        return geometryTypes[index];
    }

    bool containsPolygonGeometry() const noexcept override {
        // TODO: cache?  types can't change
        return std::ranges::any_of(geometryTypes, [](const auto type) {
            return type == GeometryType::POLYGON || type == GeometryType::MULTIPOLYGON;
        });
    }

private:
    std::vector<GeometryType> geometryTypes;
};

} // namespace mlt::geometry
