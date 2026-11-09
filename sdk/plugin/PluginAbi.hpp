// 生成宿主与插件进行严格二进制兼容性检查所需的构建指纹。
#pragma once

#include <QByteArray>
#include <QSysInfo>
#include <QtGlobal>
#include <cstdint>

// 返回静态存储的 Qt、编译器、语言及标准库 ABI 指纹，调用者不得释放。
// 宿主严格比较此指纹；这是同仓库 C++/Qt SDK，而非稳定的 C ABI。
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
