#include "processor/model/ModelHotkeys.hpp"
#include "LAppAllocator_Common.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <limits>

using namespace Live2D::Cubism::Framework;

class ModelHotkeysTest : public QObject
{
    Q_OBJECT
    LAppAllocator_Common allocator;

    static void write(const QString& path, const QByteArray& bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    static QJsonObject binding(const QString& id, const QString& file, const QString& key = "Tab")
    {
        return {{"HotkeyID", id}, {"Name", ""}, {"Action", "ToggleExpression"}, {"File", file},
            {"Triggers", QJsonObject{{"Trigger1", key}}}, {"IsActive", true}, {"IsGlobal", true},
            {"FadeSecondsAmount", 0.5}};
    }
    static void fixture(const QString& path, const QJsonArray& bindings)
    {
        QVERIFY(QDir(path).mkpath("exp"));
        write(path + "/test.vtube.json", QJsonDocument(QJsonObject{{"Hotkeys", bindings}}).toJson(QJsonDocument::Compact));
        write(path + "/exp/水印.exp3.json", R"({"Type":"Live2D Expression","Parameters":[{"Id":"ParamAngleX","Value":1,"Blend":"Add"}]})");
        write(path + "/exp/脸红.exp3.json", R"({"Parameters":[{"Id":"ParamAngleY","Value":2,"Blend":"Overwrite"}]})");
    }

private slots:
    void initTestCase()
    {
        QVERIFY(CubismFramework::StartUp(&allocator));
        CubismFramework::Initialize();
    }
    void cleanupTestCase()
    {
        CubismFramework::Dispose();
        CubismFramework::CleanUp();
    }
    void discoversExpAndKeepsIndependentPersistentState()
    {
        QTemporaryDir directory;
        fixture(directory.path(), {binding("watermark", "水印.exp3.json"), binding("blush", "脸红.exp3.json", "N1")});
        ModelHotkeys hotkeys;
        hotkeys.Load(directory.path().toStdString() + "/", "test.model3.json");
        QCOMPARE(hotkeys.Bindings().size(), size_t(2));
        QCOMPARE(hotkeys.Bindings()[0].keys[0], std::string("Tab"));
        QCOMPARE(hotkeys.Bindings()[0].name, std::string("水印.exp3.json"));
        QVERIFY(hotkeys.Bindings()[0].reason.empty());
        std::string error;
        QVERIFY(hotkeys.Trigger("watermark", error));
        hotkeys.Update(0.25f);
        QCOMPARE(hotkeys.Expressions()[0].weight, 0.5f);
        QVERIFY(hotkeys.Trigger("blush", error));
        hotkeys.Update(10);
        QVERIFY(hotkeys.IsActive(hotkeys.Bindings()[0].id));
        QVERIFY(hotkeys.IsActive(hotkeys.Bindings()[1].id));
        QVERIFY(hotkeys.Trigger("watermark", error));
        hotkeys.Update(0.25f);
        QCOMPARE(hotkeys.Expressions()[0].weight, 0.5f);
        hotkeys.Update(0.25f);
        QCOMPARE(hotkeys.Expressions()[0].weight, 0.0f);
        QVERIFY(hotkeys.IsActive(hotkeys.Bindings()[1].id));
        const auto token = hotkeys.Token();
        hotkeys.Reset();
        QVERIFY(token != hotkeys.Token());
        QVERIFY(!hotkeys.IsActive(hotkeys.Bindings()[1].id));
        QCOMPARE(hotkeys.Expressions()[1].weight, 0.0f);
        hotkeys.Load(directory.path().toStdString() + "/", "other.model3.json");
        QVERIFY(token != hotkeys.Token());
        QVERIFY(hotkeys.Bindings().empty());
        QVERIFY(hotkeys.Expressions().empty());
        QVERIFY(!hotkeys.Trigger("watermark", error));
    }
    void resolvesOnlyCurrentBindingIds()
    {
        QTemporaryDir directory;
        fixture(directory.path(), {binding("watermark", "水印.exp3.json")});
        ModelHotkeys hotkeys;
        ModelHotkeys other;
        other.Load(directory.path().toStdString() + "/", "test.model3.json");
        auto foreign = other.Bindings()[0];
        // 公开查询不接受 Binding 或任意内部索引。
        foreign.expression = std::numeric_limits<int>::max();
        QVERIFY(!hotkeys.IsActive(foreign.id));
        hotkeys.Load(directory.path().toStdString() + "/", "test.model3.json");
        const auto old = hotkeys.Bindings()[0];
        const auto token = hotkeys.Token();
        std::string error;
        QVERIFY(hotkeys.Trigger(old.id, error));
        QVERIFY(hotkeys.IsActive(foreign.id)); // 相同 ID 表示当前绑定。
        QVERIFY(!other.IsActive(old.id)); // 不能读取另一个所有者的活动状态。
        QVERIFY(!hotkeys.IsActive("missing"));
        QVERIFY(!hotkeys.IsActive(""));

        fixture(directory.path(), {binding("replacement", "脸红.exp3.json")});
        hotkeys.Load(directory.path().toStdString() + "/", "test.model3.json");
        QVERIFY(token != hotkeys.Token());
        QVERIFY(hotkeys.Trigger("replacement", error));
        QVERIFY(hotkeys.IsActive("replacement"));
        QVERIFY(!hotkeys.IsActive(old.id)); // 旧索引 0 现在属于另一个 ID。
        hotkeys.Load(directory.path().toStdString() + "/", "absent.model3.json");
        QVERIFY(!hotkeys.IsActive("replacement"));
    }
    void listsUnavailableActionsAndHonorsTimeout()
    {
        QTemporaryDir directory;
        auto timed = binding("timed", "水印.exp3.json");
        timed["DeactivateAfterSeconds"] = true;
        timed["DeactivateAfterSecondsAmount"] = 1;
        auto disabled = binding("disabled", "水印.exp3.json");
        disabled["IsActive"] = false;
        auto unsupported = binding("unsupported", "水印.exp3.json");
        unsupported["Action"] = "ChangeBackground";
        fixture(directory.path(), {timed, disabled, unsupported, binding("missing", "absent.exp3.json")});
        ModelHotkeys hotkeys;
        hotkeys.Load(directory.path().toStdString() + "/", "test.model3.json");
        QCOMPARE(hotkeys.Bindings().size(), size_t(4));
        std::string error;
        for (const auto& id : {"disabled", "unsupported", "missing", "unknown"})
            QVERIFY(!hotkeys.Trigger(id, error));
        QVERIFY(hotkeys.Trigger("timed", error));
        hotkeys.Update(0.5f);
        QVERIFY(hotkeys.IsActive(hotkeys.Bindings()[0].id));
        hotkeys.Update(1);
        QVERIFY(!hotkeys.IsActive(hotkeys.Bindings()[0].id));
        QCOMPARE(hotkeys.Expressions()[0].weight, 0.0f);
        write(directory.path() + "/test.vtube.json", "{invalid");
        hotkeys.Load(directory.path().toStdString() + "/", "test.model3.json");
        QVERIFY(!hotkeys.Error().empty());
        QVERIFY(hotkeys.Bindings().empty());
    }
    void appliesWithoutAccumulationAndRestoresBase()
    {
        QFile mocFile(QStringLiteral(SDK_RESOURCES "/Haru/Haru.moc3"));
        QVERIFY(mocFile.open(QIODevice::ReadOnly));
        const auto bytes = mocFile.readAll();
        auto* moc = CubismMoc::Create(reinterpret_cast<const csmByte*>(bytes.constData()), bytes.size());
        QVERIFY(moc);
        auto* model = moc->CreateModel();
        QVERIFY(model);
        QTemporaryDir directory;
        fixture(directory.path(), {binding("watermark", "水印.exp3.json"), binding("blush", "脸红.exp3.json")});
        ModelHotkeys hotkeys;
        hotkeys.Load(directory.path().toStdString() + "/", "test.model3.json");
        const auto x = CubismFramework::GetIdManager()->GetId("ParamAngleX");
        const auto y = CubismFramework::GetIdManager()->GetId("ParamAngleY");
        model->SetParameterValue(x, 3);
        model->SetParameterValue(y, 4);
        model->SaveParameters();
        std::string error;
        QVERIFY(hotkeys.Trigger("watermark", error));
        QVERIFY(hotkeys.Trigger("blush", error));
        for (int frame = 0; frame < 600; ++frame)
        {
            model->LoadParameters();
            hotkeys.Update(1.0f / 60);
            hotkeys.Apply(model);
        }
        QCOMPARE(model->GetParameterValue(x), 4.0f);
        QCOMPARE(model->GetParameterValue(y), 2.0f);
        QVERIFY(hotkeys.Trigger("watermark", error));
        model->LoadParameters();
        hotkeys.Update(1);
        hotkeys.Apply(model);
        QCOMPARE(model->GetParameterValue(x), 3.0f);
        QCOMPARE(model->GetParameterValue(y), 2.0f);
        moc->DeleteModel(model);
        CubismMoc::Delete(moc);
    }
    void importsLocalModelWhenProvided()
    {
        const QString path = qEnvironmentVariable("DESKWAIFU_TEST_MODEL_DIR");
        if (path.isEmpty()) QSKIP("Optional local model assets were not provided");
        ModelHotkeys hotkeys;
        hotkeys.Load(path.toStdString() + "/", QDir(path).dirName().toStdString() + ".model3.json");
        QVERIFY(hotkeys.Error().empty());
        QVERIFY(!hotkeys.Bindings().empty());
        bool foundWatermark = false;
        for (const auto& binding : hotkeys.Bindings())
        {
            QVERIFY2(binding.reason.empty(), binding.reason.c_str());
            if (binding.keys == std::vector<std::string>{"Tab"} && binding.file == "水印.exp3.json")
            {
                const auto& expression = hotkeys.Expressions()[binding.expression];
                QCOMPARE(expression.parameters[0].id, std::string("Param144"));
                QCOMPARE(expression.parameters[0].blend, std::string("Add"));
                foundWatermark = true;
            }
        }
        QVERIFY(foundWatermark);
        qInfo() << "Imported local model hotkeys:" << hotkeys.Bindings().size();
    }
};

QTEST_GUILESS_MAIN(ModelHotkeysTest)
#include "ModelHotkeysTest.moc"
