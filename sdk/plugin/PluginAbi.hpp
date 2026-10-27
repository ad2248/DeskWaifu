#pragma once

#include <QByteArray>
#include <QSysInfo>
#include <QtGlobal>
#include <cstdint>

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
