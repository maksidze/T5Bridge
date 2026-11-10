#include "musicruntime.hpp"
#include "checkpointtypes.hpp"

#include "questscripts.hpp"
#include "../papyrustool/errorlog.hpp"

#include <components/esm/fourcc.hpp>
#include <components/vfs/manager.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

namespace TES5Viewer
{
    namespace
    {
        using ESM::fourCC;
        using Bytes = std::span<const std::byte>;

        std::uint32_t integer(Bytes bytes, std::size_t offset = 0, std::size_t width = 4)
        {
            if (offset > bytes.size() || width > bytes.size() - offset)
                throw std::runtime_error("Truncated music record");
            std::uint32_t result = 0;
            for (std::size_t i = 0; i < width; ++i)
                result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
            return result;
        }

        float seconds(Bytes bytes, std::size_t offset = 0)
        {
            const float value = std::bit_cast<float>(integer(bytes, offset));
            if (!std::isfinite(value) || value < 0)
                throw std::runtime_error("Invalid time in music record");
            return value;
        }

        enum MusicFlags : std::uint32_t
        {
            OneSelection = 1,
            Abrupt = 2,
            Cycle = 4,
            Ordered = 8,
            Ducks = 0x20,
            DoesNotQueue = 0x40,
        };

        struct Track
        {
            std::uint32_t id = 0;
            std::string path, finale;
            std::vector<float> cues;
            float silence = 0, fadeOut = 0, loopBegin = 0, loopEnd = 0;
            std::uint32_t loopCount = 0;
            void checkpoint(Papyrus::Checkpoint& ar)
            { ar(id, path, finale, cues, silence, fadeOut, loopBegin, loopEnd, loopCount); }
        };

        struct Type
        {
            std::string name;
            std::uint32_t priority = 0, flags = 0;
            float duckGain = 1, fade = 0;
            std::vector<std::uint32_t> tracks, remaining;
            std::uint32_t previous = 0;
            std::uint64_t order = 0;
            bool active = true;
            std::int32_t playing = 0;
            double silenceUntil = 0;
            float gain = 1;
            Track current;
            void checkpoint(Papyrus::Checkpoint& ar)
            { ar(name, priority, flags, duckGain, fade, tracks, remaining, previous, order, active, playing, silenceUntil, gain, current); }
        };
    }

    struct MusicRuntime::Impl
    {
        const ReadWinningRecord& read;
        const VFS::Manager& vfs;
        QuestScripts& quests;
        Papyrus::ErrorLog& log;
        std::map<std::uint32_t, Type> types;
        std::minstd_rand random{std::random_device{}()};
        std::uint64_t nextOrder = 0;
        std::uint32_t top = 0;
        std::set<std::uint32_t> badTracks;
        bool dirty = false;

        std::string path(Bytes bytes) const
        {
            const auto end = std::ranges::find(bytes, std::byte{0});
            auto name = VFS::Path::normalizeFilename(
                std::string_view(reinterpret_cast<const char*>(bytes.data()), end - bytes.begin()));
            const auto start = name.find_first_not_of('/');
            if (start == std::string::npos)
                return {};
            name.erase(0, start);
            if (name.starts_with("data/"))
                name.erase(0, 5);
            if (!name.starts_with("music/"))
                name.insert(0, "music/");
            if (vfs.exists(VFS::Path::Normalized(name)))
                return name;
            if (name.ends_with(".wav"))
            {
                name.replace(name.size() - 4, 4, ".xwm");
                if (vfs.exists(VFS::Path::Normalized(name)))
                    return name;
            }
            return {};
        }

        void shuffle(std::vector<std::uint32_t>& ids) { std::ranges::shuffle(ids, random); }

