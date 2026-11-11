#include "../private/processor/ocr/OcrBackend.hpp"
#include "OcrEngine.hpp"
#include <QObject>

class NativeOcrBackend : public OcrBackend {
public:
    QJsonObject Initialize(const QString& detection, const QString& recognition) override
    {
        try {
            _engine = std::make_unique<OcrEngine>(detection, recognition);
            return { { "ok", true } };
        } catch (const OcrModelError& error) {
            return { { "ok", false }, { "code", error.code }, { "error", QString::fromUtf8(error.what()) } };
        } catch (const std::exception& error) {
            return { { "ok", false }, { "code", "model_load_failed" }, { "error", QString::fromUtf8(error.what()) } };
        }
    }

    QJsonObject Recognize(const QString& image) override
    {
        try {
            return { { "ok", true }, { "result", _engine->Recognize(image) } };
        } catch (const std::exception& error) {
            return { { "ok", false }, { "error", QString::fromUtf8(error.what()) } };
        }
    }

    void Release() noexcept override { _engine.reset(); }

private:
    std::unique_ptr<OcrEngine> _engine;
};

class OcrModule : public QObject, public OcrBackendFactory {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DeskWaifuOcrBackend_iid)
    Q_INTERFACES(OcrBackendFactory)
public:
    OcrBackend* Create() override { return new NativeOcrBackend; }
};

#include "OcrModule.moc"
