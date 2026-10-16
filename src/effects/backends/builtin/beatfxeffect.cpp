#include "effects/backends/builtin/beatfxeffect.h"

#include "engine/effects/engineeffectparameter.h"

namespace {
using namespace performance_fx;
int capacity(const mixxx::EngineParameters& p) {
    return static_cast<int>(std::max(48000.0,static_cast<double>(p.sampleRate()))*4);
}
float read(const mixxx::SampleBuffer& ring,int write,int channel,double delay) {
    const int frames=ring.size()/2;
    const int whole=static_cast<int>(delay);
    const float fraction=static_cast<float>(delay-whole);
    const int a=(write+frames-whole)%frames,b=(a+frames-1)%frames;
    return ring[2*a+channel]+fraction*(ring[2*b+channel]-ring[2*a+channel]);
}
void capture(RollState* s) {
    const int frames=s->history.size()/2;
    s->length=s->requested;s->capturedPeak=0;
    for(int f=0;f<s->length;++f) {
        const int from=(s->write+frames-s->length+f)%frames;
        for(int c=0;c<2;++c) {
            const float value=s->history[2*from+c];
            s->loop[2*f+c]=value;
            s->capturedPeak=std::max(s->capturedPeak,std::abs(value));
        }
    }
    s->read=0;s->active=true;
}
} // namespace

BeatDelayState::BeatDelayState(const mixxx::EngineParameters& p)
        : EffectState(p),history(capacity(p)*2) {
    history.clear();dry.value=dry.target=1;
}
QString BeatEchoEffect::getId() { return QStringLiteral("org.mixxx.effects.beatecho"); }
QString BeatDelayEffect::getId() { return QStringLiteral("org.mixxx.effects.beatdelay"); }
EffectManifestPointer BeatEchoEffect::getManifest() { return manifest(getId(),QObject::tr("Echo")); }
EffectManifestPointer BeatDelayEffect::getManifest() { return manifest(getId(),QObject::tr("Delay")); }
void BeatEchoEffect::loadEngineEffectParameters(const QMap<QString,EngineEffectParameterPointer>& p) {
    m_time=p.value("delay_time");m_depth=p.value("depth");
}
void BeatEchoEffect::processChannel(BeatDelayState* s,const CSAMPLE* input,CSAMPLE* output,
        const mixxx::EngineParameters& p,EffectEnableState enable,const GroupFeatureState& features) {
    const double fs=p.sampleRate();const int frames=s->history.size()/2,ramp=rampFrames(fs);
    const bool on=enabled(enable);const auto gains=mix(m_depth->value());
    s->requested=static_cast<int>(std::lround(periodFrames(m_time->value(),fs,features,frames)));
    if(s->delay==0) {
        s->delay=s->requested;
        if(!m_echo) s->inputGain.value=s->inputGain.target=1;
    }
    s->dc.configure(fs);
    s->limiter.configure(fs);
    s->inputGain.set(on || !m_echo ? 1 : 0,ramp);
    s->dry.set(on ? gains[0] : 1,ramp);
    s->wet.set(on || m_echo ? gains[1] : 0,ramp);
    for(int f=0;f<p.framesPerBuffer();++f) {
        if(s->phase==0 && s->requested!=s->delay) { s->phase=-1;s->fadeRemaining=ramp; }
        if(s->phase) {
            s->fade+=static_cast<float>(s->phase)/ramp;
            if(--s->fadeRemaining==0) {
                if(s->phase<0) { s->delay=s->requested;s->fade=0;s->phase=1;s->fadeRemaining=ramp; }
                else { s->fade=1;s->phase=0; }
            }
        }
        const float send=s->inputGain.next(),dry=s->dry.next(),wet=s->wet.next();
        float activity=0;
        for(int c=0;c<2;++c) {
            const int i=2*f+c;
            const float delayed=read(s->history,s->write,c,s->delay)*s->fade;
            s->history[2*s->write+c]=input[i]*send+(m_echo ? .7f*delayed : 0);
            const float filtered=s->dc.process(delayed,c);
            output[i]=dry*input[i]+wet*filtered;
            activity=std::max(activity,std::max(std::abs(input[i]*send),std::abs(filtered*wet)));
        }
        if (m_echo && wet > 0) s->limiter.process(output+2*f);
        else s->limiter.gain=1;
        if((on || m_echo) && activity>1e-9f) s->tailFrames=s->delay*2+static_cast<int>(fs);
        else if(s->tailFrames>0) --s->tailFrames;
        s->write=(s->write+1)%frames;
    }
}