        std::optional<Track> track(std::uint32_t id, unsigned depth)
        {
            if (depth >= 16)
                throw std::runtime_error("Music palette nesting exceeds limit");
            const auto record = read(id);
            if (record.metadata.recordType != fourCC("MUST"))
                throw std::runtime_error("Music track must resolve to a MUST record");
            std::uint32_t kind = 0;
            Track result{ .id = id };
            std::vector<std::uint32_t> palette;
            std::vector<Bytes> conditions;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                switch (tag)
                {
                    case fourCC("CNAM"): kind = integer(bytes); break;
                    case fourCC("ANAM"): result.path = path(bytes); break;
                    case fourCC("BNAM"): result.finale = path(bytes); break;
                    case fourCC("FLTV"): result.silence = seconds(bytes); break;
                    case fourCC("DNAM"): result.fadeOut = seconds(bytes); break;
                    case fourCC("LNAM"):
                        result.loopBegin = seconds(bytes);
                        result.loopEnd = seconds(bytes, 4);
                        result.loopCount = integer(bytes, 8);
                        break;
                    case fourCC("FNAM"):
                        for (std::size_t i = 0; i < bytes.size(); i += 4)
                            result.cues.push_back(seconds(bytes, i));
                        break;
                    case fourCC("SNAM"):
                        for (std::size_t i = 0; i < bytes.size(); i += 4)
                            if (const auto child = record.mapFormId(integer(bytes, i)))
                                palette.push_back(child);
                        break;
                    case fourCC("CTDA"): conditions.push_back(bytes); break;
                }
            });
            if (!quests.testConditions(0, record, conditions, 0x14))
                return std::nullopt;
            if (kind == 0x23F678C3)
            {
                shuffle(palette);
                for (const auto child : palette)
                    if (auto selected = tryTrack(child, depth + 1))
                        return selected;
                return std::nullopt;
            }
            if (kind == 0xA1A9C4D5 && result.silence > 0)
                return result;
            if (kind != 0x6ED7E048 || result.path.empty())
                throw std::runtime_error("MUST has no playable music resource");
            std::ranges::sort(result.cues);
            return result;
        }

        std::optional<Track> tryTrack(std::uint32_t id, unsigned depth)
        {
            if (badTracks.contains(id))
                return std::nullopt;
            try
            {
                return track(id, depth);
            }
            catch (const std::exception& error)
            {
                if (badTracks.insert(id).second)
                    log.write("MUST " + std::to_string(id), error.what());
                return std::nullopt;
            }
        }

        Type& add(const RecordPayload& record)
        {
            Type type;
            visitSubrecords(record, [&](auto tag, Bytes bytes) {
                switch (tag)
                {
                    case fourCC("EDID"):
                    {
                        const auto end = std::ranges::find(bytes, std::byte{0});
                        type.name.assign(reinterpret_cast<const char*>(bytes.data()), end - bytes.begin());
                        break;
                    }
                    case fourCC("FNAM"): type.flags = integer(bytes); break;
                    case fourCC("PNAM"):
                        type.priority = integer(bytes, 0, 2);
                        type.duckGain = std::pow(10.f, -static_cast<float>(integer(bytes, 2, 2)) / 2000.f);
                        break;
                    case fourCC("WNAM"): type.fade = seconds(bytes); break;
                    case fourCC("TNAM"):
                        for (std::size_t i = 0; i < bytes.size(); i += 4)
                            if (const auto id = record.mapFormId(integer(bytes, i)))
                                type.tracks.push_back(id);
                        break;
                }
            });
            type.order = ++nextOrder;
            const bool outranked = std::ranges::any_of(types, [&](const auto& item) {
                return item.second.active && item.second.priority < type.priority;
            });
            type.active = !(type.flags & DoesNotQueue) || !outranked;
            return types.insert_or_assign(record.metadata.formId, std::move(type)).first->second;
        }

        void stop(Type& type, bool abrupt, std::vector<WorldScriptAudio>& audio)
        {
            if (type.playing)
            {
                WorldScriptAudio command{ WorldScriptAudio::Stop, type.playing };
                command.fade = abrupt ? 0 : (type.current.fadeOut > 0 ? type.current.fadeOut : type.fade);
                command.music = !abrupt;
                audio.push_back(std::move(command));
            }
            type.playing = 0;
            type.silenceUntil = 0;
        }

        void finish(Type& type)
        {
            type.playing = 0;
            type.silenceUntil = 0;
            if (type.flags & OneSelection)
                type.active = false;
        }

        bool start(std::uint32_t id, Type& type, double time, float fade, float gain,
            const std::function<std::int32_t(WorldScriptAudio)>& play, std::vector<SceneScriptEvent>& events)
        {
            if (type.remaining.empty() || !(type.flags & (Cycle | Ordered)))
            {
                type.remaining = type.tracks;
                if (!(type.flags & Ordered))
                    shuffle(type.remaining);
                else
                    std::ranges::reverse(type.remaining);
                if (type.remaining.size() > 1 && type.remaining.back() == type.previous && !(type.flags & Ordered))
                    std::swap(type.remaining.back(), type.remaining.front());
            }
            while (!type.remaining.empty())
            {
                const auto candidate = type.remaining.back();
                type.remaining.pop_back();
                auto selected = tryTrack(candidate, 0);
                if (!selected)
                    continue;
                type.previous = candidate;
                type.current = std::move(*selected);
                type.gain = gain;
                if (type.current.path.empty())
                    type.silenceUntil = time + type.current.silence;
                else
                {
                    WorldScriptAudio command{ WorldScriptAudio::Play, 0, 0, type.current.path };
                    command.spatial = false;
                    command.music = true;
                    command.volume = gain;
                    command.fade = fade;
                    command.loopBegin = type.current.loopBegin;
                    command.loopEnd = type.current.loopEnd;
                    command.loopCount = type.current.loopCount;
                    command.finale = type.current.finale;
                    command.cues = type.current.cues;
                    type.playing = play(std::move(command));
                }
                events.push_back({ id, "MUSC", "TrackBegin",
                    {{ "T5_MUSIC_PLAY " + type.name + " track=" + std::to_string(type.current.id) + " " + type.current.path, 0 }},
                    time, {}, 0 });
                return true;
            }
            return false;
        }
    };

    MusicRuntime::MusicRuntime(const ReadWinningRecord& read, const VFS::Manager& vfs, QuestScripts& quests, Papyrus::ErrorLog& log)
        : mImpl(std::make_unique<Impl>(Impl{ read, vfs, quests, log }))
    {
    }

    MusicRuntime::~MusicRuntime() = default;

    bool MusicRuntime::set(const RecordPayload& record, bool added)
    {
        if (record.metadata.recordType != fourCC("MUSC"))
            return false;
        const auto found = mImpl->types.find(record.metadata.formId);
        if (!added)
        {
            if (found != mImpl->types.end())
                found->second.active = false;
        }
        else if (found == mImpl->types.end())
            mImpl->add(record);
        else
            found->second.active = true;
        mImpl->dirty = true;
        return true;
    }

    void MusicRuntime::audioFinished(std::int32_t instance, bool played)
    {
        if (instance <= 0)
            return;
        for (auto& [id, type] : mImpl->types)
            if (type.playing == instance)
            {
                if (played)
                    mImpl->finish(type);
                else
                {
                    mImpl->badTracks.insert(type.current.id);
                    type.playing = 0;
                }
                mImpl->dirty = true;
                break;
            }
    }

    void MusicRuntime::tick(double time, const std::function<std::int32_t(WorldScriptAudio)>& play,
        std::vector<WorldScriptAudio>& audio, std::vector<SceneScriptEvent>& events)
    {
        auto& state = *mImpl;
        for (auto& [id, type] : state.types)
            if (type.silenceUntil > 0 && type.silenceUntil <= time)
            {
                state.finish(type);
                state.dirty = true;
            }
        if (!std::exchange(state.dirty, false))
            return;
        std::vector<std::pair<std::uint32_t, Type*>> candidates;
        for (auto& [id, type] : state.types)
            if (type.active)
                candidates.emplace_back(id, &type);
        std::ranges::sort(candidates, [](const auto& left, const auto& right) {
            if (left.second->priority != right.second->priority)
                return left.second->priority < right.second->priority;
            return left.second->order > right.second->order;
        });

        std::uint32_t selected = 0;
        const Type* winner = nullptr;
        const auto starts = audio.size();
        for (auto& [id, type] : candidates)
        {
            const float fade = (type->flags & Abrupt) || state.top == id ? 0 : type->fade;
            if (type->playing || type->silenceUntil > time || state.start(id, *type, time, fade, 1.f, play, events))
            {
                selected = id;
                winner = type;
                break;
            }
            type->active = false;
        }

        // Queue stops before starts so the mixer can use an authored cue point
        // to schedule the incoming track and optional finale.
        const bool startedTrack = audio.size() > starts;
        for (auto& [id, type] : state.types)
        {
            const bool duck = winner && id != selected && type.active && (winner->flags & Ducks) && type.playing
                && winner->priority < type.priority;
            if (id != selected && !duck)
            {
                if (type.playing)
                    events.push_back({ id, "MUSC", "TrackStop", {{ "T5_MUSIC_STOP " + type.name, 0 }}, time, {}, 0 });
                state.stop(type, winner && (winner->flags & Abrupt), audio);
                continue;
            }
            const float gain = duck ? winner->duckGain : 1.f;
            if (type.playing && type.gain != gain)
            {
                WorldScriptAudio command{ WorldScriptAudio::Volume, type.playing };
                command.volume = gain;
                command.fade = winner ? winner->fade : type.fade;
                audio.push_back(std::move(command));
                type.gain = gain;
            }
        }
        if (startedTrack && starts + 1 < audio.size())
            std::rotate(audio.begin() + starts, audio.begin() + starts + 1, audio.end());
        state.top = selected;
        std::erase_if(state.types, [](const auto& item) { return !item.second.active; });
    }
    void MusicRuntime::checkpoint(Papyrus::Checkpoint& ar)
    { ar(mImpl->types, mImpl->nextOrder, mImpl->top, mImpl->badTracks, mImpl->dirty); ar.random(mImpl->random); }

}
