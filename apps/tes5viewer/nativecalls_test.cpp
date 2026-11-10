// Exercise the bridge host itself, including return values and authored state.
#include "ue_bridge.cpp"
#include "furniturepose.hpp"
#include <iostream>
#include <bit>

namespace
{
    using namespace TES5Viewer;
    using namespace Papyrus;
    void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    void field(RecordPayload& r, const char* tag, std::initializer_list<std::uint32_t> values)
    {
        const auto append = [&](const void* p, std::size_t n) {
            const auto* b = static_cast<const std::byte*>(p); r.bytes.insert(r.bytes.end(), b, b + n);
        };
        const auto n = static_cast<std::uint16_t>(values.size() * 4);
        append(tag, 4); append(&n, 2); for (auto value : values) append(&value, 4);
    }

    void stockPlayerDialogue(const char* data, const char* pluginList, const char* logPath)
    {
        const auto dataPath = Files::pathFromUnicodeString(data);
        const auto plugins = loadPluginList(Files::pathFromUnicodeString(pluginList), dataPath);
        LoadResult source;
        auto index = std::make_shared<WinningRecordIndex>();
        source.read = indexWinningRecords(plugins, index.get());
        source.index = index;
        source.scripts.vfs = mountRuntimeAssets(buildAssetPlan(dataPath, plugins, {}, {}, false));
        ErrorLog log;
        log.open(Files::pathFromUnicodeString(logPath));
        PapyrusState state(source, log, true);
        constexpr std::uint32_t alvor = 0x13482, riverwood = 0x45486, forge = 0xbc7e1;
        WorldScriptPose actor{index->references.at(alvor).cell, {}};
        auto player = actor;
        player.position.pos[0] = 120;
        state.world.addPlacements({{alvor, actor}, {0x14, player}});
        state.quests.addActors({{alvor, 0x13475}, {0x14, 7}});
        state.quests.startGame(riverwood);
        require(state.quests.isRunning(riverwood), "Riverwood dialogue quest did not start");

        state.controls |= 1u << 6;
        require(!state.activateReference(alvor, 0x14), "Conversation opened during an activation lock");
        state.controls = 0;
        require(state.activateReference(alvor, 0x14), "Alvor did not open a conversation");
        require(!state.dialogue.name.empty(), "Alvor's localized name is empty");
        for (int step = 0; step < 16 && state.dialogue.info; ++step)
        {
            state.time = state.dialogue.due + .1;
            state.tickDialogue();
            state.world.tick(state.time);
            state.continueDialogue();
        }
        state.world.takeAudio();
        require(state.dialogue.actor && !state.dialogue.info, "Alvor's greeting did not reach the topic menu");
        require(std::ranges::find(state.dialogue.choices, forge, &PapyrusState::DialogueChoice::id)
                != state.dialogue.choices.end(), "Alvor's forge topic is missing");
        state.chooseDialogue(0xffffffff);
        require(!state.dialogue.info, "A topic outside the menu was accepted");
        state.chooseDialogue(forge);
        require(state.dialogue.info && !state.dialogue.lines.empty() && !state.dialogue.lines[0].text.empty(),
            "Alvor's selected response did not resolve localized text");
        require(state.dialogue.voice && !state.world.takeAudio().empty(), "Alvor's original FUZ was not queued");
        std::cout << "Alvor: " << state.dialogue.lines[0].text << '\n';
        state.time = state.dialogue.due + .1;
        state.tickDialogue();
        require(state.dialogue.info && state.dialogue.line == 1 && !state.dialogue.lines[1].text.empty(),
            "The next subtitle in the same INFO did not appear");
        const auto nextLine = state.world.takeAudio();
        require(nextLine.size() == 2 && nextLine[0].command == WorldScriptAudio::Stop
                && nextLine[1].command == WorldScriptAudio::Play,
            "Consecutive lines did not replace the previous voice");
        player.position.pos[0] = 800;
        state.world.teleportPlayer(player);
        state.tickDialogue();
        require(!state.dialogue.actor, "Walking away did not close the conversation");
        const auto audio = state.world.takeAudio();
        require(audio.size() == 1 && audio[0].command == WorldScriptAudio::Stop,
            "Closing the conversation left its voice playing");
        std::cout << "PASS stock dialogue activation, conditions, localized multi-line response, FUZ and cleanup\n";

        constexpr std::uint32_t offerServices = 0x7f6bb, alvorServices = 0x5a666;
        state.quests.startGame(state.dialogueTopic(offerServices).quest);
        player.position.pos[0] = 120;
        state.world.teleportPlayer(player);
        require(state.activateReference(alvor, 0x14), "Alvor did not reopen the conversation");
        const auto advanceResponse = [&] {
            for (int step = 0; step < 20 && state.dialogue.info; ++step)
            {
                state.time = state.dialogue.due + .1;
                state.tickDialogue();
                state.world.tick(state.time);
                state.continueDialogue();
            }
        };
        advanceResponse();
        const auto offer = std::ranges::find(state.dialogue.choices, offerServices, &PapyrusState::DialogueChoice::id);
        require(offer != state.dialogue.choices.end(), "Alvor's original OfferServicesTopic is missing");
        std::cout << "Trade topic: " << offer->prompt << '\n';
        state.chooseDialogue(offerServices);
        advanceResponse();
        require(state.dialogue.vendor == alvorServices && state.dialogue.json.find("\"trade\"") != std::string::npos,
            "The stock INFO fragment did not open Alvor's barter menu");
        const auto& vendor = state.vendor(alvorServices);
        auto& stock = state.barterStock(vendor);
        auto& bag = state.inventory.inventory(0x14, 7, source.read);
        constexpr std::uint32_t gold = 0xf;
        const auto initialGold = stock[gold];
        std::cout << "Merchant chest " << std::hex << vendor.chest << std::dec
                  << " stock entries=" << stock.size() << " gold=" << initialGold << '\n';
        require(initialGold >= 500, "Alvor's fixed leveled gold was not included");
        const auto item = std::ranges::find_if(stock, [&](const auto& entry) {
            if (entry.second <= 0 || entry.first == gold) return false;
            const auto record = source.read(entry.first);
            return state.tradePrice(record, true) && state.vendorBuys(vendor, record);
        });
        require(item != stock.end(), "Alvor's merchant chest has no tradable goods");
        const auto id = item->first;
        const auto count = item->second;
        const auto owned = bag[id];
        const auto buy = state.tradePrice(source.read(id), true), sell = state.tradePrice(source.read(id), false);
        bag[gold] = 0;
        state.tradeItem(id, true);
        require(stock[id] == count && bag[id] == owned && stock[gold] == initialGold, "Unfunded purchase changed inventory");
        bag[gold] = buy;
        state.tradeItem(id, true);
        require(stock[id] == count - 1 && bag[id] == owned + 1 && bag[gold] == 0 && stock[gold] == initialGold + buy,
            "Purchase did not transfer both the item and gold");
        stock[gold] = 0;
        state.tradeItem(id, false);
        require(stock[id] == count - 1 && bag[id] == owned + 1 && bag[gold] == 0, "Unfunded sale changed inventory");
        stock[gold] = initialGold + buy;
        state.tradeItem(id, false);
        require(stock[id] == count && bag[id] == owned && bag[gold] == sell && stock[gold] == initialGold + buy - sell,
            "Sale did not transfer both the item and gold");
        state.leaveBarter();
        require(!state.dialogue.vendor && state.dialogue.actor == alvor, "Closing barter did not return to dialogue");
        require(state.barterStock(vendor)[gold] == initialGold + buy - sell, "Merchant stock was reset after closing barter");
        std::cout << "PASS Alvor's stock barter fragment, authored stock and gold, purchase, sale, balance checks and return\n";
    }

