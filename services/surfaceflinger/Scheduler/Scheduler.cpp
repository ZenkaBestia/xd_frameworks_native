/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "Scheduler"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "Scheduler.h"

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <configstore/Utils.h>
#include <input/InputWindow.h>
#include <system/window.h>
#include <ui/DisplayStatInfo.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include <FrameTimeline/FrameTimeline.h>
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>

#include "../Layer.h"
#include "DispSyncSource.h"
#include "EventThread.h"
#include "InjectVSyncSource.h"
#include "OneShotTimer.h"
#include "SchedulerUtils.h"
#include "SurfaceFlingerProperties.h"
#include "Timer.h"
#include "VSyncDispatchTimerQueue.h"
#include "VSyncPredictor.h"
#include "VSyncReactor.h"
#include "VsyncController.h"

#define RETURN_IF_INVALID_HANDLE(handle, ...)                        \
    do {                                                             \
        if (mConnections.count(handle) == 0) {                       \
            ALOGE("Invalid connection handle %" PRIuPTR, handle.id); \
            return __VA_ARGS__;                                      \
        }                                                            \
    } while (false)

using namespace std::string_literals;

namespace android {

namespace {

std::unique_ptr<scheduler::VSyncTracker> createVSyncTracker() {
    // TODO(b/144707443): Tune constants.
    constexpr int kDefaultRate = 60;
    constexpr auto initialPeriod = std::chrono::duration<nsecs_t, std::ratio<1, kDefaultRate>>(1);
    constexpr nsecs_t idealPeriod =
            std::chrono::duration_cast<std::chrono::nanoseconds>(initialPeriod).count();
    constexpr size_t vsyncTimestampHistorySize = 20;
    constexpr size_t minimumSamplesForPrediction = 6;
    constexpr uint32_t discardOutlierPercent = 20;
    return std::make_unique<scheduler::VSyncPredictor>(idealPeriod, vsyncTimestampHistorySize,
                                                       minimumSamplesForPrediction,
                                                       discardOutlierPercent);
}

std::unique_ptr<scheduler::VSyncDispatch> createVSyncDispatch(scheduler::VSyncTracker& tracker) {
    // TODO(b/144707443): Tune constants.
    constexpr std::chrono::nanoseconds vsyncMoveThreshold = 3ms;
    constexpr std::chrono::nanoseconds timerSlack = 500us;
    return std::make_unique<
            scheduler::VSyncDispatchTimerQueue>(std::make_unique<scheduler::Timer>(), tracker,
                                                timerSlack.count(), vsyncMoveThreshold.count());
}

const char* toContentDetectionString(bool useContentDetection) {
    return useContentDetection ? "on" : "off";
}

} // namespace

class PredictedVsyncTracer {
public:
    PredictedVsyncTracer(scheduler::VSyncDispatch& dispatch)
          : mRegistration(dispatch, std::bind(&PredictedVsyncTracer::callback, this),
                          "PredictedVsyncTracer") {
        scheduleRegistration();
    }

private:
    TracedOrdinal<bool> mParity = {"VSYNC-predicted", 0};
    scheduler::VSyncCallbackRegistration mRegistration;

    void scheduleRegistration() { mRegistration.schedule({0, 0, 0}); }