SpiralState::SpiralState(const mixxx::EngineParameters& p)
        : EffectState(p),history(capacity(p)*2),
          pitch(static_cast<int>(std::max(48000.0,static_cast<double>(p.sampleRate()))*.04+4)*2) {
    history.clear();pitch.clear();dry.value=dry.target=1;
}
QString SpiralEffect::getId() { return QStringLiteral("org.mixxx.effects.spiral"); }
EffectManifestPointer SpiralEffect::getManifest() { return manifest(getId(),QObject::tr("Spiral")); }
void SpiralEffect::loadEngineEffectParameters(const QMap<QString,EngineEffectParameterPointer>& p) {
    m_time=p.value("delay_time");m_depth=p.value("depth");
}
void SpiralEffect::processChannel(SpiralState* s,const CSAMPLE* input,CSAMPLE* output,
        const mixxx::EngineParameters& p,EffectEnableState enable,const GroupFeatureState& features) {
    const double fs=p.sampleRate();const int frames=s->history.size()/2,ramp=rampFrames(fs);
    const bool on=enabled(enable);
    const double target=periodFrames(m_time->value(),fs,features,frames);
    if(s->fs!=fs) {
        s->fs=fs;s->delay=target;s->velocity=0;
        s->pre.configure(fs,12000,false);
        s->feedbackHigh.configure(fs,150,true);
        s->feedbackLow.configure(fs,12100,false);
        s->outputLow.configure(fs,10000,false);
        s->dc.configure(fs);
    }
    if(enable==EffectEnableState::Enabling) s->peak=0;
    const auto gains=mix(m_depth->value());
    s->inputGain.set(on ? 1 : 0,ramp);s->dry.set(on ? gains[0] : 1,ramp);
    // Default Spiral's wet path is unity at depth=.5.
    s->wet.set(static_cast<float>(std::min(1.0,m_depth->value()*2)),ramp);
    const int pitchFrames=s->pitch.size()/2;
    // The native mode-0 controller updates once per callback. Positive and
    // negative changes use different acceleration constants; near the target
    // the remaining distance determines braking rather than a fixed ramp.
    float error=static_cast<float>(target)-static_cast<float>(s->delay);
    if(error>1) {
        const float bound=static_cast<float>(static_cast<int>(fs*4));
        error=std::min(error,bound);
        if(s->velocity>.01 && error<fs*.0010000000474974513) {
            s->desiredVelocity=.01;
            s->acceleration=(.01-s->velocity)/error;
        } else {
            const double r=error/bound;
            s->desiredVelocity=.01+.20000000298023224*(1-std::pow(1-r,3));
            s->acceleration=(s->desiredVelocity-s->velocity)*static_cast<float>(.5/fs);
        }
    } else if(error< -1) {
        const float bound=static_cast<float>(static_cast<int>(-fs*2));
        error=std::max(error,bound);
        if(s->velocity< -.01 && error> -fs*.0010000000474974513) {
            s->desiredVelocity=-.01;
            s->acceleration=(s->velocity+.01)/error;
        } else {
            const double r=error/bound;
            s->desiredVelocity=-.01-.40156862139701843*(1-std::pow(1-r,3));
            s->acceleration=(s->desiredVelocity-s->velocity)*static_cast<float>((10.0/3)/fs);
        }
    }
    if(s->velocity> -.011 && s->velocity<.011 && error> -1 && error<1) {
        s->delay=target;s->velocity=0;s->acceleration=0;s->desiredVelocity=0;
    }
    for(int f=0;f<p.framesPerBuffer();++f) {
        const float send=s->inputGain.next(),dry=s->dry.next(),wet=s->wet.next();
        s->peak=std::max(s->peak,std::abs((input[2*f]+input[2*f+1])*.5f*send));
        const float limit=.6f*s->peak;
        s->delay=std::max(fs*.001,s->delay+s->velocity);
        s->velocity=std::clamp(s->velocity+s->acceleration,-2.0,.8);
        if((s->acceleration>0 && s->velocity>=s->desiredVelocity) ||
                (s->acceleration<=0 && s->velocity<=s->desiredVelocity)) s->acceleration=0;
        s->jitter+=(s->jitterUp ? 1.0f : -1.0f)/24000.0f;
        const float jitterLimit=static_cast<float>(fs/96000);
        if(s->jitter>jitterLimit) { s->jitter=2*jitterLimit-s->jitter;s->jitterUp=false; }
        else if(s->jitter< -jitterLimit) { s->jitter=-2*jitterLimit-s->jitter;s->jitterUp=true; }
        float activity=0;
        for(int c=0;c<2;++c) {
            const int i=2*f+c;
            const float delayed=read(s->history,s->write,c,std::clamp(s->delay+s->jitter,1.0,static_cast<double>(frames-2)));
            const float pre=s->pre.process(input[i]*send,c);
            const float clipped=std::clamp(pre+delayed,-limit,limit);
            s->history[2*s->write+c]=.5f*s->feedbackLow.process(s->feedbackHigh.process(clipped,c),c);
            const float filtered=s->dc.process(s->outputLow.process(delayed,c),c);
            // The default zero-pitch branch still has a 30 ms internal offset.
            const float pitched=read(s->pitch,s->pitchWrite,c,fs*.03);
            s->pitch[2*s->pitchWrite+c]=filtered;
            output[i]=dry*input[i]+wet*pitched;
            activity=std::max(activity,std::max(std::abs(pre),std::abs(pitched*wet)));
        }
        if(activity>1e-9f) s->tailFrames=static_cast<int>(fs*5);
        else if(s->tailFrames>0) --s->tailFrames;
        s->write=(s->write+1)%frames;s->pitchWrite=(s->pitchWrite+1)%pitchFrames;
    }
    s->delay=static_cast<float>(s->delay);
    s->velocity=static_cast<float>(s->velocity);
    s->acceleration=static_cast<float>(s->acceleration);
}

