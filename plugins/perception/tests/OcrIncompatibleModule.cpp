#include <QObject>

class IncompatibleModule : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.deskwaifu.OcrBackendFactory/0.0")
};

#include "OcrIncompatibleModule.moc"
