#include "ereaderintegration.h"

#include <QDebug>
#include <qpa/qplatformintegrationplugin.h>

class EreaderIntegrationPlugin : public QPlatformIntegrationPlugin {
    // Qt declares short parameter names in this macro.
    // NOLINTNEXTLINE(readability-identifier-length)
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "ereader.json")

public:
    QPlatformIntegration* create(const QString& system, const QStringList& paramList) override;
};

QPlatformIntegration*
EreaderIntegrationPlugin::create(const QString& system, const QStringList& paramList) {
    if (system.compare(QStringLiteral("ereader"), Qt::CaseInsensitive) != 0) {
        return nullptr;
    }

    PlatformOptions options;
    QString error;
    if (!PlatformOptions::parse(paramList, options, error)) {
        qWarning() << "ereader:" << error;
        return nullptr;
    }
    return new EreaderIntegration(options);
}

#include "main.moc"
