#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "effects/defs.h"
#include "engine/effects/groupfeaturestate.h"

namespace performance_fx {

// Short, sample-counted ramps must keep their duration across callbacks.
struct Ramp {
    float value = 0;
    float target = 0;
    float step = 0;
    int remaining = 0;
    void set(float next, int frames) {
        if (next == target) return;
        target = next;
        remaining = std::max(1, frames);
        step = (target - value) / remaining;
    }
    float next() {
        if (remaining && --remaining == 0) value = target;
        else if (remaining) value += step;
        return value;
    }
};

// Second-order main filter, with the opposite first-order response in the
// resonance feedback path. A fixed-Q biquad alone does not implement this.
struct Filter {
    struct Memory { float x1=0, x2=0, y1=0, y2=0, feedback=0; };
    std::array<Memory, 2> memory{};
    float b0=1, b1=0, b2=0, a1=0, a2=0;
    float f0=0, f1=0, fa=0, resonance=0;
    void configure(double fs, double hz, bool high, float res = 0) {
        const double t = std::tan(3.14159265358979323846 *
                std::clamp(hz, 1.0, fs * 0.45) / fs);
        const double den = 1 + std::sqrt(2.0) * t + t*t;
        b0 = static_cast<float>((high ? 1 : t*t) / den);
        b1 = (high ? -2 : 2) * b0;
        b2 = b0;
        a1 = static_cast<float>(2*(t*t-1)/den);
        a2 = static_cast<float>((1-std::sqrt(2.0)*t+t*t)/den);
        f0 = static_cast<float>((high ? t : 1)/(1+t));
        f1 = high ? f0 : -f0;
        fa = static_cast<float>((t-1)/(t+1));
        resonance = res;
    }
    float process(float input, int channel) {
        auto& m = memory[channel];
        const float x = input + resonance*m.feedback;
        float y = b0*x + b1*m.x1 + b2*m.x2 - a1*m.y1 - a2*m.y2;
        m.feedback = f0*y + f1*m.y1 - fa*m.feedback;
        m.x2=m.x1; m.x1=x; m.y2=m.y1; m.y1=y;
        if (std::abs(y)<1e-30f) { y=0; m.y1=0; }
        if (std::abs(m.feedback)<1e-30f) m.feedback=0;
        return y;
    }
};

struct DcBlock {
    std::array<float,2> previousInput{}, previousOutput{};
    float pole=0;
    void configure(double fs) { pole=static_cast<float>(std::exp(-2*3.14159265358979323846*30/fs)); }
    float process(float x, int channel) {
        float y=x-previousInput[channel]+pole*previousOutput[channel];
        if (std::abs(y)<1e-30f) y=0;
        previousInput[channel]=x;previousOutput[channel]=y;
        return y;
    }
};

inline int rampFrames(double fs) { return std::max(1,static_cast<int>(fs/300)); }
inline bool enabled(EffectEnableState state) {
    return state==EffectEnableState::Enabled || state==EffectEnableState::Enabling;
}
inline double periodFrames(double beats, double fs,
        const GroupFeatureState& features, int capacity) {
    const double seconds=features.beat_length ? features.beat_length->seconds : 0.5;
    return std::clamp(beats*seconds*fs,1.0,static_cast<double>(capacity-2));
}
inline std::array<float,2> mix(double depth) {
    const float wet=static_cast<float>(std::min(1.0,depth*4/3));
    return {static_cast<float>(std::min(1.0,(1-depth)*4)),wet};
}
inline EffectManifestPointer manifest(const QString& id, const QString& name) {
    EffectManifestPointer result(new EffectManifest());
    result->setId(id);result->setName(name);result->setShortName(name);
    result->setAuthor(QStringLiteral("The LastCUE Box Team"));result->setVersion("1.0");
    result->setDescription(QObject::tr("Beat effect with default auxiliary parameters"));
    result->setEffectRampsFromDry(true);result->setMetaknobDefault(0.5);
    auto time=result->addParameter();
    time->setId("delay_time");time->setName(QObject::tr("Time"));time->setShortName(QObject::tr("Time"));
    time->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    time->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    time->setRange(0.125,1,2);
    auto depth=result->addParameter();
    depth->setId("depth");depth->setName(QObject::tr("Level"));depth->setShortName(QObject::tr("Level"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    depth->setRange(0,0.5,1);
    return result;
}
} // namespace performance_fx
