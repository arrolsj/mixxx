#pragma once

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include "controllers/legacycontrollermapping.h"
#include "controllers/legacycontrollermappingfilehandler.h"
#include "preferences/usersettings.h"

namespace mixxx::controllers {

inline QString pioneerMappingForInput(const QString& name, bool input) {
    // Match complete device/ALSA port names, never a substring or an output alias.
    static const QRegularExpression pattern(
            QStringLiteral("^(?:PIONEER DJ )?DDJ-(FLX4|400)(?: MIDI(?: [0-9]+)?)?$"),
            QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(name);
    return input && match.hasMatch()
            ? QStringLiteral("Pioneer-DDJ-%1.midi.xml").arg(match.captured(1).toUpper())
            : QString();
}

inline QString pioneerDeviceKey(QString name) {
    return name.replace(" ", "_").replace("/", "_").replace("\\", "_");
}

// ALSA inventory can precede native PortMidi visibility. Retry a new presence
// at most three times; ordinary inventory polling must not become endless I/O.
class PioneerDiscoveryState {
  public:
    void observe(const QStringList& names) {
        QSet<QString> present;
        for (const QString& name : names) {
            if (!pioneerMappingForInput(name, true).isEmpty()) {
                present.insert(name);
                if (!m_attempts.contains(name)) {
                    m_attempts.insert(name, 0);
                }
            }
        }
        for (auto it = m_attempts.begin(); it != m_attempts.end();) {
            it = present.contains(it.key()) ? ++it : m_attempts.erase(it);
        }
    }

    QStringList takeCandidates(bool blocked) {
        QStringList candidates;
        if (!blocked) {
            for (auto it = m_attempts.begin(); it != m_attempts.end(); ++it) {
                if (it.value() < 3) {
                    ++it.value();
                    candidates.append(it.key());
                }
            }
        }
        return candidates;
    }

  private:
    QHash<QString, int> m_attempts;
};

inline QString unconfiguredPioneerMapping(UserSettingsPointer config,
        const QString& name, bool input, bool safeMode) {
    const QString key = pioneerDeviceKey(name);
    // Presence matters: an explicit zero or empty/custom preset is user intent.
    if (safeMode || config->exists(ConfigKey("[Controller]", key)) ||
            config->exists(ConfigKey("[ControllerPreset]", key))) {
        return {};
    }
    return pioneerMappingForInput(name, input);
}

inline bool configureDetectedPioneer(UserSettingsPointer config,
        const QString& name, bool input, const QString& extension,
        bool safeMode, const QDir& mappings) {
    if (extension != QStringLiteral(".midi.xml")) {
        return false;
    }
    const QString filename = unconfiguredPioneerMapping(config, name, input, safeMode);
    if (filename.isEmpty()) {
        return false;
    }
    const auto mapping = LegacyControllerMappingFileHandler::loadMapping(
            QFileInfo(mappings.filePath(filename)), mappings);
    if (!mapping || mapping->getScriptFiles().isEmpty()) {
        return false;
    }
    for (const auto& script : mapping->getScriptFiles()) {
        if (!script.file.isFile() || !script.file.isReadable()) {
            return false;
        }
    }
    // Commit the pair only after the packaged mapping and scripts are available.
    const QString key = pioneerDeviceKey(name);
    config->setValue(ConfigKey("[ControllerPreset]", key), filename);
    config->setValue(ConfigKey("[Controller]", key), 1);
    return true;
}

} // namespace mixxx::controllers
