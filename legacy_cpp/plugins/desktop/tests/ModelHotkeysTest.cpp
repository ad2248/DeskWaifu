#include "processor/model/ModelHotkeys.hpp"
#include "LAppAllocator_Common.hpp"

#include <Id/CubismIdManager.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace Live2D::Cubism::Framework;

class ModelHotkeysTest : public QObject {
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
        return { { "HotkeyID", id }, { "Name", "" }, { "Action", "ToggleExpression" }, { "File", file }, { "Triggers", QJsonObject { { "Trigger1", key } } }, { "IsActive", true },
            { "IsGlobal", true }, { "FadeSecondsAmount", 0.5 } };
    }
    static void fixture(const QString& path, const QJsonArray& bindings)
    {
        QVERIFY(QDir(path).mkpath("exp"));
        write(path + "/test.vtube.json", QJsonDocument(QJsonObject { { "Hotkeys", bindings } }).toJson(QJsonDocument::Compact));
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
        fixture(directory.path(), { binding("watermark", "水印.exp3.json"), binding("blush", "脸红.exp3.json") });
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
        for (int frame = 0; frame < 600; ++frame) {
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
        if (path.isEmpty()) {
            QSKIP("Optional local model assets were not provided");
        }
        ModelHotkeys hotkeys;
        hotkeys.Load(path.toStdString() + "/", QDir(path).dirName().toStdString() + ".model3.json");
        QVERIFY(hotkeys.Error().empty());
        QVERIFY(!hotkeys.Bindings().empty());
        bool foundWatermark = false;
        for (const auto& binding : hotkeys.Bindings()) {
            QVERIFY2(binding.reason.empty(), binding.reason.c_str());
            if (binding.keys == std::vector<std::string> { "Tab" } && binding.file == "水印.exp3.json") {
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
