#include "ModelFrame.hpp"
#include "CursorLook.hpp"
#include <iostream>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Line " << __LINE__ << ": " << #condition << '\n'; return 1; \
} } while (false)

struct Runtime : IApplicationRuntime
{
    int ModelIndex() const override { return 0; }
    int ModelCount() const override { return 1; }
    bool SetHidden(bool) override { return true; }
};

// 记录型渲染边界只测试帧调度和转发数值。
// 实际输入应用由 ModelGazeTest 中的真实 LAppModel 验证。
struct Model
{
    std::pair<float, float> target{0, 0};
    std::vector<std::pair<float, float>> drags;
    std::vector<ModelFrameInput> inputs;
    std::vector<std::pair<float, float>> rendered;
    ModelTransform transform{1, 0, 0};

    void OnDrag(float x, float y) { target = {x, y}; drags.push_back(target); }
    void Render(ModelTransform value, const ModelFrameInput& input)
    {
        transform = value;
        inputs.push_back(input);
        rendered.push_back(target);
    }
};

int main()
{
    Runtime runtime;
    ApplicationService service(runtime);
    CursorLook cursor;
    Model model;
    bool editing = false;
    int updates = 0;
    float radius = 5;
    auto frame = [&] {
        RenderModelFrame(service, editing,
            [&](ModelTransform transform) {
                ++updates;
                if (!cursor.HasPosition()) return;
                const auto look = cursor.LookAt(-200, 100, 200, 200, .5f, .5f, transform, radius);
                model.OnDrag(look.first, look.second);
            },
            [&](ModelTransform transform, const ModelFrameInput& input) {
                model.Render(transform, input);
            });
    };

    frame();
    CHECK(updates == 1 && model.drags.empty() && model.rendered.size() == 1);
    cursor.GlobalPosition(-50, 150);
    const auto mouse = std::make_pair(.5f, .5f);
    frame();
    CHECK(updates == 2 && model.drags.size() == 1 && model.rendered.back() == mouse);

    // 点击释放会重置模型目标，不需要新的光标采样。
    model.OnDrag(0, 0);
    CHECK(model.target == std::make_pair(0.0f, 0.0f));
    frame();
    CHECK(updates == 3 && model.drags.size() == 3 && model.rendered.back() == mouse);
    frame();
    CHECK(updates == 4 && model.drags.size() == 4 && model.rendered.back() == mouse);

    CHECK(service.SetTransform({2, 1, -1}).ok);
    frame();
    const auto transformed = std::make_pair(-1.5f, 2.5f);
    CHECK(updates == 5 && model.drags.size() == 5 && model.rendered.back() == transformed);
    CHECK(model.transform.scale == 2 && model.transform.x == 1 && model.transform.y == -1);

    editing = true;
    cursor.GlobalPosition(-100, 200);
    frame();
    frame();
    CHECK(updates == 5 && model.drags.size() == 5 && model.rendered.size() == 7);
    CHECK(model.rendered.back() == transformed);
    editing = false;
    CHECK(service.SetTransform({1, 0, 0}).ok);
    cursor.GlobalPosition(-50, 150);

    // 使用与 IPC 命令处理和连接清理相同的服务入口。
    // 此处验证转发，而非模型对输入的应用。
    CHECK(service.SetLook("ipc:tracker", ExternalControlState::Lifetime::Stream, -.8f, .3f).ok);
    frame();
    CHECK(updates == 6 && model.drags.size() == 6 && model.drags.back() == mouse);
    CHECK(model.inputs.back().hasLook && model.inputs.back().lookX == -.8f && model.inputs.back().lookY == .3f);
    CHECK(model.rendered.back() == mouse);
    frame();
    CHECK(updates == 7 && model.drags.size() == 7 && model.rendered.back() == mouse);
    service.ReleaseControls("ipc:tracker");
    frame();
    CHECK(updates == 8 && model.drags.size() == 8 && !model.inputs.back().hasLook);
    CHECK(model.rendered.back() == mouse && model.rendered.size() == 10);

    CHECK(service.SetVisibility(false).ok);
    frame();
    CHECK(updates == 8 && model.drags.size() == 8 && model.rendered.size() == 10);
    CHECK(service.SetVisibility(true).ok);
    frame();
    CHECK(updates == 9 && model.drags.size() == 9 && model.rendered.size() == 11);
    CHECK(model.rendered.back() == mouse);
    // 配置变更无需新的光标采样即可生效。
    radius = .25f;
    frame();
    CHECK(std::abs(std::hypot(model.rendered.back().first, model.rendered.back().second) - .25f) < .00001f);
    radius = 0;
    frame();
    CHECK(model.rendered.back() == std::make_pair(0.f, 0.f));
    return 0;
}
