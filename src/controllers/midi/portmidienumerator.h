#pragma once

#include "controllers/midi/midienumerator.h"

class PortMidiController;

/// This class handles discovery and enumeration of DJ controllers that appear under the PortMIDI cross-platform API.
class PortMidiEnumerator : public MidiEnumerator {
    Q_OBJECT
  public:
    PortMidiEnumerator();
    ~PortMidiEnumerator() override;

    QList<Controller*> queryDevices() override;

  private:
    bool m_hasQueriedDevices{false};
    QList<PortMidiController*> m_devices;
    // Removed devices stay alive so queued GUI and QML signals can never
    // dereference freed Controller objects. A returning device with the same
    // name is rebound and reused.
    QList<PortMidiController*> m_retiredDevices;
};

// For testing.
bool shouldLinkInputToOutput(const QString& input_name,
        const QString& output_name);