    void callback() {
        mParity = !mParity;
        scheduleRegistration();
    }
};

Scheduler::Scheduler(const scheduler::RefreshRateConfigs& configs, ISchedulerCallback& callback)
      : Scheduler(configs, callback,
                  {.supportKernelTimer = sysprop::support_kernel_idle_timer(false),
                   .useContentDetection = sysprop::use_content_detection_for_refresh_rate(false)}) {
}

Scheduler::Scheduler(const scheduler::RefreshRateConfigs& configs, ISchedulerCallback& callback,
                     Options options)
      : Scheduler(createVsyncSchedule(options.supportKernelTimer), configs, callback,
                  createLayerHistory(configs), options) {
    using namespace sysprop;

    const int setIdleTimerMs = base::GetIntProperty("debug.sf.set_idle_timer_ms"s, 0);

    if (const auto millis = setIdleTimerMs ? setIdleTimerMs : set_idle_timer_ms(0); millis > 0) {
        const auto callback = mOptions.supportKernelTimer ? &Scheduler::kernelIdleTimerCallback
                                                          : &Scheduler::idleTimerCallback;
        mIdleTimer.emplace(
                "IdleTimer", std::chrono::milliseconds(millis),
                [this, callback] { std::invoke(callback, this, TimerState::Reset); },
                [this, callback] { std::invoke(callback, this, TimerState::Expired); });
        mIdleTimer->start();
    }

    if (const int64_t millis = set_touch_timer_ms(0); millis > 0) {
        // Touch events are coming to SF every 100ms, so the timer needs to be higher than that
        mTouchTimer.emplace(
                "TouchTimer", std::chrono::milliseconds(millis),
                [this] { touchTimerCallback(TimerState::Reset); },
                [this] { touchTimerCallback(TimerState::Expired); });
        mTouchTimer->start();
    }

    if (const int64_t millis = set_display_power_timer_ms(0); millis > 0) {
        mDisplayPowerTimer.emplace(
                "DisplayPowerTimer", std::chrono::milliseconds(millis),
                [this] { displayPowerTimerCallback(TimerState::Reset); },
                [this] { displayPowerTimerCallback(TimerState::Expired); });
        mDisplayPowerTimer->start();
    }
}

Scheduler::Scheduler(VsyncSchedule schedule, const scheduler::RefreshRateConfigs& configs,
                     ISchedulerCallback& schedulerCallback,
                     std::unique_ptr<LayerHistory> layerHistory, Options options)
      : mOptions(options),
        mVsyncSchedule(std::move(schedule)),
        mLayerHistory(std::move(layerHistory)),
        mSchedulerCallback(schedulerCallback),
        mRefreshRateConfigs(configs),
        mPredictedVsyncTracer(
                base::GetBoolProperty("debug.sf.show_predicted_vsync", false)
                        ? std::make_unique<PredictedVsyncTracer>(*mVsyncSchedule.dispatch)
                        : nullptr) {
    mSchedulerCallback.setVsyncEnabled(false);
}

Scheduler::~Scheduler() {
    // Ensure the OneShotTimer threads are joined before we start destroying state.
    mDisplayPowerTimer.reset();
    mTouchTimer.reset();
    mIdleTimer.reset();
}

Scheduler::VsyncSchedule Scheduler::createVsyncSchedule(bool supportKernelTimer) {
    auto clock = std::make_unique<scheduler::SystemClock>();
    auto tracker = createVSyncTracker();
    auto dispatch = createVSyncDispatch(*tracker);

    // TODO(b/144707443): Tune constants.
    constexpr size_t pendingFenceLimit = 20;
    auto controller =
            std::make_unique<scheduler::VSyncReactor>(std::move(clock), *tracker, pendingFenceLimit,
                                                      supportKernelTimer);
    return {std::move(controller), std::move(tracker), std::move(dispatch)};
}

std::unique_ptr<LayerHistory> Scheduler::createLayerHistory(
        const scheduler::RefreshRateConfigs& configs) {
    return std::make_unique<scheduler::LayerHistory>(configs);
}

std::unique_ptr<VSyncSource> Scheduler::makePrimaryDispSyncSource(
        const char* name, std::chrono::nanoseconds workDuration,
        std::chrono::nanoseconds readyDuration, bool traceVsync) {
    return std::make_unique<scheduler::DispSyncSource>(*mVsyncSchedule.dispatch, workDuration,
                                                       readyDuration, traceVsync, name);
}

std::optional<Fps> Scheduler::getFrameRateOverride(uid_t uid) const {
    if (!mRefreshRateConfigs.supportsFrameRateOverride()) {
        return std::nullopt;
    }

    std::lock_guard lock(mFrameRateOverridesMutex);
    {
        const auto iter = mFrameRateOverridesFromBackdoor.find(uid);
        if (iter != mFrameRateOverridesFromBackdoor.end()) {
            return std::make_optional<Fps>(iter->second);
        }
    }

    {
        const auto iter = mFrameRateOverridesByContent.find(uid);
        if (iter != mFrameRateOverridesByContent.end()) {
            return std::make_optional<Fps>(iter->second);
        }
    }

    return std::nullopt;
}

bool Scheduler::isVsyncValid(nsecs_t expectedVsyncTimestamp, uid_t uid) const {
    const auto frameRate = getFrameRateOverride(uid);
    if (!frameRate.has_value()) {
        return true;
    }

    return mVsyncSchedule.tracker->isVSyncInPhase(expectedVsyncTimestamp, *frameRate);
}

impl::EventThread::ThrottleVsyncCallback Scheduler::makeThrottleVsyncCallback() const {
    if (!mRefreshRateConfigs.supportsFrameRateOverride()) {
        return {};
    }

