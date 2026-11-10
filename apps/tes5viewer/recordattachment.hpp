#ifndef OPENMW_APPS_TES5VIEWER_RECORDATTACHMENT_HPP
#define OPENMW_APPS_TES5VIEWER_RECORDATTACHMENT_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TES5Viewer
{
    struct FormMetadata
    {
        std::uint32_t formId = 0;
        std::uint32_t recordType = 0;
        std::string sourcePlugin;
    };

    struct QuestStages
    {
        std::uint32_t formId = 0;
        std::string editorId;
        std::string sourcePlugin;
        std::vector<std::uint16_t> stages;
    };

    struct RecordPayload
    {
        FormMetadata metadata;
        std::vector<std::byte> bytes;
        // Rebase from this record's winning plugin. Runtime references may have no on-disk record.
        std::function<std::uint32_t(std::uint32_t)> mapFormId;
        std::uint32_t flags = 0;
        bool localized = false;
    };

    using ReadWinningRecord = std::function<RecordPayload(std::uint32_t)>;

    struct WinningRecordIndex
    {
        struct Plugin
        {
            std::string seqName;
            std::function<std::uint32_t(std::uint32_t)> mapFormId;
        };

        struct Reference
        {
            std::uint32_t cell = 0;
            std::uint32_t world = 0;
            std::uint32_t flags = 0;
        };

        std::vector<Plugin> plugins;
        std::vector<std::uint32_t> startGameQuests;
        std::vector<std::uint32_t> locations;
        std::map<std::uint32_t, std::multimap<std::uint32_t, std::uint32_t>> locationReferences;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::int32_t> relationshipRanks;
        std::vector<std::uint32_t> characterRaces, characterHeadParts;
        std::unordered_map<std::uint32_t, Reference> references;
        std::map<std::uint32_t, std::vector<std::uint32_t>> navigationByCell;
        std::map<std::uint32_t, std::vector<std::uint32_t>> actorsByBase;
        std::map<std::uint32_t, std::vector<std::uint32_t>> scenesByQuest;
        std::map<std::uint32_t, std::vector<std::uint32_t>> infosByTopic;
        // Category 0 topics and miscellaneous Hello topics, indexed from the winning DIAL records.
        std::vector<std::uint32_t> playerDialogueTopics;
        std::map<std::uint32_t, std::uint32_t> topicByInfo;
        std::map<std::string, float> floatSettings;
        std::map<std::uint32_t, std::uint32_t> defaultObjects;
        std::map<std::string, std::uint32_t, std::less<>> soundsByEditorId;
        std::map<std::string, std::uint32_t, std::less<>> animationObjects;
        std::map<std::string, std::uint32_t, std::less<>> movementTypesByName;
        std::vector<std::uint32_t> weatherRegions;
        std::map<std::uint32_t, std::string> projectileModels;
        std::unordered_set<std::uint32_t> explosions;
        std::map<std::string,std::uint32_t,std::less<>> globalsByName;
    };

    ReadWinningRecord indexWinningRecords(
        const std::vector<std::filesystem::path>& plugins, WinningRecordIndex* index = nullptr);
    void visitSubrecords(
        const RecordPayload& record, const std::function<void(std::uint32_t, std::span<const std::byte>)>& visitor);

    struct RecordAttachment
    {
        std::uint32_t formId = 0;
        std::uint32_t recordType = 0;
        std::string sourcePlugin;
        std::vector<std::byte> vmad;
        // Resolve in the winning file's MAST namespace. Null stays null; other
        // results must identify an existing, non-deleted winning record.
        // The callback owns its mapping and remains valid after this function returns.
        std::function<std::uint32_t(std::uint32_t)> resolveFormId;
        // Scene attachment owner is always the placed REFR, even when formId names
        // an ACTI base. Zero denotes the standalone direct-record inspection API.
        std::uint32_t ownerReferenceId = 0;
        // Canonical IDs, rather than the winning file's raw MAST namespace.
        std::function<FormMetadata(std::uint32_t)> describeForm;
        std::function<QuestStages(std::uint32_t)> readQuestStages;
        // Optional ACTI base VMAD, independently mapped in its own winning namespace.
        std::shared_ptr<RecordAttachment> baseAttachment;
    };

    // Direct REFR/ACHR/ACTI attachments only. No base-object or losing-record
    // inheritance by default. includeBase follows the winning REFR NAME to a live
    // ACTI and permits missing direct VMAD if that base supplies VMAD. Other missing
    // attachments, deleted owners and duplicate VMAD are errors.
    RecordAttachment readRecordAttachment(
        const std::vector<std::filesystem::path>& plugins, std::uint32_t canonicalFormId, bool includeBase = false);
}

#endif
