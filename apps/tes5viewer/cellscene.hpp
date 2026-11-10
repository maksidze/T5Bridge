#ifndef OPENMW_APPS_TES5VIEWER_CELLSCENE_H
#define OPENMW_APPS_TES5VIEWER_CELLSCENE_H

#include "recordattachment.hpp"

#include <components/esm/position.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace TES5Viewer
{
    struct PluginLoadInfo
    {
        std::string filename;
        std::string kind;
        std::size_t inputOrder = 0;
        std::optional<std::uint32_t> fullSlot;
        std::optional<std::uint32_t> lightSlot;
        std::uint32_t canonicalPrefix = 0;
        float headerVersion = 0.f;
        std::vector<std::string> masters;
        bool lightByExtension = false;
        bool lightByFlag = false;
    };

    struct StaticPlacement
    {
        std::uint32_t referenceId = 0;
        std::uint32_t baseId = 0;
        std::string model;
        ESM::Position position;
        float scale = 1.f;
        std::string referenceSourcePlugin;
        std::string baseSourcePlugin;
        bool initiallyDisabled = false;
        bool lootable = false;
    };

    struct ExteriorCell
    {
        std::uint32_t cellId = 0;
        int x = 0;
        int y = 0;
        std::string editorId;
        std::string sourcePlugin;
    };

    struct LandTile
    {
        static constexpr int side = 33;
        static constexpr int size = 4096;
        std::uint32_t cellId = 0;
        std::uint32_t landId = 0;
        int x = 0;
        int y = 0;
        // Row-major from the southwest corner, including the shared north/east edges.
        std::array<float, side * side> heights{};
        std::array<std::uint8_t, side * side * 3> colors{};
        bool hasColors = false;
        std::uint8_t hiddenQuadrants = 0;
        std::string sourcePlugin;
    };

    struct WorldspaceInfo
    {
        static constexpr unsigned useLand = 0x01, useLod = 0x02, useMap = 0x04;
        std::uint32_t formId = 0, parent = 0;
        unsigned parentFlags = 0, flags = 0;
        std::string editorId, sourcePlugin, mapImage;
        std::optional<std::array<int, 4>> mapBounds; // NW X/Y, SE X/Y
        std::optional<std::pair<int, int>> center;
        std::optional<std::array<float, 4>> mapOffset; // Scale, X, Y, Z
        std::vector<ExteriorCell> cells;
    };

    struct InteriorInfo
    {
        std::uint32_t formId;
        std::string editorId, sourcePlugin;
    };

    struct LocationCatalog
    {
        std::map<std::uint32_t, WorldspaceInfo> worlds;
        std::vector<InteriorInfo> interiors;

        const WorldspaceInfo& inherited(const WorldspaceInfo& world, unsigned mask) const;
    };

    LocationCatalog loadLocationCatalog(const std::vector<std::filesystem::path>& plugins);

    struct ExteriorReference
    {
        std::uint32_t referenceId = 0;
        std::uint32_t baseId = 0;
        std::uint32_t cellId = 0; // Grid cell for world-persistent references.
        std::uint32_t ownerCellId = 0; // Raw owning CELL after load-order resolution.
        std::uint32_t recordType = 0;
        ESM::Position position;
        float scale = 1.f;
        bool initiallyDisabled = false; // The reference's own flag, before evaluating XESP.
        std::uint32_t enableParentId = 0;
        bool oppositeOfParent = false;
        bool initiallyDead = false;
    };

    struct CellScene
    {
        std::uint32_t cellId = 0;
        std::string editorId;
        std::string cellSourcePlugin;
        std::vector<std::string> loadOrder;
        std::vector<PluginLoadInfo> pluginLoadOrder;
        std::map<std::string, std::size_t> overrideStatistics;
        std::vector<StaticPlacement> placements;
        std::vector<RecordAttachment> scriptAttachments;
        std::uint32_t worldspaceId = 0;
        std::vector<ExteriorCell> exteriorCells;
        std::vector<LandTile> landTiles;
        std::map<std::uint32_t, ExteriorReference> exteriorReferences;
        // References include REFR and ACHR. Exclusions may overlap for one reference.
        std::size_t referenceCount = 0;
        std::size_t staticCount = 0; // Accepted placements; Papyrus opt-in also includes ACTI bases.
        std::map<std::string, std::size_t> exclusions;
    };

    // Explicit ordered full/light plugins, with independent slot spaces. Last whole
    // record wins; CELL partial containers contribute children without replacing a
    // full CELL. No gameplay state or automatic plugin ordering.
    CellScene loadStaticCell(
        const std::vector<std::filesystem::path>& plugins, const std::string& editorId, bool withPapyrus = false);

    // Convenience overload retaining the original single-plugin API.
    CellScene loadStaticCell(const std::filesystem::path& plugin, const std::string& editorId);
    CellScene loadRuntimeInterior(
        const std::vector<std::filesystem::path>& plugins, std::uint32_t formId, bool withPapyrus = false);

    // Load a bounded rectangle of TES5 exterior cells and world-persistent placements.
    CellScene loadExteriorScene(const std::vector<std::filesystem::path>& plugins, std::uint32_t worldspaceId, int minX,
        int minY, int maxX, int maxY);
    CellScene loadRuntimeExterior(const std::vector<std::filesystem::path>& plugins, const LocationCatalog& catalog,
        std::uint32_t worldspaceId, int minX, int minY, int maxX, int maxY, bool withPapyrus = false);
}

#endif
