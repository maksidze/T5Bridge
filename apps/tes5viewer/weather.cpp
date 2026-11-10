#include "weather.hpp"
#include <components/esm/fourcc.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace TES5Viewer
{
    namespace
    {
        std::uint32_t number(std::span<const std::byte> b, std::size_t p = 0)
        {
            if (p > b.size() || b.size()-p < 4) throw std::runtime_error("Truncated weather field");
            std::uint32_t n; std::memcpy(&n,b.data()+p,4); return n;
        }
        float real(std::span<const std::byte> b, std::size_t p) { return std::bit_cast<float>(number(b,p)); }
        float fraction(float x, float a, float b) { return b>a ? std::clamp((x-a)/(b-a),0.f,1.f) : 0; }
        bool inside(double x, double y, const std::vector<std::array<float,2>>& p)
        {
            bool result=false;
            for (std::size_t i=0,j=p.size()-1; i<p.size(); j=i++)
                if ((p[i][1]>y)!=(p[j][1]>y)
                    && x<(p[j][0]-p[i][0])*(y-p[i][1])/(p[j][1]-p[i][1])+p[i][0]) result=!result;
            return result;
        }
    }
    std::vector<std::uint32_t> WorldWeather::containing(std::uint32_t w, double x, double y)
    {
        if (!regionsLoaded)
        {
            regionsLoaded=true;
            if (index) for (const auto id:index->weatherRegions)
            {
                auto record=read(id); Region region{id,0,{}}; bool weather=false; std::uint32_t rawWorld=0;
                visitSubrecords(record,[&](auto tag,auto bytes) {
                    if (tag==ESM::fourCC("WNAM")) rawWorld=number(bytes);
                    if (tag==ESM::fourCC("RDWT") && !bytes.empty()) weather=true;
                    if (tag==ESM::fourCC("RDAT") && bytes.size()>=6 && number(bytes)==3)
                        region.priority=std::to_integer<unsigned>(bytes[5]);
                    if (tag==ESM::fourCC("RPLD"))
                    {
                        if (bytes.size()%8) throw std::runtime_error("Invalid weather region polygon");
                        auto& poly=region.polygons.emplace_back();
                        for (std::size_t p=0;p<bytes.size();p+=8) poly.push_back({real(bytes,p),real(bytes,p+4)});
                    }
                });
                if (weather)
                {
                    try {region.world=record.mapFormId(rawWorld);}
                    catch(const std::exception&) {continue;} // Vanilla contains unused invalid WNAMs.
                    regions.push_back(std::move(region));
                }
            }
        }
        std::vector<std::uint32_t> result;
        unsigned priority = 0;
        for (const auto& region:regions)
            if (region.world==w && std::ranges::any_of(region.polygons,[&](const auto& p){return p.size()>2 && inside(x,y,p);}))
            {
                if (region.priority>priority) {result.clear();priority=region.priority;}
                if (region.priority==priority) result.push_back(region.id);
            }
        std::ranges::sort(result); return result;
    }
    void WorldWeather::force(std::uint32_t id, bool overrideWeather)
    {
        if (overrideId && !overrideWeather) return;
        if (read(id).metadata.recordType!=ESM::fourCC("WTHR")) throw std::runtime_error("Weather requires WTHR");
        current=id; previous=0; progress=1; overrideId=overrideWeather?id:0;
    }
    void WorldWeather::begin(std::uint32_t w, std::uint32_t initial, double x, double y)
    {
        if (initialized) return;
        world=w;
        const auto record=read(w);
        visitSubrecords(record,[&](auto tag,auto b){if(tag==ESM::fourCC("CNAM")) climate=record.mapFormId(number(b));});
        signature=containing(w,x,y);
        if (initial)
        {
            if(read(initial).metadata.recordType!=ESM::fourCC("WTHR")) throw std::runtime_error("Initial weather requires WTHR");
            current=initial;previous=0;progress=1;
        }
        initialized=true;
    }
    void WorldWeather::tick(double x, double y, double gameHours)
    {
        if (initialized)
        {
            auto next=containing(world,x,y);
            if(next!=signature)
            {
                signature=std::move(next);
                if(overrideId && overrideId!=current) {previous=current;current=overrideId;progress=0;}
            }
        }
        if(previous && progress<1)
        {
            float delta=0;
            visitSubrecords(read(current),[&](auto tag,auto b){if(tag==ESM::fourCC("DATA") && b.size()>3) delta=std::to_integer<unsigned>(b[3])/16.f;});
            progress=std::clamp(progress+static_cast<float>(std::max(0.,gameHours))*delta,0.f,1.f);
            if(progress==1) previous=0;
        }
    }
    std::array<float,21> WorldWeather::sampleOne(std::uint32_t id, float hour) const
    {
        std::array<float,21> out{}; out[12]=hour;out[13]=1;out[14]=1;
        if(!id)return out;
        float extension=.5f;
        if(index)if(auto it=index->floatSettings.find("fdaytimecolorextension");it!=index->floatSettings.end())extension=it->second;
        std::array<float,4> times{5,10,16,21};
        if(climate)visitSubrecords(read(climate),[&](auto tag,auto b){
            if(tag==ESM::fourCC("TNAM") && b.size()>=4)
            {for(int i=0;i<4;++i)times[i]=std::to_integer<unsigned>(b[i])/6.f;times[0]-=extension;times[3]+=extension;}
        });
        int a=3,b=3;float blend=0;
        const float rise=(times[0]+times[1])*.5f, set=(times[2]+times[3])*.5f;
        if(hour>=times[0] && hour<rise){a=3;b=0;blend=fraction(hour,times[0],rise);}
        else if(hour>=rise && hour<times[1]){a=0;b=1;blend=fraction(hour,rise,times[1]);}
        else if(hour>=times[1] && hour<times[2]){a=b=1;}
        else if(hour>=times[2] && hour<set){a=1;b=2;blend=fraction(hour,times[2],set);}
        else if(hour>=set && hour<times[3]){a=2;b=3;blend=fraction(hour,set,times[3]);}
        const float day=hour<times[1]?fraction(hour,times[0],times[1]):1-fraction(hour,times[2],times[3]);
        visitSubrecords(read(id),[&](auto tag,auto bytes){
            if(tag==ESM::fourCC("NAM0") && bytes.size()>=80)
            {
                constexpr std::array<unsigned,3> slots{4,3,1};
                for(unsigned i=0;i<3;++i)for(unsigned c=0;c<3;++c)
                    out[i*3+c]=std::lerp(float(std::to_integer<unsigned>(bytes[slots[i]*16+a*4+c])),
                        float(std::to_integer<unsigned>(bytes[slots[i]*16+b*4+c])),blend)/255.f;
                for (unsigned c = 0; c < 3; ++c)
                {
                    out[15+c] = std::lerp(float(std::to_integer<unsigned>(bytes[a*4+c])),
                        float(std::to_integer<unsigned>(bytes[b*4+c])), blend) / 255.f;
                    constexpr unsigned fogFar = 12 * 16;
                    out[18+c] = bytes.size() < fogFar + 16 ? out[6+c]
                        : std::lerp(float(std::to_integer<unsigned>(bytes[fogFar+a*4+c])),
                            float(std::to_integer<unsigned>(bytes[fogFar+b*4+c])), blend) / 255.f;
                }
            }
            if(tag==ESM::fourCC("FNAM") && bytes.size()>=32)
            {
                out[9]=std::lerp(real(bytes,8),real(bytes,0),day);
                out[10]=std::lerp(real(bytes,12),real(bytes,4),day);
                out[11]=std::lerp(real(bytes,28),real(bytes,24),day);
                out[14]=std::lerp(real(bytes,20),real(bytes,16),day);
            }
        });
        return out;
    }
    std::array<float,21> WorldWeather::sample(float hour) const
    {
        auto result=sampleOne(current,hour);
        if(previous)
        {
            const auto from=sampleOne(previous,hour);
            for(unsigned i=0;i<12;++i) result[i]=std::lerp(from[i],result[i],progress);
            for (unsigned i = 14; i < result.size(); ++i) result[i] = std::lerp(from[i], result[i], progress);
        }
        result[13]=progress;return result;
    }
}
