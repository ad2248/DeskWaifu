#pragma once

#include "processor/Processor.hpp"
#include "KWinCursorTracker.hpp"
#include "contracts/InputSample.hpp"
#include <QTimer>
#include <memory>

// 在 Qt 应用线程归一化窗口输入与 KWin 光标采样，并合并未消费的连续移动。
// 借用的事件循环和交互处理器须存活至停止完成；不从渲染线程调用。
class InputListener final : public Processor
{
public:
    InputListener(ProcessorEventLoop& loop, IProcessorPlugin& interaction);
    // 返回是否消费事件；窗口状态广播只更新缓存并返回 false。
    bool HandleEvent(const ProcessorEvent&) override;
    // 尝试启动光标跟踪并启动独立采样定时器，返回跟踪启动是否成功。
    // 总线已建立后的脚本加载失败可由轮询重试；总线连接失败不会自动重连。
    bool StartCursor();
    const std::string& CursorError() const { return _cursor.Error(); }
    // 发送离散输入，并补齐缺失的毫秒时间戳；暂停或停止后忽略。
    void Event(const QString& kind, QVariantMap data = {});
    // 接收窗口局部坐标；cursor 区分独立光标采样与原生指针移动。
    void Move(double x, double y, bool cursor = false);
    // 停止采样、注销待处理输入并释放临时脚本；停止后不再转发。
    void Stop() noexcept override;
    // 输出切换时暂停转发；进入暂停时丢弃旧窗口排队的输入。
    void SetPaused(bool paused);
private:
    void PollCursor();
    IProcessorPlugin& _interaction;
    int _width = 0, _height = 0;
    std::string _output;
    KWinCursorTracker _cursor;
    QTimer _cursorTimer;
    InputSamplePtr _move, _global;
    bool _stopped = false, _paused = false;
};