    return [this](nsecs_t expectedVsyncTimestamp, uid_t uid) {
        return !isVsyncValid(expectedVsyncTimestamp, uid);
    };
}

impl::EventThread::GetVsyncPeriodFunction Scheduler::makeGetVsyncPeriodFunction() const {
    return [this](uid_t uid) {
        nsecs_t basePeriod = mRefreshRateConfigs.getCurrentRefreshRate().getVsyncPeriod();
        const auto frameRate = getFrameRateOverride(uid);
        if (!frameRate.has_value()) {
            return basePeriod;
        }

        const auto divider = scheduler::RefreshRateConfigs::getFrameRateDivider(
            mRefreshRateConfigs.getCurrentRefreshRate().getFps(), *frameRate);
        if (divider <= 1) {
            return basePeriod;
        }
        return basePeriod * divider;
    };
}

Scheduler::ConnectionHandle Scheduler::createConnection(
        const char* connectionName, frametimeline::TokenManager* tokenManager,
        std::chrono::nanoseconds workDuration, std::chrono::nanoseconds readyDuration,
        impl::EventThread::InterceptVSyncsCallback interceptCallback) {
    auto vsyncSource = makePrimaryDispSyncSource(connectionName, workDuration, readyDuration);
    auto throttleVsync = makeThrottleVsyncCallback();
    auto getVsyncPeriod = makeGetVsyncPeriodFunction();
    auto eventThread = std::make_unique<impl::EventThread>(std::move(vsyncSource), tokenManager,
                                                           std::move(interceptCallback),
                                                           std::move(throttleVsync),
                                                           std::move(getVsyncPeriod));
    bool triggerRefresh = !strcmp(connectionName, "app");
    return createConnection(std::move(eventThread), triggerRefresh);
}

Scheduler::ConnectionHandle Scheduler::createConnection(std::unique_ptr<EventThread> eventThread,
                                                        bool triggerRefresh) {
    const ConnectionHandle handle = ConnectionHandle{mNextConnectionHandleId++};
    ALOGV("Creating a connection handle with ID %" PRIuPTR, handle.id);

    auto connection = createConnectionInternal(eventThread.get(), triggerRefresh);

    std::lock_guard<std::mutex> lock(mConnectionsLock);
    mConnections.emplace(handle, Connection{connection, std::move(eventThread)});
    return handle;
}

sp<EventThreadConnection> Scheduler::createConnectionInternal(EventThread* eventThread,
                 bool triggerRefresh, ISurfaceComposer::EventRegistrationFlags eventRegistration) {
    // Refresh need to be triggered from app thread alone.
    // Triggering it from sf connection can result in infinite loop due to requestnextvsync.
    if (triggerRefresh) {
        return eventThread->createEventConnection([&] { resyncAndRefresh(); }, eventRegistration);
    } else {
        return eventThread->createEventConnection([&] { resync(); }, eventRegistration);
    }
}

sp<IDisplayEventConnection> Scheduler::createDisplayEventConnection(ConnectionHandle handle,
                 bool triggerRefresh, ISurfaceComposer::EventRegistrationFlags eventRegistration) {
    std::lock_guard<std::mutex> lock(mConnectionsLock);
    RETURN_IF_INVALID_HANDLE(handle, nullptr);
    return createConnectionInternal(mConnections[handle].thread.get(), triggerRefresh,
                                    eventRegistration);
}

sp<EventThreadConnection> Scheduler::getEventConnection(ConnectionHandle handle) {
    std::lock_guard<std::mutex> lock(mConnectionsLock);
    RETURN_IF_INVALID_HANDLE(handle, nullptr);
    return mConnections[handle].connection;
}

void Scheduler::onHotplugReceived(ConnectionHandle handle, PhysicalDisplayId displayId,
                                  bool connected) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }

    thread->onHotplugReceived(displayId, connected);
}

void Scheduler::onScreenAcquired(ConnectionHandle handle) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onScreenAcquired();
}

void Scheduler::onScreenReleased(ConnectionHandle handle) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onScreenReleased();
}

void Scheduler::onFrameRateOverridesChanged(ConnectionHandle handle, PhysicalDisplayId displayId) {
    std::vector<FrameRateOverride> overrides;
    {
        std::lock_guard lock(mFrameRateOverridesMutex);
        for (const auto& [uid, frameRate] : mFrameRateOverridesFromBackdoor) {
            overrides.emplace_back(FrameRateOverride{uid, frameRate.getValue()});
        }
        for (const auto& [uid, frameRate] : mFrameRateOverridesByContent) {
            if (mFrameRateOverridesFromBackdoor.count(uid) == 0) {
                overrides.emplace_back(FrameRateOverride{uid, frameRate.getValue()});
            }
        }
    }
    android::EventThread* thread;
    {
        std::lock_guard lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onFrameRateOverridesChanged(displayId, std::move(overrides));
}

void Scheduler::onPrimaryDisplayModeChanged(ConnectionHandle handle, PhysicalDisplayId displayId,
                                            DisplayModeId modeId, nsecs_t vsyncPeriod) {
    {
        std::lock_guard<std::mutex> lock(mFeatureStateLock);
        // Cache the last reported modes for primary display.
        mFeatures.cachedModeChangedParams = {handle, displayId, modeId, vsyncPeriod};

        // Invalidate content based refresh rate selection so it could be calculated
        // again for the new refresh rate.
        mFeatures.contentRequirements.clear();
    }
    onNonPrimaryDisplayModeChanged(handle, displayId, modeId, vsyncPeriod);
}

void Scheduler::dispatchCachedReportedMode() {
    // Check optional fields first.
    if (!mFeatures.modeId.has_value()) {
        ALOGW("No mode ID found, not dispatching cached mode.");
        return;
    }
    if (!mFeatures.cachedModeChangedParams.has_value()) {
        ALOGW("No mode changed params found, not dispatching cached mode.");
        return;
    }

    const auto modeId = *mFeatures.modeId;
    // If the modeId is not the current mode, this means that a
    // mode change is in progress. In that case we shouldn't dispatch an event
    // as it will be dispatched when the current mode changes.
    if (mRefreshRateConfigs.getCurrentRefreshRate().getModeId() != modeId) {
        return;
    }

    const auto vsyncPeriod = mRefreshRateConfigs.getRefreshRateFromModeId(modeId).getVsyncPeriod();

    // If there is no change from cached mode, there is no need to dispatch an event
    if (modeId == mFeatures.cachedModeChangedParams->modeId &&
        vsyncPeriod == mFeatures.cachedModeChangedParams->vsyncPeriod) {
        return;
    }

    mFeatures.cachedModeChangedParams->modeId = modeId;
    mFeatures.cachedModeChangedParams->vsyncPeriod = vsyncPeriod;
    onNonPrimaryDisplayModeChanged(mFeatures.cachedModeChangedParams->handle,
                                   mFeatures.cachedModeChangedParams->displayId,
                                   mFeatures.cachedModeChangedParams->modeId,
                                   mFeatures.cachedModeChangedParams->vsyncPeriod);
}

void Scheduler::onNonPrimaryDisplayModeChanged(ConnectionHandle handle, PhysicalDisplayId displayId,
                                               DisplayModeId modeId, nsecs_t vsyncPeriod) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onModeChanged(displayId, modeId, vsyncPeriod);
}

