/*
* Copyright (c) 2019, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#define LOG_TAG "audio_hw::BatteryListener"
#include <log/log.h>
#include <aidl/android/hardware/health/BnHealthInfoCallback.h>
#include <aidl/android/hardware/health/IHealth.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/hardware/health/2.0/IHealth.h>
#include <healthhalutils/HealthHalUtils.h>
#include <hidl/HidlTransportSupport.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "battery_listener.h"

namespace {
namespace ah = aidl::android::hardware::health;
namespace hh = android::hardware::health::V2_0;
using BatteryStatus = android::hardware::health::V1_0::BatteryStatus;
using namespace std::chrono_literals;

bool statusToBool(BatteryStatus status) {
    return status == BatteryStatus::CHARGING || status == BatteryStatus::FULL;
}

BatteryStatus fromAidl(ah::BatteryStatus status) {
    switch (status) {
        case ah::BatteryStatus::CHARGING: return BatteryStatus::CHARGING;
        case ah::BatteryStatus::DISCHARGING: return BatteryStatus::DISCHARGING;
        case ah::BatteryStatus::NOT_CHARGING: return BatteryStatus::NOT_CHARGING;
        case ah::BatteryStatus::FULL: return BatteryStatus::FULL;
        default: return BatteryStatus::UNKNOWN;
    }
}

// Callback objects outlive unregistration on some transports. They only hold
// weak state, never a raw pointer to the listener or to the audio device.
struct ListenerState {
    std::mutex lock;
    std::condition_variable cond;
    BatteryStatus status = BatteryStatus::UNKNOWN;
    uint64_t generation = 0;
    bool done = false;
    bool reconnect = false;

    void update(uint64_t source, BatteryStatus value) {
        std::lock_guard<std::mutex> guard(lock);
        if (!done && source == generation && status != value) {
            status = value;
            cond.notify_all();
        }
    }

    void died(uint64_t source) {
        std::lock_guard<std::mutex> guard(lock);
        if (!done && source == generation) {
            reconnect = true;
            cond.notify_all();
        }
    }
};

struct AidlCallback : public ah::BnHealthInfoCallback {
    std::weak_ptr<ListenerState> state;
    const uint64_t generation;
    AidlCallback(const std::shared_ptr<ListenerState>& s, uint64_t g)
        : state(s), generation(g) {}
    ndk::ScopedAStatus healthInfoChanged(const ah::HealthInfo& info) override {
        if (auto s = state.lock()) s->update(generation, fromAidl(info.batteryStatus));
        return ndk::ScopedAStatus::ok();
    }
};

struct HidlCallback : public hh::IHealthInfoCallback,
                      public android::hardware::hidl_death_recipient {
    std::weak_ptr<ListenerState> state;
    const uint64_t generation;
    HidlCallback(const std::shared_ptr<ListenerState>& s, uint64_t g)
        : state(s), generation(g) {}
    android::hardware::Return<void> healthInfoChanged(const hh::HealthInfo& info) override {
        if (auto s = state.lock()) s->update(generation, info.legacy.batteryStatus);
        return android::hardware::Void();
    }
    void serviceDied(uint64_t, const android::wp<android::hidl::base::V1_0::IBase>&) override {
        if (auto s = state.lock()) s->died(generation);
    }
};

struct DeathCookie {
    std::weak_ptr<ListenerState> state;
    uint64_t generation;
};

void onAidlDied(void* opaque) {
    auto* cookie = static_cast<DeathCookie*>(opaque);
    if (auto state = cookie->state.lock()) state->died(cookie->generation);
}

void onAidlUnlinked(void* opaque) {
    // Also called if linkToDeath fails, and after any in-flight death callback.
    delete static_cast<DeathCookie*>(opaque);
}

class BatteryListener {
public:
    explicit BatteryListener(battery_status_change_fn_t callback) : mCallback(callback) {
        // Match the bounded initial discovery used by the old client. Callbacks
        // only update state until the dispatch thread has been created.
        for (int attempt = 0; attempt < 5 && !mConnected; ++attempt) {
            mConnected = connect();
            if (!mConnected && attempt != 4) std::this_thread::sleep_for(500ms);
        }
        if (!mConnected) ALOGW("Health unavailable; retrying on the listener thread");
        mThread = std::thread(&BatteryListener::run, this);
    }

    ~BatteryListener() { stop(); }

    void stop() {
        {
            std::lock_guard<std::mutex> guard(mState->lock);
            mState->done = true;
            mState->cond.notify_all();
        }
        if (mThread.joinable()) mThread.join();
    }

    bool isCharging() const {
        std::lock_guard<std::mutex> guard(mState->lock);
        return statusToBool(mState->status);
    }

private:
    const std::shared_ptr<ListenerState> mState = std::make_shared<ListenerState>();
    const battery_status_change_fn_t mCallback;
    std::thread mThread;
    bool mConnected = false;
    std::shared_ptr<ah::IHealth> mAidl;
    std::shared_ptr<AidlCallback> mAidlCallback;
    ndk::ScopedAIBinder_DeathRecipient mAidlDeath;
    android::sp<hh::IHealth> mHidl;
    android::sp<HidlCallback> mHidlCallback;

    void disconnect() {
        // Never hold ListenerState::lock across a synchronous Binder call.
        // Increment the generation first so late callbacks are ignored.
        {
            std::lock_guard<std::mutex> guard(mState->lock);
            ++mState->generation;
        }
        if (mAidl) {
            auto result = mAidl->unregisterCallback(mAidlCallback);
            if (!result.isOk()) ALOGV("AIDL Health callback already gone");
        }
        mAidlDeath.set(nullptr);
        mAidlCallback.reset();
        mAidl.reset();
        if (mHidl) {
            auto result = mHidl->unregisterCallback(mHidlCallback);
            if (!result.isOk()) ALOGV("HIDL Health callback already gone");
            auto unlinked = mHidl->unlinkToDeath(mHidlCallback);
            if (!unlinked.isOk()) ALOGV("HIDL Health death recipient already gone");
        }
        mHidlCallback.clear();
        mHidl.clear();
        mConnected = false;
    }

    bool connect() {
        uint64_t generation;
        {
            std::lock_guard<std::mutex> guard(mState->lock);
            if (mState->done) return false;
            generation = ++mState->generation;
            mState->reconnect = false;
        }
        const std::string instance = std::string(ah::IHealth::descriptor) + "/default";
        if (AServiceManager_isDeclared(instance.c_str())) {
            // Do not wait indefinitely in audio initialization, and do not probe
            // nonexistent HIDL services on an AIDL-only device.
            ndk::SpAIBinder binder(AServiceManager_checkService(instance.c_str()));
            if (!binder.get()) return false;
            mAidl = ah::IHealth::fromBinder(binder);
            if (!mAidl) return false;
            ABinderProcess_startThreadPool();
            mAidlCallback = ndk::SharedRefBase::make<AidlCallback>(mState, generation);
            mAidlDeath.set(AIBinder_DeathRecipient_new(onAidlDied));
            AIBinder_DeathRecipient_setOnUnlinked(mAidlDeath.get(), onAidlUnlinked);
            auto* cookie = new DeathCookie{mState, generation};
            if (AIBinder_linkToDeath(binder.get(), mAidlDeath.get(), cookie) != STATUS_OK) {
                disconnect();
                return false;
            }
            ah::BatteryStatus status = ah::BatteryStatus::UNKNOWN;
            auto initial = mAidl->getChargeStatus(&status);
            if (initial.isOk()) mState->update(generation, fromAidl(status));
            auto registered = mAidl->registerCallback(mAidlCallback);
            if (!registered.isOk()) {
                disconnect();
                return false;
            }
            // Close the query/register gap using the provider's normal update path.
            auto updated = mAidl->update();
            if (!updated.isOk()) ALOGW("AIDL Health initial update failed");
            ALOGI("Using AIDL Health battery notifications");
            return true;
        }

        // Preserve devices which still declare the legacy HIDL provider.
        mHidl = hh::get_health_service();
        if (!mHidl) return false;
        mHidlCallback = new HidlCallback(mState, generation);
        auto linked = mHidl->linkToDeath(mHidlCallback, generation);
        if (!linked.isOk() || !static_cast<bool>(linked)) {
            disconnect();
            return false;
        }
        auto initial = mHidl->getChargeStatus([&](hh::Result result, BatteryStatus status) {
            if (result == hh::Result::SUCCESS) mState->update(generation, status);
        });
        if (!initial.isOk()) ALOGW("HIDL Health initial status query failed");
        auto registered = mHidl->registerCallback(mHidlCallback);
        if (!registered.isOk() || static_cast<hh::Result>(registered) != hh::Result::SUCCESS) {
            disconnect();
            return false;
        }
        auto updated = mHidl->update();
        if (!updated.isOk()) ALOGW("HIDL Health initial update failed");
        ALOGI("Using HIDL Health battery notifications");
        return true;
    }

    void run() {
        std::unique_lock<std::mutex> guard(mState->lock);
        auto delivered = mState->status;
        while (!mState->done) {
            if (!mConnected || mState->reconnect) {
                guard.unlock();
                disconnect();
                mConnected = connect();
                guard.lock();
                if (!mConnected) {
                    mState->cond.wait_for(guard, 500ms, [&] { return mState->done; });
                }
                continue;
            }
            if (delivered == mState->status) {
                mState->cond.wait(guard, [&] {
                    return mState->done || mState->reconnect || delivered != mState->status;
                });
                continue;
            }
            const auto status = mState->status;
            if (status == BatteryStatus::NOT_CHARGING &&
                mState->cond.wait_for(guard, 3s, [&] {
                    return mState->done || mState->reconnect || mState->status != status;
                })) {
                continue;
            }
            delivered = status;
            guard.unlock();
            mCallback(statusToBool(status));
            guard.lock();
        }
        guard.unlock();
        disconnect();
    }
};

std::mutex lifecycleLock;
std::mutex listenerLock;
std::shared_ptr<BatteryListener> listener;
}  // namespace

extern "C" {
void battery_properties_listener_init(battery_status_change_fn_t callback) {
    if (!callback) return;
    std::lock_guard<std::mutex> lifecycle(lifecycleLock);
    {
        std::lock_guard<std::mutex> guard(listenerLock);
        if (listener) return;
    }
    auto created = std::make_shared<BatteryListener>(callback);
    std::lock_guard<std::mutex> guard(listenerLock);
    listener = std::move(created);
}

void battery_properties_listener_deinit() {
    std::lock_guard<std::mutex> lifecycle(lifecycleLock);
    std::shared_ptr<BatteryListener> old;
    {
        std::lock_guard<std::mutex> guard(listenerLock);
        old = std::move(listener);
    }
    if (old) old->stop();
}

bool battery_properties_is_charging() {
    std::shared_ptr<BatteryListener> current;
    {
        std::lock_guard<std::mutex> guard(listenerLock);
        current = listener;
    }
    return current && current->isCharging();
}
}  // extern "C"
