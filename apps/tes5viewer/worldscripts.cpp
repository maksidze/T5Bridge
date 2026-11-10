#include "worldscripts.hpp"
#include "furniturepose.hpp"
#include "checkpointtypes.hpp"

#include "archery.hpp"
#include "cartroute.hpp"
#include "hkxmovement.hpp"
#include "assetresources.hpp"
#include "actorappearance.hpp"
#include "cellscene.hpp"
#include "questscripts.hpp"
#include "musicruntime.hpp"
#include "../papyrustool/errorlog.hpp"

#include <components/esm/fourcc.hpp>
#include <components/esm4/loadnavm.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/vfs/manager.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/extra.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <numbers>
#include <queue>
#include <random>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace TES5Viewer
{
    bool WorldScriptTriggerBox::intersects(
        const osg::Vec3d& from, const osg::Vec3d& to, const osg::Vec3d& padding) const
    {
        const auto start = from * worldToBox;
        const auto delta = to * worldToBox - start;
        double enter = 0, leave = 1;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const auto extent = halfBounds[axis] + padding[axis];
            if (std::abs(delta[axis]) < 1e-8)
            {
                if (std::abs(start[axis]) > extent)
                    return false;
                continue;
            }
            const auto low = (-extent - start[axis]) / delta[axis];
            const auto high = (extent - start[axis]) / delta[axis];
            enter = std::max(enter, std::min(low, high));
            leave = std::min(leave, std::max(low, high));
            if (enter > leave)
                return false;
        }
        return true;
    }

    namespace
    {
        using namespace Papyrus;
        using ESM::fourCC;
        using Bytes = std::span<const std::byte>;
        using Conditions = std::vector<Bytes>;

        constexpr std::uint32_t firstScriptActorValue = 68; // TES5 actor value enum: Variable01 through Variable10.
        constexpr std::array<std::string_view, 10> scriptActorValues{
            "Variable01", "Variable02", "Variable03", "Variable04", "Variable05",
            "Variable06", "Variable07", "Variable08", "Variable09", "Variable10"};

        std::uint32_t integer(Bytes bytes, std::size_t offset = 0, std::size_t size = 4)
        {
            if (offset > bytes.size() || size > bytes.size() - offset)
                throw std::runtime_error("Truncated SCEN or PACK field");
            std::uint32_t result = 0;
            for (std::size_t i = 0; i < size; ++i)
                result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
            return result;
        }

        float real(Bytes bytes, std::size_t offset = 0)
        {
            const float result = std::bit_cast<float>(integer(bytes, offset));
            if (!std::isfinite(result))
                throw std::runtime_error("Non-finite SCEN or PACK field");
            return result;
        }

        std::string string(Bytes bytes)
        {
            if (bytes.empty())
                return {};
            const auto end = std::ranges::find(bytes, std::byte{});
            return {reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(end - bytes.begin())};
        }

        void arity(const std::vector<Value>& args, std::size_t min, std::size_t max)
        {
            if (args.size() < min || args.size() > max)
                throw std::runtime_error("Invalid SCEN/PACK native argument count");
        }

        template <class T>
        T argument(const std::vector<Value>& args, std::size_t index)
        {
            if (index < args.size())
                if (const auto* value = std::get_if<T>(&args[index]))
                    return *value;
            throw std::runtime_error("Invalid SCEN/PACK native argument type");
        }

        std::uint32_t formArgument(const std::vector<Value>& args, std::size_t index)
        {
            if (index >= args.size() || std::holds_alternative<std::monostate>(args[index]))
                return 0;
            return argument<FormReference>(args, index).formId;
        }

        float number(const std::vector<Value>& args, std::size_t index)
        {
            if (index >= args.size())
                return 0;
            if (const auto* value = std::get_if<std::int32_t>(&args[index]))
                return static_cast<float>(*value);
            const auto value = argument<float>(args, index);
            if (!std::isfinite(value))
                throw std::runtime_error("Non-finite world transform argument");
            return value;
        }

        Value formValue(std::uint32_t id) { return id ? Value(FormReference{id}) : Value{}; }

        double voiceDuration(std::istream& input)
        {
            std::vector<std::byte> bytes;
            std::array<char, 4096> chunk;
            while (input.read(chunk.data(), chunk.size()) || input.gcount())
            {
                if (bytes.size() + input.gcount() > 16 * 1024 * 1024)
                    throw std::runtime_error("Scene voice exceeds the FUZ size limit");
                const auto first = reinterpret_cast<const std::byte*>(chunk.data());
                bytes.insert(bytes.end(), first, first + input.gcount());
            }
            if (integer(bytes) != fourCC("FUZE") || integer(bytes, 4) != 1)
                throw std::runtime_error("Unsupported scene FUZ format");
            const auto audio = std::size_t(12) + integer(bytes, 8);
            if (integer(bytes, audio) != fourCC("RIFF"))
                throw std::runtime_error("Scene FUZ has no RIFF audio");
            const auto type = integer(bytes, audio + 8);
            std::uint32_t sampleRate = 0, channels = 0, bits = 0, decodedBytes = 0, dataBytes = 0;
            for (auto offset = audio + 12; offset + 8 <= bytes.size();)
            {
                const auto tag = integer(bytes, offset), count = integer(bytes, offset + 4);
                offset += 8;
                if (count > bytes.size() - offset)
                    throw std::runtime_error("Truncated scene voice audio chunk");
                if (tag == fourCC("fmt ") && count >= 16)
                {
                    channels = integer(bytes, offset + 2, 2);
                    sampleRate = integer(bytes, offset + 4);
                    bits = integer(bytes, offset + 14, 2);
                }
                else if (tag == fourCC("dpds") && count >= 4 && count % 4 == 0)
                    decodedBytes = integer(bytes, offset + count - 4);
                else if (tag == fourCC("data"))
                    dataBytes = count;
                offset += count + (count & 1);
            }
            if (type == fourCC("WAVE") && !decodedBytes)
                decodedBytes = dataBytes;
            if (!decodedBytes || !sampleRate || !channels || !bits)
                throw std::runtime_error("Scene voice has no usable decoded sample count");
            return (8. * decodedBytes) / (double(sampleRate) * channels * bits);
        }

        struct Phase
        {
            std::string name;
            Conditions start, complete;
        };

        struct Action
        {
            std::uint32_t id = 0, type = 0, actor = 0, first = 0, last = 0, topic = 0;
            float seconds = 0;
            std::vector<std::uint32_t> packages;
            bool hasFirst = false;
            std::int32_t headtrack = -1, emotion = -1;
            std::uint32_t flags = 0, emotionValue = 0;
        };

        struct Scene
        {
            RecordPayload record;
            SceneVmad vmad;
            std::uint32_t owner = 0, flags = 0;
            std::vector<Phase> phases;
            std::map<std::uint32_t, bool> actors;
            std::vector<Action> actions;
            Conditions conditions;

            explicit Scene(RecordPayload source) : record(std::move(source))
            {
                Phase* phase = nullptr;
                Action* action = nullptr;
                unsigned markers = 0;
                std::optional<std::uint32_t> actor;
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("HNAM"))
                    {
                        phase = phase ? nullptr : &phases.emplace_back();
                        markers = 0;
                    }
                    else if (phase)
                    {
                        if (tag == fourCC("NAM0"))
                            phase->name = string(bytes);
                        else if (tag == fourCC("NEXT"))
                            ++markers;
                        else if (tag == fourCC("CTDA") && markers < 2)
                            (markers ? phase->complete : phase->start).push_back(bytes);
                    }
                    else if (tag == fourCC("ANAM"))
                        action = bytes.empty() ? nullptr : &actions.emplace_back(Action{.type = integer(bytes, 0, 2)});
                    else if (action)
                    {
                        if (tag == fourCC("HTID")) action->headtrack = std::bit_cast<std::int32_t>(integer(bytes));
                        else if (tag == fourCC("FNAM")) action->flags = integer(bytes);
                        else if (tag == fourCC("DEMO")) action->emotion = static_cast<int>(integer(bytes));
                        else if (tag == fourCC("DEVA")) action->emotionValue = integer(bytes);
                        else if (tag == fourCC("ALID"))
                            action->actor = integer(bytes);
                        else if (tag == fourCC("INAM"))
                            action->id = integer(bytes);
                        else if (tag == fourCC("SNAM"))
                        {
                            if (action->hasFirst)
                                action->seconds = real(bytes);
                            else
                            {
                                action->first = integer(bytes);
                                action->hasFirst = true;
                            }
                        }
                        else if (tag == fourCC("ENAM"))
                            action->last = integer(bytes);
                        else if (tag == fourCC("PNAM"))
                            action->packages.push_back(record.mapFormId(integer(bytes)));
                        else if (tag == fourCC("DATA"))
                            action->topic = record.mapFormId(integer(bytes));
                    }
                    else if (tag == fourCC("VMAD"))
                        vmad = readSceneVmad(bytes);
                    else if (tag == fourCC("FNAM"))
                        flags = integer(bytes);
                    else if (tag == fourCC("ALID"))
                        actor = integer(bytes);
                    else if (tag == fourCC("LNAM") && actor)
                        actors[*actor] = (integer(bytes) & 2) != 0;
                    else if (tag == fourCC("PNAM"))
                        owner = record.mapFormId(integer(bytes));
                    else if (tag == fourCC("CTDA"))
                        conditions.push_back(bytes);
                });
                if (phase || action || !owner)
                    throw std::runtime_error("Malformed SCEN phase, action, or owning quest");
                for (const auto& entry : actions)
                    if (!entry.hasFirst || entry.first > entry.last || entry.last >= phases.size())
                        throw std::runtime_error("SCEN action phase is outside its scene");
            }
        };

        struct Target
        {
            bool location;
            std::uint32_t type, value, radius;
        };

        struct Input
        {
            std::optional<Target> target;
            Bytes value;
        };

        constexpr std::uint32_t preferredSpeedFlag = 0x2000;

        struct Procedure
        {
            std::string name;
            std::vector<std::uint8_t> inputs;
            Conditions conditions;
            std::optional<std::uint8_t> speedOverride;
            bool completesPackage = false;
        };

        struct Package
        {
            RecordPayload record;
            PackageVmad vmad;
            std::uint32_t owner = 0, templateId = 0;
            std::uint8_t speed = 0;
            bool hasPreferredSpeed = false;
            std::uint32_t beginIdle = 0, endIdle = 0, changeIdle = 0;
            std::map<std::uint8_t, Input> inputs;
            Conditions conditions;
            std::vector<Procedure> procedures;
            bool anySchedule = true, mustComplete = false, repeats = false, otherBranches = false, inventoryWeapon = false;

            std::uint8_t preferredSpeed(const Procedure& procedure) const
            {
                return procedure.speedOverride.value_or(hasPreferredSpeed ? speed : 0);
            }

            explicit Package(RecordPayload source) : record(std::move(source))
            {
                bool tree = false;
                std::vector<std::string> branches;
                Procedure* procedure = nullptr;
                std::vector<Input> values;
                std::size_t valueIndex = 0;
                std::uint32_t* idle = nullptr;
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("VMAD"))
                        vmad = readPackageVmad(bytes);
                    else if (tag == fourCC("PSDT"))
                        anySchedule = integer(bytes, 0, 1) == 255 && integer(bytes, 1, 1) == 255
                            && integer(bytes, 2, 1) == 0 && integer(bytes, 3, 1) == 255
                            && integer(bytes, 4, 1) == 255;
                    else if (tag == fourCC("PKDT"))
                    {
                        speed = static_cast<std::uint8_t>(integer(bytes, 6, 1));
                        const auto flags = integer(bytes);
                        hasPreferredSpeed = (flags & preferredSpeedFlag) != 0;
                        mustComplete = (flags & 4) != 0;
                    }
                    else if (tag == fourCC("PKCU"))
                        templateId = record.mapFormId(integer(bytes, 4));
                    else if (tag == fourCC("QNAM") && !tree)
                        owner = record.mapFormId(integer(bytes));
                    else if (tag == fourCC("XNAM"))
                        tree = true;
                    else if (tag == fourCC("CTDA"))
                        (procedure ? procedure->conditions : conditions).push_back(bytes);
                    else if (tag == fourCC("ANAM"))
                    {
                        if (!tree)
                            values.emplace_back();
                        else
                        {
                            const auto branch = string(bytes);
                            branches.push_back(branch);
                            if (branch == "Procedure")
                                procedure = &procedures.emplace_back();
                            else
                            {
                                otherBranches |= branch != "Sequence" || procedure != nullptr;
                                procedure = nullptr;
                            }
                        }
                    }
                    else if (!tree && !values.empty())
                    {
                        if (tag == fourCC("UNAM"))
                        {
                            if (valueIndex >= values.size())
                                throw std::runtime_error("PACK has more data indexes than values");
                            inputs.emplace(static_cast<std::uint8_t>(integer(bytes, 0, 1)), values[valueIndex++]);
                        }
                        else if (tag == fourCC("PLDT") || tag == fourCC("PTDA"))
                        {
                            const bool location = tag == fourCC("PLDT");
                            const auto type = integer(bytes);
                            const auto value = integer(bytes, 4);
                            const bool isForm = type == 0 || (location ? type == 1 || type == 4 || type == 6 : type == 1 || type == 3);
                            values.back().target = Target{location, type, isForm ? record.mapFormId(value) : value,
                                integer(bytes, 8)};
                        }
                        else if (tag == fourCC("CNAM"))
                            values.back().value = bytes;
                    }
                    else if (tag == fourCC("PRCB") && !procedure)
                        repeats |= (integer(bytes, 4) & 1) != 0;
                    else if (procedure && tag == fourCC("PNAM") && procedure->name.empty())
                        procedure->name = string(bytes);
                    else if (procedure && tag == fourCC("PKC2"))
                        procedure->inputs.push_back(static_cast<std::uint8_t>(integer(bytes, 0, 1)));
                    else if (procedure && tag == fourCC("PFO2"))
                    {
                        if (integer(bytes, 4) & preferredSpeedFlag)
                            procedure->speedOverride = 0;
                        else if (integer(bytes) & preferredSpeedFlag)
                            procedure->speedOverride = static_cast<std::uint8_t>(integer(bytes, 12, 1));
                    }
                    else if (procedure && tag == fourCC("FNAM"))
                        procedure->completesPackage = (integer(bytes) & 1) != 0;
                    else if (tag == fourCC("POBA") || tag == fourCC("POEA") || tag == fourCC("POCA"))
                        idle = tag == fourCC("POBA") ? &beginIdle : tag == fourCC("POEA") ? &endIdle : &changeIdle;
                    else if (tag == fourCC("INAM") && idle)
                        *idle = record.mapFormId(integer(bytes));
                });
                if (branches == std::vector<std::string>{"Stacked", "Procedure"} && procedures.size() == 1)
                    otherBranches = false;
                // Bethesda's SitTarget: approach/enter furniture, then Sit+Wait.
                // A zero duration completes the scene action on arrival while the
                // actor keeps the furniture pose until another package takes over.
                if (branches == std::vector<std::string>{"Sequence", "Procedure", "Simultaneous", "Procedure", "Procedure"}
                    && procedures.size() == 3 && procedures[0].name == "Sit"
                    && procedures[1].name == "Sit" && procedures[2].name == "Wait"
                    && std::ranges::all_of(procedures, [](const auto& p) { return p.conditions.empty(); }))
                {
                    procedures[0].inputs.insert(procedures[0].inputs.end(), procedures[2].inputs.begin(), procedures[2].inputs.end());
                    procedures.resize(1);
                    otherBranches = false;
                }
                if (procedures.size() == 5 && procedures[0].name == "Find" && procedures[1].name == "Travel"
                    && procedures[2].name == "Wander" && procedures[3].name == "Sit" && procedures[4].name == "Wait"
                    && std::ranges::all_of(procedures, [](const auto& p) { return p.conditions.empty(); }))
                {
                    // Bethesda Sit: find the selected furniture; otherwise move to
                    // the authored location. The object-list is a Find output.
                    auto find = procedures.front(); find.name = "FindSit";
                    find.inputs.resize(2);
                    find.inputs.insert(find.inputs.end(), procedures[4].inputs.begin(), procedures[4].inputs.end());
                    find.completesPackage = true; procedures = {std::move(find)}; otherBranches = false;
                }
                // Inventory bow variant: acquisition/search branches are already
                // satisfied by the actor's CNTO weapon. Keep approach and attack.
                const bool simpleWeapon = procedures.size() == 3 && procedures[0].name == "UseWeapon"
                    && procedures[1].name == "Wait" && procedures[2].name == "Travel";
                const bool searchWeapon = procedures.size() == 8 && procedures[0].name == "Travel"
                    && procedures[1].name == "Find" && procedures[2].name == "Wander"
                    && procedures[3].name == "Acquire" && procedures[4].name == "Travel"
                    && procedures[5].name == "UseWeapon" && procedures[6].name == "Wait" && procedures[7].name == "Travel";
                if (simpleWeapon || searchWeapon)
                {
                    auto attack = procedures[simpleWeapon ? 0 : 5];
                    const auto& wait = procedures[simpleWeapon ? 1 : 6];
                    attack.inputs.resize(18, 255);
                    if (!wait.inputs.empty()) attack.inputs.push_back(wait.inputs[0]);
                    attack.completesPackage = true;
                    if (searchWeapon) procedures = {procedures[0], std::move(attack)};
                    else procedures = {std::move(attack)};
                    otherBranches = false; inventoryWeapon = true;
                }
            }
        };
    }

    struct WorldScripts::Impl
    {
        ReadWinningRecord read;
        std::shared_ptr<const WinningRecordIndex> index;
        std::shared_ptr<const VFS::Manager> vfs;
        QuestScripts& quests;
        ErrorLog& log;
        ResolveQuestAlias aliases;
        WorldScriptTransforms authored, moved;
        std::map<std::uint32_t, std::uint32_t> relocations;
        std::map<std::uint32_t, WorldScriptMovingReference> movingReferences;
        std::map<std::tuple<std::uint32_t, int, int>, std::uint32_t> exteriorCells;
        std::map<std::uint32_t, std::uint32_t> cellWorlds;
        using NavNode = std::pair<std::uint32_t, std::uint16_t>;
        struct NavTriangle
        {
            std::array<osg::Vec3f, 3> vertices;
            std::array<NavNode, 3> neighbors;
            osg::Vec3f center() const { return (vertices[0] + vertices[1] + vertices[2]) / 3.f; }
        };
        std::map<std::uint32_t, std::map<std::uint16_t, NavTriangle>> navigation;
        std::set<std::uint32_t> loaded;
        std::set<std::uint32_t> fixedReferences;
        std::map<std::uint32_t, Scene> scenes;
        std::map<std::uint32_t, Package> packages;
        std::map<std::string, std::shared_ptr<const PexFile>> pex;
        std::map<std::tuple<std::uint32_t, std::uint8_t, bool, bool>, float> speeds;
        std::map<std::string, double> voiceDurations;
        std::set<std::uint32_t> normalizedVehicleActors;
        std::set<std::uint32_t> normalizedFurnitureActors;
        std::map<std::uint32_t, float> visualScaleCache;
        std::set<std::uint32_t> autostart, started, seen, reevaluate, explicitReevaluate;
        std::map<std::uint32_t, bool> requestedScenes;
        std::map<std::uint32_t, std::set<std::uint32_t>> finishedActions;
        std::vector<SceneScriptEvent> events;
        std::vector<WorldScriptAnimation> animations;
        std::map<std::uint32_t, std::string> locomotion;
        std::vector<WorldScriptAudio> audio;
        MusicRuntime music;
        std::int32_t nextSound = 1;
        std::minstd_rand soundRandom{std::random_device{}()};
        std::map<std::uint32_t, WorldScriptTriggerBox> triggerBoxes;
        std::map<std::uint32_t, std::set<std::uint32_t>> triggerInside;
        std::set<std::uint32_t> triggerActors, teleported;
        std::map<std::uint32_t, osg::BoundingBoxd> triggerActorBounds;
        std::map<std::uint32_t, osg::Vec3d> triggerActorPositions;
        std::vector<WorldScriptTriggerEvent> triggerEvents;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> furnitureActivations;
        std::map<std::uint32_t, std::uint32_t> occupiedPerches;
        std::map<std::uint32_t, std::uint32_t> enteringExecutionBlock;
        std::set<std::uint32_t> executionChops;
        struct ShoutCast
        {
            enum Phase { Aim, Inhale, Exhale } phase = Aim;
            std::uint32_t voice = 0, spell = 0, target = 0;
            void checkpoint(Checkpoint& ar) { ar(phase, voice, spell, target); }
        };
        std::map<std::uint32_t, ShoutCast> shouts;
        std::vector<std::array<std::uint32_t, 3>> spellCasts;
        std::map<std::uint32_t, std::uint32_t> combatTargets;
        std::map<std::uint32_t, std::uint32_t> landingPerches;
        std::set<std::uint32_t> noFlight;
        std::map<std::uint32_t, bool> ghostOverrides;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::int32_t> relationshipOverrides, factionOverrides;
        struct FlightProfile
        {
            std::uint32_t movement = 0;
            std::string behavior;
        };
        std::map<std::uint32_t, FlightProfile> flightProfiles;
        struct FlightGrab
        {
            enum Phase { Approach, Enter, Exit, Failed } phase = Approach;
            std::uint32_t target = 0;
            void checkpoint(Checkpoint& ar) { ar(phase, target); }
        };
        std::map<std::uint32_t, FlightGrab> flightGrabs;
        struct FallingActor
        {
            std::uint32_t source = 0;
            osg::Vec3f velocity;
            float highestZ = 0;
            void checkpoint(Checkpoint& ar) { ar(source, velocity, highestZ); }
        };
        std::map<std::uint32_t, FallingActor> fallingActors;
        std::map<std::uint32_t, osg::Vec3f> carrierVelocities;
        std::vector<WorldScriptFall> fallRequests;
        std::map<std::uint32_t, std::vector<Nif::BSFurnitureMarker::FurniturePosition>> furnitureMarkers;
        double time = 0, delta = 0;
        std::map<std::pair<std::uint32_t, std::uint32_t>, float> actorValues;
        std::set<std::uint32_t> deadActors;

        struct Attachment
        {
            std::uint32_t parent;
            ESM::Position childAtBind, parentAtBind;
            void checkpoint(Checkpoint& ar) { ar(parent, childAtBind, parentAtBind); }
        };
        std::map<std::uint32_t, Attachment> attachments;
        std::set<std::uint32_t> animationDriven;
        std::set<std::uint32_t> physicsDriven;
        std::map<std::uint32_t, int> motionTypes;

        struct Instance
        {
            std::string name;
            ScriptInstance vm;
            bool initialized = false, failed = false;
            std::uint32_t owner = 0, questOwner = 0;
        };
        using Instances = std::map<std::string, std::shared_ptr<Instance>>;
        struct Call
        {
            std::string script, function;
            std::vector<Value> args;
            void checkpoint(Checkpoint& ar) { ar(script, function, args); }
        };
        struct FragmentTask
        {
            std::uint32_t owner;
            std::shared_ptr<Instance> instance;
            Call call;
            std::shared_ptr<bool> finished;
            double due = 0;
            std::optional<std::uint64_t> continuation;
            std::shared_ptr<const bool> nativeWait;
        };
        std::deque<FragmentTask> tasks;
        Instances dialogueScripts;

        struct ActionState
        {
            std::uint32_t actor;
            std::optional<double> due;
            bool complete = false;
            std::vector<std::int32_t> recordings;
            void checkpoint(Checkpoint& ar) { ar(actor, due, complete, recordings); }
        };
        std::vector<WorldScriptFace> faces;
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> gazeOwners;
        struct PlayingScene
        {
            std::size_t phase = 0;
            enum Step { Begin, EnterPhase, Actions, LeavePhase, End } step = Begin;
            std::map<std::uint32_t, std::uint32_t> actors;
            std::map<std::uint32_t, ActionState> actions;
            Instances scripts;
            std::vector<std::shared_ptr<bool>> fragments;
            bool stop = false;
        };
        std::map<std::uint32_t, PlayingScene> playing;

        struct BowAttack
        {
            enum Phase { Equip, Draw, Release, Recover };
            ArrowLoadout weapon;
            std::uint32_t target = 0;
            Phase phase = Equip;
            bool animationDone = false, released = false;
            bool alwaysHit = false, repeat = false, noDamage = false, holdWhenBlocked = false;
            void checkpoint(Checkpoint& ar) { ar(weapon, target, phase, animationDone, released, alwaysHit, repeat, noDamage, holdWhenBlocked); }
        };
        struct FiredArrow
        {
            std::uint32_t actor, target, bow, projectile;
            float damage;
            double expires;
            void checkpoint(Checkpoint& ar) { ar(actor, target, bow, projectile, damage, expires); }
        };
        struct BowVolley
        {
            double resumeAt = 0;
            int shotsLeft = 1, barrages = 1, minShots = 1, maxShots = 1;
            float minPause = 0, maxPause = 0;
            bool pauses = false;
            void checkpoint(Checkpoint& ar) { ar(resumeAt, shotsLeft, barrages, minShots, maxShots, minPause, maxPause, pauses); }
        };
        std::map<std::uint32_t, BowVolley> bowVolleys;
        std::vector<WorldScriptArrow> arrows;
        std::map<std::uint32_t, FiredArrow> fired;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> arrowsUsed;
        std::map<std::pair<std::uint32_t, std::string>, std::pair<std::int32_t, bool>> taggedSounds;
        std::map<std::uint32_t, double> arrowVictims;
        std::uint32_t nextArrow = 1;

        struct RunningPackage
        {
            std::uint32_t id, owner, scene, action;
            const Package* tree;
            std::uint32_t mount = 0;
            Instances scripts;
            std::shared_ptr<bool> fragment;
            std::optional<CartRoute> route;
            // Preserve the authored approach when a new patrol branches from it.
            std::optional<CartRoute> approach;
            double approachDistance = 0;
            std::uint32_t approachMover = 0;
            std::size_t procedure = 0;
            double distance = 0, length = 0, wait = 0, speed = 0, turnSpeed = 0;
            std::optional<double> loopStart;
            bool prepared = false, ending = false, done = false, forceEnd = false;
            std::optional<BowAttack> bow;
        };
        std::map<std::uint32_t, RunningPackage> running;
        std::map<std::uint32_t, std::optional<BowAttack>> combatBows;
        struct BowPosition
        {
            std::uint32_t marker = 0;
            bool reached = false;
            void checkpoint(Checkpoint& ar) { ar(marker, reached); }
        };
        std::map<std::uint32_t, BowPosition> bowPositions;
        WorldScriptTransforms bowDestinations;
        std::vector<WorldScriptFiringPosition> firingRequests;
        // Original [Cart] fHorseOffsetX/Y/Z defaults (bhkCartTether initialization).
        osg::Vec3f cartHorseOffset{0.f, 200.f, 0.f};

        Impl(ReadWinningRecord reader, std::shared_ptr<const VFS::Manager> files, QuestScripts& questScripts, ErrorLog& errors)
            : read(std::move(reader)), vfs(std::move(files)), quests(questScripts), log(errors),
              aliases(questAliasResolver(read)), music(read, *vfs, quests, log)
        {
        }

        void checkpoint(Checkpoint& ar)
        {
            ar(authored, moved, relocations, movingReferences, exteriorCells, cellWorlds, loaded, fixedReferences,
                autostart, started, seen, reevaluate, explicitReevaluate, requestedScenes, finishedActions, events,
                animations, locomotion, audio, nextSound, triggerBoxes, triggerInside, triggerActors, teleported,
                triggerActorBounds, triggerActorPositions, triggerEvents, time, delta, actorValues, deadActors,
                attachments, animationDriven, physicsDriven, motionTypes, faces, gazeOwners, arrows, fired,
                arrowsUsed, taggedSounds, arrowVictims, nextArrow, cartHorseOffset);
            ar.random(soundRandom); music.checkpoint(ar);
            // Cache only record identities. SCEN/PACK definitions are re-read from the ordered plugin set.
            auto sceneIt = scenes.begin();
            ar.count(scenes.size(), [&](auto) { std::uint32_t id = ar.loading ? 0 : sceneIt++->first; ar(id); if (ar.loading) scene(id); });
            const auto instance = [&](std::shared_ptr<Instance>& ptr) {
                ar.shared(ptr, [&](auto current) {
                    std::string name = ar.loading ? "" : current->name;
                    std::uint32_t owner = ar.loading ? 0 : current->owner, quest = ar.loading ? 0 : current->questOwner;
                    ar(name, owner, quest);
                    if (ar.loading)
                    {
                        auto callbacks = quests.host();
                        callbacks.getOwningQuest = [this, owner, quest](auto id) { return id == owner ? quest : owningQuest(id); };
                        current = std::make_shared<Instance>(Instance{name, ScriptInstance(checkpointScript(*vfs, name), owner, {}, std::move(callbacks))});
                        current->owner = owner; current->questOwner = quest;
                    }
                    return current;
                }, [&](auto& current) { ar(current.vm, current.initialized, current.failed); });
            };
            const auto instances = [&](Instances& values) {
                auto it = values.begin();
                ar.count(values.size(), [&](auto) {
                    std::string name = ar.loading ? "" : it->first; ar(name);
                    auto& value = ar.loading ? values[name] : it++->second; instance(value);
                });
            };
            auto playingIt = playing.begin();
            ar.count(playing.size(), [&](auto) {
                std::uint32_t id = ar.loading ? 0 : playingIt->first; ar(id);
                auto& active = ar.loading ? playing[id] : playingIt++->second;
                ar(active.phase, active.step, active.actors, active.actions, active.fragments, active.stop);
                instances(active.scripts);
            });
            auto runningIt = running.begin();
            ar.count(running.size(), [&](auto) {
                std::uint32_t actor = ar.loading ? 0 : runningIt->first; ar(actor);
                auto& active = ar.loading ? running[actor] : runningIt++->second;
                ar(active.id, active.owner, active.scene, active.action, active.mount, active.fragment,
                    active.route, active.approach, active.approachDistance, active.approachMover,
                    active.procedure, active.distance, active.length, active.wait, active.speed, active.turnSpeed,
                    active.loopStart, active.prepared, active.ending, active.done, active.forceEnd, active.bow);
                std::uint32_t tree = ar.loading ? 0 : active.tree->record.metadata.formId; ar(tree);
                if (ar.loading) active.tree = &package(tree);
                instances(active.scripts);
            });
            ar.count(tasks.size(), [&](auto i) {
                if (ar.loading) tasks.push_back({}); auto& task = tasks[i];
                ar(task.owner, task.call, task.finished, task.due, task.continuation, task.nativeWait); instance(task.instance);
            });
        }

        Scene& scene(std::uint32_t id)
        {
            if (const auto found = scenes.find(id); found != scenes.end())
                return found->second;
            auto record = read(id);
            if (record.metadata.recordType != fourCC("SCEN"))
                throw std::runtime_error("Scene method requires a SCEN receiver");
            return scenes.try_emplace(id, std::move(record)).first->second;
        }

        Package& package(std::uint32_t id)
        {
            if (const auto found = packages.find(id); found != packages.end())
                return found->second;
            auto record = read(id);
            if (record.metadata.recordType != fourCC("PACK"))
                throw std::runtime_error("Expected a PACK record");
            return packages.try_emplace(id, std::move(record)).first->second;
        }

        std::uint32_t owningQuest(std::uint32_t id)
        {
            if (const auto found = scenes.find(id); found != scenes.end())
                return found->second.owner;
            for (const auto& [actor, active] : running)
                if (active.id == id)
                    return active.owner;
            const auto record = read(id);
            if (record.metadata.recordType == fourCC("SCEN"))
                return scene(id).owner;
            if (record.metadata.recordType == fourCC("PACK"))
                return package(id).owner;
            return 0;
        }

        std::optional<WorldScriptPose> authoredPlacement(std::uint32_t id) const
        {
            if (!id)
                return {};
            if (const auto found = authored.find(id); found != authored.end())
                return found->second;
            if (quests.inventoryReferences().contains(id))
                return {};
            if (const auto found = quests.createdReferences().find(id); found != quests.createdReferences().end())
                return authoredPlacement(found->second.anchor);
            if (id == 0x14)
                return {};
            const auto record = read(id);
            if (record.metadata.recordType != fourCC("ACHR") && record.metadata.recordType != fourCC("REFR"))
                return {};
            std::optional<WorldScriptPose> result;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag != fourCC("DATA"))
                    return;
                result.emplace();
                if (index)
                    if (const auto location = index->references.find(id); location != index->references.end())
                        result->cell = location->second.cell;
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    result->position.pos[axis] = real(bytes, 4 * axis);
                    result->position.rot[axis] = real(bytes, 12 + 4 * axis);
                }
            });
            return result;
        }

        std::optional<WorldScriptPose> placement(std::uint32_t id) const
        {
            if (const auto found = moved.find(id); found != moved.end())
                return found->second;
            return authoredPlacement(id);
        }

        std::uint32_t exteriorCell(std::uint32_t id, const WorldScriptPose& pose) const
        {
            std::uint32_t world = 0;
            if (const auto known = cellWorlds.find(pose.cell); known != cellWorlds.end())
                world = known->second;
            else if (index)
                if (const auto location = index->references.find(id); location != index->references.end()
                    && location->second.cell == pose.cell)
                    world = location->second.world;
            if (!world)
                return pose.cell;
            const auto x = std::floor(pose.position.pos[0] / LandTile::size);
            const auto y = std::floor(pose.position.pos[1] / LandTile::size);
            if (!(x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767))
                return pose.cell;
            const auto cell = exteriorCells.find({world, static_cast<int>(x), static_cast<int>(y)});
            return cell == exteriorCells.end() ? pose.cell : cell->second;
        }

        void setExteriorCells(std::uint32_t world, std::span<const ExteriorCell> cells)
        {
            if (!world)
                return;
            for (const auto& cell : cells)
            {
                exteriorCells.insert_or_assign({world, cell.x, cell.y}, cell.cellId);
                cellWorlds.insert_or_assign(cell.cellId, world);
            }
            for (auto& [id, pose] : moved)
                pose.cell = exteriorCell(id, pose);
        }

        void move(std::uint32_t id, WorldScriptPose pose, std::uint32_t anchor = 0)
        {
            pose.cell = exteriorCell(id, pose);
            moved.insert_or_assign(id, pose);
            if (id == 0x14 || quests.createdReferences().contains(id))
                return;
            if (auto found = movingReferences.find(id); found != movingReferences.end())
            {
                if (anchor)
                    found->second.anchor = anchor;
                return;
            }
            const auto record = read(id);
            if (record.metadata.recordType != fourCC("REFR") && record.metadata.recordType != fourCC("ACHR"))
                return;
            WorldScriptMovingReference ref;
            ref.anchor = anchor;
            ref.recordType = record.metadata.recordType;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("NAME"))
                    ref.base = record.mapFormId(integer(bytes));
                else if (tag == fourCC("XSCL"))
                    ref.scale = real(bytes);
            });
            if (ref.base)
                movingReferences.emplace(id, ref);
        }

        std::uint32_t linked(std::uint32_t id, std::uint32_t keyword = 0)
        {
            if (!id || id == 0x14 || quests.createdReferences().contains(id))
                return 0;
            const auto record = read(id);
            std::uint32_t result = 0;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag != fourCC("XLKR")) return;
                if (bytes.size() == 4 && !keyword)
                    result = record.mapFormId(integer(bytes));
                else if (bytes.size() == 8 && record.mapFormId(integer(bytes)) == keyword)
                    result = record.mapFormId(integer(bytes, 4));
            });
            return result;
        }

        std::uint32_t actorBase(std::uint32_t id) const
        {
            if (id == 0x14)
                return 7;
            if (const auto found = quests.createdReferences().find(id); found != quests.createdReferences().end())
                return found->second.actor ? found->second.base : 0;
            const auto record = read(id);
            std::uint32_t result = 0;
            if (record.metadata.recordType == fourCC("ACHR"))
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("NAME"))
                        result = record.mapFormId(integer(bytes));
                });
            return result;
        }

        bool hasKeyword(std::uint32_t id, std::uint32_t keyword)
        {
            if (const auto base = actorBase(id)) id = base;
            const auto record = read(id);
            bool found = false;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("KWDA"))
                    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4)
                        found |= record.mapFormId(integer(bytes, i)) == keyword;
            });
            return found;
        }

        std::string editorId(std::uint32_t id)
        {
            std::string name;
            visitSubrecords(read(id), [&](auto tag, Bytes bytes) { if (tag == fourCC("EDID")) name = string(bytes); });
            return name;
        }

        bool isFurniture(std::uint32_t reference)
        {
            const auto ref = read(reference);
            std::uint32_t base = 0;
            visitSubrecords(ref, [&](auto tag, Bytes bytes) { if (tag == fourCC("NAME")) base = ref.mapFormId(integer(bytes)); });
            return base && read(base).metadata.recordType == fourCC("FURN");
        }

        WorldScriptPose furniturePose(std::uint32_t actor, std::uint32_t reference)
        {
            auto pose = placement(reference);
            if (!pose) throw std::runtime_error("Furniture has no placed transform");
            const auto ref = read(reference);
            std::uint32_t base = 0;
            float scale = 1;
            visitSubrecords(ref, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("NAME")) base = ref.mapFormId(integer(bytes));
                if (tag == fourCC("XSCL")) scale = real(bytes);
            });
            const auto record = read(base);
            if (record.metadata.recordType != fourCC("FURN")) return *pose;
            std::string model;
            std::map<std::uint32_t, std::uint32_t> requirements;
            std::uint32_t slot = 0;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("MODL")) model = string(bytes);
                if (tag == fourCC("ENAM")) slot = integer(bytes);
                if (tag == fourCC("FNMK")) requirements[slot] = record.mapFormId(integer(bytes));
            });
            auto found = furnitureMarkers.find(base);
            if (found == furnitureMarkers.end())
            {
                const VFS::Path::Normalized path("meshes/" + model);
                Nif::NIFFile file(path);
                Nif::Reader(file, nullptr).parse(vfs->get(path));
                std::vector<Nif::BSFurnitureMarker::FurniturePosition> markers;
                for (const auto& block : file.mRecords)
                    if (const auto* positions = dynamic_cast<const Nif::BSFurnitureMarker*>(block.get()))
                        markers.insert(markers.end(), positions->mMarkers.begin(), positions->mMarkers.end());
                found = furnitureMarkers.emplace(base, std::move(markers)).first;
            }
            const auto& markers = found->second;
            std::optional<std::size_t> chosen;
            for (std::size_t i = 0; i < markers.size(); ++i)
                if (const auto requirement = requirements.find(i); requirement != requirements.end())
                {
                    if (hasKeyword(actor, requirement->second)) { chosen = i; break; }
                }
                else if (!chosen) chosen = i;
            if (!chosen) throw std::runtime_error("Furniture has no compatible marker for actor");
            const auto& seat = markers[*chosen];
            pose->position = furnitureMarkerPose(pose->position, seat.mOffset, seat.mHeading, scale);
            return *pose;
        }

        bool isTowerPerch(const RecordPayload& furniture)
        {
            bool tower = false;
            if (furniture.metadata.recordType == fourCC("FURN"))
                visitSubrecords(furniture, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("MODL"))
                        tower = Misc::StringUtils::ciEqual(std::filesystem::path(modelAssetPath(string(bytes))).stem().string(), "MarkerTowerIdle");
                });
            return tower;
        }

        bool isTowerPerch(std::uint32_t marker)
        {
            if (!marker)
                return false;
            const auto ref = read(marker);
            bool tower = false;
            visitSubrecords(ref, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("NAME"))
                    if (const auto base = ref.mapFormId(integer(bytes)); base)
                        tower = isTowerPerch(read(base));
            });
            return tower;
        }

        bool flyingFurniture(std::uint32_t actor, std::uint32_t marker)
        {
            if (flightProfile(actor).behavior.empty()) return false;
            if (isTowerPerch(marker)) return true;
            const auto host = quests.host();
            const auto animation = host.callFormMethod
                ? host.callFormMethod(marker, "HasFurnitureAnimation", {std::string("Open")}) : std::nullopt;
            return animation && std::get<bool>(*animation);
        }

        void enterFurniture(std::uint32_t actor, std::uint32_t marker)
        {
            const auto ref = read(marker);
            std::uint32_t base = 0;
            visitSubrecords(ref, [&](auto tag, Bytes bytes) { if (tag == fourCC("NAME")) base = ref.mapFormId(integer(bytes)); });
            const auto record = read(base);
            if (record.metadata.recordType == fourCC("IDLM"))
            {
                if (!idle(actor, idleMarkerAnimation(marker))) throw std::runtime_error("Sit has no idle marker animation");
                return;
            }
            if (record.metadata.recordType != fourCC("FURN")) throw std::runtime_error("Sit requires furniture");
            const auto type = editorId(base);
            std::string event, behavior = "Actors/Character/Behaviors/0_Master.hkx";
            if (flyingFurniture(actor, marker))
            {
                if (const auto occupied = occupiedPerches.find(actor); occupied != occupiedPerches.end() && occupied->second == marker)
                    return;
                occupiedPerches.insert_or_assign(actor, marker);
                event = "EnterPerchTowerInstant";
                behavior = flightProfile(actor).behavior;
            }
            else if (Misc::StringUtils::ciEqual(type, "WallCornerPeekL")) event = "IdleWallCornerPeekLEnter";
            else if (Misc::StringUtils::ciEqual(type, "KneelingMarker")) event = "IdleKneelingEnter";
            else if (Misc::StringUtils::ciEqual(type, "HadvarWriteLedger")) event = "IdleWriteLedgerChargenEnter";
            else if (Misc::StringUtils::ciEqual(type, "RailLeanMarker")) event = "IdleRailLeanEnter";
            else if (Misc::StringUtils::ciEqual(type, "SitCrossLeggedMarker")) event = "IdleSitCrossLeggedEnter";
            else if (Misc::StringUtils::ciEqual(type, "Wounded01FloorMarker")) event = "IdleWounded_01";
            else if (Misc::StringUtils::ciEqual(type, "Wounded02FloorMarker")) event = "IdleWounded_02";
            else if (Misc::StringUtils::ciEqual(type, "Wounded03FloorMarker")) event = "IdleWounded_03";
            else if (Misc::StringUtils::ciEqual(type, "ExecutionerChoppingBlock"))
            {
                event = "IdleExecutioneeIdle";
                const auto npc = read(actorBase(actor));
                visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("KWDA"))
                        for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4)
                        {
                            const auto keyword = editorId(npc.mapFormId(integer(bytes, i)));
                            if (Misc::StringUtils::ciEqual(keyword, "isExecutioner")) event = "IdleExecutionerIdle";
                            if (Misc::StringUtils::ciEqual(keyword, "isExecutionerGuard")) event = "IdleExecutionGuardIdle";
                        }
                });
            }
            else throw std::runtime_error("Unsupported Sit furniture " + type);
            // The rail-lean interaction fits an adult to the marker's body size.
            // The ledger and other furniture retain the actor's authored scale.
            if (Misc::StringUtils::ciEqual(type, "RailLeanMarker")) normalizedFurnitureActors.insert(actor);
            else normalizedFurnitureActors.erase(actor);
            locomotion.erase(actor);
            if (actor == 0x14 && event == "IdleExecutioneeIdle")
            {
                enteringExecutionBlock.insert_or_assign(actor, marker);
                animations.push_back({actor, 0, {}, "T5Execution", "actors/character/animations/aoexecutioneeenter.hkx"});
                return;
            }
            animations.push_back({actor, 0, behavior, event});
            furnitureActivations.emplace_back(marker, actor);
        }

        std::uint32_t idleMarkerAnimation(std::uint32_t marker)
        {
            const auto ref = read(marker);
            if (ref.metadata.recordType != fourCC("REFR"))
                throw std::runtime_error("UseIdleMarker requires a placed idle marker");
            std::uint32_t base = 0;
            visitSubrecords(ref, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("NAME"))
                    base = ref.mapFormId(integer(bytes));
            });
            const auto data = read(base);
            if (data.metadata.recordType != fourCC("IDLM"))
                throw std::runtime_error("UseIdleMarker target does not use an IDLM base");
            std::vector<std::uint32_t> markerIdles;
            bool random = false;
            visitSubrecords(data, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("IDLF"))
                    random = (integer(bytes, 0, 1) & 1) == 0;
                else if (tag == fourCC("IDLA"))
                    for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
                        if (const auto form = data.mapFormId(integer(bytes, offset)); form)
                            markerIdles.push_back(form);
            });
            if (markerIdles.empty())
                return 0;
            return random ? markerIdles[std::uniform_int_distribution<std::size_t>(0, markerIdles.size() - 1)(soundRandom)]
                          : markerIdles.front();
        }

        std::uint32_t packageMount(std::uint32_t actor)
        {
            if (actor == 0x14 || quests.createdReferences().contains(actor))
                return 0;
            const auto record = read(actor);
            std::uint32_t horse = 0;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("XHOR"))
                    horse = record.mapFormId(integer(bytes));
            });
            if (!horse || !loaded.contains(horse) || !actorBase(horse))
                return 0;
            const auto host = quests.host();
            if (host.isDisabled && host.isDisabled(horse))
                return 0;
            return horse;
        }

        void releasePackageMount(std::uint32_t actor, const RunningPackage& active)
        {
            if (shouts.erase(actor))
            {
                stopCastSounds(actor);
                faces.push_back({0, actor, 0});
            }
            landingPerches.erase(actor);
            if (const auto grab = flightGrabs.extract(actor); !grab.empty())
            {
                const auto bound = attachments.find(grab.mapped().target);
                if (bound != attachments.end() && bound->second.parent == actor)
                    attachments.erase(bound);
            }
            carrierVelocities.erase(actor);
            if (active.bow)
            {
                arrows.push_back({WorldScriptArrow::Lower, actor});
                std::erase_if(taggedSounds, [&](const auto& sound) {
                    if (sound.first.first != actor)
                        return false;
                    if (sound.second.second)
                        audio.push_back({WorldScriptAudio::Stop, sound.second.first});
                    return true;
                });
            }
            if (active.mount)
                if (const auto bound = attachments.find(actor);
                    bound != attachments.end() && bound->second.parent == active.mount)
                    attachments.erase(bound);
        }

        const FlightProfile& flightProfile(std::uint32_t actor)
        {
            const auto base = actorBase(actor);
            if (const auto profile = flightProfiles.find(base); profile != flightProfiles.end())
                return profile->second;
            FlightProfile profile;
            std::uint32_t race = 0;
            bool female = false;
            if (base)
            {
                const auto npc = read(base);
                visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("RNAM")) race = npc.mapFormId(integer(bytes));
                    if (tag == fourCC("ACBS")) female = (integer(bytes) & 1) != 0;
                });
            }
            if (race)
            {
                const auto record = read(race);
                std::vector<std::string> projects;
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("FLMV")) profile.movement = record.mapFormId(integer(bytes));
                    if (tag == fourCC("MODL"))
                    {
                        auto path = modelAssetPath(string(bytes));
                        if (path.ends_with(".hkx")) projects.push_back(std::move(path));
                    }
                });
                if (profile.movement && !projects.empty())
                    profile.behavior = hkxBehaviorPath(projects[female && projects.size() > 1 ? 1 : 0],
                        [&](const std::string& path) {
                            const auto stream = vfs->get(VFS::Path::Normalized(path));
                            return std::string(std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>{});
                        });
            }
            return flightProfiles.emplace(base, std::move(profile)).first->second;
        }

        void playFlightEvent(std::uint32_t actor, std::string_view event)
        {
            const auto& profile = flightProfile(actor);
            if (profile.behavior.empty())
                throw std::runtime_error("Flying actor's RACE project has no behavior graph");
            animations.push_back({actor, 0, profile.behavior, std::string(event)});
        }

        float movementSpeed(std::uint32_t actor, std::uint8_t preferred, bool angular, bool allowFlight)
        {
            if (preferred > 3)
                throw std::runtime_error("Invalid PACK preferred speed");
            const auto base = actorBase(actor);
            const auto key = std::tuple{base, preferred, angular, allowFlight};
            if (const auto found = speeds.find(key); found != speeds.end())
                return found->second;
            std::uint32_t race = 0, walk = 0, run = 0, flight = 0;
            bool female = false;
            if (base)
            {
                const auto npc = read(base);
                visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("RNAM"))
                        race = npc.mapFormId(integer(bytes));
                    else if (tag == fourCC("ACBS"))
                        female = (integer(bytes) & 1) != 0;
                });
            }
            if (race)
            {
                const auto record = read(race);
                std::vector<std::string> projects;
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("WKMV"))
                        walk = record.mapFormId(integer(bytes));
                    else if (tag == fourCC("RNMV"))
                        run = record.mapFormId(integer(bytes));
                    else if (tag == fourCC("FLMV"))
                        flight = record.mapFormId(integer(bytes));
                    else if (tag == fourCC("MODL"))
                    {
                        auto path = modelAssetPath(string(bytes));
                        if (path.ends_with(".hkx")) projects.push_back(std::move(path));
                    }
                });
                if ((!walk || !run) && index && vfs && !projects.empty())
                {
                    const auto& project = projects[female && projects.size() > 1 ? 1 : 0];
                    try
                    {
                        const auto names = hkxLocomotionTypes(project, [&](const std::string& path) {
                            const auto stream = vfs->get(VFS::Path::Normalized(path));
                            return std::string(std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>{});
                        });
                        const auto mapped = [&](const std::string& name) {
                            const auto found = index->movementTypesByName.find(name);
                            return found == index->movementTypesByName.end() ? 0u : found->second;
                        };
                        if (!walk) walk = mapped(names[0]);
                        if (!run) run = mapped(names[1]);
                    }
                    catch (const std::exception& error) { log.write("PACK movement " + project, error.what()); }
                }
            }
            if (!walk || !run)
            {
                if (!index)
                    throw std::runtime_error("PACK movement needs the DOBJ default object index");
                const auto getDefault = [&](std::uint32_t key) {
                    const auto found = index->defaultObjects.find(key);
                    return found == index->defaultObjects.end() ? 0u : found->second;
                };
                if (!walk)
                    walk = getDefault(fourCC("DMWL"));
                if (!run)
                    run = getDefault(fourCC("DMRN"));
            }
            const auto speed = [&](std::uint32_t id, std::size_t offset) {
                if (!id)
                    throw std::runtime_error("PACK has no RACE or DOBJ movement type");
                const auto movement = read(id);
                float value = 0;
                visitSubrecords(movement, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("SPED"))
                        value = real(bytes, offset == 40 && bytes.size() == 40 ? 36 : offset);
                });
                if (!std::isfinite(value) || value <= 0)
                    throw std::runtime_error("PACK movement type has no positive authored speed");
                return value;
            };
            const std::size_t walkOffset = angular ? 32 : 16;
            const std::size_t runOffset = angular ? 40 : 20;
            if (allowFlight && flight)
                walk = run = flight;
            float result = 0;
            if (preferred == 0)
                result = speed(walk, walkOffset);
            else if (preferred == 2)
                result = speed(run, runOffset);
            else
            {
                if (!index)
                    throw std::runtime_error("PACK speed interpolation needs the winning GMST index");
                const auto name = preferred == 1 ? "fjoginterpolationbetweenwalkandrun" : "ffastwalkinterpolationbetweenwalkandrun";
                const auto setting = index->floatSettings.find(name);
                if (setting == index->floatSettings.end() || !std::isfinite(setting->second))
                    throw std::runtime_error("Missing PACK movement setting " + std::string(name));
                result = std::lerp(speed(walk, walkOffset), speed(run, runOffset), std::clamp(setting->second, 0.f, 1.f));
            }
            return speeds.emplace(key, result).first->second;
        }

        float visualScale(std::uint32_t actor)
        {
            if (!normalizedVehicleActors.contains(actor) && !normalizedFurnitureActors.contains(actor)) return 1;
            if (actor != 0x14)
                if (const auto found = visualScaleCache.find(actor); found != visualScaleCache.end()) return found->second;
            const auto base = actorBase(actor);
            const auto size = base ? actorBodySize(read, base) : ActorBodySize{};
            if (size.child)
            {
                if (actor != 0x14) visualScaleCache.insert_or_assign(actor, 1.f);
                return 1.f;
            }
            float scale = size.height;
            if (actor != 0x14 && !quests.createdReferences().contains(actor))
                visitSubrecords(read(actor), [&](auto tag, Bytes bytes) { if (tag == fourCC("XSCL")) scale *= real(bytes); });
            const auto correction = std::isfinite(scale) && scale > 0 ? 1.f / scale : 1.f;
            if (actor != 0x14) visualScaleCache.insert_or_assign(actor, correction);
            return correction;
        }

        bool idle(std::uint32_t actor, std::uint32_t id)
        {
            if (!id || !actor)
                return false;
            const auto record = read(id);
            if (record.metadata.recordType != fourCC("IDLE"))
                throw std::runtime_error("Actor.PlayIdle expects an IDLE record");
            WorldScriptAnimation command{actor, id};
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("DNAM"))
                    command.behavior = string(bytes);
                else if (tag == fourCC("ENAM"))
                    command.event = string(bytes);
            });
            if (command.event.empty())
                return false;
            locomotion.erase(actor);
            animations.push_back(std::move(command));
            return true;
        }

        void playLocomotion(std::uint32_t actor, std::string_view gait, float playbackSpeed = 1.f)
        {
            normalizedFurnitureActors.erase(actor);
            occupiedPerches.erase(actor);
            enteringExecutionBlock.erase(actor);
            const auto [current, inserted] = locomotion.try_emplace(actor, gait);
            if (!inserted && current->second == gait)
                return;
            current->second = gait;
            if (gait == "BHR_Default::FlyStartCruise")
                playFlightEvent(actor, gait);
            else
                animations.push_back({actor, 0, {}, std::string(gait), {}, playbackSpeed});
        }

        void stopLocomotion(std::uint32_t actor)
        {
            if (locomotion.contains(actor))
                playLocomotion(actor, "T5Idle");
        }

        std::optional<Value> getFormProperty(std::uint32_t, std::string_view name) const
        {
            const auto property = Misc::StringUtils::lowerCase(name);
            static constexpr std::string_view names[] = {"motion_dynamic", "motion_sphereintertia",
                "motion_boxintertia", "motion_keyframed", "motion_fixed", "motion_thinboxintertia",
                "motion_character"};
            if (const auto found = std::ranges::find(names, property); found != std::end(names))
                return Value(static_cast<std::int32_t>(found - std::begin(names) + 1));
            return std::nullopt;
        }

        void attach(std::uint32_t child, std::uint32_t parent, bool mounted = false)
        {
            if (!parent)
            {
                attachments.erase(child);
                return;
            }
            for (auto id = parent; id;)
            {
                if (id == child)
                    throw std::runtime_error("Vehicle attachment would create a cycle");
                const auto next = attachments.find(id);
                id = next == attachments.end() ? 0 : next->second.parent;
            }
            const auto childPose = placement(child);
            const auto parentPose = placement(parent);
            if (!childPose || !parentPose)
                throw std::runtime_error("Vehicle attachment requires world transforms for both references");
            attachments.insert_or_assign(child,
                Attachment{parent, mounted ? parentPose->position : childPose->position, parentPose->position});
        }

        void updateAttachments()
        {
            std::set<std::uint32_t> done;
            const auto update = [&](auto&& self, std::uint32_t id) -> void {
                const auto found = attachments.find(id);
                if (found == attachments.end() || animationDriven.contains(id) || physicsDriven.contains(id) || fixedReferences.contains(id) || !done.insert(id).second)
                    return;
                const auto& link = found->second;
                self(self, link.parent);
                auto pose = placement(link.parent);
                if (!pose)
                    return;
                const auto& old = link.parentAtBind;
                const auto& child = link.childAtBind;
                const float yaw = old.rot[2] - pose->position.rot[2];
                const float x = child.pos[0] - old.pos[0], y = child.pos[1] - old.pos[1];
                pose->position.pos[0] += x * std::cos(yaw) - y * std::sin(yaw);
                pose->position.pos[1] += x * std::sin(yaw) + y * std::cos(yaw);
                pose->position.pos[2] += child.pos[2] - old.pos[2];
                for (unsigned i = 0; i < 3; ++i)
                    pose->position.rot[i] += child.rot[i] - old.rot[i];
                move(id, *pose);
            };
            for (const auto& [id, attachment] : attachments)
                update(update, id);
        }

        void setTriggers(std::span<const PreparedSceneScript> scripts, const std::map<std::uint32_t, std::uint32_t>& actors,
            const std::map<std::uint32_t, osg::BoundingBoxd>& bounds)
        {
            std::set<std::uint32_t> seenTriggers;
            auto oldBoxes = std::exchange(triggerBoxes, {});
            for (const auto& script : scripts)
            {
                if (!loaded.contains(script.owner) || seenTriggers.contains(script.owner))
                    continue;
                const auto& states = script.script->states;
                const auto handlesTrigger = [](const Papyrus::Function& function) {
                    return Misc::StringUtils::ciEqual(function.name, "OnTriggerEnter")
                        || Misc::StringUtils::ciEqual(function.name, "OnTriggerLeave");
                };
                if (!std::ranges::any_of(states, [&](const auto& state) {
                        return std::ranges::any_of(state.functions, handlesTrigger);
                    }))
                    continue;
                seenTriggers.insert(script.owner);
                if (const auto old = oldBoxes.find(script.owner); old != oldBoxes.end())
                {
                    triggerBoxes.emplace(script.owner, old->second);
                    continue;
                }
                try
                {
                    const auto pose = authoredPlacement(script.owner);
                    if (!pose)
                        continue;
                    visitSubrecords(read(script.owner), [&](auto tag, Bytes bytes) {
                        if (tag != fourCC("XPRM") || bytes.size() != 32 || integer(bytes, 28) != 1)
                            return;
                        const osg::Vec3d bounds{real(bytes, 0), real(bytes, 4), real(bytes, 8)};
                        if (bounds.x() <= 0 || bounds.y() <= 0 || bounds.z() <= 0)
                            throw std::runtime_error("XPRM trigger box has non-positive bounds");
                        const auto transform = osg::Matrixd::rotate(Misc::Convert::makeOsgQuat(pose->position))
                            * osg::Matrixd::translate(pose->position.asVec3());
                        triggerBoxes.emplace(script.owner, WorldScriptTriggerBox{
                            script.owner, osg::Matrixd::inverse(transform), bounds});
                    });
                }
                catch (const std::exception& error)
                {
                    log.write("Trigger " + std::to_string(script.owner), error.what());
                }
            }
            triggerActors.clear();
            triggerActorBounds = bounds;
            if (loaded.contains(0x14))
                triggerActors.insert(0x14);
            for (const auto& [id, base] : actors)
                if (loaded.contains(id))
                    triggerActors.insert(id);
            std::erase_if(triggerActorPositions, [&](const auto& entry) { return !triggerActors.contains(entry.first); });
            std::erase_if(triggerInside, [&](const auto& entry) { return !triggerBoxes.contains(entry.first); });
            for (auto& [id, inside] : triggerInside)
                std::erase_if(inside, [&](auto actor) { return !triggerActors.contains(actor); });
        }

        void tickTriggers()
        {
            if (triggerBoxes.empty())
                return;
            const auto host = quests.host();
            std::vector<std::uint32_t> enabled;
            for (const auto& [id, box] : triggerBoxes)
                if (!host.isDisabled || !host.isDisabled(id))
                    enabled.push_back(id);
                else
                    triggerInside.erase(id);
            for (const auto actor : triggerActors)
            {
                const auto pose = placement(actor);
                if (!pose)
                    continue;
                auto current = osg::Vec3d(pose->position.asVec3());
                osg::Matrixd rotation;
                osg::Vec3d half;
                if (const auto body = triggerActorBounds.find(actor); body != triggerActorBounds.end()
                    && !attachments.contains(actor))
                {
                    rotation = osg::Matrixd::rotate(Misc::Convert::makeOsgQuat(pose->position));
                    const auto scale = visualScale(actor);
                    current += body->second.center() * scale * rotation;
                    half = (body->second._max - body->second._min) * (.5 * scale);
                }
                const auto previous = triggerActorPositions.find(actor);
                const auto from = previous != triggerActorPositions.end() && !teleported.contains(actor)
                    ? previous->second : current;
                const bool isDisabled = actor != 0x14 && host.isDisabled && host.isDisabled(actor);
                for (auto id : enabled)
                {
                    const auto& box = triggerBoxes.at(id);
                    auto& inside = triggerInside[id];
                    const bool wasInside = inside.contains(actor);
                    osg::Vec3d padding;
                    if (half.length2() > 0)
                    {
                        const auto bodyToBox = rotation * box.worldToBox;
                        for (unsigned axis = 0; axis < 3; ++axis)
                            for (unsigned i = 0; i < 3; ++i)
                                padding[axis] += std::abs(bodyToBox(i, axis)) * half[i];
                    }
                    const bool nowInside = !isDisabled && box.intersects(current, current, padding);
                    const bool passed = !isDisabled && !wasInside && !nowInside && from != current
                        && box.intersects(from, current, padding);
                    if ((!wasInside && nowInside) || passed)
                        triggerEvents.push_back({id, actor, true});
                    if ((wasInside && !nowInside) || passed)
                        triggerEvents.push_back({id, actor, false});
                    if (nowInside)
                        inside.insert(actor);
                    else
                        inside.erase(actor);
                }
                triggerActorPositions.insert_or_assign(actor, current);
            }
            teleported.clear();
        }

        std::shared_ptr<Instance> script(Instances& instances, const RecordPayload& record, const Vmad& vmad,
            std::string_view name, std::uint32_t owner)
        {
            const auto key = Misc::StringUtils::lowerCase(name);
            if (const auto found = instances.find(key); found != instances.end())
                return found->second;
            if (key.empty() || !std::ranges::all_of(key, [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                }))
                throw std::runtime_error("Invalid SCEN/PACK PEX name");
            auto& file = pex[key];
            if (!file)
                file = std::make_shared<PexFile>(readPex(*vfs->get(VFS::Path::Normalized("scripts/" + key + ".pex"))));
            const auto definition = std::ranges::find_if(file->scripts,
                [&](const auto& value) { return Misc::StringUtils::ciEqual(value.name, key); });
            if (definition == file->scripts.end())
                throw std::runtime_error("SCEN/PACK PEX class does not match its VMAD name");
            RecordAttachment source;
            source.resolveFormId = record.mapFormId;
            ScriptBindings bindings;
            applyVmadTable(bindings, vmad.scripts, source, key, true);
            const auto binding = bindings.find(key);
            auto properties = binding == bindings.end() ? std::map<std::string, Value>{}
                                                        : resolveProperties(binding->second, aliases);
            auto callbacks = quests.host();
            callbacks.getOwningQuest = [this, form = record.metadata.formId, owner](auto id) {
                return id == form ? owner : owningQuest(id);
            };
            auto instance = std::make_shared<Instance>(Instance{definition->name,
                ScriptInstance(std::shared_ptr<const Script>(file, &*definition), record.metadata.formId,
                    properties, std::move(callbacks))});
            instance->owner = record.metadata.formId; instance->questOwner = owner;
            return instances.emplace(key, std::move(instance)).first->second;
        }

        std::shared_ptr<bool> queue(
            Instances& instances, const RecordPayload& record, const Vmad& vmad, std::uint32_t owner, Call call)
        {
            if (call.function.empty())
                return {};
            auto instance = script(instances, record, vmad, call.script, owner);
            if (!instance->vm.hasFunction(call.function) && record.metadata.recordType == fourCC("PACK"))
            {
                const auto& file = pex.at(Misc::StringUtils::lowerCase(call.script));
                const auto& definition = *std::ranges::find_if(file->scripts,
                    [&](const auto& candidate) { return Misc::StringUtils::ciEqual(candidate.name, call.script); });
                // The CK can leave a PACK VMAD event pointing at a generated
                // class containing only the two state accessors.
                const bool generatedStub = std::ranges::all_of(definition.states, [](const auto& state) {
                    return std::ranges::all_of(state.functions, [](const auto& function) {
                        return Misc::StringUtils::ciEqual(function.name, "GetState")
                            || Misc::StringUtils::ciEqual(function.name, "GotoState");
                    });
                });
                if (generatedStub)
                    return {};
            }
            auto finished = std::make_shared<bool>(false);
            tasks.push_back({record.metadata.formId, std::move(instance), std::move(call), finished});
            return finished;
        }

        bool advanceFragment(FragmentTask& task)
        {
            auto& instance = *task.instance;
            if (instance.failed)
                return true;
            if (task.due > time || (task.nativeWait && !*task.nativeWait))
                return false;
            const bool initialize = !instance.initialized;
            const auto result = task.continuation ? instance.vm.resume(*task.continuation)
                : initialize ? instance.vm.startInitialization() : instance.vm.startEvent(task.call.function, task.call.args);
            if (result.waitSeconds || (result.completed && result.completed->eventExecuted))
                events.push_back({task.owner, instance.name, initialize ? "OnInit" : task.call.function,
                    result.waitSeconds ? result.waitTraces : result.completed->traces, time, result.waitSeconds, 0});
            if (result.waitSeconds)
            {
                task.due = time + *result.waitSeconds;
                task.continuation = result.continuation;
                task.nativeWait = result.nativeWait;
                return false;
            }
            task.continuation.reset();
            task.nativeWait.reset();
            if (initialize)
            {
                instance.initialized = true;
                return advanceFragment(task);
            }
            if (!result.completed || !result.completed->eventExecuted)
                throw std::runtime_error("Fragment function is missing from its PEX");
            return true;
        }

        void tickFragments()
        {
            const auto count = std::min<std::size_t>(tasks.size(), 128);
            std::set<const Instance*> active;
            for (std::size_t i = 0; i < count; ++i)
            {
                auto task = std::move(tasks.front());
                tasks.pop_front();
                try
                {
                    if (!active.insert(task.instance.get()).second || !advanceFragment(task))
                    {
                        tasks.push_back(std::move(task));
                        continue;
                    }
                }
                catch (const std::exception& error)
                {
                    task.instance->failed = true;
                    log.write(task.instance->name + " " + task.call.function, error.what());
                }
                *task.finished = true;
            }
        }

        std::uint32_t destination(const Target& target, std::uint32_t actor, std::uint32_t owner)
        {
            if (target.type == 0)
                return target.value;
            if ((!target.location && target.type == 4) || (target.location && target.type == 8))
                return quests.referenceAlias(owner, target.value);
            if ((!target.location && target.type == 3) || (target.location && target.type == 6))
                return linked(actor, target.value);
            if ((!target.location && target.type == 6)
                || (target.location && (target.type == 2 || target.type == 3 || target.type == 12)))
                return actor;
            throw std::runtime_error("Unsupported PACK target selector " + std::to_string(target.type));
        }

        const Input* input(const Package& selected, const Package& tree, const Procedure& procedure, std::size_t argumentIndex)
        {
            if (argumentIndex >= procedure.inputs.size())
                return nullptr;
            const auto key = procedure.inputs[argumentIndex];
            if (const auto found = selected.inputs.find(key); found != selected.inputs.end())
                return &found->second;
            if (const auto found = tree.inputs.find(key); found != tree.inputs.end())
                return &found->second;
            return nullptr;
        }

        const Package* selectPackage(std::span<const std::uint32_t> candidates, std::uint32_t actor, std::uint32_t owner)
        {
            for (auto id : candidates)
            {
                if (!id)
                    continue;
                const auto& selected = package(id);
                if (!selected.anySchedule)
                    throw std::runtime_error("Scheduled PACK needs an in-game calendar");
                const auto quest = selected.owner ? selected.owner : owner ? owner : quests.packageQuestForActor(actor, id);
                if (quests.testConditions(quest, selected.record, selected.conditions, actor))
                    return &selected;
            }
            return nullptr;
        }

        std::shared_ptr<bool> packageFragment(RunningPackage& active, std::uint32_t actor,
            const std::string& function, std::uint32_t animation)
        {
            if (animation)
                idle(actor, animation);
            const auto& selected = package(active.id);
            return queue(active.scripts, selected.record, selected.vmad.common, active.owner,
                {selected.vmad.fragmentFile, function, {FormReference{actor}}});
        }

        void finishPackage(std::uint32_t actor, RunningPackage& active)
        {
            if (active.scene)
                finishedActions[active.scene].insert(active.action);
            active.done = true;
            stopLocomotion(active.mount ? active.mount : actor);
            releasePackageMount(actor, active);
            if (!active.scene)
                reevaluate.insert(actor);
            events.push_back({active.id, "PACK", "Completed", {}, time, {}, 0});
        }

        void startPackage(std::uint32_t actor, const Package& selected, std::uint32_t owner,
            std::uint32_t sceneId = 0, std::uint32_t actionId = 0)
        {
            const auto id = selected.record.metadata.formId;
            const auto& tree = selected.templateId ? package(selected.templateId) : selected;
            if (tree.otherBranches || tree.procedures.empty())
                throw std::runtime_error("Unsupported PACK procedure tree for FormID " + std::to_string(id));
            if (tree.inventoryWeapon)
            {
                const auto& attack = tree.procedures.back();
                const auto* weapon = input(selected, tree, attack, 1);
                if (!weapon || !weapon->target || (weapon->target->type != 0
                    && (weapon->target->type != 2 || weapon->target->value != 20)))
                    throw std::runtime_error("UseWeapon search/acquisition requires an inventory bow selector");
            }
            std::optional<CartRoute> approach;
            double approachDistance = 0;
            std::uint32_t approachMover = 0;
            if (auto found = running.find(actor); found != running.end())
            {
                auto& previous = found->second;
                if (previous.id == id && previous.scene == sceneId && previous.action == actionId)
                    return;
                if (!previous.done)
                {
                    const auto& before = package(previous.id);
                    packageFragment(previous, actor, before.vmad.onChange, before.changeIdle);
                }
                if (previous.route && previous.procedure < previous.tree->procedures.size()
                    && previous.tree->procedures[previous.procedure].name == "Patrol")
                {
                    approach = previous.route;
                    approachDistance = previous.distance;
                    approachMover = previous.mount ? previous.mount : actor;
                }
                releasePackageMount(actor, previous);
                running.erase(found);
            }
            auto& active = running.try_emplace(actor,
                RunningPackage{id, selected.owner ? selected.owner : owner, sceneId, actionId, &tree}).first->second;
            active.approach = std::move(approach);
            active.approachDistance = approachDistance;
            active.approachMover = approachMover;
            active.fragment = packageFragment(active, actor, selected.vmad.onBegin, selected.beginIdle);
            events.push_back({id, "PACK", "Begin", {{"actor=" + std::to_string(actor), 0}}, time, {}, 0});
        }

        void evaluate(std::uint32_t actor, bool explicitRequest)
        {
            if (const auto active = running.find(actor); active != running.end())
                if (active->second.scene || (!explicitRequest && !active->second.done && package(active->second.id).mustComplete))
                    return;
            auto candidates = quests.packagesForActor(actor);
            const auto base = actorBase(actor);
            if (base)
            {
                const auto record = read(base);
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("PKID"))
                        candidates.push_back(record.mapFormId(integer(bytes)));
                });
            }
            if (const auto* selected = selectPackage(candidates, actor, 0))
            {
                const auto active = running.find(actor);
                if (active != running.end() && active->second.done
                    && active->second.id == selected->record.metadata.formId
                    && std::ranges::any_of(active->second.tree->procedures,
                        [](const Procedure& procedure) { return procedure.name == "Activate"; }))
                    return; // An occupied marker needs no second activation of the same package.
                startPackage(actor, *selected, quests.packageQuestForActor(actor, selected->record.metadata.formId));
            }
            else if (const auto active = running.find(actor); active != running.end())
            {
                stopLocomotion(active->second.mount ? active->second.mount : actor);
                const auto& previous = package(active->second.id);
                if (!active->second.done)
                    packageFragment(active->second, actor, previous.vmad.onChange, previous.changeIdle);
                releasePackageMount(actor, active->second);
                running.erase(active);
            }
        }

        struct TravelDoor
        {
            WorldScriptPose door, arrival;
            std::uint32_t world = 0;
        };

        std::optional<TravelDoor> travelDoor(std::uint32_t target, const WorldScriptPose& from)
        {
            const auto end = placement(target);
            if (!index || !end || end->cell == from.cell)
                return {};
            const auto worldAt = [&](std::uint32_t cell) {
                const auto found = cellWorlds.find(cell);
                return found == cellWorlds.end() ? 0u : found->second;
            };
            const auto fromWorld = worldAt(from.cell);
            auto endWorld = worldAt(end->cell);
            if (const auto indexed = index->references.find(target);
                !endWorld && indexed != index->references.end() && indexed->second.cell == end->cell)
                endWorld = indexed->second.world;
            if (fromWorld && fromWorld == endWorld) return {};

            std::optional<TravelDoor> closest;
            double distance = std::numeric_limits<double>::infinity();
            const auto host = quests.host();
            for (const auto& [id, pose] : authored)
            {
                if (fromWorld ? worldAt(pose.cell) != fromWorld : pose.cell != from.cell) continue;
                const auto ref = index->references.find(id);
                if (ref == index->references.end()) continue;
                const auto toDoor = (pose.position.asVec3() - from.position.asVec3()).length();
                if (toDoor >= distance) continue;
                const auto record = read(id);
                if (record.metadata.recordType != fourCC("REFR")) continue;
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag != fourCC("XTEL") || bytes.size() < 28) return;
                    const auto destination = index->references.find(record.mapFormId(integer(bytes)));
                    if (destination == index->references.end()
                        || (endWorld ? destination->second.world != endWorld : destination->second.cell != end->cell)
                        || (host.isDisabled && host.isDisabled(id))) return;
                    WorldScriptPose arrival{destination->second.cell};
                    for (unsigned axis = 0; axis < 3; ++axis)
                    {
                        arrival.position.pos[axis] = real(bytes, 4 + 4 * axis);
                        arrival.position.rot[axis] = real(bytes, 16 + 4 * axis);
                    }
                    const auto cost = toDoor + (arrival.position.asVec3() - end->position.asVec3()).length();
                    if (cost < distance)
                    {
                        closest = TravelDoor{pose, arrival, destination->second.world};
                        distance = cost;
                    }
                });
            }
            return closest;
        }

        const std::map<std::uint16_t, NavTriangle>& navMesh(std::uint32_t id)
        {
            auto [entry, fresh] = navigation.try_emplace(id);
            if (!fresh) return entry->second;
            const auto record = read(id);
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                if (tag != fourCC("NVNM") || bytes.size() < 20 || integer(bytes) != 12) return;
                std::size_t offset = 16;
                const auto array = [&](std::size_t stride) {
                    const auto count = integer(bytes, offset);
                    offset += 4;
                    if (count > (bytes.size() - offset) / stride)
                        throw std::runtime_error("Truncated NAVM array");
                    const auto span = bytes.subspan(offset, count * stride);
                    offset += span.size();
                    return span;
                };
                const auto vertices = array(12);
                constexpr auto triangleSize = sizeof(ESM4::NavMesh::Triangle);
                constexpr auto linkSize = sizeof(ESM4::NavMesh::ExtConnection);
                const auto triangles = array(triangleSize);
                const auto links = array(linkSize);
                for (std::size_t i = 0; i < triangles.size() / triangleSize && i < 0x8000; ++i)
                {
                    const auto raw = triangles.subspan(i * triangleSize, triangleSize);
                    const auto flags = integer(raw, 12, 2);
                    if (flags & 8) continue; // Deleted triangle.
                    NavTriangle triangle{};
                    bool valid = true;
                    for (unsigned edge = 0; edge < 3; ++edge)
                    {
                        const auto vertex = integer(raw, 2 * edge, 2) * 12;
                        if (vertex >= vertices.size()) { valid = false; break; }
                        triangle.vertices[edge].set(real(vertices, vertex), real(vertices, vertex + 4), real(vertices, vertex + 8));
                        const auto adjacent = integer(raw, 6 + 2 * edge, 2);
                        if (adjacent >= 0x8000) continue;
                        if (!(flags & (1 << edge)))
                            triangle.neighbors[edge] = {id, static_cast<std::uint16_t>(adjacent)};
                        else if (adjacent < links.size() / linkSize)
                        {
                            const auto link = links.subspan(adjacent * linkSize, linkSize);
                            triangle.neighbors[edge] = {record.mapFormId(integer(link, 4)),
                                static_cast<std::uint16_t>(integer(link, 8, 2))};
                        }
                    }
                    if (valid) entry->second.emplace(static_cast<std::uint16_t>(i), triangle);
                }
            });
            return entry->second;
        }

        void appendGroundPath(CartRoute& route, std::uint32_t actor, const WorldScriptPose& start, const WorldScriptPose& end)
        {
            if (!index) return;
            const auto startCell = exteriorCell(actor, start);
            const auto world = cellWorlds.find(startCell);
            std::set<std::uint32_t> cells{startCell};
            if (world != cellWorlds.end())
            {
                const auto grid = [](float position) { return static_cast<int>(std::floor(position / LandTile::size)); };
                const int x = grid(start.position.pos[0]), y = grid(start.position.pos[1]);
                const int goalX = grid(end.position.pos[0]), goalY = grid(end.position.pos[1]);
                if (std::abs(x - goalX) > 3 || std::abs(y - goalY) > 3) return;
                for (int i = std::min(x, goalX) - 1; i <= std::max(x, goalX) + 1; ++i)
                    for (int j = std::min(y, goalY) - 1; j <= std::max(y, goalY) + 1; ++j)
                        if (const auto cell = exteriorCells.find({world->second, i, j}); cell != exteriorCells.end())
                            cells.insert(cell->second);
            }
            else if (start.cell != end.cell) return;
            std::map<NavNode, const NavTriangle*> graph;
            for (const auto cell : cells)
                if (const auto meshes = index->navigationByCell.find(cell); meshes != index->navigationByCell.end())
                    for (auto id : meshes->second)
                        for (const auto& [i, triangle] : navMesh(id))
                            graph.emplace(NavNode{id, i}, &triangle);
            if (graph.empty()) return;
            const auto nearest = [&](const osg::Vec3f& point) {
                NavNode found{};
                float best = 192.f * 192.f;
                for (const auto& [key, triangle] : graph)
                {
                    const auto& v = triangle->vertices;
                    const auto first = v[1] - v[0], second = v[2] - v[0], offset = point - v[0];
                    const float area = (first ^ second).z();
                    if (std::abs(area) < .001f) continue;
                    const float u = (offset ^ second).z() / area, w = (first ^ offset).z() / area;
                    float distance = (v[0] + first * u + second * w - point).length2();
                    if (u < 0 || w < 0 || u + w > 1)
                    {
                        distance = std::numeric_limits<float>::infinity();
                        for (unsigned edge = 0; edge < 3; ++edge)
                        {
                            const auto line = v[(edge + 1) % 3] - v[edge];
                            const auto delta = point - v[edge];
                            const float fraction = std::clamp((line * delta) / line.length2(), 0.f, 1.f);
                            distance = std::min(distance, (delta - line * fraction).length2());
                        }
                    }
                    if (distance < best) { best = distance; found = key; }
                }
                return found;
            };
            const auto source = nearest(start.position.asVec3()), destination = nearest(end.position.asVec3());
            if (!source.first || !destination.first || source == destination) return;
            std::map<NavNode, std::pair<NavNode, osg::Vec3f>> parents;
            std::map<NavNode, float> costs{{source, 0}};
            using Candidate = std::pair<float, NavNode>;
            std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> frontier;
            frontier.emplace(0, source);
            while (!frontier.empty())
            {
                const auto [cost, node] = frontier.top();
                frontier.pop();
                if (node == destination) break;
                if (cost > costs.at(node)) continue;
                const auto& current = *graph.at(node);
                for (unsigned edge = 0; edge < 3; ++edge)
                {
                    const auto next = graph.find(current.neighbors[edge]);
                    if (next == graph.end()) continue;
                    const auto weight = cost + (current.center() - next->second->center()).length();
                    if (const auto seen = costs.find(next->first); seen != costs.end() && seen->second <= weight) continue;
                    costs.insert_or_assign(next->first, weight);
                    parents.insert_or_assign(next->first, std::pair(node, (current.vertices[edge] + current.vertices[(edge + 1) % 3]) * .5f));
                    frontier.emplace(weight, next->first);
                }
            }
            if (!parents.contains(destination)) return;
            std::vector<osg::Vec3f> portals;
            for (auto node = destination; node != source;)
            {
                const auto& [parent, portal] = parents.at(node);
                portals.push_back(portal);
                node = parent;
            }
            for (const auto& portal : portals | std::views::reverse)
            {
                auto pose = end.position;
                for (unsigned axis = 0; axis < 3; ++axis) pose.pos[axis] = portal[axis];
                route.points.push_back({0, pose});
            }
        }

        void prepareProcedure(std::uint32_t actor, RunningPackage& active, const Procedure& procedure)
        {
            const auto& selected = package(active.id);
            const auto* first = input(selected, *active.tree, procedure, 0);
            active.prepared = true;
            active.distance = 0;
            active.wait = 0;
            active.loopStart.reset();
            if (procedure.name == "Orbit")
            {
                const auto parameter = [&](std::size_t slot) {
                    const auto* value = input(selected, *active.tree, procedure, slot);
                    if (!value || value->value.size() != 4 || !std::isfinite(real(value->value)))
                        throw std::runtime_error("Orbit needs finite PACK radius and height inputs");
                    return real(value->value);
                };
                if (!first || !first->target || !flightProfile(actor).movement)
                    throw std::runtime_error("Orbit requires a flying actor and a location selector");
                const auto target = destination(*first->target, actor, active.owner);
                const auto center = target ? placement(target) : std::nullopt;
                const auto origin = placement(actor);
                const float inner = parameter(1), outer = parameter(2), height = parameter(3);
                if (!center || !origin || inner < 0 || outer <= 0 || outer < inner)
                    throw std::runtime_error("Invalid Orbit center or radii");
                const auto offset = origin->position.asVec3() - center->position.asVec3();
                const float radialDistance = std::hypot(offset.x(), offset.y());
                const float radius = radialDistance > 0 ? std::clamp(radialDistance, inner, outer) : outer;
                const float angle = radialDistance > 0 ? std::atan2(offset.y(), offset.x()) : -origin->position.rot[2];
                const float direction = std::cos(origin->position.rot[2] + angle) >= 0 ? 1.f : -1.f;
                CartRoute route{active.id, {{actor, origin->position}}, selected.speed};
                constexpr unsigned segments = 48;
                for (unsigned i = 0; i <= segments; ++i)
                {
                    const auto bearing = angle + direction * (2 * std::numbers::pi_v<float> * i / segments);
                    auto pose = center->position;
                    pose.pos[0] += radius * std::cos(bearing);
                    pose.pos[1] += radius * std::sin(bearing);
                    pose.pos[2] += height;
                    pose.rot[2] = -bearing + (direction < 0 ? std::numbers::pi_v<float> : 0.f);
                    route.points.push_back({target, pose});
                    if (!i) active.loopStart = cartRouteLength(route);
                }
                active.length = cartRouteLength(route);
                active.speed = movementSpeed(actor, selected.speed, false, true);
                active.turnSpeed = movementSpeed(actor, selected.speed, true, true);
                active.route = std::move(route);
                events.push_back({active.id, "PACK", "Orbit", {{std::format("actor={} center={} radius={} height={}", actor, target, radius, height), 0}},
                    time, {}, 0});
                return;
            }
            if (procedure.name == "FlightGrab")
            {
                const auto* ignoreCollision = input(selected, *active.tree, procedure, 1);
                if (!first || !first->target || !ignoreCollision || ignoreCollision->value.size() != 1
                    || !integer(ignoreCollision->value, 0, 1) || !flightProfile(actor).movement)
                    throw std::runtime_error("FlightGrab needs a flying actor, target and the PACK no-collision option");
                const auto target = destination(*first->target, actor, active.owner);
                const auto source = placement(actor);
                const auto victim = target ? placement(target) : std::nullopt;
                if (!source || !victim || !actorBase(target))
                    return;
                flightGrabs.insert_or_assign(actor, FlightGrab{FlightGrab::Approach, target});
                auto end = victim->position;
                const auto direction = end.asVec3() - source->position.asVec3();
                end.rot[2] = std::atan2(direction.x(), direction.y());
                active.route = CartRoute{active.id, {{actor, source->position}, {target, end}}, selected.speed};
                active.length = cartRouteLength(*active.route);
                active.speed = movementSpeed(actor, 0, false, true);
                active.turnSpeed = movementSpeed(actor, 0, true, true);
                return;
            }
            if (procedure.name == "Shout")
            {
                const auto* shout = input(selected, *active.tree, procedure, 1);
                const auto* targetInput = input(selected, *active.tree, procedure, 2);
                if (!shout || !shout->target || !occupiedPerches.contains(actor) || !index)
                    throw std::runtime_error("Shout requires a selected voice power and a supported perch");
                const auto voice = shout->target->value;
                const auto record = read(voice);
                if (record.metadata.recordType != fourCC("SHOU"))
                    throw std::runtime_error("Shout requires a SHOU record");
                std::uint32_t spell = 0;
                visitSubrecords(record, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("SNAM") && !spell) spell = record.mapFormId(integer(bytes, 4));
                });
                if (!spell || read(spell).metadata.recordType != fourCC("SPEL"))
                    throw std::runtime_error("Shout word has no spell");
                const auto target = targetInput && targetInput->target ? destination(*targetInput->target, actor, active.owner) : 0;
                const auto aimSeconds = index->floatSettings.find("faishoutminaimseconds");
                if (aimSeconds == index->floatSettings.end() || !std::isfinite(aimSeconds->second) || aimSeconds->second < 0)
                    throw std::runtime_error("Shout needs fAIShoutMinAimSeconds");
                shouts.insert_or_assign(actor, ShoutCast{ShoutCast::Aim, voice, spell, target});
                if (target && target != actor) faces.push_back({0, actor, target});
                active.wait = aimSeconds->second;
                return;
            }
            if (procedure.name == "UseWeapon")
            {
                const auto* target = input(selected, *active.tree, procedure, 2);
                const auto* weapon = input(selected, *active.tree, procedure, 1);
                if (!target || !target->target || !weapon || !weapon->target)
                    throw std::runtime_error("UseWeapon needs a weapon selector and a target");
                const auto& selector = *weapon->target;
                if (selector.type != 0 && (selector.type != 2 || selector.value != 20))
                    throw std::runtime_error("UseWeapon currently supports bows and the bow inventory selector");
                const auto flag = [&](std::size_t slot) {
                    const auto* value = input(selected, *active.tree, procedure, slot);
                    return value && !value->value.empty() && integer(value->value, 0, 1) != 0;
                };
                const auto loadout = readArrowLoadout(read, actorBase(actor), selector.type == 0 ? selector.value : 0);
                if (!loadout)
                    return;
                auto& attack = active.bow.emplace(BowAttack{*loadout, destination(*target->target, actor, active.owner)});
                attack.alwaysHit = flag(3);
                attack.repeat = flag(4);
                attack.noDamage = flag(7);
                attack.holdWhenBlocked = flag(8);
                const auto value = [&](std::size_t slot, bool floating, float fallback) {
                    const auto* entry = input(selected, *active.tree, procedure, slot);
                    return !entry || entry->value.size() != 4 ? fallback
                        : floating ? real(entry->value) : static_cast<float>(integer(entry->value));
                };
                auto& volley = bowVolleys[actor];
                volley = {};
                volley.pauses = flag(5);
                volley.minShots = static_cast<int>(std::clamp(value(12, false, 1), 1.f, 1000.f));
                volley.maxShots = static_cast<int>(std::clamp(value(13, false, volley.minShots), float(volley.minShots), 1000.f));
                volley.shotsLeft = std::uniform_int_distribution<int>(volley.minShots, volley.maxShots)(soundRandom);
                volley.barrages = attack.repeat ? -1 : static_cast<int>(std::clamp(value(11, false, 1), 1.f, 1000.f));
                volley.minPause = std::clamp(value(9, true, 0), 0.f, 86400.f);
                volley.maxPause = std::clamp(value(10, true, 0), volley.minPause, 86400.f);
                const auto* duration = input(selected, *active.tree, procedure, 18);
                active.wait = duration && duration->value.size() == 4 ? real(duration->value) : 0;
                if (active.wait > 0) active.wait += time;
                return;
            }
            if (procedure.name == "Wait")
            {
                stopLocomotion(active.mount ? active.mount : actor);
                active.wait = !first || first->value.size() != 4 ? 0 : real(first->value);
                return;
            }
            if (procedure.name == "UseIdleMarker")
            {
                if (!first || !first->target)
                    throw std::runtime_error("UseIdleMarker procedure has no target marker");
                const auto marker = destination(*first->target, actor, active.owner);
                const auto pose = placement(marker);
                if (!pose || !idle(actor, idleMarkerAnimation(marker)))
                    throw std::runtime_error("UseIdleMarker has no playable animation");
                move(actor, *pose);
                active.wait = std::numeric_limits<double>::infinity();
                return;
            }
            const bool activateFurniture = procedure.name == "Activate";
            const bool escort = procedure.name == "Escort";
            if (escort) first = input(selected, *active.tree, procedure, 2);
            if (procedure.name != "Travel" && procedure.name != "Patrol" && procedure.name != "Sit"
                && procedure.name != "FindSit" && !activateFurniture && !escort)
                throw std::runtime_error("Unsupported PACK procedure " + procedure.name);
            if (!first || !first->target)
                throw std::runtime_error("PACK movement procedure has no target data");
            const bool patrol = procedure.name == "Patrol";
            if (const auto* ride = input(selected, *active.tree, procedure, escort ? 6 : patrol ? 4 : 1);
                ride && ride->value.size() == 1 && integer(ride->value, 0, 1))
            {
                if (const auto horse = packageMount(actor); horse && !attachments.contains(actor))
                {
                    attach(actor, horse, true);
                    active.mount = horse;
                }
            }
            const auto mover = active.mount ? active.mount : actor;
            auto target = destination(*first->target, mover, active.owner);
            if (activateFurniture)
            {
                const auto* count = input(selected, *active.tree, procedure, 1);
                if (!target || !count || count->value.size() != 4 || integer(count->value) != 1 || !isFurniture(target))
                    throw std::runtime_error("Activate currently requires one activation of a furniture reference");
            }
            if (procedure.name == "FindSit")
            {
                const auto* selector = input(selected, *active.tree, procedure, 1);
                if (selector && selector->target && (selector->target->type == 0 || selector->target->type == 4))
                    target = destination(*selector->target, mover, active.owner);
                else
                {
                    const auto center = placement(target);
                    double nearest = first->target->radius;
                    std::uint32_t found = 0;
                    if (center)
                        for (const auto& [candidate, pose] : authored)
                        {
                            if (candidate == 0x14 || pose.cell != center->cell || !index || !index->references.contains(candidate)) continue;
                            const auto distance = (pose.position.asVec3() - center->position.asVec3()).length();
                            if (distance < nearest && isFurniture(candidate)) { nearest = distance; found = candidate; }
                        }
                    if (found) target = found;
                }
            }
            const auto start = placement(mover);
            auto end = (activateFurniture || procedure.name == "Sit" || (procedure.name == "FindSit" && isFurniture(target)))
                ? std::optional<WorldScriptPose>{furniturePose(actor, target)} : placement(target);
            if (start && procedure.name == "Travel")
                if (const auto door = travelDoor(target, *start)) end = door->door;
            if (!start || !end)
                throw std::runtime_error("PACK movement target or actor has no position");
            CartRoute route{active.id, {{mover, start->position}}, selected.preferredSpeed(procedure)};
            const auto* repeat = input(selected, *active.tree, procedure, 2);
            const bool repeatable = repeat && repeat->value.size() == 1 && integer(repeat->value, 0, 1);
            const auto append = [&](std::uint32_t marker, std::uint32_t until) {
                std::set<std::uint32_t> visited;
                while (marker && marker != until)
                {
                    if (!visited.insert(marker).second)
                    {
                        if (marker != target)
                            throw std::runtime_error("PACK patrol has a non-entry cycle");
                        if (repeatable)
                            route.points.push_back({marker, end->position});
                        else
                            active.loopStart.reset();
                        break;
                    }
                    const auto pose = placement(marker);
                    if (!pose || visited.size() > 512)
                        throw std::runtime_error("PACK patrol marker chain is invalid");
                    route.points.push_back({marker, pose->position});
                    marker = linked(marker);
                }
                return marker == until;
            };
            if (patrol)
            {
                if (!locomotion.contains(mover) && !append(linked(mover), target))
                    route.points.resize(1);
                active.loopStart = cartRouteLength(route)
                    + (route.points.back().position.asVec3() - end->position.asVec3()).length();
                if (append(target, 0) || !repeatable)
                    active.loopStart.reset();
            }
            else
            {
                if (!active.mount && (procedure.name == "Travel" || escort) && flightProfile(mover).behavior.empty())
                    try { appendGroundPath(route, mover, *start, *end); }
                    catch (const std::exception& error) { log.write("NAVM path", error.what()); }
                route.points.push_back({target, end->position});
            }
            if (patrol && active.approach && active.approachMover == mover && route.points.size() > 1)
            {
                // joinCartRoute expects the first point to be the new patrol target,
                // not the actor's current position. It keeps only forward approach legs.
                route.points.erase(route.points.begin());
                const double before = cartRouteLength(route);
                joinCartRoute(route, *active.approach, active.approachDistance);
                route.points.front().position = start->position;
                if (active.loopStart)
                    *active.loopStart = cartRouteLength(route) - before;
            }
            active.approach.reset();
            std::ostringstream routeTrace;
            routeTrace << "T5_PACK_ROUTE actor=" << std::hex << actor << " mover=" << mover
                << " package=" << active.id << " target=" << target << std::dec << " markers=";
            for (const auto& point : route.points)
                routeTrace << std::hex << point.referenceId << std::dec << ',';
            const bool perch = (procedure.name == "Sit" || procedure.name == "FindSit") && flyingFurniture(actor, target);
            const bool allowFlight = patrol || perch;
            if (perch) route.preferredSpeed = 0;
            const auto speed = route.preferredSpeed;
            active.speed = movementSpeed(mover, speed, false, allowFlight);
            active.turnSpeed = movementSpeed(mover, speed, true, allowFlight);
            routeTrace << " length=" << cartRouteLength(route) << " speed=" << active.speed
                << " turnRate=" << active.turnSpeed << " preferred=" << unsigned(speed);
            events.push_back({active.id, "PACK", "Route", {{routeTrace.str(), 0}}, time, {}, 0});
            active.length = cartRouteLength(route);
            if (procedure.name == "Travel")
                active.length = std::max(0., active.length - first->target->radius);
            active.route = std::move(route);
        }

        void playBowClip(std::uint32_t actor, const BowAttack& attack, const char* clip)
        {
            animations.push_back({actor, 0, {}, "T5Bow", std::string("actors/character/animations/") + clip + ".hkx",
                attack.weapon.drawSpeed});
        }

        bool stepBow(std::uint32_t actor, std::optional<BowAttack>& bow, double until = 0, bool usePositions = false)
        {
            if (!bow)
                return true;
            auto& attack = *bow;
            const auto& weapon = attack.weapon;
            if (!attack.target || deadActors.contains(actor))
            {
                arrows.push_back({WorldScriptArrow::Lower, actor});
                bow.reset();
                return true;
            }
            if (!loaded.contains(actor) || !loaded.contains(attack.target))
                return false;
            const bool outOfArrows = arrowsUsed[{actor, weapon.ammo}] >= weapon.count;
            if ((deadActors.contains(attack.target) || outOfArrows || (until > 0 && time >= until))
                && attack.phase != BowAttack::Recover)
            {
                attack.phase = BowAttack::Recover;
                attack.animationDone = false;
                playBowClip(actor, attack, "bow_unequip");
                return false;
            }
            auto source = placement(actor);
            const auto target = placement(attack.target);
            if (!source || !target)
                return false;
            if (usePositions && attack.phase != BowAttack::Recover)
                if (auto selected = bowPositions.find(actor); selected != bowPositions.end() && !selected->second.reached)
                {
                    const auto destination = bowDestinations.find(actor);
                    const auto position = destination != bowDestinations.end() ? std::optional{destination->second}
                        : selected->second.marker ? placement(selected->second.marker) : std::nullopt;
                    if (position)
                    {
                        const auto offset = position->position.asVec3() - source->position.asVec3();
                        const auto distance = offset.length();
                        const auto step = movementSpeed(actor, 0, false, false) * delta;
                        if (distance > step)
                        {
                            playLocomotion(actor, "T5Walk");
                            for (unsigned axis = 0; axis < 3; ++axis)
                                source->position.pos[axis] += static_cast<float>(offset[axis] * step / distance);
                            source->position.rot[2] = advanceRouteHeading(source->position.rot[2], std::atan2(offset.x(), offset.y()),
                                movementSpeed(actor, 0, true, false), delta);
                            move(actor, *source);
                            return false;
                        }
                        source = position;
                    }
                    selected->second.reached = true;
                    attack.phase = BowAttack::Equip;
                    attack.animationDone = attack.released = false;
                    events.push_back({actor, "Archery", "PositionReached", {{std::format("marker={}", selected->second.marker), 0}},
                        time, {}, 0});
                }
            const auto direction = target->position.asVec3() - source->position.asVec3();
            source->position.rot[2] = std::atan2(direction.x(), direction.y());
            move(actor, *source);
            if (attack.phase == BowAttack::Equip && !attack.animationDone && !attack.released)
            {
                attack.released = true;
                playBowClip(actor, attack, "bow_equip");
                return false;
            }
            if (const auto volley = bowVolleys.find(actor); volley != bowVolleys.end()
                && attack.phase == BowAttack::Equip && time < volley->second.resumeAt)
                return false;
            if (!std::exchange(attack.animationDone, false))
                return false;
            if (attack.phase == BowAttack::Release && bowVolleys.contains(actor))
            {
                auto& volley = bowVolleys.at(actor);
                if (--volley.shotsLeft <= 0)
                {
                    if (volley.barrages > 0) --volley.barrages;
                    if (volley.barrages != 0)
                    {
                        volley.shotsLeft = std::uniform_int_distribution<int>(volley.minShots, volley.maxShots)(soundRandom);
                        volley.resumeAt = time + (volley.pauses
                            ? std::uniform_real_distribution<float>(volley.minPause, volley.maxPause)(soundRandom) : 0);
                    }
                }
                if (volley.shotsLeft > 0)
                {
                    attack.phase = BowAttack::Equip; attack.animationDone = true;
                    return false;
                }
            }
            if (attack.phase == BowAttack::Equip || (attack.phase == BowAttack::Release && attack.repeat && !bowVolleys.contains(actor)))
            {
                attack.phase = BowAttack::Draw;
                attack.released = false;
                playBowClip(actor, attack, "bow_drawlight");
                return false;
            }
            if (attack.phase == BowAttack::Draw)
            {
                attack.phase = BowAttack::Release;
                playBowClip(actor, attack, "bow_release");
                return false;
            }
            if (attack.phase == BowAttack::Release)
            {
                attack.phase = BowAttack::Recover;
                playBowClip(actor, attack, "bow_unequip");
                return false;
            }
            arrows.push_back({WorldScriptArrow::Lower, actor});
            bow.reset();
            bowVolleys.erase(actor);
            return true;
        }

        void bowEvent(std::uint32_t actor, std::string_view event)
        {
            const auto scripted = running.find(actor);
            const auto combat = combatBows.find(actor);
            auto* bow = scripted != running.end() && scripted->second.bow ? &scripted->second.bow
                : combat != combatBows.end() ? &combat->second : nullptr;
            if (!bow || !*bow)
                return;
            auto& attack = **bow;
            const auto& weapon = attack.weapon;
            if (event == "T5BowObstructed")
            {
                if (attack.phase == BowAttack::Release && attack.released)
                {
                    auto& consumed = arrowsUsed[{actor, weapon.ammo}];
                    if (consumed) --consumed;
                }
                if (combat == combatBows.end() || bow != &combat->second)
                    return;
                const auto previous = bowPositions.find(actor);
                if (previous != bowPositions.end() && !previous->second.reached)
                    return;
                const auto marker = previous == bowPositions.end() ? linked(actor) : 0;
                firingRequests.push_back({actor, attack.target, marker, weapon.range, weapon.radius});
                return;
            }
            if (event == "T5ClipEnd")
                attack.animationDone = true;
            else if (event == "weaponDraw" && attack.phase == BowAttack::Equip)
                arrows.push_back({WorldScriptArrow::Equip, actor, 0, 0, weapon.bowModel});
            else if (event == "arrowAttach" && attack.phase == BowAttack::Draw)
                arrows.push_back({WorldScriptArrow::Nock, actor, attack.target, 0, weapon.ammoModel});
            else if (event == "weaponSheathe" && attack.phase == BowAttack::Recover)
                arrows.push_back({WorldScriptArrow::Lower, actor});
            else if (event == "arrowRelease" && attack.phase == BowAttack::Release && !std::exchange(attack.released, true))
            {
                const auto serial = nextArrow++;
                fired.emplace(serial, FiredArrow{actor, attack.target, weapon.bow, weapon.projectile,
                    attack.noDamage ? 0.f : weapon.damage, time + 10});
                arrows.push_back({WorldScriptArrow::Release, actor, attack.target, serial, weapon.projectileModel,
                    weapon.speed, weapon.gravity, weapon.range, weapon.radius, weapon.lifetime,
                    attack.alwaysHit, attack.holdWhenBlocked});
                ++arrowsUsed[{actor, weapon.ammo}];
                if (weapon.releaseSound)
                    callSound(weapon.releaseSound, {FormReference{actor}});
                events.push_back({actor, "Archery", "arrowRelease", {{"shot=" + std::to_string(serial), 0}}, time, {}, 0});
            }
        }

        void arrowImpact(std::uint32_t shot, std::uint32_t target)
        {
            const auto entry = fired.extract(shot);
            if (entry.empty() || !target || deadActors.contains(target) || !actorBase(target))
                return;
            const auto& arrow = entry.mapped();
            arrowVictims.insert_or_assign(target, time + 2.);
            quests.actorHit(target, arrow.actor, arrow.bow, arrow.projectile);
            events.push_back({target, "Archery", "OnHit", {{"shot=" + std::to_string(shot)
                + " aggressor=" + std::to_string(arrow.actor), 0}}, time, {}, 0});
            damageActor(target, arrow.actor, arrow.damage);
        }

        static constexpr std::uint32_t healthValue = 24;

        bool isDead(std::uint32_t actor) const
        {
            return actorBase(actor) && (deadActors.contains(actor)
                || (actor != 0x14 && !quests.createdReferences().contains(actor) && (read(actor).flags & 0x200) != 0));
        }

        void damageActor(std::uint32_t target, std::uint32_t source, float damage)
        {
            if (damage <= 0 || isDead(target)) return;
            const auto [base, flags] = actorHealth(actorBase(target));
            const auto ghost = ghostOverrides.find(target);
            if (base <= 0 || (flags & 0x80000000)
                || (ghost == ghostOverrides.end() ? (flags & 0x20000000) != 0 : ghost->second))
                return;
            auto& current = actorValues.try_emplace(std::pair{target, healthValue}, base).first->second;
            const bool protectedActor = (flags & 0x800) && source != 0x14;
            current = std::max((flags & 2) || protectedActor ? 1.f : 0.f, current - damage);
            if (current <= 0) killActor(target, source);
        }

        std::pair<float, std::uint32_t> actorHealth(std::uint32_t id) const
        {
            std::set<std::uint32_t> statTemplates;
            std::uint32_t race = 0, flags = 0;
            float health = 0, offset = 0;
            while (id && statTemplates.insert(id).second && statTemplates.size() <= 16)
            {
                const auto npc = read(id);
                if (npc.metadata.recordType != fourCC("NPC_"))
                    break;
                std::uint32_t parent = 0, inheritance = 0;
                visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("ACBS") && bytes.size() >= 22)
                    {
                        if (statTemplates.size() == 1)
                            flags = integer(bytes);
                        inheritance = integer(bytes, 18, 2);
                        offset = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(integer(bytes, 20, 2)));
                    }
                    else if (tag == fourCC("DNAM") && bytes.size() >= 38)
                        health = static_cast<float>(integer(bytes, 36, 2));
                    else if (tag == fourCC("RNAM") && !race)
                        race = npc.mapFormId(integer(bytes));
                    else if (tag == fourCC("TPLT"))
                        parent = npc.mapFormId(integer(bytes));
                });
                if (!(inheritance & 2) || !parent)
                    break;
                id = parent;
                health = 0;
            }
            if (health > 0)
                return {health, flags}; // DNAM stores the resolved pool, including the ACBS offset.
            if (race)
                visitSubrecords(read(race), [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("DATA") && bytes.size() >= 40)
                        health = real(bytes, 36);
                });
            return {std::max(0.f, health + offset), flags};
        }

        void killActor(std::uint32_t actor, std::uint32_t killer)
        {
            if (!deadActors.insert(actor).second)
                return;
            actorValues.insert_or_assign({actor, healthValue}, 0.f);
            attachments.erase(actor);
            if (arrowVictims.contains(actor))
                animations.push_back({actor, 0, {}, "T5Death", "actors/character/animations/deathanimationa.hkx"});
            quests.actorDeath(actor, killer);
        }

        bool stepProcedure(std::uint32_t actor, RunningPackage& active, const Procedure& procedure)
        {
            if (!active.prepared)
                prepareProcedure(actor, active, procedure);
            if (procedure.name == "UseWeapon")
                return stepBow(actor, active.bow, active.wait);
            if (procedure.name == "Shout")
            {
                const auto shout = shouts.find(actor);
                if (shout == shouts.end()) return active.wait <= 0;
                if (shout->second.phase == ShoutCast::Aim && (active.wait -= delta) <= 0)
                {
                    shout->second.phase = ShoutCast::Inhale;
                    playFlightEvent(actor, "BHR_Perch_Tower::shoutStart");
                }
                return false;
            }
            if (!active.route)
            {
                if (enteringExecutionBlock.contains(actor) || landingPerches.contains(actor) || flightGrabs.contains(actor))
                    return false;
                active.wait -= delta;
                return active.wait <= 0;
            }
            const auto mover = active.mount ? active.mount : actor;
            if (attachments.contains(mover) || animationDriven.contains(mover) || physicsDriven.contains(mover) || fixedReferences.contains(mover))
                return false;
            if (procedure.name == "Escort")
            {
                const auto& selected = package(active.id);
                const auto* follower = input(selected, *active.tree, procedure, 0);
                const auto* distance = input(selected, *active.tree, procedure, 3);
                if (!follower || !follower->target || !distance || distance->value.size() != 4)
                    throw std::runtime_error("Escort requires a follower and a wait distance");
                const auto leader = placement(mover);
                const auto companion = placement(destination(*follower->target, actor, active.owner));
                const auto waitDistance = real(distance->value);
                if (!leader || !companion || (leader->position.asVec3() - companion->position.asVec3()).length() > waitDistance)
                {
                    stopLocomotion(mover);
                    return false;
                }
            }
            const bool flying = (procedure.name == "Patrol" || procedure.name == "Orbit" || procedure.name == "FlightGrab"
                || ((procedure.name == "Sit" || procedure.name == "FindSit")
                    && flyingFurniture(actor, active.route->points.back().referenceId)))
                && !flightProfile(mover).behavior.empty();
            if (flying && noFlight.contains(mover)) return false;
            if (!flying)
            {
                // Checkpoints may contain routes prepared before PACK speed flags were applied.
                const auto preferred = package(active.id).preferredSpeed(procedure);
                if (active.route->preferredSpeed != preferred)
                {
                    active.route->preferredSpeed = preferred;
                    active.speed = movementSpeed(mover, preferred, false, false);
                    active.turnSpeed = movementSpeed(mover, preferred, true, false);
                }
            }
            if (active.length > active.distance && !deadActors.contains(mover))
            {
                if (flying)
                    playLocomotion(mover, "BHR_Default::FlyStartCruise");
                else
                {
                    switch (active.route->preferredSpeed)
                    {
                        case 1: playLocomotion(mover, "T5Jog", active.speed / movementSpeed(mover, 2, false, false)); break;
                        case 2: playLocomotion(mover, "T5Run"); break;
                        case 3: playLocomotion(mover, "T5FastWalk", active.speed / movementSpeed(mover, 0, false, false)); break;
                        default: playLocomotion(mover, "T5Walk"); break;
                    }
                }
            }
            const double speed = procedure.name == "Orbit" && active.loopStart && active.distance >= *active.loopStart
                ? std::min(active.speed, (active.length - *active.loopStart) * active.turnSpeed / (2 * std::numbers::pi))
                : active.speed;
            active.distance = std::min(active.length, active.distance + delta * speed);
            const auto previous = placement(mover);
            auto pose = sampleCartRoute(*active.route, active.distance);
            const float targetHeading = pose.rot[2];
            if (previous)
                pose.rot[2] = advanceRouteHeading(previous->position.rot[2], targetHeading, active.turnSpeed, delta);
            move(mover, WorldScriptPose{previous ? previous->cell : 0, pose});
            if (active.distance < active.length)
                return false;
            // Arrival orientation is part of the destination pose. Do not snap a
            // mounted actor and its attachments on the final frame of a package.
            if (std::abs(std::remainder(pose.rot[2] - targetHeading, 2 * std::numbers::pi_v<float>)) > .0001f)
                return false;
            if (procedure.name == "FlightGrab")
            {
                active.route.reset();
                locomotion.erase(actor);
                auto& grab = flightGrabs.at(actor);
                grab.phase = FlightGrab::Enter;
                playFlightEvent(actor, "BHR_Default::to_Flight_Kill_Grab_Enter");
                return false;
            }
            if (procedure.name == "Sit" || procedure.name == "FindSit" || procedure.name == "Activate")
            {
                const auto target = active.route->points.back().referenceId;
                if (procedure.name == "FindSit" && !isFurniture(target)) return true;
                stopLocomotion(actor);
                const auto* duration = procedure.name == "Activate" ? nullptr
                    : input(package(active.id), *active.tree, procedure, procedure.name == "Sit" ? 1 : 2);
                active.wait = duration && duration->value.size() == 4 ? std::max(0.f, real(duration->value)) : 0;
                if (flying && (!occupiedPerches.contains(actor) || occupiedPerches.at(actor) != target))
                {
                    locomotion.erase(actor);
                    playFlightEvent(actor, "BHR_Land_Perch_Tower::BeginLand");
                    landingPerches.insert_or_assign(actor, target);
                    active.route.reset();
                    return false;
                }
                enterFurniture(actor, target);
                if (procedure.name == "Activate")
                    active.forceEnd = true; // The requested single activation is complete even in a repeating template.
                active.route.reset();
                return active.wait <= 0 && !enteringExecutionBlock.contains(actor);
            }
            if (procedure.name == "Travel" && !active.route->points.empty())
            {
                const auto target = active.route->points.back().referenceId;
                if (previous)
                    if (const auto door = travelDoor(target, *previous);
                        door && (door->door.position.asVec3() - pose.asVec3()).length2() < 1.f)
                    {
                        if (door->world)
                            cellWorlds.try_emplace(door->arrival.cell, door->world);
                        move(mover, door->arrival);
                        teleported.insert(mover);
                        events.push_back({mover, "PACK", "DoorArrival",
                            {{std::format("T5_PACK_DOOR_ARRIVAL actor={:08X} cell={:08X}", mover, door->arrival.cell), 0}},
                            time, {}, 0});
                        active.route.reset();
                        active.prepared = false;
                        active.distance = 0;
                        return false;
                    }
            }
            if (!active.loopStart || *active.loopStart >= active.length)
                return true;
            active.distance = *active.loopStart;
            return false;
        }

        void flightAnimationEvent(std::uint32_t actor, std::string_view event)
        {
            if (auto shout = shouts.find(actor); shout != shouts.end())
            {
                auto& cast = shout->second;
                if (event == "T5ClipEnd" && cast.phase == ShoutCast::Inhale)
                {
                    cast.phase = ShoutCast::Exhale;
                    playFlightEvent(actor, "BHR_Perch_Tower_Shout/ST_Perch_Tower_Shout_Inhale_Firebreath::shoutRelease");
                }
                else if (event == "Voice_SpellFire_Event" && cast.phase == ShoutCast::Exhale)
                {
                    spellCasts.push_back({cast.spell, actor, cast.target});
                    events.push_back({actor, "Shout", "Cast", {{std::format("voice={} spell={} target={}", cast.voice, cast.spell, cast.target), 0}},
                        time, {}, 0});
                }
                else if (event == "shoutStop" && cast.phase == ShoutCast::Exhale)
                    stopCastSounds(actor);
                else if (event == "T5ClipEnd" && cast.phase == ShoutCast::Exhale)
                {
                    stopCastSounds(actor);
                    faces.push_back({0, actor, 0});
                    shouts.erase(shout);
                }
                return;
            }
            if (event == "T5ClipEnd")
                if (const auto perch = landingPerches.extract(actor); !perch.empty())
                {
                    move(actor, furniturePose(actor, perch.mapped()));
                    enterFurniture(actor, perch.mapped());
                    return;
                }
            const auto found = flightGrabs.find(actor);
            if (found == flightGrabs.end())
                return;
            auto& grab = found->second;
            const auto host = quests.host();
            const bool targetReady = loaded.contains(grab.target) && !deadActors.contains(grab.target)
                && (!host.isDisabled || !host.isDisabled(grab.target));
            if (event == "T5ClipEnd" && grab.phase == FlightGrab::Enter)
            {
                grab.phase = targetReady ? FlightGrab::Exit : FlightGrab::Failed;
                playFlightEvent(actor, targetReady ? "BHR_Flight_Kill_Grab::to_Flight_Kill_Grab_Action"
                    : "BHR_Flight_Kill_Grab::to_Flight_Kill_Grab_Action_Failed");
            }
            else if (event == "FlightActionGrab" && grab.phase == FlightGrab::Exit && targetReady)
            {
                attach(grab.target, actor);
                events.push_back({actor, "PACK", "FlightActionGrab", {{std::format("target={}", grab.target), 0}}, time, {}, 0});
            }
            else if (event == "FlightActionRelease" && grab.phase == FlightGrab::Exit)
            {
                const auto bound = attachments.find(grab.target);
                if (bound != attachments.end() && bound->second.parent == actor)
                {
                    attachments.erase(bound);
                    if (const auto pose = placement(grab.target))
                    {
                        const auto velocity = carrierVelocities.find(actor);
                        fallingActors.insert_or_assign(grab.target, FallingActor{actor,
                            velocity == carrierVelocities.end() ? osg::Vec3f{} : velocity->second, pose->position.pos[2]});
                        physicsDriven.insert(grab.target);
                    }
                    events.push_back({actor, "PACK", "FlightActionRelease", {{std::format("target={}", grab.target), 0}}, time, {}, 0});
                }
            }
            else if (event == "T5ClipEnd" && (grab.phase == FlightGrab::Exit || grab.phase == FlightGrab::Failed))
            {
                flightGrabs.erase(found);
                carrierVelocities.erase(actor);
            }
        }

        void tickPackages()
        {
            for (auto& [actor, active] : running)
            {
                if (active.done || (active.fragment && !*active.fragment) || (combatBows.contains(actor) && !active.bow))
                    continue;
                try
                {
                    if (active.ending)
                    {
                        finishPackage(actor, active);
                        continue;
                    }
                    const auto& tree = *active.tree;
                    if (active.procedure < tree.procedures.size())
                    {
                        const auto& procedure = tree.procedures[active.procedure];
                        const bool activeProcedure = active.prepared
                            || quests.testConditions(active.owner, tree.record, procedure.conditions, actor);
                        if (activeProcedure && !stepProcedure(actor, active, procedure))
                            continue;
                        if (activeProcedure && procedure.completesPackage)
                        {
                            active.procedure = tree.procedures.size();
                            active.forceEnd = true;
                        }
                        else
                            ++active.procedure;
                        active.prepared = false;
                        active.route.reset();
                        continue;
                    }
                    if (tree.repeats && !active.forceEnd)
                    {
                        active.procedure = 0;
                        continue;
                    }
                    const auto& selected = package(active.id);
                    active.fragment = packageFragment(active, actor, selected.vmad.onEnd, selected.endIdle);
                    active.ending = true;
                }
                catch (const std::exception& error)
                {
                    active.done = true;
                    releasePackageMount(actor, active);
                    log.write("PACK " + std::to_string(active.id), error.what());
                }
            }
        }

        void sceneFragment(PlayingScene& active, const Scene& source, std::string scriptName, std::string function,
            std::vector<Value> args = {})
        {
            if (auto fragment = queue(active.scripts, source.record, source.vmad.common, source.owner,
                    {std::move(scriptName), std::move(function), std::move(args)}))
                active.fragments.push_back(std::move(fragment));
        }

        void phaseFragments(PlayingScene& active, const Scene& source, std::uint8_t flag)
        {
            for (const auto& fragment : source.vmad.phases)
                if (fragment.phase == active.phase && (fragment.flags & flag))
                    sceneFragment(active, source, fragment.script, fragment.function,
                        fragment.legacyAliasParameter ? std::vector<Value>{Value{}} : std::vector<Value>{});
        }

        bool startScene(std::uint32_t id, bool force)
        {
            if (playing.contains(id))
                return true;
            const auto& source = scene(id);
            if (!quests.isRunning(source.owner)
                || !quests.testConditions(source.owner, source.record, source.conditions, 0))
                return false;
            PlayingScene active;
            for (const auto& [alias, optional] : source.actors)
            {
                const auto actor = quests.referenceAlias(source.owner, alias);
                if (!actor && !optional && !quests.isOptionalAlias(source.owner, alias))
                    throw std::runtime_error("Required SCEN actor alias is empty: " + std::to_string(alias));
                active.actors.emplace(alias, actor);
            }
            for (auto& [otherId, other] : playing)
            {
                if (other.stop)
                    continue;
                const auto overlap = std::ranges::any_of(active.actors, [&](const auto& a) {
                    return a.second && std::ranges::any_of(other.actors, [&](const auto& b) { return a.second == b.second; });
                });
                if (overlap && !force && !(scene(otherId).flags & 16))
                    return false;
                if (overlap)
                    other.stop = true;
            }
            sceneFragment(active, source, source.vmad.fragmentFile, source.vmad.onBegin);
            finishedActions[id].clear();
            playing.emplace(id, std::move(active));
            events.push_back({id, "SCEN", "Begin", {}, time, {}, 0});
            return true;
        }

        void completeAction(std::uint32_t id, std::uint32_t action, ActionState& state)
        {
            if (state.complete)
                return;
            if (const auto owner = gazeOwners.find(state.actor); owner != gazeOwners.end()
                && owner->second == std::pair{id, action})
            {
                faces.push_back({0, state.actor, 0});
                gazeOwners.erase(owner);
            }
            state.complete = true;
            finishedActions[id].insert(action);
        }

        std::string voiceType(std::uint32_t actor)
        {
            for (auto base = actorBase(actor), depth = 0u; base && depth < 16; ++depth)
            {
                const auto npc = read(base);
                std::uint32_t voice = 0, parent = 0;
                visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("VTCK"))
                        voice = npc.mapFormId(integer(bytes));
                    else if (tag == fourCC("TPLT"))
                        parent = npc.mapFormId(integer(bytes));
                });
                if (voice)
                {
                    std::string result;
                    visitSubrecords(read(voice), [&](auto tag, Bytes bytes) {
                        if (tag == fourCC("EDID"))
                            result = Misc::StringUtils::lowerCase(string(bytes));
                    });
                    return result;
                }
                base = parent;
            }
            return {};
        }

        std::string voiceAsset(const RecordPayload& response, std::uint32_t number, std::string_view voice)
        {
            const auto id = response.metadata.formId;
            const auto local = id & ((id >> 24) == 0xfe ? 0xfff : 0xffffff);
            const auto suffix = std::format("_{:08x}_{}.fuz", local, number);
            const auto prefix = "sound/voice/" + Misc::StringUtils::lowerCase(response.metadata.sourcePlugin) + '/';
            const auto direct = voice.empty() ? prefix : prefix + std::string(voice) + '/';
            for (const auto& path : vfs->getRecursiveDirectoryIterator(direct))
                if (path.value().ends_with(suffix))
                    return std::string(path.value());
            // Overrides can share the originating plugin's recording.
            if (!voice.empty())
            {
                const auto folder = '/' + std::string(voice) + '/';
                for (const auto& path : vfs->getRecursiveDirectoryIterator("sound/voice/"))
                    if (path.value().ends_with(suffix) && path.value().find(folder) != std::string_view::npos)
                        return std::string(path.value());
            }
            throw std::runtime_error("Scene INFO response has no FUZ recording " + suffix + " for " + std::string(voice));
        }

        struct Dialogue
        {
            double duration = 0;
            std::vector<Trace> traces;
            std::vector<WorldScriptAudio> recordings;
        };

        Dialogue dialogue(std::uint32_t topic, std::uint32_t actor, std::uint32_t owner)
        {
            if (!index)
                throw std::runtime_error("SCEN dialogue needs the winning INFO index");
            if (read(topic).metadata.recordType != fourCC("DIAL"))
                throw std::runtime_error("SCEN dialogue action points outside DIAL");
            const auto infos = index->infosByTopic.find(topic);
            if (infos == index->infosByTopic.end())
                return {};
            for (const auto id : infos->second)
            {
                auto info = read(id);
                Conditions conditions;
                std::uint32_t speaker = 0;
                visitSubrecords(info, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("CTDA"))
                        conditions.push_back(bytes);
                    else if (tag == fourCC("ANAM"))
                        speaker = info.mapFormId(integer(bytes));
                });
                if ((speaker && speaker != actorBase(actor)) || !quests.testConditions(owner, info, conditions, actor))
                    continue;
                for (unsigned depth = 0; depth < 16; ++depth)
                {
                    std::uint32_t shared = 0;
                    visitSubrecords(info, [&](auto tag, Bytes bytes) {
                        if (tag == fourCC("DNAM"))
                            shared = info.mapFormId(integer(bytes));
                    });
                    if (!shared)
                        break;
                    if (depth == 15)
                        throw std::runtime_error("SCEN shared INFO response chain exceeds limit");
                    info = read(shared);
                    if (info.metadata.recordType != fourCC("INFO"))
                        throw std::runtime_error("SCEN shared INFO response has wrong type");
                }
                const auto voice = voiceType(actor);
                Dialogue result;
                visitSubrecords(info, [&](auto tag, Bytes bytes) {
                    if (tag != fourCC("TRDT"))
                        return;
                    const auto number = integer(bytes, 12, 1);
                    const auto asset = voiceAsset(info, number, voice);
                    auto found = voiceDurations.find(asset);
                    if (found == voiceDurations.end())
                        found = voiceDurations.emplace(asset, voiceDuration(*vfs->get(VFS::Path::Normalized(asset)))).first;
                    result.recordings.push_back({WorldScriptAudio::Play, 0, actor, asset, result.duration});
                    result.recordings.back().emotion = static_cast<int>(integer(bytes, 0));
                    result.recordings.back().emotionStrength = std::clamp(integer(bytes, 4) / 100.f, 0.f, 1.f);
                    double padding = 0;
                    if (const auto setting = index->floatSettings.find("fgameplayvoicefilepadding"); setting != index->floatSettings.end())
                        padding = std::max(0.f, setting->second);
                    result.duration += found->second + padding;
                    result.traces.push_back({"actor=" + std::to_string(actor) + " topic=" + std::to_string(topic)
                        + " info=" + std::to_string(id) + " duration=" + std::to_string(found->second) + " file=" + asset, 0});
                });
                return result;
            }
            return {};
        }

        void startActions(std::uint32_t id, const Scene& source, PlayingScene& active)
        {
            for (const auto& action : source.actions)
            {
                if (action.first != active.phase)
                    continue;
                const auto actor = active.actors.contains(action.actor) ? active.actors.at(action.actor)
                    : quests.referenceAlias(source.owner, action.actor);
                auto& state = active.actions.try_emplace(action.id, ActionState{actor}).first->second;
                try
                {
                    if (actor && ((action.flags & 0x20000) || action.headtrack >= 0))
                    {
                        const auto target = (action.flags & 0x20000) ? 0x14u
                            : active.actors.contains(action.headtrack) ? active.actors.at(action.headtrack)
                            : quests.referenceAlias(source.owner, action.headtrack);
                        faces.push_back({0, actor, target});
                        gazeOwners[actor] = {id, action.id};
                    }
                    if (action.type == 0 && actor && action.topic)
                    {
                        auto spoken = dialogue(action.topic, actor, source.owner);
                        for (auto& recording : spoken.recordings)
                        {
                            if (action.emotion >= 0)
                            {
                                recording.emotion = action.emotion;
                                recording.emotionStrength = std::clamp(action.emotionValue / 100.f, 0.f, 1.f);
                            }
                            state.recordings.push_back(playSound(std::move(recording)));
                        }
                        if (!spoken.traces.empty())
                            events.push_back({id, "SCEN", "Dialogue", std::move(spoken.traces), time, spoken.duration, 0});
                        state.due = time + spoken.duration;
                    }
                    else if (action.type == 1 && actor)
                    {
                        if (const auto* selected = selectPackage(action.packages, actor, source.owner))
                            startPackage(actor, *selected, source.owner, id, action.id);
                        else
                            completeAction(id, action.id, state);
                    }
                    else if (action.type == 2)
                        state.due = time + action.seconds;
                    else
                        completeAction(id, action.id, state);
                }
                catch (const std::exception& error)
                {
                    log.write("SCEN " + std::to_string(id) + " action " + std::to_string(action.id), error.what());
                }
            }
        }

        std::pair<bool, bool> actionsComplete(std::uint32_t id, const Scene& source, PlayingScene& active)
        {
            bool relevant = false, ready = true;
            for (const auto& action : source.actions)
            {
                auto found = active.actions.find(action.id);
                if (found == active.actions.end())
                    continue;
                auto& state = found->second;
                if ((state.due && time >= *state.due) || finishedActions[id].contains(action.id))
                    completeAction(id, action.id, state);
                if (action.last == active.phase)
                {
                    relevant = true;
                    ready &= state.complete;
                }
            }
            return {relevant, ready};
        }

        void releaseScenePackages(std::uint32_t id, const Scene& source, std::optional<std::size_t> phase)
        {
            for (auto it = gazeOwners.begin(); it != gazeOwners.end();)
            {
                const auto action = std::ranges::find(source.actions, it->second.second, &Action::id);
                if (it->second.first == id && (!phase || (action != source.actions.end() && action->last == *phase)))
                { faces.push_back({0, it->first, 0}); it = gazeOwners.erase(it); }
                else ++it;
            }

            for (auto it = running.begin(); it != running.end();)
            {
                auto& active = it->second;
                const auto action = std::ranges::find(source.actions, active.action, &Action::id);
                if (active.scene != id || (phase && (action == source.actions.end() || action->last != *phase)))
                {
                    ++it;
                    continue;
                }
                const auto actor = it->first;
                if (!active.done)
                    try
                    {
                        const auto& selected = package(active.id);
                        packageFragment(active, actor, selected.vmad.onChange, selected.changeIdle);
                    }
                    catch (const std::exception& error)
                    {
                        log.write("PACK " + std::to_string(active.id), error.what());
                    }
                releasePackageMount(actor, active);
                reevaluate.insert(actor);
                it = running.erase(it);
            }
        }

        bool advanceScene(std::uint32_t id, PlayingScene& active)
        {
            const auto& source = scene(id);
            if (!quests.isRunning(source.owner))
                active.stop = true;
            if (active.stop && active.step != PlayingScene::End)
            {
                stopRecordings(active);
                active.fragments.clear();
                active.step = PlayingScene::End;
                sceneFragment(active, source, source.vmad.fragmentFile, source.vmad.onEnd);
            }
            if (std::ranges::any_of(active.fragments, [](const auto& item) { return !*item; }))
                return false;
            active.fragments.clear();
            switch (active.step)
            {
                case PlayingScene::Begin:
                    active.step = PlayingScene::EnterPhase;
                    break;
                case PlayingScene::EnterPhase:
                    if (active.phase == source.phases.size())
                    {
                        if ((source.flags & 8) && !source.phases.empty()
                            && quests.testConditions(source.owner, source.record, source.conditions, 0))
                        {
                            active.phase = 0;
                            active.actions.clear();
                            finishedActions[id].clear();
                            break;
                        }
                        active.step = PlayingScene::End;
                        sceneFragment(active, source, source.vmad.fragmentFile, source.vmad.onEnd);
                        break;
                    }
                    if (!quests.testConditions(source.owner, source.record, source.phases[active.phase].start, 0))
                    {
                        ++active.phase;
                        break;
                    }
                    events.push_back({id, "SCEN", "PhaseBegin",
                        {{std::to_string(active.phase) + " " + source.phases[active.phase].name, 0}}, time, {}, 0});
                    phaseFragments(active, source, 1);
                    active.step = PlayingScene::Actions;
                    break;
                case PlayingScene::Actions:
                    startActions(id, source, active);
                    active.step = PlayingScene::LeavePhase;
                    break;
                case PlayingScene::LeavePhase:
                {
                    const bool ready = actionsComplete(id, source, active).second;
                    const auto& conditions = source.phases[active.phase].complete;
                    // Explicit completion conditions define when this phase ends.
                    // Finished actions cannot bypass an authored wait-for-arrival gate.
                    if (conditions.empty() ? ready
                        : quests.testConditions(source.owner, source.record, conditions, 0))
                    {
                        releaseScenePackages(id, source, active.phase);
                        phaseFragments(active, source, 2);
                        ++active.phase;
                        active.step = PlayingScene::EnterPhase;
                    }
                    break;
                }
                case PlayingScene::End:
                    if ((source.flags & 2) && quests.isRunning(source.owner))
                    {
                        const auto host = quests.host();
                        if (host.callFormMethod)
                            host.callFormMethod(source.owner, "Stop", {});
                    }
                    events.push_back({id, "SCEN", "End", {}, time, {}, 0});
                    return true;
            }
            return false;
        }

        void tickScenes()
        {
            for (auto it = requestedScenes.begin(); it != requestedScenes.end();)
            {
                try
                {
                    const auto& source = scene(it->first);
                    if (quests.isRunning(source.owner) && !startScene(it->first, it->second))
                    {
                        ++it;
                        continue;
                    }
                }
                catch (const std::exception& error)
                {
                    log.write("SCEN " + std::to_string(it->first), error.what());
                }
                it = requestedScenes.erase(it);
            }
            for (auto id : autostart)
            {
                const auto& source = scene(id);
                if (!quests.isRunning(source.owner))
                    started.erase(id);
                else if (!started.contains(id))
                    try
                    {
                        if (startScene(id, false))
                            started.insert(id);
                    }
                    catch (const std::exception& error)
                    {
                        log.write("SCEN " + std::to_string(id), error.what());
                        started.insert(id);
                    }
            }
            for (auto it = playing.begin(); it != playing.end();)
            {
                const auto id = it->first;
                try
                {
                    if (!advanceScene(id, it->second))
                    {
                        ++it;
                        continue;
                    }
                }
                catch (const std::exception& error)
                {
                    log.write("SCEN " + std::to_string(id), error.what());
                }
                releaseScenePackages(id, scene(id), {});
                stopRecordings(it->second);
                it = playing.erase(it);
            }
        }

        std::int32_t playSound(WorldScriptAudio request)
        {
            if (nextSound == std::numeric_limits<std::int32_t>::max())
                throw std::runtime_error("T5 audio instance limit reached");
            request.instance = nextSound++;
            audio.push_back(std::move(request));
            return audio.back().instance;
        }

        void stopTaggedSound(const std::pair<std::uint32_t, std::string>& key)
        {
            if (const auto instance = taggedSounds.extract(key); !instance.empty())
                audio.push_back({WorldScriptAudio::Stop, instance.mapped().first});
        }

        void playTaggedSound(std::pair<std::uint32_t, std::string> key, std::uint32_t descriptor)
        {
            stopTaggedSound(key);
            const auto sound = callSound(descriptor, {FormReference{key.first}});
            if (sound && std::get<std::int32_t>(*sound) > 0)
                taggedSounds.emplace(std::move(key), std::pair{std::get<std::int32_t>(*sound), audio.back().loop});
        }

        void animationSound(std::uint32_t actor, std::string_view event)
        {
            const auto dot = event.find('.');
            if (!index || dot == std::string_view::npos) return;
            const auto action = event.substr(0, dot);
            if (action != "SoundPlay" && action != "SoundStop") return;
            auto key = std::pair(actor, Misc::StringUtils::lowerCase(event.substr(dot + 1)));
            if (action == "SoundStop")
                stopTaggedSound(key);
            else if (const auto descriptor = index->soundsByEditorId.find(key.second); descriptor != index->soundsByEditorId.end())
                playTaggedSound(std::move(key), descriptor->second);
        }

        void castSounds(std::uint32_t spellId, std::uint32_t caster)
        {
            const auto spell = read(spellId);
            std::set<std::pair<std::uint32_t, std::uint32_t>> sounds;
            visitSubrecords(spell, [&](auto tag, Bytes effectId) {
                if (tag != fourCC("EFID") || effectId.size() != 4) return;
                const auto effect = read(spell.mapFormId(integer(effectId)));
                visitSubrecords(effect, [&](auto type, Bytes rows) {
                    if (type != fourCC("SNDD")) return;
                    for (std::size_t at = 0; at + 8 <= rows.size(); at += 8)
                    {
                        const auto phase = integer(rows, at);
                        const auto id = effect.mapFormId(integer(rows, at + 4));
                        if (id && (phase == 3 || phase == 4)) sounds.emplace(phase, id);
                    }
                });
            });
            for (const auto& [phase, id] : sounds)
                if (phase == 3) callSound(id, {FormReference{caster}});
                else if (shouts.contains(caster))
                    playTaggedSound({caster, "cast:" + std::to_string(id)}, id);
        }

        void stopCastSounds(std::uint32_t caster)
        {
            std::erase_if(taggedSounds, [&](const auto& sound) {
                if (sound.first.first != caster || !sound.first.second.starts_with("cast:")) return false;
                audio.push_back({WorldScriptAudio::Stop, sound.second.first});
                return true;
            });
        }

        void stopRecordings(PlayingScene& active)
        {
            for (auto& [id, action] : active.actions)
            {
                for (auto instance : action.recordings)
                    audio.push_back({WorldScriptAudio::Stop, instance});
                action.recordings.clear();
            }
        }

        std::optional<Value> callSound(std::uint32_t id, const std::vector<Value>& args)
        {
            auto descriptor = read(id);
            if (descriptor.metadata.recordType == fourCC("SOUN"))
            {
                std::uint32_t target = 0;
                visitSubrecords(descriptor, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("SDSC"))
                        target = descriptor.mapFormId(integer(bytes));
                });
                if (!target)
                    return Value{std::int32_t{0}};
                descriptor = read(target);
            }
            if (descriptor.metadata.recordType != fourCC("SNDR"))
                return std::nullopt;
            arity(args, 1, 1);
            WorldScriptAudio request{WorldScriptAudio::Play, 0, formArgument(args, 0)};
            if (!request.source)
                return Value{std::int32_t{0}};
            std::vector<std::string> files;
            std::uint32_t model = 0, category = 0;
            int pitchShift = 0, pitchVariance = 0;
            float attenuation = 0, attenuationVariance = 0;
            visitSubrecords(descriptor, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("ANAM"))
                {
                    auto path = Misc::StringUtils::lowerCase(string(bytes));
                    std::ranges::replace(path, '\\', '/');
                    if (path.starts_with("data/"))
                        path.erase(0, 5);
                    if (!path.empty())
                        files.push_back(path.starts_with("sound/") ? std::move(path) : "sound/" + path);
                }
                else if (tag == fourCC("ONAM"))
                    model = descriptor.mapFormId(integer(bytes));
                else if (tag == fourCC("GNAM"))
                    category = descriptor.mapFormId(integer(bytes));
                else if (tag == fourCC("LNAM") && bytes.size() >= 2)
                    request.loop = (integer(bytes, 1, 1) & 0x38) != 0;
                else if (tag == fourCC("BNAM") && bytes.size() >= 6)
                {
                    pitchShift = static_cast<std::int8_t>(integer(bytes, 0, 1));
                    pitchVariance = std::abs(static_cast<int>(static_cast<std::int8_t>(integer(bytes, 1, 1))));
                    attenuationVariance = static_cast<float>(integer(bytes, 3, 1));
                    attenuation = integer(bytes, 4, 2) / 100.f;
                }
            });
            std::erase_if(files, [&](const auto& file) { return !vfs->exists(VFS::Path::Normalized(file)); });
            if (files.empty())
            {
                log.write("Sound.Play " + std::to_string(id), "SNDR has no available audio files");
                return Value{std::int32_t{0}};
            }
            request.path = files[std::uniform_int_distribution<std::size_t>(0, files.size() - 1)(soundRandom)];
            const auto variance = [&](float amount) {
                return amount > 0.f ? std::uniform_real_distribution<float>(-amount, amount)(soundRandom) : 0.f;
            };
            request.pitch = std::max(.01f, 1.f + (pitchShift + variance(static_cast<float>(pitchVariance))) / 100.f);
            request.volume = std::min(1.f, std::pow(10.f, -(attenuation + variance(attenuationVariance)) / 20.f));
            for (int depth = 0; category && depth < 8; ++depth)
            {
                const auto soundCategory = read(category);
                if (soundCategory.metadata.recordType != fourCC("SNCT"))
                    break;
                category = 0;
                visitSubrecords(soundCategory, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("VNAM") && bytes.size() >= 2)
                        request.volume *= integer(bytes, 0, 2) / 65535.f;
                    else if (tag == fourCC("PNAM"))
                        category = soundCategory.mapFormId(integer(bytes));
                });
            }
            if (model)
            {
                const auto output = read(model);
                if (output.metadata.recordType != fourCC("SOPM"))
                    throw std::runtime_error("SNDR output model is not SOPM");
                bool attenuates = false;
                visitSubrecords(output, [&](auto tag, Bytes bytes) {
                    if (tag == fourCC("NAM1"))
                        attenuates = (integer(bytes, 0, 1) & 1) != 0;
                    else if (tag == fourCC("MNAM"))
                        request.spatial = integer(bytes) == 0;
                    else if (tag == fourCC("ANAM") && bytes.size() >= 12)
                    {
                        request.minDistance = real(bytes, 4);
                        request.maxDistance = real(bytes, 8);
                    }
                });
                if (!attenuates)
                    request.minDistance = request.maxDistance = 0;
            }
            return Value{playSound(std::move(request))};
        }

        std::optional<Value> callSoundStatic(std::string_view method, const std::vector<Value>& args)
        {
            const auto name = Misc::StringUtils::lowerCase(method);
            if (name != "stopinstance" && name != "setinstancevolume")
                return std::nullopt;
            const bool stop = name == "stopinstance";
            arity(args, stop ? 1 : 2, stop ? 1 : 2);
            const auto instance = argument<std::int32_t>(args, 0);
            WorldScriptAudio request{stop ? WorldScriptAudio::Stop : WorldScriptAudio::Volume, instance};
            if (!stop)
                request.volume = std::clamp(number(args, 1), 0.f, 1.f);
            if (instance > 0)
                audio.push_back(std::move(request));
            return Value{};
        }

        std::int32_t factionRank(std::uint32_t actor, std::uint32_t faction) const
        {
            if (const auto rank = factionOverrides.find({actor, faction}); rank != factionOverrides.end())
                return rank->second;
            auto base = actorBase(actor);
            for (unsigned depth = 0; base && depth < 16; ++depth)
            {
                const auto npc = read(base);
                if (npc.metadata.recordType != fourCC("NPC_")) break;
                std::uint32_t parent = 0, inheritance = 0;
                std::int32_t rank = -1;
                visitSubrecords(npc, [&](auto tag, Bytes data) {
                    if (tag == fourCC("TPLT")) parent = npc.mapFormId(integer(data));
                    if (tag == fourCC("ACBS") && data.size() >= 20) inheritance = integer(data, 18, 2);
                    if (tag == fourCC("SNAM") && data.size() >= 5 && npc.mapFormId(integer(data)) == faction)
                        rank = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(integer(data, 4, 1)));
                });
                if (!parent || !(inheritance & 4)) return rank;
                base = parent;
            }
            return -1;
        }

        std::optional<Value> callForm(std::uint32_t id, std::string_view method, const std::vector<Value>& args)
        {
            const auto name = Misc::StringUtils::lowerCase(method);
            if (name == "getrelationshiprank" || name == "setrelationshiprank")
            {
                const bool write = name == "setrelationshiprank";
                arity(args, write ? 2 : 1, write ? 2 : 1);
                const auto other = formArgument(args, 0);
                const auto base = actorBase(id), otherBase = actorBase(other);
                if (!base || !otherBase) return write ? Value{} : Value(std::int32_t(0));
                const auto pair = std::pair(std::min(id, other), std::max(id, other));
                if (write)
                {
                    relationshipOverrides[pair] = std::clamp(argument<std::int32_t>(args, 1), -4, 4);
                    return Value{};
                }
                if (const auto rank = relationshipOverrides.find(pair); rank != relationshipOverrides.end())
                    return Value(rank->second);
                if (index)
                    if (const auto rank = index->relationshipRanks.find({std::min(base, otherBase), std::max(base, otherBase)});
                        rank != index->relationshipRanks.end())
                        return Value(rank->second);
                return Value(std::int32_t(0));
            }
            if (name == "isinfaction" || name == "getfactionrank" || name == "addtofaction"
                || name == "removefromfaction" || name == "setfactionrank")
            {
                const bool setRank = name == "setfactionrank";
                arity(args, setRank ? 2 : 1, setRank ? 2 : 1);
                const auto faction = formArgument(args, 0);
                const auto rank = factionRank(id, faction);
                if (name == "isinfaction") return Value(rank >= 0);
                if (name == "getfactionrank") return Value(rank);
                if (faction && actorBase(id))
                    factionOverrides[{id, faction}] = setRank ? argument<std::int32_t>(args, 1)
                        : name == "addtofaction" ? std::max(rank, 0) : -1;
                return Value{};
            }
            if (name == "setghost" || name == "isghost")
            {
                const auto base = actorBase(id);
                if (!base) return std::nullopt;
                const bool write = name == "setghost";
                arity(args, write, write);
                if (write)
                {
                    ghostOverrides[id] = argument<bool>(args, 0);
                    return Value{};
                }
                const auto it = ghostOverrides.find(id);
                return Value(it == ghostOverrides.end() ? (actorHealth(base).second & 0x20000000) != 0 : it->second);
            }
            if (name == "setlookat" || name == "clearlookat" || name == "setheadtracking"
                || name == "setexpressionoverride" || name == "clearexpressionoverride")
            {
                if (name == "setlookat")
                {
                    arity(args, 1, 2);
                    faces.push_back({0, id, formArgument(args, 0)});
                    gazeOwners[id] = {0, 0};
                }
                else if (name == "clearlookat")
                {
                    arity(args, 0, 0); faces.push_back({0, id, 0}); gazeOwners.erase(id);
                }
                else if (name == "setheadtracking")
                {
                    arity(args, 1, 1); faces.push_back({1, id, 0, argument<bool>(args, 0) ? 1.f : 0.f});
                }
                else
                {
                    const bool clear = name == "clearexpressionoverride";
                    arity(args, clear ? 0 : 1, clear ? 0 : 2);
                    faces.push_back({2, id, clear ? ~0u : static_cast<std::uint32_t>(argument<std::int32_t>(args, 0)),
                        clear ? 0.f : args.size() > 1 ? std::clamp(number(args, 1)/100.f, 0.f, 1.f) : 1.f});
                }
                return Value{};
            }
            if ((name == "add" || name == "remove") && args.empty() && music.set(read(id), name == "add"))
                return Value{};
            if (name == "setactorvalue" || name == "setav" || name == "getactorvalue" || name == "getav"
                || name == "getbaseactorvalue" || name == "getbaseav")
            {
                const bool write = name == "setactorvalue" || name == "setav";
                arity(args, write ? 2 : 1, write ? 2 : 1);
                const auto key = argument<std::string>(args, 0);
                const auto slot = std::ranges::find_if(scriptActorValues,
                    [&](auto value) { return Misc::StringUtils::ciEqual(value, key); });
                const bool health = Misc::StringUtils::ciEqual(key, "Health");
                const auto base = actorBase(id);
                if ((!health && slot == scriptActorValues.end()) || !base)
                    return std::nullopt;
                const auto value = health ? healthValue
                    : firstScriptActorValue + static_cast<std::uint32_t>(slot - scriptActorValues.begin());
                if (!write && health && (name.starts_with("getbase") || !actorValues.contains({id, healthValue})))
                    return Value(actorHealth(base).first);
                if (!write)
                    return Value(*getActorValue(id, value));
                const auto amount = number(args, 1);
                if (!std::isfinite(amount))
                    throw std::runtime_error("Actor.SetActorValue requires a finite value");
                actorValues.insert_or_assign({id, value}, amount);
                return Value{};
            }
            if (name == "haskeyword")
            {
                arity(args, 1, 1);
                return Value{hasKeyword(id, formArgument(args, 0))};
            }
            if (name == "getcombattarget")
            {
                arity(args, 0, 0);
                const auto active = running.find(id);
                if (active != running.end() && active->second.bow)
                    return formValue(active->second.bow->target);
                if (const auto target = combatTargets.find(id); target != combatTargets.end())
                    return formValue(target->second);
                const auto combat = combatBows.find(id);
                return formValue(combat != combatBows.end() && combat->second && combat->second->repeat ? combat->second->target : 0);
            }
            if (name == "isincombat")
            {
                arity(args, 0, 0);
                return Value(combatTargets.contains(id) || combatBows.contains(id));
            }
            if (name == "startcombat")
            {
                arity(args, 1, 1);
                const auto target = formArgument(args, 0);
                const auto base = actorBase(id);
                if (!base || !target || target == id || !actorBase(target) || deadActors.contains(id) || deadActors.contains(target))
                    return Value{};
                const auto current = combatTargets.find(id);
                if (current == combatTargets.end() || current->second != target)
                {
                    combatTargets.insert_or_assign(id, target);
                    reevaluate.insert(id);
                    events.push_back({id, "Actor", "StartCombat", {{"target=" + std::to_string(target), 0}}, time, {}, 0});
                }
                if (auto combat = combatBows.find(id); combat != combatBows.end() && combat->second)
                {
                    if (combat->second->target != target)
                    {
                        bowPositions.erase(id);
                        bowDestinations.erase(id);
                    }
                    combat->second->target = target;
                    return Value{};
                }
                const auto weapon = readArrowLoadout(read, base);
                if (!weapon)
                    return Value{};
                auto& attack = combatBows[id].emplace(BowAttack{*weapon, target});
                attack.repeat = true;
                attack.holdWhenBlocked = true;
                stopLocomotion(id);
                events.push_back({id, "Archery", "StartCombat", {{"target=" + std::to_string(target), 0}}, time, {}, 0});
                return Value{};
            }
            if (name == "stopcombat" && actorBase(id))
            {
                arity(args, 0, 0);
                if (combatTargets.erase(id))
                {
                    reevaluate.insert(id);
                    events.push_back({id, "Actor", "StopCombat", {}, time, {}, 0});
                }
                bowPositions.erase(id);
                bowDestinations.erase(id);
                if (const auto bow = combatBows.find(id); bow != combatBows.end() && bow->second)
                {
                    auto& attack = *bow->second;
                    attack.repeat = false;
                    attack.phase = BowAttack::Recover;
                    attack.animationDone = false;
                    playBowClip(id, attack, "bow_unequip");
                }
                return Value{};
            }
            if (name == "pushactoraway")
            {
                arity(args, 2, 2);
                const auto target = formArgument(args, 0);
                const auto source = placement(id);
                const auto victim = placement(target);
                const float force = number(args, 1);
                if (!std::isfinite(force))
                    throw std::runtime_error("PushActorAway force must be finite");
                if (!source || !victim || force == 0 || !actorBase(target) || deadActors.contains(target) || attachments.contains(target))
                    return Value{};
                auto direction = victim->position.asVec3() - source->position.asVec3();
                if (direction.normalize() == 0)
                    direction.set(std::sin(source->position.rot[2]), std::cos(source->position.rot[2]), 0);
                float maxForce = std::numeric_limits<float>::max();
                if (index)
                    if (const auto setting = index->floatSettings.find("fknockbackforcemax"); setting != index->floatSettings.end())
                        maxForce = std::max(0.f, setting->second);
                auto velocity = direction * (std::clamp(force, -maxForce, maxForce) * Constants::UnitsPerMeter);
                velocity.z() = std::max(0.f, velocity.z());
                fallingActors.insert_or_assign(target, FallingActor{id, velocity, victim->position.pos[2]});
                physicsDriven.insert(target);
                events.push_back({id, "Actor", "PushActorAway", {{std::format("target={} force={}", target, force), 0}}, time, {}, 0});
                return Value{};
            }
            if (name == "play")
                return callSound(id, args);
            if (name == "movetomyeditorlocation")
            {
                arity(args, 0, 0);
                const auto pose = authoredPlacement(id);
                if (!pose)
                    return std::nullopt;
                attachments.erase(id);
                occupiedPerches.erase(id);
                enteringExecutionBlock.erase(id);
                relocations.erase(id);
                if (auto movedRef = movingReferences.find(id); movedRef != movingReferences.end())
                    movedRef->second.anchor = 0;
                move(id, *pose);
                if (!triggerBoxes.empty())
                    teleported.insert(id);
                return Value{};
            }
            if (name == "moveto")
            {
                arity(args, 1, 5);
                const auto target = formArgument(args, 0);
                auto pose = placement(target);
                if (!pose)
                    throw std::runtime_error("ObjectReference.MoveTo target has no position");
                if (args.size() == 5 && !argument<bool>(args, 4))
                    if (const auto original = placement(id))
                        std::ranges::copy(original->position.rot, pose->position.rot);
                for (unsigned axis = 0; axis < 3; ++axis)
                    pose->position.pos[axis] += number(args, axis + 1);
                attachments.erase(id);
                occupiedPerches.erase(id);
                enteringExecutionBlock.erase(id);
                move(id, *pose, target);
                if (!triggerBoxes.empty())
                    teleported.insert(id);
                if (target != id)
                    relocations.insert_or_assign(id, target);
                return Value{};
            }
            if (name == "setvehicle" || name == "tethertohorse")
            {
                arity(args, 1, 1);
                const auto parent = formArgument(args, 0);
                if (name == "tethertohorse" && parent)
                {
                    const auto cart = placement(id);
                    auto horse = placement(parent);
                    if (!cart || !horse)
                        throw std::runtime_error("TetherToHorse requires loaded cart and horse transforms");
                    // The native first aligns the horse to the carriage. Preserving the
                    // arbitrary editor placements instead leaves it beside/outside the shafts.
                    const auto offset = Misc::Convert::makeOsgQuat(cart->position) * osg::Vec3d(cartHorseOffset);
                    horse->position.pos[0] = cart->position.pos[0] + offset.x();
                    horse->position.pos[1] = cart->position.pos[1] + offset.y();
                    horse->position.pos[2] += cartHorseOffset.z();
                    for (unsigned axis = 0; axis < 3; ++axis)
                        horse->position.rot[axis] = cart->position.rot[axis];
                    move(parent, *horse);
                }
                attach(id, parent);
                if (name == "setvehicle" && parent && actorBase(id) && !actorBase(parent))
                    normalizedVehicleActors.insert(id);
                return Value{};
            }
            if (name == "setmotiontype")
            {
                arity(args, 1, 2);
                const auto type = argument<std::int32_t>(args, 0);
                if (type < 1 || type > 7)
                    throw std::runtime_error("ObjectReference.SetMotionType expects a motion type between 1 and 7");
                if (args.size() == 2 && !argument<bool>(args, 1))
                    throw std::runtime_error("ObjectReference.SetMotionType activation suppression needs a UE interaction hook");
                motionTypes.insert_or_assign(id, type);
                if (type == 4 || type == 5)
                    fixedReferences.insert(id);
                else
                    fixedReferences.erase(id);
                return Value{};
            }
            if (name == "is3dloaded")
            {
                arity(args, 0, 0);
                return Value(loaded.contains(id));
            }
            if (name == "getparentcell")
            {
                arity(args, 0, 0);
                const auto pose = placement(id);
                return formValue(pose ? pose->cell : 0);
            }
            if (name == "getpositionx" || name == "getpositiony" || name == "getpositionz" || name == "setposition")
            {
                const bool setter = name == "setposition";
                arity(args, setter ? 3 : 0, setter ? 3 : 0);
                auto pose = placement(id);
                if (!pose) return std::nullopt;
                if (!setter) return Value(pose->position.pos[name.back() - 'x']);
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    pose->position.pos[axis] = number(args, axis);
                    if (!std::isfinite(pose->position.pos[axis]))
                        throw std::runtime_error("SetPosition coordinates must be finite");
                }
                move(id, *pose);
                return Value{};
            }
            if (name == "getdistance")
            {
                arity(args, 1, 1);
                const auto other = formArgument(args, 0);
                const auto source = placement(id);
                const auto target = placement(other);
                if (!source || !target)
                    return std::nullopt;
                return Value((source->position.asVec3() - target->position.asVec3()).length());
            }
            if (name == "getlinkedref")
            {
                arity(args, 0, 1);
                return formValue(linked(id, formArgument(args, 0)));
            }
            if (name == "gettriggerobjectcount")
            {
                arity(args, 0, 0);
                const auto found = triggerInside.find(id);
                return Value(std::int32_t(found == triggerInside.end() ? 0 : found->second.size()));
            }
            if (name == "enablelinkchain" || name == "disablelinkchain")
            {
                const bool disable = name == "disablelinkchain";
                arity(args, 0, disable ? 2 : 1);
                const auto keyword = formArgument(args, 0);
                const bool fade = args.size() == 2 && argument<bool>(args, 1);
                const auto host = quests.host();
                std::set<std::uint32_t> seen{id};
                for (auto ref = linked(id, keyword); ref && seen.insert(ref).second; ref = linked(ref, keyword))
                    if (host.setDisabled)
                        host.setDisabled(ref, disable, fade);
                return Value{};
            }
            if (name == "getactorbase")
            {
                arity(args, 0, 0);
                return formValue(actorBase(id));
            }
            if (name == "isdead" || name == "kill")
            {
                const bool kill = name == "kill";
                arity(args, 0, kill ? 1 : 0);
                if (!actorBase(id))
                    return std::nullopt;
                if (!kill)
                    return Value(isDead(id));
                if (!isDead(id))
                    killActor(id, formArgument(args, 0));
                return Value{};
            }
            if (name == "playidle")
            {
                arity(args, 1, 1);
                return Value(idle(id, formArgument(args, 0)));
            }
            if (name == "setallowflying" && actorBase(id))
            {
                arity(args, 1, 1);
                if (argument<bool>(args, 0)) noFlight.erase(id);
                else noFlight.insert(id);
                return Value{};
            }
            if (name == "evaluatepackage")
            {
                arity(args, 0, 0);
                explicitReevaluate.insert(id);
                events.push_back({id, "AI", "EvaluatePackage", {}, time, {}, 0});
                return Value{};
            }
            if (name == "getcurrentpackage")
            {
                arity(args, 0, 0);
                const auto found = running.find(id);
                return formValue(found == running.end() ? 0 : found->second.id);
            }
            if (name == "getowningquest")
            {
                arity(args, 0, 0);
                return formValue(owningQuest(id));
            }
            if (name == "gettemplate")
            {
                arity(args, 0, 0);
                return formValue(package(id).templateId);
            }
            if (name != "start" && name != "forcestart" && name != "stop" && name != "isplaying"
                && name != "isactioncomplete")
                return std::nullopt;
            if (read(id).metadata.recordType != fourCC("SCEN"))
                return std::nullopt;
            if (name == "isactioncomplete")
            {
                arity(args, 1, 1);
                return Value(finishedActions[id].contains(static_cast<std::uint32_t>(argument<std::int32_t>(args, 0))));
            }
            arity(args, 0, 0);
            if (name == "isplaying")
                return Value(playing.contains(id) && !playing.at(id).stop);
            if (name == "stop")
            {
                requestedScenes.erase(id);
                if (const auto found = playing.find(id); found != playing.end())
                    found->second.stop = true;
            }
            else
            {
                std::ostringstream status;
                status << "owner=" << scene(id).owner << " running=" << quests.isRunning(scene(id).owner);
                for (const auto& [other, active] : playing)
                {
                    status << " scene=" << other << ":phase=" << active.phase << ":step=" << active.step << ":stop=" << active.stop;
                    for (const auto& [actor, pack] : running)
                        if (pack.scene == other)
                            status << " actor=" << actor << ":pack=" << pack.id << ":distance=" << pack.distance << '/' << pack.length
                                << ":anim=" << animationDriven.contains(actor) << ":physics=" << physicsDriven.contains(actor)
                                << ":fixed=" << fixedReferences.contains(actor) << ":done=" << pack.done;
                }
                events.push_back({id, "SCEN", "StartRequested", {{status.str(), 0}}, time, {}, 0});
                if (const bool force = name == "forcestart"; !startScene(id, force))
                    requestedScenes.insert_or_assign(id, force);
            }
            return Value{};
        }

        std::optional<float> getActorValue(std::uint32_t actor, std::uint32_t value) const
        {
            if (value != healthValue && (value < firstScriptActorValue || value - firstScriptActorValue >= scriptActorValues.size()))
                return std::nullopt;
            if (const auto found = actorValues.find({actor, value}); found != actorValues.end())
                return found->second;
            if (value == healthValue && actor)
                return actorHealth(actorBase(actor)).first;
            return 0.f;
        }

        void tick(double now)
        {
            delta = std::max(0., now - time);
            time = now;
            std::erase_if(fired, [&](const auto& shot) { return shot.second.expires < time; });
            std::erase_if(arrowVictims, [&](const auto& victim) { return victim.second < time; });
            tickFragments();
            tickScenes();
            const auto changed = quests.takeChangedPackageQuests();
            if (!changed.empty())
            {
                // Include actors whose previous package failed before it could be
                // registered. Otherwise a fixed native cannot recover their AI.
                for (const auto actor : loaded)
                    for (const auto id : quests.packagesForActor(actor))
                        if (changed.contains(quests.packageQuestForActor(actor, id))) { reevaluate.insert(actor); break; }
            }
            const auto explicitRequests = std::exchange(explicitReevaluate, {});
            reevaluate.insert(explicitRequests.begin(), explicitRequests.end());
            for (const auto actor : std::exchange(reevaluate, {}))
                try
                {
                    evaluate(actor, explicitRequests.contains(actor));
                }
                catch (const std::exception& error)
                {
                    log.write("Actor.EvaluatePackage " + std::to_string(actor), error.what());
                }
            tickPackages();
            const auto host = quests.host();
            std::erase_if(combatTargets, [&](const auto& entry) {
                return deadActors.contains(entry.first) || deadActors.contains(entry.second)
                    || (host.isDisabled && (host.isDisabled(entry.first) || host.isDisabled(entry.second)));
            });
            std::erase_if(combatBows, [&](auto& entry) {
                if (host.isDisabled && host.isDisabled(entry.first))
                {
                    arrows.push_back({WorldScriptArrow::Lower, entry.first});
                    bowPositions.erase(entry.first);
                    bowDestinations.erase(entry.first);
                    return true;
                }
                const auto active = running.find(entry.first);
                if ((active != running.end() && active->second.bow) || !stepBow(entry.first, entry.second, 0, true))
                    return false;
                bowPositions.erase(entry.first);
                bowDestinations.erase(entry.first);
                return true;
            });
            updateAttachments();
            for (const auto& [actor, fall] : fallingActors)
                if (loaded.contains(actor) && delta > 0)
                    fallRequests.push_back({actor, fall.velocity, delta});
            tickTriggers();
            music.tick(time, [this](WorldScriptAudio request) { return playSound(std::move(request)); }, audio, events);
        }
    };

    WorldScripts::WorldScripts(ReadWinningRecord read, std::shared_ptr<const VFS::Manager> vfs, QuestScripts& quests,
        Papyrus::ErrorLog& log) : mImpl(std::make_unique<Impl>(std::move(read), std::move(vfs), quests, log))
    {
    }

    WorldScripts::~WorldScripts() = default;

    void WorldScripts::setRecordIndex(std::shared_ptr<const WinningRecordIndex> index) { mImpl->index = std::move(index); }

    void WorldScripts::addScenes(std::span<const std::uint32_t> sceneIds)
    {
        for (auto id : sceneIds)
            if (mImpl->seen.insert(id).second)
                try
                {
                    if (mImpl->scene(id).flags & 1)
                        mImpl->autostart.insert(id);
                }
                catch (const std::exception& error)
                {
                    mImpl->log.write("SCEN " + std::to_string(id), error.what());
                }
    }

    void WorldScripts::addPlacements(const WorldScriptTransforms& placements)
    {
        mImpl->loaded.clear();
        if (mImpl->authored.contains(0x14))
            mImpl->loaded.insert(0x14);
        for (const auto& [id, pose] : placements)
        {
            mImpl->authored.insert_or_assign(id, pose);
            mImpl->loaded.insert(id);
        }
    }

    void WorldScripts::setExteriorCells(std::uint32_t world, std::span<const ExteriorCell> cells)
    {
        mImpl->setExteriorCells(world, cells);
    }

    void WorldScripts::teleportPlayer(const WorldScriptPose& pose, std::uint32_t world)
    {
        if (world && pose.cell)
            mImpl->cellWorlds.insert_or_assign(pose.cell, world);
        mImpl->attachments.erase(0x14);
        mImpl->animationDriven.erase(0x14);
        mImpl->move(0x14, pose);
        mImpl->loaded.insert(0x14);
        mImpl->teleported.insert(0x14);
    }

    bool WorldScripts::isDead(std::uint32_t actor) const { return mImpl->isDead(actor); }
    bool WorldScripts::wasKilled(std::uint32_t actor) const { return mImpl->deadActors.contains(actor); }

    void WorldScripts::setTriggerActorBounds(std::uint32_t actor, const osg::BoundingBoxd& bounds)
    {
        if (bounds.valid()) mImpl->triggerActorBounds.insert_or_assign(actor, bounds);
    }

    bool WorldScripts::strike(std::uint32_t target, std::uint32_t source, std::uint32_t weapon, float damage)
    {
        if (!mImpl->loaded.contains(target) || !mImpl->actorBase(target) || mImpl->isDead(target)) return false;
        mImpl->quests.actorHit(target, source, weapon, 0);
        mImpl->arrowVictims.insert_or_assign(target, mImpl->time + 2.);
        mImpl->damageActor(target, source, damage);
        mImpl->events.push_back({target, "Weapon", "PlayerHit",
            {{std::format("T5_PLAYER_HIT target={:08X} weapon={:08X} damage={}", target, weapon, damage), 0}},
            mImpl->time, {}, 0});
        return true;
    }

    std::uint32_t WorldScripts::registerPlayerArrow(const ArrowLoadout& loadout)
    {
        if (mImpl->nextArrow >= 0xff800000u) throw std::runtime_error("Arrow handle space exhausted");
        const auto shot = mImpl->nextArrow++;
        mImpl->fired.emplace(shot, Impl::FiredArrow{0x14, 0, loadout.bow, loadout.projectile,
            loadout.damage, mImpl->time + (loadout.lifetime > 0 ? loadout.lifetime : loadout.range / loadout.speed) + 1.});
        if (loadout.releaseSound) mImpl->callSound(loadout.releaseSound, {Papyrus::FormReference{0x14}});
        return shot;
    }

    std::optional<std::pair<float, float>> WorldScripts::resourcePool(std::uint32_t actor, std::uint32_t value) const
    {
        if (value < 24 || value > 26) return std::nullopt;
        const auto base = mImpl->actorBase(actor);
        if (!base) return std::nullopt;
        float maximum = value == 24 ? mImpl->actorHealth(base).first : 0.f;
        if (value != 24)
        {
            const auto npc = mImpl->read(base);
            visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                if (tag != ESM::fourCC("RNAM")) return;
                const auto race = mImpl->read(npc.mapFormId(integer(bytes)));
                visitSubrecords(race, [&](auto type, Bytes data) {
                    const auto offset = 36 + (value - 24) * 4;
                    if (type == ESM::fourCC("DATA") && data.size() >= offset + 4) maximum = real(data, offset);
                });
            });
        }
        if (!std::isfinite(maximum) || maximum <= 0) return std::nullopt;
        const auto current = mImpl->actorValues.find({actor, value});
        return std::pair{current == mImpl->actorValues.end() ? maximum : current->second, maximum};
    }

    bool WorldScripts::restoreActorValue(std::uint32_t actor, std::uint32_t value, float amount)
    {
        if (!(amount > 0) || !std::isfinite(amount) || isDead(actor)) return false;
        const auto pool = resourcePool(actor, value);
        if (!pool) return false;
        const auto [previous, maximum] = *pool;
        auto& current = mImpl->actorValues.try_emplace(std::pair{actor, value}, maximum).first->second;
        current = std::min(maximum, previous + amount);
        return true;
    }

    void WorldScripts::setTriggers(
        std::span<const PreparedSceneScript> scripts, const std::map<std::uint32_t, std::uint32_t>& actors,
        const std::map<std::uint32_t, osg::BoundingBoxd>& bounds)
    {
        mImpl->setTriggers(scripts, actors, bounds);
    }

    std::optional<WorldScriptPose> WorldScripts::authoredPlacement(std::uint32_t id) const
    {
        return mImpl->authoredPlacement(id);
    }
    std::uint32_t WorldScripts::editorCell(std::uint32_t id) const
    {
        const auto pose = mImpl->authoredPlacement(id);
        return pose ? mImpl->exteriorCell(id, *pose) : 0;
    }
    std::optional<WorldScriptPose> WorldScripts::placement(std::uint32_t id) const { return mImpl->placement(id); }
    const WorldScriptTransforms& WorldScripts::transforms() const { return mImpl->moved; }
    const std::map<std::uint32_t, std::uint32_t>& WorldScripts::relocations() const { return mImpl->relocations; }
    const std::map<std::uint32_t, WorldScriptMovingReference>& WorldScripts::movingReferences() const
    {
        return mImpl->movingReferences;
    }
    std::uint32_t WorldScripts::actorBase(std::uint32_t id) { return mImpl->actorBase(id); }
    void WorldScripts::animationPose(std::uint32_t actor, const ESM::Position& position, bool active)
    {
        if (auto pose = mImpl->placement(actor))
        {
            if (mImpl->flightGrabs.contains(actor) && mImpl->delta > 0)
                mImpl->carrierVelocities.insert_or_assign(actor,
                    (position.asVec3() - pose->position.asVec3()) / static_cast<float>(mImpl->delta));
            pose->position = position;
            mImpl->move(actor, *pose);
            if (active) mImpl->animationDriven.insert(actor);
            else mImpl->animationDriven.erase(actor);
        }
    }

    void WorldScripts::fallResult(std::uint32_t actor, const osg::Vec3f& position, const osg::Vec3f& velocity, bool grounded)
    {
        const auto found = mImpl->fallingActors.find(actor);
        auto pose = mImpl->placement(actor);
        if (found == mImpl->fallingActors.end() || !pose) return;
        for (unsigned axis = 0; axis < 3; ++axis) pose->position.pos[axis] = position[axis];
        mImpl->move(actor, *pose);
        auto& fall = found->second;
        fall.highestZ = std::max(fall.highestZ, position.z());
        fall.velocity = velocity;
        if (!grounded) return;
        mImpl->physicsDriven.erase(actor);
        const auto setting = [&](std::string_view name) {
            if (!mImpl->index) return 0.f;
            const auto& settings = mImpl->index->floatSettings;
            const auto it = settings.find(std::string(name) + (actor == 0x14 ? "" : "npc"));
            return it != settings.end() ? it->second : 0.f;
        };
        const float height = fall.highestZ - position.z();
        const float excess = std::max(0.f, height - setting("fjumpfallheightmin"));
        const float damage = excess > 0 ? setting("fjumpfallheightmult") * std::pow(excess, setting("fjumpfallheightexponent")) : 0.f;
        const auto source = fall.source;
        mImpl->fallingActors.erase(found);
        if (const auto base = mImpl->actorBase(actor); base && damage > 0 && !mImpl->deadActors.contains(actor))
        {
            const auto [health, flags] = mImpl->actorHealth(base);
            if (health > 0 && !(flags & 0xA0000000))
            {
                auto& current = mImpl->actorValues.try_emplace(std::pair{actor, Impl::healthValue}, health).first->second;
                current = std::max((flags & 2) || ((flags & 0x800) && source != 0x14) ? 1.f : 0.f, current - damage);
                if (current <= 0)
                {
                    mImpl->killActor(actor, source);
                    mImpl->animations.push_back({actor, 0, {}, "T5Death", "actors/character/animations/deathanimationa.hkx"});
                }
            }
        }
        mImpl->events.push_back({actor, "Physics", "FallImpact", {{std::format("height={} damage={}", height, damage), 0}},
            mImpl->time, {}, 0});
    }

    void WorldScripts::firingPosition(std::uint32_t actor, std::uint32_t target, const osg::Vec3f& position)
    {
        const auto attack = mImpl->combatBows.find(actor);
        auto pose = mImpl->placement(actor);
        if (!pose || attack == mImpl->combatBows.end() || !attack->second || attack->second->target != target
            || attack->second->phase == Impl::BowAttack::Recover) return;
        for (unsigned axis = 0; axis < 3; ++axis) pose->position.pos[axis] = position[axis];
        mImpl->bowPositions.insert_or_assign(actor, Impl::BowPosition{});
        mImpl->bowDestinations.insert_or_assign(actor, *pose);
        mImpl->arrows.push_back({WorldScriptArrow::Lower, actor});
        mImpl->events.push_back({actor, "Archery", "Reposition", {{std::format("target={} position=({},{},{})",
            target, position.x(), position.y(), position.z()), 0}}, mImpl->time, {}, 0});
    }
    int WorldScripts::motionType(std::uint32_t reference) const
    {
        const auto it=mImpl->motionTypes.find(reference);
        return it==mImpl->motionTypes.end()?0:it->second;
    }
    void WorldScripts::physicsPose(std::uint32_t reference, const ESM::Position& position, bool active)
    {
        if (auto pose = mImpl->placement(reference))
        {
            pose->position = position;
            mImpl->move(reference, *pose);
            if (active) mImpl->physicsDriven.insert(reference);
            else mImpl->physicsDriven.erase(reference);
        }
    }
    void WorldScripts::animationEvent(std::uint32_t actor, std::string_view event)
    {
        if (const auto furniture = mImpl->occupiedPerches.find(actor); furniture != mImpl->occupiedPerches.end())
        {
            const auto host = mImpl->quests.host();
            if (host.callFormMethod)
                host.callFormMethod(furniture->second, "PlayFurnitureAnimation", {std::string(event)});
        }
        mImpl->animationSound(actor, event);
        mImpl->bowEvent(actor, event);
        mImpl->flightAnimationEvent(actor, event);
        if (event == "ExecutionNextClip" && mImpl->executionChops.erase(actor))
            mImpl->animations.push_back({actor, 0, "actors/character/behaviors/0_master.hkx",
                "Behavior37/AOExecutioneeChop_1stPerson::ExecutionNextClip"});
        if (Misc::StringUtils::ciEqual(event, "T5ClipEnd"))
        {
            if (const auto entering = mImpl->enteringExecutionBlock.extract(actor); !entering.empty())
                mImpl->furnitureActivations.emplace_back(entering.mapped(), actor);
            if (const auto active = mImpl->running.find(actor); active != mImpl->running.end())
                if (!mImpl->shouts.contains(actor) && active->second.prepared && active->second.procedure < active->second.tree->procedures.size()
                    && active->second.tree->procedures[active->second.procedure].name == "Shout") active->second.wait = 0;
        }

        if (Misc::StringUtils::ciEqual(event, "ExitCartEnd") && mImpl->attachments.erase(actor))
            mImpl->events.push_back({actor, "Animation", "ExitCartEnd", {}, mImpl->time, {}, 0});
    }
    std::optional<float> WorldScripts::getActorValue(std::uint32_t actor, std::uint32_t value) const
    {
        return mImpl->getActorValue(actor, value);
    }
    std::uint32_t WorldScripts::vehicle(std::uint32_t id) const
    {
        const auto found = mImpl->attachments.find(id);
        return found == mImpl->attachments.end() ? 0 : found->second.parent;
    }
    void WorldScripts::setCartHorseOffset(const osg::Vec3f& offset)
    {
        mImpl->cartHorseOffset = offset;
    }
    std::uint32_t WorldScripts::owningQuest(std::uint32_t id) { return mImpl->owningQuest(id); }

    std::optional<Papyrus::Value> WorldScripts::callFormMethod(
        std::uint32_t id, std::string_view name, const std::vector<Papyrus::Value>& args)
    {
        return mImpl->callForm(id, name, args);
    }
    std::optional<Papyrus::Value> WorldScripts::callSoundStatic(
        std::string_view name, const std::vector<Papyrus::Value>& args)
    {
        return mImpl->callSoundStatic(name, args);
    }
    std::optional<Papyrus::Value> WorldScripts::getFormProperty(std::uint32_t id, std::string_view name) const
    {
        return mImpl->getFormProperty(id, name);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> WorldScripts::takeFurnitureActivations()
    { return std::exchange(mImpl->furnitureActivations, {}); }
    void WorldScripts::playPlayerExecution(std::uint32_t actor, bool player)
    {
        mImpl->locomotion.erase(actor);
        if (player)
        {
            mImpl->executionChops.insert(actor);
            mImpl->animations.push_back({actor, 0, "actors/character/behaviors/0_master.hkx",
                "Behavior37/AOExecutioneeIdle.hkx::IdleExecutionerChop_Player"});
        }
        else
            mImpl->animations.push_back({actor, 0, "actors/character/behaviors/0_master.hkx", "IdleExecutionerChop_Player",
                "actors/character/animations/aoexecutionerchopplayer.hkx"});
    }
    std::uint32_t WorldScripts::playExecution(std::uint32_t victim, std::span<const std::uint32_t> dependents)
    {
        constexpr auto behavior = "actors/character/behaviors/mt_behavior.hkx";
        std::uint32_t executioner = 0;
        for (const auto actor : dependents)
        {
            bool guard = false;
            const auto npc = mImpl->read(mImpl->actorBase(actor));
            visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                if (tag == ESM::fourCC("KWDA"))
                    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4)
                        guard |= Misc::StringUtils::ciEqual(mImpl->editorId(npc.mapFormId(integer(bytes, i))), "isExecutionerGuard");
            });
            if (!guard) executioner = actor;
            mImpl->locomotion.erase(actor);
            mImpl->animations.push_back({actor, 0, behavior, "T5PairedExecution", std::string("actors/character/animations/")
                + (guard ? "aoexecutionguardchop.hkx" : "aoexecutionerchop.hkx")});
        }
        mImpl->locomotion.erase(victim);
        mImpl->animations.push_back({victim, 0, behavior, "T5Execution", "actors/character/animations/aoexecutioneechop.hkx"});
        return executioner;
    }
    void WorldScripts::recoverPackages()
    {
        for (auto& [actor, active] : mImpl->running)
        {
            if (!active.route || active.done || active.route->points.empty()) continue;
            const auto target = active.route->points.back().referenceId;
            const auto start = mImpl->authoredPlacement(actor);
            if (start)
                if (const auto entrance = mImpl->travelDoor(target, *start);
                    entrance && (active.route->points.back().position.asVec3() - entrance->door.position.asVec3()).length() > 1)
                {
                    // Earlier snapshots joined two different cell coordinate systems
                    // into one enormous route. Rebuild that invalid route at its
                    // authored exterior origin and walk through the actual XTEL door.
                    auto origin = *start;
                    if (mImpl->index && mImpl->index->references.contains(actor))
                    {
                        const auto record = mImpl->read(actor);
                        origin.cell = mImpl->index->references.at(actor).cell;
                        visitSubrecords(record, [&](auto tag, Bytes bytes) {
                            if (tag == ESM::fourCC("DATA") && bytes.size() == 24)
                                for (unsigned i = 0; i < 3; ++i)
                                { origin.position.pos[i] = real(bytes, i * 4); origin.position.rot[i] = real(bytes, 12 + i * 4); }
                        });
                    }
                    mImpl->move(actor, origin);
                    active.route.reset(); active.prepared = false; active.distance = 0;
                }
        }
        for (const auto actor : mImpl->loaded)
            if (!mImpl->running.contains(actor))
                for (const auto id : mImpl->quests.packagesForActor(actor))
                {
                    const auto& pack = mImpl->package(id);
                    const auto& tree = pack.templateId ? mImpl->package(pack.templateId) : pack;
                    if (!tree.otherBranches && tree.procedures.size() == 1 && tree.procedures.front().name == "Sit")
                    { mImpl->reevaluate.insert(actor); break; }
                }
        for (auto& [id, active] : mImpl->playing)
        {
            const auto& source = mImpl->scene(id);
            for (const auto& action : source.actions)
            {
                const auto state = active.actions.find(action.id);
                if (action.type != 1 || state == active.actions.end() || state->second.complete
                    || active.phase < action.first || active.phase > action.last || !state->second.actor) continue;
                const auto running = mImpl->running.find(state->second.actor);
                if (running != mImpl->running.end() && running->second.scene == id && running->second.action == action.id) continue;
                if (const auto* selected = mImpl->selectPackage(action.packages, state->second.actor, source.owner))
                    mImpl->startPackage(state->second.actor, *selected, source.owner, id, action.id);
            }
        }
    }
    void WorldScripts::tick(double time) { mImpl->tick(time); }
    std::vector<SceneScriptEvent> WorldScripts::takeEvents() { return std::exchange(mImpl->events, {}); }
    std::vector<WorldScriptAnimation> WorldScripts::takeAnimations() { return std::exchange(mImpl->animations, {}); }
    std::vector<WorldScriptArrow> WorldScripts::takeArrows() { return std::exchange(mImpl->arrows, {}); }
    std::vector<WorldScriptFall> WorldScripts::takeFalls() { return std::exchange(mImpl->fallRequests, {}); }
    std::vector<WorldScriptFiringPosition> WorldScripts::takeFiringPositions() { return std::exchange(mImpl->firingRequests, {}); }
    void WorldScripts::arrowImpact(std::uint32_t shot, std::uint32_t target) { mImpl->arrowImpact(shot, target); }
    std::vector<WorldScriptAudio> WorldScripts::takeAudio() { return std::exchange(mImpl->audio, {}); }
    std::vector<WorldScriptFace> WorldScripts::takeFaces() { return std::exchange(mImpl->faces, {}); }
    void WorldScripts::audioFinished(std::int32_t instance, bool played) { mImpl->music.audioFinished(instance, played); }

    bool WorldScripts::isInCombat(std::uint32_t actor) const
    {
        return mImpl->combatTargets.contains(actor) || mImpl->combatBows.contains(actor);
    }

    std::pair<std::int32_t, double> WorldScripts::playTopicLine(const RecordPayload& info, std::uint32_t actor,
        std::uint32_t number, int emotion, float strength)
    {
        if (!number) return {};
        try
        {
            const auto asset = mImpl->voiceAsset(info, number, mImpl->voiceType(actor));
            auto& durations = mImpl->voiceDurations;
            auto found = durations.find(asset);
            if (found == durations.end())
                found = durations.emplace(asset, voiceDuration(*mImpl->vfs->get(VFS::Path::Normalized(asset)))).first;
            WorldScriptAudio request{WorldScriptAudio::Play, 0, actor, asset};
            request.emotion = emotion;
            request.emotionStrength = strength;
            return {mImpl->playSound(std::move(request)), found->second};
        }
        catch (const std::exception&)
        {
            // Text-only responses and mods without a recording remain playable.
            return {};
        }
    }

    void WorldScripts::stopTopicLine(std::int32_t instance)
    {
        if (instance) mImpl->audio.push_back({WorldScriptAudio::Stop, instance});
    }

    void WorldScripts::topicFragment(const RecordPayload& info, std::uint32_t quest, std::uint32_t actor, bool begin)
    {
        if (begin) mImpl->dialogueScripts.clear();
        try
        {
            visitSubrecords(info, [&](auto tag, Bytes bytes) {
                if (tag != fourCC("VMAD")) return;
                auto vmad = readInfoVmad(bytes);
                mImpl->queue(mImpl->dialogueScripts, info, vmad.common, quest,
                    {vmad.fragmentFile, begin ? vmad.onBegin : vmad.onEnd, {FormReference{actor}}});
            });
        }
        catch (const std::exception& error)
        {
            mImpl->log.write("INFO " + std::to_string(info.metadata.formId) + " fragment", error.what());
        }
        if (!begin) mImpl->dialogueScripts.clear();
    }
    std::vector<WorldScriptTriggerEvent> WorldScripts::takeTriggerEvents()
    {
        return std::exchange(mImpl->triggerEvents, {});
    }
    void WorldScripts::checkpoint(Papyrus::Checkpoint& ar) { mImpl->checkpoint(ar); }
    void WorldScripts::checkpointExtensions(Papyrus::Checkpoint& ar)
    {
        ar(mImpl->bowVolleys);
        if (!ar.loading || !ar.atEnd()) ar(mImpl->normalizedVehicleActors);
        if (ar.loading) mImpl->visualScaleCache.clear();
    }
    float WorldScripts::visualScale(std::uint32_t actor) { return mImpl->visualScale(actor); }
    void WorldScripts::checkpointFurniture(Papyrus::Checkpoint& ar)
    {
        ar(mImpl->normalizedFurnitureActors);
        if (!ar.loading || !ar.atEnd())
            ar(mImpl->occupiedPerches, mImpl->enteringExecutionBlock, mImpl->combatBows);
        if (!ar.loading || !ar.atEnd())
            ar(mImpl->landingPerches, mImpl->flightGrabs, mImpl->bowPositions);
        if (!ar.loading || !ar.atEnd())
            ar(mImpl->shouts, mImpl->executionChops, mImpl->fallingActors, mImpl->carrierVelocities, mImpl->bowDestinations);
    }
    void WorldScripts::checkpointCombat(Papyrus::Checkpoint& ar) { ar(mImpl->combatTargets, mImpl->spellCasts); }
    void WorldScripts::checkpointFlightPermissions(Papyrus::Checkpoint& ar) { ar(mImpl->noFlight); }
    void WorldScripts::checkpointGhosts(Papyrus::Checkpoint& ar)
    { ar(mImpl->ghostOverrides); }
    void WorldScripts::checkpointSocialState(Papyrus::Checkpoint& ar)
    { ar(mImpl->relationshipOverrides, mImpl->factionOverrides); }
    std::vector<std::uint32_t> WorldScripts::loadedActors() const
    {
        std::vector<std::uint32_t> actors;
        for (auto id : mImpl->loaded)
            if (mImpl->actorBase(id))
                actors.push_back(id);
        return actors;
    }
    bool WorldScripts::sameSpace(std::uint32_t a, std::uint32_t cellA, std::uint32_t b, std::uint32_t cellB) const
    {
        if (cellA == cellB) return true;
        const auto worldOf = [&](std::uint32_t id, std::uint32_t cell) {
            if (const auto known = mImpl->cellWorlds.find(cell); known != mImpl->cellWorlds.end())
                return known->second;
            if (mImpl->index)
                if (const auto indexed = mImpl->index->references.find(id);
                    indexed != mImpl->index->references.end() && indexed->second.cell == cell)
                    return indexed->second.world;
            return 0u;
        };
        const auto first = worldOf(a, cellA);
        return first && first == worldOf(b, cellB);
    }
    void WorldScripts::deletePlacement(std::uint32_t id) { mImpl->moved.erase(id); }
    std::vector<std::array<std::uint32_t, 3>> WorldScripts::takeSpellCasts() { return std::exchange(mImpl->spellCasts, {}); }
    void WorldScripts::playCastSounds(std::uint32_t spell, std::uint32_t caster) { mImpl->castSounds(spell, caster); }

}
