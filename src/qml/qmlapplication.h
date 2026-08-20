#pragma once

#include <QApplication>
#include <QQmlApplicationEngine>

#include "coreservices.h"
#include "qmlautoreload.h"

class ControlObject;

namespace mixxx {
namespace qml {

class QmlApplication : public QObject {
    Q_OBJECT
  public:
    QmlApplication(
            QApplication* app,
            std::shared_ptr<CoreServices> pCoreServices,
            const QString& mainFilePath = QString());
    ~QmlApplication() override;

  public slots:
    void loadQml(const QString& path);

  private:
    std::shared_ptr<CoreServices> m_pCoreServices;

    QString m_mainFilePath;

    std::unique_ptr<ControlObject> m_pPerformancePadModeDeck1Control;
    std::unique_ptr<ControlObject> m_pPerformancePadModeDeck2Control;
    std::unique_ptr<ControlObject> m_pPerformancePadParameterDeck1Control;
    std::unique_ptr<ControlObject> m_pPerformancePadParameterDeck2Control;

    std::unique_ptr<QQmlApplicationEngine> m_pAppEngine;
    QmlAutoReload m_autoReload;
};

} // namespace qml
} // namespace mixxx