size_t Scheduler::getEventThreadConnectionCount(ConnectionHandle handle) {
    std::lock_guard<std::mutex> lock(mConnectionsLock);
    RETURN_IF_INVALID_HANDLE(handle, 0);
    return mConnections[handle].thread->getEventThreadConnectionCount();
}

void Scheduler::dump(ConnectionHandle handle, std::string& result) const {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections.at(handle).thread.get();
    }
    thread->dump(result);
}

void Scheduler::setDuration(ConnectionHandle handle, std::chrono::nanoseconds workDuration,
                            std::chrono::nanoseconds readyDuration) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->setDuration(workDuration, readyDuration);
}

DisplayStatInfo Scheduler::getDisplayStatInfo(nsecs_t now) {
    const auto vsyncTime = mVsyncSchedule.tracker->nextAnticipatedVSyncTimeFrom(now);
    const auto vsyncPeriod = mVsyncSchedule.tracker->currentPeriod();
    return DisplayStatInfo{.vsyncTime = vsyncTime, .vsyncPeriod = vsyncPeriod};
}

Scheduler::ConnectionHandle Scheduler::enableVSyncInjection(bool enable) {
    if (mInjectVSyncs == enable) {
        return {};
    }

    ALOGV("%s VSYNC injection", enable ? "Enabling" : "Disabling");

    if (!mInjectorConnectionHandle) {
        auto vsyncSource = std::make_unique<InjectVSyncSource>();
        mVSyncInjector = vsyncSource.get();

        auto eventThread =
                std::make_unique<impl::EventThread>(std::move(vsyncSource),
                                                    /*tokenManager=*/nullptr,
                                                    impl::EventThread::InterceptVSyncsCallback(),
                                                    impl::EventThread::ThrottleVsyncCallback(),
                                                    impl::EventThread::GetVsyncPeriodFunction());

        // EventThread does not dispatch VSYNC unless the display is connected and powered on.
        eventThread->onHotplugReceived(PhysicalDisplayId::fromPort(0), true);
        eventThread->onScreenAcquired();

        mInjectorConnectionHandle = createConnection(std::move(eventThread), false /*No Refresh*/);
    }

    mInjectVSyncs = enable;
    return mInjectorConnectionHandle;
}

bool Scheduler::injectVSync(nsecs_t when, nsecs_t expectedVSyncTime, nsecs_t deadlineTimestamp) {
    if (!mInjectVSyncs || !mVSyncInjector) {
        return false;
    }

    mVSyncInjector->onInjectSyncEvent(when, expectedVSyncTime, deadlineTimestamp);
    return true;
}

void Scheduler::enableHardwareVsync() {
    std::lock_guard<std::mutex> lock(mHWVsyncLock);
    if (!mPrimaryHWVsyncEnabled && mHWVsyncAvailable) {
        mVsyncSchedule.tracker->resetModel();
        mSchedulerCallback.setVsyncEnabled(true);
        mPrimaryHWVsyncEnabled = true;
    }
}

void Scheduler::disableHardwareVsync(bool makeUnavailable) {
    std::lock_guard<std::mutex> lock(mHWVsyncLock);
    if (mPrimaryHWVsyncEnabled) {
        mSchedulerCallback.setVsyncEnabled(false);
        mPrimaryHWVsyncEnabled = false;
    }
    if (makeUnavailable) {
        mHWVsyncAvailable = false;
    }
}

