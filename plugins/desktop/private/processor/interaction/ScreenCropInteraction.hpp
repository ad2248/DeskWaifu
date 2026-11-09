#pragma once

#include "CropInteraction.hpp"

// 本地和控制面板屏幕编辑共用的指针手势。
// 此处点击不会进入模型点击或双击键盘捕获识别器。
class ScreenCropInteraction
{
public:
    void Press(const TransformSettings& settings, int x, int y, bool model = false)
    {
        _start = settings; _x = x; _y = y; _moved = false;
        _handle = !model && settings.cropped ? CropInteraction::Hit(settings.region, x, y) : CropInteraction::None;
        if (_handle == CropInteraction::None) _handle = CropInteraction::Model;
    }

    // 超过点击阈值后申请本地屏幕编辑事务并开始模型拖动。
    // 未越过阈值或事务申请失败时返回 false，不启动手势。
    bool StartModelDrag(TransformEditor& editor, int startX, int startY,
        int x, int y, int width, int height)
    {
        if (InputController::IsTap(startX, startY, x, y)) return false;
        if (!editor.BeginLocalScreenEdit(width, height).ok) return false;
        _clicks = ModelDoubleClick{};
        Press(editor.Display(), startX, startY, true);
        Move(editor, x, y, height);
        return true;
    }

    void Move(TransformEditor& editor, int x, int y, int height)
    {
        if (_handle == CropInteraction::None) return;
        if (!InputController::IsTap(_x, _y, x, y)) _moved = true;
        if (!_moved) return;
        _clicks = ModelDoubleClick{};
        editor.Update(editor.Owner(), CropInteraction::Drag(_start, _handle, x - _x, y - _y, height));
    }

    // 结束手势；裁剪框外的单击或边框上的双击会提交当前编辑事务。
    // 返回提交结果；未触发提交时返回成功，拖动草稿仍留给编辑器管理。
    AppResult Release(TransformEditor& editor, uint32_t now, int x, int y)
    {
        const auto handle = _handle;
        _handle = CropInteraction::None;
        if (handle == CropInteraction::None) return AppResult::Success();
        if (!_moved && InputController::IsTap(_x, _y, x, y) &&
            handle == CropInteraction::Model &&
            OutsideCrop(_start, _x, _y) && OutsideCrop(editor.Display(), x, y) &&
            CropInteraction::Hit(editor.Display().region, x, y) == CropInteraction::None) {
            _clicks = ModelDoubleClick{};
            return editor.Apply(editor.Owner());
        }
        if (_moved || !InputController::IsTap(_x, _y, x, y) ||
            handle == CropInteraction::Model || !editor.Display().cropped ||
            CropInteraction::Hit(editor.Display().region, x, y) == CropInteraction::None) {
            _clicks = ModelDoubleClick{};
            return AppResult::Success();
        }
        if (_clicks.Release(now, _x, _y, x, y) == ModelDoubleClick::DoubleClick)
            return editor.Apply(editor.Owner());
        return AppResult::Success();
    }

    bool DraggingModel() const { return _moved && _handle == CropInteraction::Model; }
    int Handle() const { return _handle; }

private:
    static bool OutsideCrop(const TransformSettings& settings, int x, int y)
    {
        const auto r = settings.region;
        return settings.cropped && (x < r.x || y < r.y ||
            int64_t(x) >= int64_t(r.x) + r.width || int64_t(y) >= int64_t(r.y) + r.height);
    }

    TransformSettings _start;
    int _x = 0, _y = 0, _handle = CropInteraction::None;
    bool _moved = false;
    ModelDoubleClick _clicks;
};
