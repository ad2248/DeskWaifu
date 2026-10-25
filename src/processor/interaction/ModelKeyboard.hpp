#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <linux/input-event-codes.h>

// 原始 evdev 按键，刻意限定为常用且不依赖键盘布局的按键。
inline std::string ModelKeyName(uint32_t key)
{
    if (key >= KEY_Q && key <= KEY_P) return std::string(1, "QWERTYUIOP"[key - KEY_Q]);
    if (key >= KEY_A && key <= KEY_L) return std::string(1, "ASDFGHJKL"[key - KEY_A]);
    if (key >= KEY_Z && key <= KEY_M) return std::string(1, "ZXCVBNM"[key - KEY_Z]);
    if (key >= KEY_1 && key <= KEY_9) return "N" + std::to_string(key - KEY_1 + 1);
    if (key == KEY_0) return "N0";
    if (key == KEY_TAB) return "Tab";
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL) return "Ctrl";
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) return "Shift";
    if (key == KEY_LEFTALT || key == KEY_RIGHTALT) return "Alt";
    return {};
}

inline std::string ModelBindingKey(const std::string& name)
{
    if (name == "LeftControl" || name == "RightControl" || name == "Ctrl") return "Ctrl";
    if (name == "LeftShift" || name == "RightShift" || name == "Shift") return "Shift";
    if (name == "LeftAlt" || name == "RightAlt" || name == "Alt") return "Alt";
    if (name.size() == 2 && name[0] == 'N' && name[1] >= '0' && name[1] <= '9') return name;
    if (name == "Tab") return name;
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return name;
    return {};
}

class ModelKeyboard
{
public:
    void Reset() { _pressed.clear(); }

    // 仅对新按下且可识别的按键返回 true；释放和重复事件不触发。
    bool Key(uint32_t code, bool down)
    {
        const std::string name = ModelKeyName(code);
        if (name.empty()) return false;
        if (!down) { _pressed.erase(code); return false; }
        return _pressed.insert(code).second;
    }

    bool Matches(const std::vector<std::string>& keys) const
    {
        std::set<std::string> expected, pressed;
        if (keys.empty()) return false;
        for (const auto& key : keys)
        {
            auto name = ModelBindingKey(key);
            if (name.empty() || !expected.insert(name).second) return false;
        }
        for (auto code : _pressed) pressed.insert(ModelKeyName(code));
        return pressed == expected;
    }

private:
    std::set<uint32_t> _pressed;
};

// 点击计数与渲染器分离，以便测试时间和拖动判定规则。
class ModelDoubleClick
{
public:
    enum Result { Pending, DoubleClick };
    bool HasPending() const { return _pending; }
    bool IsSecondClick(uint32_t time, int x, int y) const
    {
        return _pending && static_cast<uint32_t>(time - _time) <= Delay
            && x - _x > -Distance && x - _x < Distance
            && y - _y > -Distance && y - _y < Distance;
    }
    // 仅在非拖动释放时调用。拖动时应派发或取出待处理点击，而非调用 Release，
    // 避免将拖动手势计为点击。
    Result Release(uint32_t time, int x, int y, int tapX, int tapY)
    {
        if (IsSecondClick(time, x, y))
        {
            _pending = false;
            return DoubleClick;
        }
        _pending = true;
        _time = time;
        _x = x;
        _y = y;
        _tapX = tapX;
        _tapY = tapY;
        return Pending;
    }
    // 双击距离使用按下坐标，最终点击使用释放坐标。
    bool Expired(uint32_t time) const { return _pending && static_cast<uint32_t>(time - _time) > Delay; }
    bool Take() { bool pending = _pending; _pending = false; return pending; }
    int X() const { return _tapX; }
    int Y() const { return _tapY; }
    static constexpr uint32_t Delay = 350;
    static constexpr int Distance = 10;
private:
    bool _pending = false;
    uint32_t _time = 0;
    int _x = 0, _y = 0;
    int _tapX = 0, _tapY = 0;
};