void Scheduler::resyncToHardwareVsync(bool makeAvailable, nsecs_t period, bool force_resync) {
    {
        std::lock_guard<std::mutex> lock(mHWVsyncLock);
        if (makeAvailable) {
            mHWVsyncAvailable = makeAvailable;
        } else if (!mHWVsyncAvailable) {
            // Hardware vsync is not currently available, so abort the resync
            // attempt for now
            return;
        }
    }

    if (period <= 0) {
        return;
    }

    setVsyncPeriod(period, force_resync);
}

void Scheduler::resyncAndRefresh() {
    resync();

    if (!mDisplayIdle) {
        return;
    }

    ATRACE_CALL();
    const auto& refreshRate = mRefreshRateConfigs.getCurrentRefreshRate();
    mSchedulerCallback.repaintEverythingForHWC();
    resyncToHardwareVsync(true /* makeAvailable */, refreshRate.getVsyncPeriod(), true);
    mDisplayIdle = false;
}

void Scheduler::resync() {
    static constexpr nsecs_t kIgnoreDelay = ms2ns(750);

    const nsecs_t now = systemTime();
    const nsecs_t last = mLastResyncTime.exchange(now);

    if (now - last > kIgnoreDelay) {
        resyncToHardwareVsync(false, mRefreshRateConfigs.getCurrentRefreshRate().getVsyncPeriod());
    }
}

void Scheduler::setVsyncPeriod(nsecs_t period, bool force_resync) {
    std::lock_guard<std::mutex> lock(mHWVsyncLock);
    mVsyncSchedule.controller->startPeriodTransition(period);

    if (!mPrimaryHWVsyncEnabled || force_resync) {
        mVsyncSchedule.tracker->resetModel();
        mSchedulerCallback.setVsyncEnabled(true);
        mPrimaryHWVsyncEnabled = true;
    }
}

void Scheduler::addResyncSample(nsecs_t timestamp, std::optional<nsecs_t> hwcVsyncPeriod,
                                bool* periodFlushed) {
    bool needsHwVsync = false;
    *periodFlushed = false;
    { // Scope for the lock
        std::lock_guard<std::mutex> lock(mHWVsyncLock);
        if (mPrimaryHWVsyncEnabled) {
            needsHwVsync = mVsyncSchedule.controller->addHwVsyncTimestamp(timestamp, hwcVsyncPeriod,
                                                                          periodFlushed);
        }
    }

    if (needsHwVsync) {
        enableHardwareVsync();
    } else {
        disableHardwareVsync(false);
    }
}

void Scheduler::addPresentFence(const std::shared_ptr<FenceTime>& fenceTime) {
    if (mVsyncSchedule.controller->addPresentFence(fenceTime)) {
        enableHardwareVsync();
    } else {
        disableHardwareVsync(false);
    }
}

void Scheduler::setIgnorePresentFences(bool ignore) {
    mVsyncSchedule.controller->setIgnorePresentFences(ignore);
}

void Scheduler::registerLayer(Layer* layer) {
    scheduler::LayerHistory::LayerVoteType voteType;

    if (!mOptions.useContentDetection ||
        layer->getWindowType() == InputWindowInfo::Type::STATUS_BAR) {
        voteType = scheduler::LayerHistory::LayerVoteType::NoVote;
    } else if (layer->getWindowType() == InputWindowInfo::Type::WALLPAPER) {
        // Running Wallpaper at Min is considered as part of content detection.
        voteType = scheduler::LayerHistory::LayerVoteType::Min;
    } else {
        voteType = scheduler::LayerHistory::LayerVoteType::Heuristic;
    }

    // If the content detection feature is off, we still keep the layer history,
    // since we use it for other features (like Frame Rate API), so layers
    // still need to be registered.
    mLayerHistory->registerLayer(layer, voteType);
}

void Scheduler::deregisterLayer(Layer* layer) {
    mLayerHistory->deregisterLayer(layer);
}

void Scheduler::recordLayerHistory(Layer* layer, nsecs_t presentTime,
                                   LayerHistory::LayerUpdateType updateType) {
    if (mRefreshRateConfigs.canSwitch()) {
        mLayerHistory->record(layer, presentTime, systemTime(), updateType);
    }
}

void Scheduler::setModeChangePending(bool pending) {
    mLayerHistory->setModeChangePending(pending);
}