    void solveClawRings(PapyrusState& state, const auto& prepared, const auto& advance)
    {
        unsigned wheels = 0;
        for (const auto& script : prepared)
            if (Misc::StringUtils::ciEqual(script.script->name, "HallofStoriesDiskScript")
                && script.owner >= 0x4e2ba && script.owner <= 0x4e2bc)
            {
                ScriptInstance authored(script.script, script.owner, script.properties);
                auto position = std::get<std::int32_t>(authored.readAutoProperty("initialState"));
                const auto solution = std::get<std::int32_t>(authored.readAutoProperty("solveState"));
                for (int turns = 0; turns < 3 && position != solution; ++turns, position = position % 3 + 1)
                {
                    require(state.activateReference(script.owner, 0x14), "A claw ring is not bound");
                    advance(50);
                }
                ++wheels;
            }
        require(wheels == 3 && state.activateReference(0x4e2b9, 0x14), "The three stock rings did not bind");
    }

    void stockEarlyQuests(const char* data, const char* pluginList, const char* logPath)
    {
        const auto dataPath = Files::pathFromUnicodeString(data);
        const auto paths = loadPluginList(Files::pathFromUnicodeString(pluginList), dataPath);
        LoadResult source;
        auto index = std::make_shared<WinningRecordIndex>();
        source.read = indexWinningRecords(paths, index.get());
        source.index = index;
        source.scripts.vfs = mountRuntimeAssets(buildAssetPlan(dataPath, paths, {}, {}, false));
        ErrorLog log;
        log.open(Files::pathFromUnicodeString(logPath));
        const auto catalog = loadLocationCatalog(paths);
        const auto keep = loadRuntimeInterior(paths, 0x5de24, true);
        std::map<std::uint32_t, WorldScriptPose> keepPlacements;
        for (const auto& [id, ref] : keep.exteriorReferences)
            keepPlacements.emplace(id, WorldScriptPose{ref.cellId, ref.position});
        for (const auto [quest, timer] : {std::pair{0x2bf9cu, 0xac13bu}, {0x2610au, 0xac134u}})
        {
            PapyrusState escort(source, log, true);
            const auto companion = quest == 0x2bf9c ? 0x2bfa2u : 0x2bf9eu;
            const auto exit = keepPlacements.at(0x91608);
            auto placements = keepPlacements;
            placements[companion] = exit;
            escort.world.addPlacements(placements);
            escort.world.setExteriorCells(0x3c, catalog.worlds.at(0x3c).cells);
            const auto marker = *escort.world.placement(0xad242);
            escort.world.teleportPlayer(marker, 0x3c);
            const auto escortHost = escort.quests.host();
            require(escortHost.setQuestStageLatent(quest, 5).accepted
                && escortHost.setQuestStageLatent(0x4e50d, 5).accepted, "Could not start the stock escort");
            for (int i = 1; i <= 200; ++i)
            {
                escort.quests.tick(i * .1);
                escort.world.tick(i * .1);
            }
            const auto arrived = *escort.world.placement(companion);
            require(escort.world.sameSpace(companion, arrived.cell, 0xad242, marker.cell)
                && (arrived.position.asVec3() - marker.position.asVec3()).length() < 330.f,
                "Stock companion's Travel package did not use the keep exit");
            std::cout << "PASS stock MQ102 companion uses authored exit " << std::hex << companion << std::dec << '\n';

            PapyrusState state(source, log, true);
            const auto host = state.quests.host();
            state.world.teleportPlayer({0x5de24, {}});
            state.quests.updateLocations();
            require(host.setQuestStageLatent(quest, 20).accepted, "Could not start the stock MQ102 branch");
            for (int i = 1; i <= 30; ++i) state.quests.tick(i * .1);
            require(host.isQuestStageDone(quest, 20), "Branch did not reach stage 20");
            state.world.teleportPlayer({0x9732, {}});
            state.quests.updateLocations();
            for (int i = 31; i <= 80; ++i) state.quests.tick(i * .1);
            for (const auto& event : state.quests.takeEvents())
                if (event.owner == quest)
                    std::cout << event.script << ' ' << event.event << ' ' << event.time << '\n';
            require(host.isQuestStageDone(quest, 30), "OnLocationChange did not detect leaving Helgen");
            require(host.isQuestStageDone(quest, 35), "OnLocationChange did not detect Riverwood");
            std::cout << "PASS stock MQ102 location changes " << std::hex << quest << std::dec << '\n';

            PapyrusState timed(source, log, true);
            const auto timedHost = timed.quests.host();
            timed.world.teleportPlayer({0x5de24, {}});
            require(timedHost.setQuestStageLatent(quest, 20).accepted, "Could not schedule the stock calendar timer");
            for (int i = 1; i <= 30; ++i) timed.quests.tick(i * .1);
            timed.quests.tick(4323);
            timed.quests.tick(4324);
            require(timedHost.isQuestStageDone(quest, 35), "Calendar timer did not reach MQ102 stage 35");

            PapyrusState scene(source, log, true);
            const auto sceneHost = scene.quests.host();
            scene.world.teleportPlayer({0x5de24, {}});
            require(sceneHost.setQuestStageLatent(quest, 20).accepted, "Could not start the stock scene owner");
            for (int i = 1; i <= 30; ++i) scene.quests.tick(i * .1);
            scene.world.addScenes(std::array{timer});
            require(scene.world.callFormMethod(timer, "Start", {}).has_value(), "SCEN start was not accepted");
            for (int i = 31; i <= 40; ++i)
            {
                scene.quests.tick(i * .1);
                scene.world.tick(i * .1);
            }
            scene.world.tick(3599);
            scene.quests.tick(3599);
            require(!sceneHost.isQuestStageDone(quest, 35), "Scene timer finished early");
            for (int i = 3605; i <= 3612; ++i)
            {
                scene.world.tick(i);
                scene.quests.tick(i);
            }
            require(sceneHost.isQuestStageDone(quest, 35), "Scene timer did not run its original completion fragment");
            std::cout << "PASS stock MQ102 calendar and SCEN timers " << std::hex << timer << std::dec << '\n';
        }

        PapyrusState dungeon(source, log, true);
        for (const auto [ref, end, expected] : {
                std::tuple{0xaa8cbu, "Turned02", .9666666}, {0x4e2bau, "Turned02", 3.0}})
        {
            const auto result = dungeon.makeHost(true).callLatentFormMethod(ref, "PlayAnimationAndWait",
                {std::string("Trigger02"), std::string(end)});
            require(result && result->finished && !*result->finished, "Stock HKX native did not suspend");
            const auto start = dungeon.time;
            dungeon.time = start + expected - .01;
            dungeon.updateModelAnimations();
            require(!*result->finished, "Stock HKX event fired early");
            dungeon.time = start + expected + .01;
            dungeon.updateModelAnimations();
            require(*result->finished && std::get<bool>(*result->deferredReturn), "Stock HKX notification did not resume the wait");
        }
        std::cout << "PASS stock pillar and claw-ring HKX notifications\n";

        const auto cell = loadRuntimeInterior(paths, 0x371dd, true);
        const auto prepared = prepareSceneScripts(cell, *source.scripts.vfs,
            [&](auto context, auto error) { log.write(context, error); }, nullptr, questAliasResolver(source.read));
        PapyrusState claw(source, log, true);
        std::map<std::uint32_t, WorldScriptPose> placements;
        for (const auto& [id, ref] : cell.exteriorReferences)
        {
            placements.emplace(id, WorldScriptPose{ref.cellId, ref.position});
            claw.report.visibility[id] = !ref.initiallyDisabled;
        }
        claw.world.addPlacements(placements);
        claw.world.teleportPlayer({0x371dd, {}});
        claw.scripts.install(prepared);
        const auto advance = [&](int frames) {
            for (int i = 0; i < frames; ++i)
            {
                claw.time += .1;
                claw.updateModelAnimations();
                claw.scripts.tick(claw.time);
            }
        };
        advance(50);
        require(claw.activateReference(0x4e2b9, 0x14), "The claw keyhole is not bound");
        advance(50);
        require(!claw.modelAnimations.contains(0x4e2b8), "The claw door opened without the key");
        ActorInventory::add(claw.inventory.inventory(0x14, 7, source.read), 0x39647, 1);
        solveClawRings(claw, prepared, advance);
        advance(60);
        require(claw.modelAnimations.contains(0x4e2b8), "The stock keyhole did not open the solved claw door");
        std::cout << "PASS stock claw door checks the key and opens after all three rings match\n";
    }

