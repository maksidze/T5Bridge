#pragma once

#include <osg/Vec3f>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <vector>

namespace TES5Viewer
{
    struct CharacterRim
    {
        std::vector<std::size_t> vertices;
        std::vector<std::pair<std::size_t,std::size_t>> edges;
        osg::Vec3f center;
    };

    // Weld positions for topology discovery only. Keep original UV seams and indices.
    inline std::vector<CharacterRim> characterRims(std::span<const osg::Vec3f> positions, std::span<const unsigned> triangles)
    {
        using Key = std::tuple<long long,long long,long long>;
        std::map<Key,std::size_t> welded;
        std::vector<std::size_t> ids(positions.size());
        for (std::size_t i=0;i<positions.size();++i)
        {
            const auto& p = positions[i];
            const Key key{std::llround(p.x()*1000),std::llround(p.y()*1000),std::llround(p.z()*1000)};
            ids[i] = welded.try_emplace(key,i).first->second;
        }
        std::map<std::pair<std::size_t,std::size_t>,unsigned> counts;
        for (std::size_t i=0;i+2<triangles.size();i+=3)
            for (unsigned e=0;e<3;++e)
            {
                auto a=ids.at(triangles[i+e]), b=ids.at(triangles[i+(e+1)%3]);
                if (a!=b) ++counts[std::minmax(a,b)];
            }
        std::map<std::size_t,std::set<std::size_t>> adjacency;
        for (const auto& [edge,count] : counts) if (count==1)
        { adjacency[edge.first].insert(edge.second); adjacency[edge.second].insert(edge.first); }
        std::set<std::size_t> seen;
        std::vector<CharacterRim> result;
        for (const auto& [start,neighbors] : adjacency)
        {
            if (seen.contains(start)) continue;
            CharacterRim rim; std::vector<std::size_t> todo{start}; bool closed=true;
            while (!todo.empty())
            {
                auto i=todo.back(); todo.pop_back();
                if (!seen.insert(i).second) continue;
                rim.vertices.push_back(i); rim.center+=positions[i];
                closed &= adjacency[i].size()==2;
                for (auto next : adjacency[i])
                {
                    if (i<next) rim.edges.emplace_back(i,next);
                    if (!seen.contains(next)) todo.push_back(next);
                }
            }
            if (closed && rim.vertices.size()>=8)
            { rim.center/=float(rim.vertices.size()); result.push_back(std::move(rim)); }
        }
        return result;
    }

    template<class Skin> struct CharacterNeck
    {
        std::vector<osg::Vec3f> positions, normals;
        std::vector<Skin> weights;
        CharacterRim rim;
        bool ready = false;
        void consider(const std::vector<osg::Vec3f>& p, const std::vector<osg::Vec3f>& n,
            const std::vector<Skin>& w, const std::vector<CharacterRim>& rims)
        {
            for (const auto& candidate : rims)
                if (!ready || candidate.center.z()>rim.center.z())
                { positions=p; normals=n; weights=w; rim=candidate; ready=true; }
        }
    };
    struct CharacterSeamResult
    {
        std::vector<float> morphMask;
        std::size_t vertices=0;
        float before=0, after=0;
    };

    template<class Skin> Skin blendCharacterSkin(const Skin& a,const Skin& b,float t)
    {
        if (t>=1.f-1e-6f) return b;
        if (t<=1e-6f) return a;
        std::map<unsigned,float> influences;
        for (std::size_t i=0;i<a.weights.size();++i)
        { influences[a.joints[i]]+=a.weights[i]*(1-t); influences[b.joints[i]]+=b.weights[i]*t; }
        std::vector<std::pair<unsigned,float>> sorted(influences.begin(),influences.end());
        std::ranges::sort(sorted,[](const auto& x,const auto& y) { return x.second>y.second; });
        Skin result{}; float total=0;
        for (std::size_t i=0;i<std::min(sorted.size(),result.weights.size());++i)
        { result.joints[i]=sorted[i].first; result.weights[i]=sorted[i].second; total+=sorted[i].second; }
        if (total>0) for (auto& value : result.weights) value/=total;
        return result;
    }

    // The generated head has a fixed neck contour; the body has the actual NPC
    // weight blend. Join their real boundary rings, including skin weights, then
    // fade the correction through the neck. Facial features remain untouched.
    template<class Skin> CharacterSeamResult fitCharacterNeck(std::vector<osg::Vec3f>& positions,
        std::vector<osg::Vec3f>& normals,std::vector<Skin>& weights,std::span<const unsigned> triangles,
        const CharacterNeck<Skin>& body)
    {
        CharacterSeamResult result; result.morphMask.assign(positions.size(),1);
        if (!body.ready) return result;
        auto rims=characterRims(positions,triangles);
        auto chosen=std::ranges::min_element(rims,{},[&](const auto& rim) { return (rim.center-body.rim.center).length2(); });
        if (chosen==rims.end() || (chosen->center-body.rim.center).length()>5) return result;
        struct Anchor { osg::Vec3f original, delta, normal; Skin skin; };
        std::vector<Anchor> anchors;
        for (auto index : chosen->vertices)
        {
            float best=std::numeric_limits<float>::max(); std::size_t nearest=0;
            for (auto candidate : body.rim.vertices)
            {
                const float distance=(positions[index]-body.positions[candidate]).length2();
                if (distance<best) { best=distance; nearest=candidate; }
            }
            if (best>9) return CharacterSeamResult{};
            result.before=std::max(result.before,std::sqrt(best));
            anchors.push_back({positions[index],body.positions[nearest]-positions[index],body.normals[nearest],body.weights[nearest]});
        }
        for (std::size_t i=0;i<positions.size();++i)
        {
            auto nearest=std::ranges::min_element(anchors,{},[&](const auto& anchor) { return (positions[i]-anchor.original).length2(); });
            const float distance=(positions[i]-nearest->original).length();
            if (distance>=4) continue;
            float x=std::clamp(distance/4,0.f,1.f); const float strength=1-x*x*(3-2*x);
            positions[i]+=nearest->delta*strength;
            normals[i]=normals[i]*(1-strength)+nearest->normal*strength; normals[i].normalize();
            weights[i]=blendCharacterSkin(weights[i],nearest->skin,strength);
            result.morphMask[i]=1-strength;
            if (distance<.001f) ++result.vertices;
        }
        for (auto index : chosen->vertices)
        {
            float best=std::numeric_limits<float>::max();
            for (auto candidate : body.rim.vertices) best=std::min(best,(positions[index]-body.positions[candidate]).length());
            result.after=std::max(result.after,best);
        }
        return result;
    }
}
