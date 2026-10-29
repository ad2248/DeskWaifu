#include "processor/model/ModelHitTest.hpp"
#include <atomic>
#include <iostream>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

int main()
{
    ModelHitSnapshot snapshot; snapshot.width = 100; snapshot.height = 100; snapshot.modelToken = "model";
    snapshot.partsEnabled = true; snapshot.xScale = 1; snapshot.yScale = 1;
    snapshot.areas = {{"Head", 10, 10, 40, 40}, {"Body", 20, 20, 60, 80}, {"Custom", 25, 25, 30, 30}};
    snapshot.regions = {{20, 20, 10, 10}};
    const ModelHitQuery query{25, 25, 100, 100, "model"};
    auto hit = snapshot.Test(query);
    CHECK(hit.available && hit.inside && hit.parts.size() == 3);
    hit = snapshot.Test({10, 10, 100, 100, "model"});
    CHECK(hit.available && !hit.inside && hit.parts == std::vector<std::string>{"Head"});
    CHECK(!snapshot.Test({25, 25, 101, 100, "model"}).available);
    CHECK(!snapshot.Test({25, 25, 100, 100, "old"}).available);
    snapshot.hidden = true; hit = snapshot.Test(query);
    CHECK(hit.available && !hit.inside && hit.parts.empty());
    snapshot.hidden = false;

    auto fallback = snapshot;
    fallback.fallback = true; fallback.fallbackYScale = -1; fallback.fallbackYOffset = 50;
    CHECK(fallback.Test(query).parts == std::vector<std::string>{"Head"});
    CHECK(fallback.Test({25, 50, 100, 100, "model"}).parts == std::vector<std::string>{"Body"});
    CHECK(fallback.Test({25, 75, 100, 100, "model"}).parts == std::vector<std::string>{"Body"});
    fallback.partsEnabled = false;
    CHECK(fallback.Test(query).parts.empty());

    auto box = std::make_shared<ModelHitMailbox>();
    auto callback = box->Callback(box);
    CHECK(!callback(query).available);
    box->snapshot = std::make_shared<ModelHitSnapshot>(snapshot);
    CHECK(callback(query).inside);
    std::atomic<bool> correct{true};
    std::thread reader([&] {
        for (int i = 0; i < 10000; ++i) {
            const auto result = callback(query);
            if (!result.available || result.parts.size() != 3) correct = false;
        }
    });
    for (int i = 0; i < 10000; ++i) {
        auto next = std::make_shared<ModelHitSnapshot>(snapshot);
        std::lock_guard<std::mutex> lock(box->mutex); box->snapshot = std::move(next);
    }
    reader.join(); CHECK(correct);
    { std::lock_guard<std::mutex> lock(box->mutex); box->active = false; }
    CHECK(!callback(query).available);
    box.reset(); CHECK(!callback(query).available);
    return 0;
}
