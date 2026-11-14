// 测试专用模块：只替换网络目的地，沿用生产插件、状态机和真实音频采集。
// 本入口不进入 voice_input.so，生产环境不读取此测试变量。
#include "private/VoiceInputController.hpp"
#include "private/asr/NativeAsrClient.hpp"

class SmokeNativeAsrClient final : public NativeAsrClient {
public:
    SmokeNativeAsrClient()
        : NativeAsrClient(QUrl(qEnvironmentVariable("DESKWAIFU_TEST_ASR_TRANSPORT")), true)
    {
    }
};

#define NativeAsrClient SmokeNativeAsrClient
#include "../VoiceInputPlugin.cpp"
#undef NativeAsrClient
