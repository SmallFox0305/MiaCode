#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <QtGlobal>

namespace miacode::preview_audio {

// Worker-owned native observation; GUI/render callers never query the audio driver.
struct PlaybackClockSample {
    bool valid = false;
    double second = 0.0;
    double rate = 1.0;
    qint64 sampledAtNs = 0;
};

inline qint64 playbackClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline constexpr double kPlaybackClockMaxExtrapolationSeconds = 0.1;
inline constexpr qint64 kPlaybackClockMaxQueryNs = 10000000;

inline double extrapolatePlaybackClock(const PlaybackClockSample& sample, qint64 nowNs)
{
    // Freeze after a missing worker update rather than running arbitrarily far ahead
    // of a blocked audio device. A new observation removes accumulated underrun drift.
    const double elapsed = std::clamp(
        static_cast<double>(nowNs - sample.sampledAtNs) / 1.0e9,
        0.0, kPlaybackClockMaxExtrapolationSeconds);
    return sample.second + elapsed * sample.rate;
}

// GUI-owned interpolator: quantized device positions and a recovered underrun may
// move the observation backwards. Hold briefly until audio catches up, never rewind
// note judgments. A transport generation/rate change starts a new monotonic segment.
class PlaybackClockFollower
{
public:
    double follow(const PlaybackClockSample& sample, quint64 generation, qint64 nowNs)
    {
        const double next = extrapolatePlaybackClock(sample, nowNs);
        if (!active_ || generation_ != generation || rate_ != sample.rate) {
            lastSecond_ = next;
        } else {
            lastSecond_ = std::max(lastSecond_, next);
        }
        active_ = true;
        generation_ = generation;
        rate_ = sample.rate;
        return lastSecond_;
    }
    void reset() { active_ = false; }
private:
    bool active_ = false;
    quint64 generation_ = 0;
    double rate_ = 1.0;
    double lastSecond_ = 0.0;
};

}  // namespace miacode::preview_audio
