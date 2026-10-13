#include <array>
#include <cmath>

#include "control/controlobject.h"
#include "engine/enginevumeter.h"
#include "test/mixxxtest.h"

namespace {

class EngineVuMeterTest : public MixxxTest {
  protected:
    EngineVuMeterTest()
            : m_rate(ConfigKey("[App]", "samplerate")),
              m_meter(QStringLiteral("[MeterTest]")) {
        m_rate.set(44100);
    }

    double peakDb() const {
        return ControlObject::get(ConfigKey("[MeterTest]", "peak_meter_db"));
    }

    void publish(CSAMPLE left, CSAMPLE right) {
        std::array<CSAMPLE, 1024> buffer;
        for (size_t i = 0; i < buffer.size(); i += 2) {
            buffer[i] = left;
            buffer[i + 1] = right;
        }
        const auto original = buffer;
        for (int i = 0; i < 3; ++i) {
            m_meter.process(buffer.data(), static_cast<int>(buffer.size()));
        }
        EXPECT_EQ(original, buffer);
    }

    ControlObject m_rate;
    EngineVuMeter m_meter;
};

TEST_F(EngineVuMeterTest, SilenceAndReset) {
    EXPECT_DOUBLE_EQ(-192, peakDb());
    publish(0, 0);
    EXPECT_DOUBLE_EQ(-192, peakDb());
    publish(1, 1);
    EXPECT_DOUBLE_EQ(0, peakDb());
    m_meter.reset();
    EXPECT_DOUBLE_EQ(-192, peakDb());
}

TEST_F(EngineVuMeterTest, UnityReferenceAndUnclippedHeadroom) {
    for (const CSAMPLE amplitude : {0.001f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f}) {
        publish(amplitude, -amplitude);
        EXPECT_NEAR(20.0 * std::log10(amplitude), peakDb(), 1e-5);
    }
}

TEST_F(EngineVuMeterTest, LouderSideWinsWithoutStereoAveraging) {
    publish(0.5f, 0.01f);
    EXPECT_NEAR(-6.0205999, peakDb(), 1e-5);
    publish(0.01f, -0.5f);
    EXPECT_NEAR(-6.0205999, peakDb(), 1e-5);
}

TEST_F(EngineVuMeterTest, FirstNegativeSampleAndTransientSurviveWindow) {
    std::array<CSAMPLE, 1024> buffer{};
    buffer[0] = -0.5f;
    m_meter.process(buffer.data(), static_cast<int>(buffer.size()));
    EXPECT_DOUBLE_EQ(-192, peakDb());
    buffer[0] = 0;
    m_meter.process(buffer.data(), static_cast<int>(buffer.size()));
    m_meter.process(buffer.data(), static_cast<int>(buffer.size()));
    EXPECT_NEAR(-6.0205999, peakDb(), 1e-5);
    publish(0, 0);
    EXPECT_DOUBLE_EQ(-192, peakDb());
}

TEST_F(EngineVuMeterTest, DistinguishesLevelsAfterLegacyVuSaturates) {
    publish(0.5f, 0.5f);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[MeterTest]", "VuMeter")));
    EXPECT_NEAR(-6.0205999, peakDb(), 1e-5);
    publish(1, 1);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[MeterTest]", "VuMeter")));
    EXPECT_DOUBLE_EQ(0, peakDb());
}

TEST_F(EngineVuMeterTest, ResetDiscardsUnpublishedPeak) {
    std::array<CSAMPLE, 1024> buffer{};
    buffer[3] = 1;
    m_meter.process(buffer.data(), static_cast<int>(buffer.size()));
    m_meter.reset();
    publish(0, 0);
    EXPECT_DOUBLE_EQ(-192, peakDb());
}

} // namespace