void Scheduler::chooseRefreshRateForContent() {
    if (!mRefreshRateConfigs.canSwitch()) return;

    ATRACE_CALL();

    scheduler::LayerHistory::Summary summary = mLayerHistory->summarize(systemTime());
    scheduler::RefreshRateConfigs::GlobalSignals consideredSignals;
    DisplayModeId newModeId;
    bool frameRateChanged;
    bool frameRateOverridesChanged;
    {
        std::lock_guard<std::mutex> lock(mFeatureStateLock);
        mFeatures.contentRequirements = summary;

        newModeId = calculateRefreshRateModeId(&consideredSignals);
        auto newRefreshRate = mRefreshRateConfigs.getRefreshRateFromModeId(newModeId);
        frameRateOverridesChanged =
                updateFrameRateOverrides(consideredSignals, newRefreshRate.getFps());

        if (mFeatures.modeId == newModeId) {
            // We don't need to change the display mode, but we might need to send an event
            // about a mode change, since it was suppressed due to a previous idleConsidered
            if (!consideredSignals.idle) {
                dispatchCachedReportedMode();
            }
            frameRateChanged = false;
        } else {
            mFeatures.modeId = newModeId;

            if (mThermalFps > 0 && (int32_t)newRefreshRate.getFps().getValue() >
                (int32_t)mThermalFps) {
                DisplayModePtr mode;
                mSchedulerCallback.getModeFromFps(mThermalFps, mode);
                mFeatures.modeId = mode->getId();
            }

            frameRateChanged = true;
        }
    }
    if (frameRateChanged) {
        auto newRefreshRate = mRefreshRateConfigs.getRefreshRateFromModeId(newModeId);

        if (mThermalFps > 0 && (int32_t)newRefreshRate.getFps().getValue() >
            (int32_t)mThermalFps) {
            DisplayModePtr mode;
            mSchedulerCallback.getModeFromFps(mThermalFps, mode);
            auto newThermalRefreshRate = mRefreshRateConfigs.
                                         getRefreshRateFromModeId(mode->getId());
            mSchedulerCallback.changeRefreshRate(newThermalRefreshRate,
                                              consideredSignals.idle ? ModeEvent::None
                                                                     : ModeEvent::Changed);
        } else {
            mSchedulerCallback.changeRefreshRate(newRefreshRate,
                                             consideredSignals.idle ? ModeEvent::None
                                                                    : ModeEvent::Changed);
        }
    }
    if (frameRateOverridesChanged) {
        mSchedulerCallback.triggerOnFrameRateOverridesChanged();
    }
}

void Scheduler::resetIdleTimer() {
    if (mIdleTimer) {
        mIdleTimer->reset();
    }
}

void Scheduler::notifyTouchEvent() {
    if (mTouchTimer) {
        mTouchTimer->reset();

        if (mOptions.supportKernelTimer && mIdleTimer) {
            mIdleTimer->reset();
        }
    }
}

void Scheduler::setDisplayPowerState(bool normal) {
    {
        std::lock_guard<std::mutex> lock(mFeatureStateLock);
        mFeatures.isDisplayPowerStateNormal = normal;
    }

    if (mDisplayPowerTimer) {
        mDisplayPowerTimer->reset();
    }

    // Display Power event will boost the refresh rate to performance.
    // Clear Layer History to get fresh FPS detection
    mLayerHistory->clear();
}

void Scheduler::kernelIdleTimerCallback(TimerState state) {
    ATRACE_INT("ExpiredKernelIdleTimer", static_cast<int>(state));

    // TODO(145561154): cleanup the kernel idle timer implementation and the refresh rate
    // magic number
    const auto& refreshRate = mRefreshRateConfigs.getCurrentRefreshRate();
    constexpr Fps FPS_THRESHOLD_FOR_KERNEL_TIMER{65.0f};
    if (state == TimerState::Reset &&
        refreshRate.getFps().greaterThanWithMargin(FPS_THRESHOLD_FOR_KERNEL_TIMER)) {
        // If we're not in performance mode then the kernel timer shouldn't do
        // anything, as the refresh rate during DPU power collapse will be the
        // same.
        resyncToHardwareVsync(true /* makeAvailable */, refreshRate.getVsyncPeriod());
    } else if (state == TimerState::Expired &&
               refreshRate.getFps().lessThanOrEqualWithMargin(FPS_THRESHOLD_FOR_KERNEL_TIMER)) {
        // Disable HW VSYNC if the timer expired, as we don't need it enabled if
        // we're not pushing frames, and if we're in PERFORMANCE mode then we'll
        // need to update the VsyncController model anyway.
        disableHardwareVsync(false /* makeUnavailable */);
    }

    mSchedulerCallback.kernelTimerChanged(state == TimerState::Expired);
}

void Scheduler::idleTimerCallback(TimerState state) {
    if (mHandleIdleTimeout) {
        handleTimerStateChanged(&mFeatures.idleTimer, state);
    }
    ATRACE_INT("ExpiredIdleTimer", static_cast<int>(state));
}

void Scheduler::touchTimerCallback(TimerState state) {
    const TouchState touch = state == TimerState::Reset ? TouchState::Active : TouchState::Inactive;
    // Touch event will boost the refresh rate to performance.
    // Clear layer history to get fresh FPS detection.
    // NOTE: Instead of checking all the layers, we should be checking the layer
    // that is currently on top. b/142507166 will give us this capability.
    if (handleTimerStateChanged(&mFeatures.touch, touch)) {
        mLayerHistory->clear();
    }
    ATRACE_INT("TouchState", static_cast<int>(touch));
}

