#include "audio/PreviewAudioClock.h"
#include <QString>
#include <QTextStream>

#include "audio/BassPreviewSfxSchedulerPolicy.h"

namespace {

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << Qt::endl;
    }
    return condition;
}

}  // namespace

int main()
{
    using miacode::preview_audio::bass::SfxSchedulerAnchor;
    using miacode::preview_audio::bass::chartSecondForMixerSecond;
    using miacode::preview_audio::bass::mixerSecondForChartSecond;
    using miacode::preview_audio::bass::shouldLogDisarm;

    QTextStream err(stderr);
    QTextStream out(stdout);
    bool ok = true;

    ok &= require(
        shouldLogDisarm(true, false, -1),
        QStringLiteral("an active scheduler disarm remains visible"), err);
    ok &= require(
        shouldLogDisarm(false, true, -1),
        QStringLiteral("a disarm with a sync remains visible"), err);
    ok &= require(
        shouldLogDisarm(false, false, 0),
        QStringLiteral("a disarm with a group remains visible"), err);
    ok &= require(
        !shouldLogDisarm(false, false, -1),
        QStringLiteral("only a proven no-op disarm is suppressed"), err);

    const SfxSchedulerAnchor oneX {10.0, 120.0, 1.0};
    ok &= require(
        mixerSecondForChartSecond(oneX, 12.5) == 122.5,
        QStringLiteral("one-times chart seconds advance the master mixer equally"), err);

    const SfxSchedulerAnchor halfX {10.0, 120.0, 0.5};
    ok &= require(
        mixerSecondForChartSecond(halfX, 12.5) == 125.0,
        QStringLiteral("half-speed chart seconds map through the active playback rate"), err);

    const SfxSchedulerAnchor invalidRate {10.0, 120.0, 0.0};
    ok &= require(
        mixerSecondForChartSecond(invalidRate, 12.5) == 122.5,
        QStringLiteral("an invalid rate falls back to one-times scheduling"), err);

    const SfxSchedulerAnchor rebuildAnchor {5.0, 100.0, 1.0};
    ok &= require(
        chartSecondForMixerSecond(rebuildAnchor, 150.0) == 55.0,
        QStringLiteral("a settings rebuild reanchors at the current master position, not the last SFX"), err);

    using namespace miacode::preview_audio;
    PlaybackClockFollower follower;
    PlaybackClockSample clock{true, 10.0, 1.0, 1000000000};
    ok &= require(std::abs(follower.follow(clock, 1, 1050000000) - 10.05) < 1e-9,
                  QStringLiteral("worker timestamps interpolate between audio observations"), err);
    clock = {true, 10.035, 1.0, 1050000000}; // 15 ms lost to an output underrun
    ok &= require(std::abs(follower.follow(clock, 1, 1050000000) - 10.05) < 1e-9,
                  QStringLiteral("underrun correction holds instead of rewinding visuals"), err);
    ok &= require(std::abs(follower.follow(clock, 1, 1080000000) - 10.065) < 1e-9,
                  QStringLiteral("visuals resume on audio time without retaining the lost 15 ms"), err);
    ok &= require(std::abs(follower.follow(clock, 1, 2050000000) - 10.135) < 1e-9,
                  QStringLiteral("missing worker updates cannot extrapolate indefinitely"), err);
    clock = {true, 2.0, 1.0, 3000000000};
    ok &= require(follower.follow(clock, 2, 3000000000) == 2.0,
                  QStringLiteral("a backwards seek starts a new clock segment"), err);
    clock = {true, 1.0, 0.5, 4000000000};
    ok &= require(std::abs(follower.follow(clock, 2, 4050000000) - 1.025) < 1e-9,
                  QStringLiteral("a rate change resets interpolation and scales elapsed time"), err);
    follower.reset();
    clock = {true, -1.0, 1.0, 5000000000};
    ok &= require(follower.follow(clock, 2, 5000000000) == -1.0,
                  QStringLiteral("negative pre-roll remains valid after a transport reset"), err);

    if (ok) {
        out << "BASS preview SFX scheduler policy spec passed." << Qt::endl;
    }
    return ok ? 0 : 1;
}
