#pragma once

#include <QByteArray>
#include <QString>
#include <QSysInfo>
#include <QtGlobal>
#include <cstdint>

class PluginContext;

class IPlugin
{
public:
    virtual ~IPlugin() = default;
    // Register endpoints and read configuration only. No cross-plugin calls yet.
    virtual bool Initialize(PluginContext& context, QString* error) = 0;
    virtual bool Start(QString* error) { Q_UNUSED(error); return true; }
    // Must also handle a partially failed Initialize. Stop/join workers here.
    virtual void Stop() noexcept = 0;
};

constexpr std::uint32_t DeskWaifuPluginAbiVersion = 1;
constexpr const char* DeskWaifuPluginEntrySymbol = "DeskWaifuPluginEntry";

// Deliberately strict: this is a same-repository C++/Qt SDK, not a stable C ABI.
static inline const char* DeskWaifuPluginBuildAbi()
{
    static const QByteArray abi = QSysInfo::buildAbi().toUtf8()
        + ";compiler=" + QByteArray(__VERSION__)
        + ";cxx=" + QByteArray::number(__cplusplus)
#ifdef __GLIBCXX__
        + ";libstdcxx=" + QByteArray::number(__GLIBCXX__)
#endif
#ifdef _GLIBCXX_USE_CXX11_ABI
        + ";stringabi=" + QByteArray::number(_GLIBCXX_USE_CXX11_ABI)
#endif
#ifdef _GLIBCXX_DEBUG
        + ";glibcxx_debug=1"
#else
        + ";glibcxx_debug=0"
#endif
#ifdef _LIBCPP_VERSION
        + ";libcxx=" + QByteArray::number(_LIBCPP_VERSION)
#endif
        ;
    return abi.constData();
}

struct PluginApi
{
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t qtVersion;
    const char* buildAbi;
    IPlugin* (*create)();
    void (*destroy)(IPlugin*);
};
using PluginEntry = const PluginApi* (*)();

template<class T> static const PluginApi* DeskWaifuPluginApi()
{
    static const PluginApi api{sizeof(PluginApi), DeskWaifuPluginAbiVersion, QT_VERSION,
        DeskWaifuPluginBuildAbi(), []() -> IPlugin* { return new T; }, [](IPlugin* plugin) { delete plugin; }};
    return &api;
}

#define DESKWAIFU_EXPORT_PLUGIN(Type) \
    extern "C" Q_DECL_EXPORT const PluginApi* DeskWaifuPluginEntry() \
    { return DeskWaifuPluginApi<Type>(); }