    void stockDungeon(const char* data, const char* pluginList, const char* logPath)
    {
        const auto dataPath = Files::pathFromUnicodeString(data);
        const auto paths = loadPluginList(Files::pathFromUnicodeString(pluginList), dataPath);
        LoadResult source;
        auto index = std::make_shared<WinningRecordIndex>();
        source.read = indexWinningRecords(paths, index.get());
        source.index = index;
        source.scripts.vfs = mountRuntimeAssets(buildAssetPlan(dataPath, paths, {}, {}, false));
        ErrorLog log;
        log.open(Files::pathFromUnicodeString(logPath));
        PapyrusState state(source, log, true);
        const auto loadCell = [&](std::uint32_t cellId) {
            const auto cell = loadRuntimeInterior(paths, cellId, true);
            const auto prepared = prepareSceneScripts(cell, *source.scripts.vfs,
                [&](auto context, auto error) { log.write(context, error); }, nullptr, questAliasResolver(source.read));
            std::map<std::uint32_t, WorldScriptPose> placements;
            std::map<std::uint32_t, std::uint32_t> actors;
            std::vector<std::uint32_t> loaded;
            for (const auto& [id, ref] : cell.exteriorReferences)
            {
                placements.emplace(id, WorldScriptPose{ref.cellId, ref.position});
                state.report.visibility[id] = !ref.initiallyDisabled;
                loaded.push_back(id);
                if (ref.recordType == ESM::fourCC("ACHR")) actors[id] = ref.baseId;
            }
            state.world.addPlacements(placements);
            state.quests.setLoadedReferences(loaded);
            state.quests.addActors(actors);
            state.scripts.install(prepared);
            state.world.setTriggers(prepared, actors, {});
            return prepared;
        };
        const auto tick = [&](int frames) {
            for (int i = 0; i < frames; ++i)
            {
                state.time += .1;
                state.quests.tick(state.time);
                state.syncQuestItems();
                state.world.tick(state.time);
                for (const auto& event : state.world.takeTriggerEvents())
                    state.scripts.trigger(event.trigger, event.actor, event.entering);
                state.updateModelAnimations();
                state.tickImageFades();
                state.scripts.tick(state.time);
            }
        };
        constexpr std::uint32_t quest = 0x39645, web = 0x1fc43, arvel = 0x39655, claw = 0x39647, word = 0x13e22;
        loadCell(0x371de);
        state.world.addScenes(index->scenesByQuest.at(quest));
        const auto host = state.quests.host();
        require(host.setQuestStageLatent(quest, 20).accepted, "The stock Golden Claw quest could not fill its aliases");
        tick(20);
        auto player = *state.world.placement(web);
        player.position.pos[0] += 25;
        state.world.teleportPlayer(player);
        state.weaponDrawn = true;
        for (int i = 0; i < 8 && !host.isQuestStageDone(quest, 35); ++i)
        {
            state.playerAttack(web);
            tick(20);
        }
        require(host.isQuestStageDone(quest, 35), "Original web destruction handler did not release Arvel");
        require(state.damagedObjects.at(web).noCollision && !state.damagedObjects.at(web).model.empty(),
            "Destroyed web did not retain its authored replacement model and open passage");
        require(!std::get<bool>(*state.world.callFormMethod(arvel, "IsGhost", {})), "Arvel is still protected after release");
        const auto& items = state.inventory.inventory(arvel, state.world.actorBase(arvel), source.read);
        require(items.contains(claw) && items.at(claw) == 1, "The quest did not create the claw in Arvel's inventory");
        for (int i = 0; i < 60 && !state.world.isDead(arvel); ++i)
        {
            const auto target = *state.world.placement(arvel);
            state.world.teleportPlayer(target);
            state.playerAttack(arvel);
            tick(20);
        }
        require(state.world.isDead(arvel), "Ordinary weapon hits could not kill Arvel");
        state.world.teleportPlayer(*state.world.placement(arvel));
        require(state.takeItem(arvel, claw, 1), "The claw could not be looted from Arvel's body");
        tick(20);
        require(host.isQuestStageDone(quest, 40), "The claw's reference alias did not receive OnContainerChanged");
        std::cout << "PASS original destructible web, Arvel release and claw loot with stage 40\n";

        const auto sanctum = loadCell(0x371dd);
        tick(50);
        solveClawRings(state, sanctum, tick);
        tick(60);
        require(state.modelAnimations.contains(0x4e2b8), "The claw from Arvel did not open the solved sanctum door");
        std::cout << "PASS claw obtained from Arvel opens the original sanctum door after solving its rings\n";
        state.lineOfSight = [](void*, std::uint32_t, std::uint32_t) { return 1; };
        state.world.teleportPlayer(*state.world.placement(0x709d4));
        state.scripts.trigger(0x709d2, 0x14, true);
        tick(150);
        require(state.learnedWords.contains(word), "The original Word Wall script did not teach Fus");
        require(host.isQuestStageDone(quest, 60), "The Word Wall did not advance Golden Claw stage 60");
        std::cout << "PASS original Word Wall trigger teaches Fus and advances stage 60\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        using namespace TES5Viewer;
        using namespace Papyrus;
        if (argc == 5 && std::string_view(argv[1]) == "--stock-early")
        {
            stockEarlyQuests(argv[2], argv[3], argv[4]);
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--stock-dungeon")
        {
            stockDungeon(argv[2], argv[3], argv[4]);
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--stock-dialogue")
        {
            stockPlayerDialogue(argv[2], argv[3], argv[4]);
            return 0;
        }
        ESM::Position furniture{};
        const auto seated = furnitureMarkerPose(furniture, {68.7735f, 87.6582f, 0}, 4.712389f);
        const auto head = seated.asVec3() + Misc::Convert::makeOsgQuat(seated) * osg::Vec3f(4.88f, 88.38f, 36.21f);
        require((head - osg::Vec3f(-19.6065f, 92.5382f, 36.21f)).length() < .01,
            "Furniture heading points the execution animation away from the block");
        std::map<std::uint32_t, RecordPayload> records;
        const auto record = [&](std::uint32_t id, const char (&type)[5]) -> RecordPayload& {
            auto& r = records[id]; r.metadata = {id, ESM::fourCC(type), "Fixture.esm"};
            r.mapFormId = [](auto id) { return id; }; return r;
        };
        auto& npc = record(7, "NPC_"); field(npc, "CNTO", {100, 4}); field(npc, "SPLO", {200});
        auto& actor = record(20, "ACHR"); field(actor, "NAME", {7});
        auto& sourceActor = record(42, "ACHR"); field(sourceActor, "NAME", {7});
        auto& box = record(30, "REFR"); field(box, "NAME", {31});
        auto& lockedDoor = record(32, "REFR"); field(lockedDoor, "NAME", {33}); field(lockedDoor, "XLOC", {255, 0, 0});
        record(33, "DOOR");
        record(31, "CONT"); record(100, "MISC"); record(200, "SPEL"); record(201, "SPEL");
        auto& inherited = record(10, "NPC_");
        field(inherited, "ACBS", {0, 0, 0, 0, 0x08u << 16, 0});
        field(inherited, "TPLT", {7}); field(inherited, "SPLO", {201});
        auto& ownSpells = record(11, "NPC_");
        field(ownSpells, "ACBS", {0, 0, 0, 0, 0x10u << 16, 0});
        field(ownSpells, "TPLT", {7}); field(ownSpells, "SPLO", {201});
        auto& imod = record(300, "IMAD");
        field(imod, "DNAM", {1, std::bit_cast<std::uint32_t>(2.f)});
        field(imod, "RIAD", {0, 0, std::bit_cast<std::uint32_t>(1.f), std::bit_cast<std::uint32_t>(-1.f)});
        auto& message = record(301, "MESG"); field(message, "DESC", {0x6b6c6157, 0x776f6e20, 0});
        auto& tallNpc = record(43, "NPC_"); field(tallNpc, "RNAM", {45});
        auto& tallActor = record(41, "ACHR"); field(tallActor, "NAME", {43}); field(tallActor, "XSCL", {std::bit_cast<unsigned>(.97f)});
        auto& tallRace = record(45, "RACE"); field(tallRace, "DATA", {0,0,0,0,std::bit_cast<unsigned>(1.03f),std::bit_cast<unsigned>(1.f)});
        auto& weatherWorld=record(500,"WRLD");field(weatherWorld,"CNAM",{501});
        auto& climate=record(501,"CLMT");field(climate,"TNAM",{0x7b603c21});
        for(unsigned id:{502,503})
        {
            auto& weather=record(id,"WTHR");
            field(weather,"NAM0",{20,100,60,0,20,100,60,0,20,100,60,0,20,100,60,0,20,100,60,0});
            field(weather,"FNAM",{0,std::bit_cast<unsigned>(id==502?25000.f:100000.f),0,
                std::bit_cast<unsigned>(id==502?15000.f:50000.f),std::bit_cast<unsigned>(.4f),std::bit_cast<unsigned>(.375f),std::bit_cast<unsigned>(.95f),std::bit_cast<unsigned>(.95f)});
            field(weather,"DATA",{0x7d000000});
        }
        auto& region=record(504,"REGN");field(region,"WNAM",{500});field(region,"RDWT",{503,100,0});
        field(region,"RDAT",{3,0x3401});
        field(region,"RPLD",{0,0,std::bit_cast<unsigned>(10.f),0,std::bit_cast<unsigned>(10.f),std::bit_cast<unsigned>(10.f),0,std::bit_cast<unsigned>(10.f)});
        auto& lowRegion=record(505,"REGN");field(lowRegion,"WNAM",{500});field(lowRegion,"RDWT",{503,100,0});field(lowRegion,"RDAT",{3,0x3200});
        field(lowRegion,"RPLD",{std::bit_cast<unsigned>(4.f),0,std::bit_cast<unsigned>(20.f),0,std::bit_cast<unsigned>(20.f),std::bit_cast<unsigned>(10.f),std::bit_cast<unsigned>(4.f),std::bit_cast<unsigned>(10.f)});
        auto& hour=record(600,"GLOB");field(hour,"FLTV",{std::bit_cast<unsigned>(7.f)});
        auto& scale=record(601,"GLOB");field(scale,"FLTV",{std::bit_cast<unsigned>(20.f)});
        auto& days=record(602,"GLOB");field(days,"FLTV",{std::bit_cast<unsigned>(7.f/24.f)});
        LoadResult source;
        source.read = [records](auto id) { return records.at(id); };
        auto index=std::make_shared<WinningRecordIndex>();index->weatherRegions={504,505};
        index->globalsByName={{"gamehour",600},{"timescale",601},{"gamedayspassed",602}};source.index=index;
        source.scripts.vfs = std::make_shared<VFS::Manager>();
        ErrorLog log;
        PapyrusState state(source, log, false);
        WorldWeather weather(source.read,index);weather.force(503,true);weather.begin(500,502,2,5);
        require(std::abs(weather.sample(7.5f)[10]-20000)<.01f,"Morning fog did not use CLMT sunrise and color extension");
        weather.tick(5,5,0);require(weather.current==502,"Entering a lower-priority region replaced the dominant weather region");
        weather.tick(11,5,8./125.);
        require(weather.current==503 && weather.previous==502 && std::abs(weather.progress-.5f)<.0001f,"Region crossing lost pending weather or WTHR transition rate");
        require(std::abs(weather.sample(7.5f)[10]-47500)<.01f,"Weather fog transition is not blended");
        Checkpoint skySaved;weather.checkpoint(skySaved);WorldWeather skyRestored(source.read,index);Checkpoint skyRead{skySaved.bytes()};skyRestored.checkpoint(skyRead);
        require(skyRestored.sample(7.5f)==weather.sample(7.5f),"Snapshot lost in-progress weather blend");
        skyRestored.tick(11,5,8./125.);require(skyRestored.progress==1 && !skyRestored.previous,"Weather transition did not finish");
        PapyrusState clock(source,log,false);clock.quests.tick(360);
        require(std::abs(clock.quests.globalValue("gamehour",0)-9)<.0001f,"GameHour did not advance using TimeScale");
        Checkpoint clockSaved;clock.quests.checkpoint(clockSaved);PapyrusState clockRestored(source,log,false);Checkpoint clockRead{clockSaved.bytes()};clockRestored.quests.checkpoint(clockRead);
        clock.quests.tick(361);clockRestored.quests.tick(361);
        require(clock.quests.globalValue("gamehour",0)==clockRestored.quests.globalValue("gamehour",0),"Restored game clock diverged");
        clock.quests.host().setGlobalValue(600,12.f);
        require(std::abs(clock.quests.globalValue("gamedayspassed",0)-.5f)<.0001f,"GameHour setter did not update the calendar fraction");
        WorldScriptPose passenger{}, vehiclePose{}; passenger.cell = vehiclePose.cell = 42;
        state.world.addPlacements({{41, passenger}, {30, vehiclePose}});
        require(state.world.visualScale(41) == 1, "Unattached actor was normalized");
        state.world.callFormMethod(41, "SetVehicle", {FormReference{30}});
        require(std::abs(state.world.visualScale(41) * .97f * 1.03f - 1) < .00001f, "Vehicle passenger did not normalize reference and race scale");
        state.world.callFormMethod(41, "SetVehicle", {FormReference{0}});
        require(std::abs(state.world.visualScale(41) * .97f * 1.03f - 1) < .00001f, "Dismount lost the established 3D scale");
        Checkpoint vehicleSaved; state.world.checkpointExtensions(vehicleSaved);
        PapyrusState vehicleRestored(source, log, false); Checkpoint vehicleRead{vehicleSaved.bytes()};
        vehicleRestored.world.checkpointExtensions(vehicleRead);
        require(vehicleRestored.world.visualScale(41) == state.world.visualScale(41), "Snapshot lost vehicle actor normalization");
        Checkpoint legacyVehicle; std::map<std::uint32_t, std::uint32_t> noVolleys;
        // The old extension with no volleys is an empty map count.
        legacyVehicle(noVolleys); Checkpoint legacyRead{legacyVehicle.bytes()};
        PapyrusState legacyRestored(source, log, false); legacyRestored.world.checkpointExtensions(legacyRead);
        require(legacyRead.atEnd() && legacyRestored.world.visualScale(41) == 1, "Legacy snapshot extension was rejected");
        require(state.inventory.spellbook(70, 10, source.read) == std::set<std::uint32_t>{200},
            "Use Spell List must inherit bit 0x0008");
        require(state.inventory.spellbook(71, 11, source.read) == std::set<std::uint32_t>{201},
            "Use AI Data must not inherit spells");
        auto host = state.makeHost(false);
        auto noWaitScript = std::make_shared<Script>();
        noWaitScript->name = "NoWaitSmoke"; noWaitScript->parent = "ObjectReference";
        Function init; init.name = "OnInit"; init.returnType = "None";
        init.instructions = {
            {Opcode::CallMethod, {Identifier{"EnableNoWait"}, Identifier{"self"}, Identifier{"::nonevar"}, std::int32_t(1), true}},
            {Opcode::CallMethod, {Identifier{"DisableNoWait"}, Identifier{"self"}, Identifier{"::nonevar"}, std::int32_t(0)}},
            {Opcode::Return, {Value{}}}};
        noWaitScript->states.push_back({"", {init}});
        std::vector<std::pair<bool,bool>> visibilityCalls;
        auto noWaitHost = host;
        noWaitHost.setDisabled = [&](auto id, bool disabled, bool fade) {
            require(id == 30, "NoWait targeted the wrong reference"); visibilityCalls.emplace_back(disabled,fade);
        };
        ScriptInstance noWait(noWaitScript, 30, {}, noWaitHost);
        const auto initialized = noWait.startInitialization();
        require(initialized.completed.has_value() && !initialized.continuation && !initialized.nativeWait
            && visibilityCalls == std::vector<std::pair<bool,bool>>{{false,true},{true,false}},
            "EnableNoWait/DisableNoWait changed fade/default arguments or suspended the VM");
        const auto call = [&](auto id, auto name, std::vector<Value> args) {
            auto result = host.callFormMethod(id, name, args);
            require(result.has_value(), "Native was not handled"); return *result;
        };
        require(std::get<bool>(call(32, "IsLocked", {})), "Authored XLOC was not loaded");
        call(32, "SetLockLevel", {std::int32_t(5)});
        require(std::get<bool>(call(32, "IsLocked", {}))
            && std::get<std::int32_t>(call(32, "GetLockLevel", {})) == 5, "Difficulty changed the lock state");
        Checkpoint locksSaved;
        state.checkpointPapyrusExtensions(locksSaved);
        PapyrusState locksRestored(source, log, false);
        Checkpoint locksRead{locksSaved.bytes()};
        locksRestored.checkpointPapyrusExtensions(locksRead);
        require(locksRead.atEnd() && locksRestored.lockState(32) == state.lockState(32), "Checkpoint lost mutable lock state");
        call(32, "SetOpen", {true});
        require(!std::get<bool>(call(32, "IsLocked", {})), "Opening a door did not remove the lock");
        auto trigger = std::make_shared<Script>();
        trigger->name = "LoadBeforeTrigger"; trigger->parent = "ObjectReference";
        Function loadEvent; loadEvent.name = "OnLoad"; loadEvent.returnType = "None";
        loadEvent.instructions = {{Opcode::Return, {Value{}}}};
        Function enterEvent = loadEvent; enterEvent.name = "OnTriggerEnter";
        enterEvent.parameters.push_back({"actor", "ObjectReference"});
        trigger->states.push_back({"", {loadEvent, enterEvent}});
        state.report.visibility[30] = true;
        state.scripts.install({{30, trigger, "Fixture", {}}});
        state.scripts.trigger(30, 20, true);
        state.scripts.tick(1);
        state.scripts.tick(2);
        const auto events = state.scripts.takeEvents();
        require(events.size() == 2 && events[0].event == "OnLoad" && events[1].event == "OnTriggerEnter",
            "OnTriggerEnter ran before OnLoad or loaded twice");
        require(std::get<std::int32_t>(call(20, "GetItemCount", {FormReference{100}})) == 4, "Authored CNTO missing");
        call(20, "RemoveItem", {FormReference{100}, std::int32_t(3), true, FormReference{30}});
        require(std::get<std::int32_t>(call(20, "GetItemCount", {FormReference{100}})) == 1, "Source count after transfer");
        require(std::get<std::int32_t>(call(30, "GetItemCount", {FormReference{100}})) == 3, "Transfer did not reach container");
        call(20, "RemoveItem", {FormReference{100}, std::int32_t(999)});
        require(std::get<std::int32_t>(call(20, "GetItemCount", {FormReference{100}})) == 0, "Over-removal must clamp to zero");
        call(20, "AddItem", {FormReference{100}, std::int32_t(2)});
        call(20, "RemoveItem", {FormReference{100}, std::int32_t(-2)});
        require(std::get<std::int32_t>(call(20, "GetItemCount", {FormReference{100}})) == 2, "Negative removal added items");
        require(std::get<bool>(call(20, "RemoveSpell", {FormReference{200}})), "Authored SPLO was not removed");
        require(!std::get<bool>(call(20, "RemoveSpell", {FormReference{200}})), "Absent spell removal must return false");
        require(std::get<bool>(call(20, "AddSpell", {FormReference{201}, true})), "Spell was not added");
        require(!std::get<bool>(call(20, "AddSpell", {FormReference{201}})), "Duplicate spell must return false");
        require(std::get<bool>(call(20, "HasSpell", {FormReference{201}})), "HasSpell disagrees with AddSpell");
        Checkpoint saved; state.inventory.checkpoint(saved);
        ActorInventory restored; Checkpoint loaded{saved.bytes()}; restored.checkpoint(loaded);
        require(loaded.atEnd() && restored.items == state.inventory.items && restored.spells == state.inventory.spells,
            "Inventory/spell snapshot lost state");
        require(state.callGame("RequestSave", {}).has_value(), "RequestSave not handled");
        require(state.callGame("RequestAutoSave", {}).has_value(), "RequestAutoSave not handled");
        require(state.presentation == std::vector<std::string>{"[\"requestsave\"]", "[\"requestautosave\"]"},
            "Save requests did not reach the UE host queue");
        require(!std::get<bool>(*state.callGame("UsingGamepad", {})), "Keyboard must be the initial input mode");
        state.visuals.gamepad = true;
        require(std::get<bool>(*state.callGame("UsingGamepad", {})), "Host gamepad mode was ignored");
        state.time = 10;
        call(300, "Apply", {.5f});
        require(state.visuals.sample(state.read, 11) == "[\"imagespace\",1,0.75,1,1,0,0,0,0,1]",
            "IMAD strength, normalized time or brightness-add sampling is wrong");
        Checkpoint visualsSaved; state.visuals.checkpoint(visualsSaved);
        PresentationState visualsRestored; Checkpoint visualsRead{visualsSaved.bytes()}; visualsRestored.checkpoint(visualsRead);
        require(visualsRestored.sample(state.read, 11) == state.visuals.sample(state.read, 11), "IMAD snapshot lost its clock");
        state.visuals.sample(state.read, 12);
        require(state.visuals.effects.empty(), "Animated IMAD did not expire");
        call(300, "Apply", {}); call(300, "Remove", {});
        require(state.visuals.effects.empty(), "IMAD Remove did not clear active effect");
        state.presentation.clear();
        const std::vector<Value> help{std::string("Move"), 5.f, 1.f, std::int32_t(1)};
        call(301, "ShowAsHelpMessage", help); call(301, "ShowAsHelpMessage", help);
        require(state.presentation.size() == 1 && state.presentation[0].find("Walk now") != std::string::npos,
            "Help message text/count suppression failed");
        if (argc == 2)
        {
            std::ifstream input(argv[1], std::ios::binary);
            require(bool(input), "Could not open the stock push-effect PEX");
            auto pex = std::make_shared<PexFile>(readPex(input));
            require(pex->scripts.size() == 1, "Expected one push-effect class");
            const std::shared_ptr<const Script> effect(pex, &pex->scripts.front());
            PapyrusState magic(source, log, false);
            WorldScriptPose caster{}, npcTarget{}, playerTarget{};
            npcTarget.position.pos[0] = 200;
            playerTarget.position.pos[1] = 200;
            magic.world.addPlacements({{42, caster}, {41, npcTarget}, {0x14, playerTarget}});
            for (const auto [handle, target] : {std::pair{0xffffffffu, 41u}, std::pair{0xfffffffeu, 0x14u}})
                magic.scripts.addEffect(handle, target, 42, 1,
                    {{handle, effect, "Skyrim.esm", {{"PushForce", std::int32_t(15)}}}});
            magic.scripts.tick(.25);
            magic.world.tick(.25);
            const auto falls = magic.world.takeFalls();
            require(falls.size() == 1 && falls.front().actor == 41 && falls.front().velocity.x() > 0,
                "The stock effect did not push the NPC away, or incorrectly pushed the player");
            std::cout << "PASS stock push-effect PEX, NPC launch and player exclusion\n";
        }
        std::cout << "PASS input mode, IMAD strength/curves/expiry/remove/checkpoint and help suppression\n";
        std::cout << "PASS inventory transfers, clamped removal, spell booleans, persistence and both save requests\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
