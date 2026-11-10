set(TES5_RUNTIME_SOURCES
    actorappearance.hpp
    actorappearance.cpp
    actorpose.hpp
    actorpose.cpp
    archery.cpp
    cellscene.hpp
    cellscene.cpp
    runtimecache.cpp
    runtimecache.hpp
    cartroute.hpp
    cartroute.cpp
    hkxmovement.hpp
    hkxmovement.cpp
    weather.cpp
    weather.hpp
    pluginlist.hpp
    pluginlist.cpp
    runtimeexport.hpp
    runtimeexport.cpp
    exportpreview.hpp
    exportpreview.cpp
    assetresources.hpp
    assetresources.cpp
    scenescripts.hpp
    scenescripts.cpp
    questscripts.hpp
    questscripts.cpp
    worldscripts.hpp
    worldscripts.cpp
    musicruntime.cpp
    modelmechanics.cpp
    ../papyrustool/errorlog.hpp
    ../papyrustool/pex.cpp
    ../papyrustool/vmad.cpp
    ../papyrustool/vmadbinding.cpp
    ../papyrustool/vm.cpp
)
add_library(tes5frontend STATIC ${TES5_RUNTIME_SOURCES})
target_link_libraries(tes5frontend PRIVATE components)

add_executable(t5movement_tests EXCLUDE_FROM_ALL hkxmovement_test.cpp hkxmovement.cpp)
add_executable(tes5route_tests EXCLUDE_FROM_ALL cartroute_test.cpp)
target_link_libraries(tes5route_tests PRIVATE tes5frontend components)
add_executable(t5model_probe EXCLUDE_FROM_ALL modelphysics_probe.cpp)
target_link_libraries(t5model_probe PRIVATE tes5frontend components)
add_library(T5DataBridge SHARED ue_bridge.cpp audiodecode.cpp audiodecode.hpp)
target_include_directories(T5DataBridge SYSTEM PRIVATE ${FFmpeg_INCLUDE_DIRS})
target_link_libraries(T5DataBridge PRIVATE tes5frontend components ${FFmpeg_AVFORMAT_LIBRARIES}
    ${FFmpeg_AVCODEC_LIBRARIES} ${FFmpeg_SWRESAMPLE_LIBRARIES} ${FFmpeg_AVUTIL_LIBRARIES})
add_executable(t5native_tests EXCLUDE_FROM_ALL nativecalls_test.cpp audiodecode.cpp)
target_include_directories(t5native_tests SYSTEM PRIVATE ${FFmpeg_INCLUDE_DIRS})
target_link_libraries(t5native_tests PRIVATE tes5frontend components ${FFmpeg_AVFORMAT_LIBRARIES}
    ${FFmpeg_AVCODEC_LIBRARIES} ${FFmpeg_SWRESAMPLE_LIBRARIES} ${FFmpeg_AVUTIL_LIBRARIES})