void Scheduler::displayPowerTimerCallback(TimerState state) {
    handleTimerStateChanged(&mFeatures.displayPowerTimer, state);
    ATRACE_INT("ExpiredDisplayPowerTimer", static_cast<int>(state));
}

void Scheduler::dump(std::string& result) const {
    using base::StringAppendF;

    StringAppendF(&result, "+  Idle timer: %s\n", mIdleTimer ? mIdleTimer->dump().c_str() : "off");
    StringAppendF(&result, "+  Touch timer: %s\n",
                  mTouchTimer ? mTouchTimer->dump().c_str() : "off");
    StringAppendF(&result, "+  Content detection: %s %s\n\n",
                  toContentDetectionString(mOptions.useContentDetection),
                  mLayerHistory ? mLayerHistory->dump().c_str() : "(no layer history)");

    {
        std::lock_guard lock(mFrameRateOverridesMutex);
        StringAppendF(&result, "Frame Rate Overrides (backdoor): {");
        for (const auto& [uid, frameRate] : mFrameRateOverridesFromBackdoor) {
            StringAppendF(&result, "[uid: %d frameRate: %s], ", uid, to_string(frameRate).c_str());
        }
        StringAppendF(&result, "}\n");

        StringAppendF(&result, "Frame Rate Overrides (setFrameRate): {");
        for (const auto& [uid, frameRate] : mFrameRateOverridesByContent) {
            StringAppendF(&result, "[uid: %d frameRate: %s], ", uid, to_string(frameRate).c_str());
        }
        StringAppendF(&result, "}\n");
    }
}

void Scheduler::dumpVsync(std::string& s) const {
    using base::StringAppendF;

    StringAppendF(&s, "VSyncReactor:\n");
    mVsyncSchedule.controller->dump(s);
    StringAppendF(&s, "VSyncDispatch:\n");
    mVsyncSchedule.dispatch->dump(s);
}

bool Scheduler::updateFrameRateOverrides(
        scheduler::RefreshRateConfigs::GlobalSignals consideredSignals, Fps displayRefreshRate) {
    if (!mRefreshRateConfigs.supportsFrameRateOverride()) {
        return false;
    }

    if (!consideredSignals.idle) {
        const auto frameRateOverrides =
                mRefreshRateConfigs.getFrameRateOverrides(mFeatures.contentRequirements,
                                                          displayRefreshRate,
                                                          consideredSignals.touch);
        std::lock_guard lock(mFrameRateOverridesMutex);
        if (!std::equal(mFrameRateOverridesByContent.begin(), mFrameRateOverridesByContent.end(),
                        frameRateOverrides.begin(), frameRateOverrides.end(),
                        [](const std::pair<uid_t, Fps>& a, const std::pair<uid_t, Fps>& b) {
                            return a.first == b.first && a.second.equalsWithMargin(b.second);
                        })) {
            mFrameRateOverridesByContent = frameRateOverrides;
            return true;
        }
    }
    return false;
}

template <class T>
bool Scheduler::handleTimerStateChanged(T* currentState, T newState) {
    DisplayModeId newModeId;
    bool refreshRateChanged = false;
    bool frameRateOverridesChanged;
    scheduler::RefreshRateConfigs::GlobalSignals consideredSignals;
    {
        std::lock_guard<std::mutex> lock(mFeatureStateLock);
        if (*currentState == newState) {
            return false;
        }
        *currentState = newState;
        newModeId = calculateRefreshRateModeId(&consideredSignals);
        const RefreshRate& newRefreshRate = mRefreshRateConfigs.getRefreshRateFromModeId(newModeId);
        frameRateOverridesChanged =
                updateFrameRateOverrides(consideredSignals, newRefreshRate.getFps());
        if (mFeatures.modeId == newModeId) {
            // We don't need to change the display mode, but we might need to send an event
            // about a mode change, since it was suppressed due to a previous idleConsidered
            if (!consideredSignals.idle) {
                dispatchCachedReportedMode();
            }
        } else {
            mFeatures.modeId = newModeId;

            if (mThermalFps > 0 && (int32_t)newRefreshRate.getFps().getValue() >
                (int32_t)mThermalFps) {
                DisplayModePtr mode;
                mSchedulerCallback.getModeFromFps(mThermalFps, mode);
                mFeatures.modeId = mode->getId();
            }

            refreshRateChanged = true;
        }
    }
    if (refreshRateChanged) {
        const RefreshRate& newRefreshRate = mRefreshRateConfigs.getRefreshRateFromModeId(newModeId);

        if (mThermalFps > 0 && (int32_t)newRefreshRate.getFps().getValue() >
            (int32_t)mThermalFps) {
            DisplayModePtr mode;
            mSchedulerCallback.getModeFromFps(mThermalFps, mode);
            auto newThermalRefreshRate = mRefreshRateConfigs.
                                         getRefreshRateFromModeId(mode->getId());
            mSchedulerCallback.changeRefreshRate(newThermalRefreshRate,
                                              consideredSignals.idle ? ModeEvent::None
                                                                     : ModeEvent::Changed);
        } else {
            mSchedulerCallback.changeRefreshRate(newRefreshRate,
                                             consideredSignals.idle ? ModeEvent::None
                                                                    : ModeEvent::Changed);
        }
    }
    if (frameRateOverridesChanged) {
        mSchedulerCallback.triggerOnFrameRateOverridesChanged();
    }
    return consideredSignals.touch;
}