RollState::RollState(const mixxx::EngineParameters& p)
        : EffectState(p),history(capacity(p)*2),loop(capacity(p)*2) { history.clear();loop.clear(); }
QString RollEffect::getId() { return QStringLiteral("org.mixxx.effects.roll"); }
EffectManifestPointer RollEffect::getManifest() { return manifest(getId(),QObject::tr("Roll")); }
void RollEffect::loadEngineEffectParameters(const QMap<QString,EngineEffectParameterPointer>& p) {
    m_time=p.value("delay_time");m_depth=p.value("depth");
}
void RollEffect::processChannel(RollState* s,const CSAMPLE* input,CSAMPLE* output,
        const mixxx::EngineParameters& p,EffectEnableState enable,const GroupFeatureState& features) {
    const double fs=p.sampleRate();const int frames=s->history.size()/2,ramp=rampFrames(fs);
    const bool on=enabled(enable);
    s->requested=static_cast<int>(std::lround(periodFrames(m_time->value(),fs,features,frames)));
    if(on && (!s->wasEnabled || enable==EffectEnableState::Enabling)) {
        s->active=false;s->wait=s->requested;
        if(features.beat_fraction_buffer_end && features.beat_length) {
            const double beatFrames=features.beat_length->seconds*fs;
            const double fraction=*features.beat_fraction_buffer_end-p.framesPerBuffer()/beatFrames;
            const double period=m_time->value();
            double phase=std::fmod(fraction,period);if(phase<0) phase+=period;
            s->wait=std::max(1,static_cast<int>(std::lround((period-phase)*beatFrames)));
        }
        s->wet.set(0,ramp);
    }
    if(!on) { s->wait=0;s->wet.set(0,ramp); }
    for(int f=0;f<p.framesPerBuffer();++f) {
        for(int c=0;c<2;++c) s->history[2*s->write+c]=input[2*f+c];
        s->write=(s->write+1)%frames;
        if(on && s->wait>0 && --s->wait==0) capture(s);
        if(on && s->active) s->wet.set(static_cast<float>(std::min(1.0,m_depth->value()*2)),ramp);
        const float wet=s->wet.next();
        const int seam=std::min(ramp,std::max(1,s->length/2));
        for(int c=0;c<2;++c) {
            const int i=2*f+c;float loop=s->active ? s->loop[2*s->read+c] : input[i];
            // A short seam envelope is independent of the full beat window.
            if(s->active && s->read<seam) {
                const float t=static_cast<float>(s->read)/seam;
                loop=t*loop+(1-t)*s->loop[2*(s->length-seam+s->read)+c];
            }
            output[i]=input[i]+wet*(loop-input[i]);
        }
        if(s->active && ++s->read>=s->length) {
            s->read=0;
            if(on && s->requested!=s->length) capture(s);
        }
        if(!on && s->wet.remaining==0) { s->active=false;s->capturedPeak=0; }
    }
    s->wasEnabled=on;
}