DisplayModeId Scheduler::calculateRefreshRateModeId(
        scheduler::RefreshRateConfigs::GlobalSignals* consideredSignals) {
    ATRACE_CALL();
    if (consideredSignals) *consideredSignals = {};

    // If Display Power is not in normal operation we want to be in performance mode. When coming
    // back to normal mode, a grace period is given with DisplayPowerTimer.
    if (mDisplayPowerTimer &&
        (!mFeatures.isDisplayPowerStateNormal ||
         mFeatures.displayPowerTimer == TimerState::Reset)) {
        return mRefreshRateConfigs.getMaxRefreshRateByPolicy().getModeId();
    }

    const bool touchActive = mTouchTimer && mFeatures.touch == TouchState::Active;
    const bool idle = mIdleTimer && mFeatures.idleTimer == TimerState::Expired;

    return mRefreshRateConfigs
            .getBestRefreshRate(mFeatures.contentRequirements, {.touch = touchActive, .idle = idle},
                                consideredSignals)
            .getModeId();
}

std::optional<DisplayModeId> Scheduler::getPreferredModeId() {
    std::lock_guard<std::mutex> lock(mFeatureStateLock);
    // Make sure that the default mode ID is first updated, before returned.
    if (mFeatures.modeId.has_value()) {
        mFeatures.modeId = calculateRefreshRateModeId();
    }
    return mFeatures.modeId;
}

void Scheduler::onNewVsyncPeriodChangeTimeline(const hal::VsyncPeriodChangeTimeline& timeline) {
    if (timeline.refreshRequired) {
        mSchedulerCallback.repaintEverythingForHWC();
    }

    std::lock_guard<std::mutex> lock(mVsyncTimelineLock);
    mLastVsyncPeriodChangeTimeline = std::make_optional(timeline);

    const auto maxAppliedTime = systemTime() + MAX_VSYNC_APPLIED_TIME.count();
    if (timeline.newVsyncAppliedTimeNanos > maxAppliedTime) {
        mLastVsyncPeriodChangeTimeline->newVsyncAppliedTimeNanos = maxAppliedTime;
    }
}

void Scheduler::onDisplayRefreshed(nsecs_t timestamp) {
    bool callRepaint = false;
    {
        std::lock_guard<std::mutex> lock(mVsyncTimelineLock);
        if (mLastVsyncPeriodChangeTimeline && mLastVsyncPeriodChangeTimeline->refreshRequired) {
            if (mLastVsyncPeriodChangeTimeline->refreshTimeNanos < timestamp) {
                mLastVsyncPeriodChangeTimeline->refreshRequired = false;
            } else {
                // We need to send another refresh as refreshTimeNanos is still in the future
                callRepaint = true;
            }
        }
    }

    if (callRepaint) {
        mSchedulerCallback.repaintEverythingForHWC();
    }
}

void Scheduler::onPrimaryDisplayAreaChanged(uint32_t displayArea) {
    mLayerHistory->setDisplayArea(displayArea);
}

void Scheduler::setPreferredRefreshRateForUid(FrameRateOverride frameRateOverride) {
    if (frameRateOverride.frameRateHz > 0.f && frameRateOverride.frameRateHz < 1.f) {
        return;
    }

    std::lock_guard lock(mFrameRateOverridesMutex);
    if (frameRateOverride.frameRateHz != 0.f) {
        mFrameRateOverridesFromBackdoor[frameRateOverride.uid] = Fps(frameRateOverride.frameRateHz);
    } else {
        mFrameRateOverridesFromBackdoor.erase(frameRateOverride.uid);
    }
}

std::chrono::steady_clock::time_point Scheduler::getPreviousVsyncFrom(
        nsecs_t expectedPresentTime) const {
    const auto presentTime = std::chrono::nanoseconds(expectedPresentTime);
    const auto vsyncPeriod = std::chrono::nanoseconds(mVsyncSchedule.tracker->currentPeriod());
    return std::chrono::steady_clock::time_point(presentTime - vsyncPeriod);
}

void Scheduler::setIdleState() {
    mDisplayIdle = true;
}

void Scheduler::updateThermalFps(float fps) {
    mThermalFps = fps;
    mLayerHistory->updateThermalFps(fps);
}

} // namespace android
