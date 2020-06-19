/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "VibratorHalWrapper"

#include <android/hardware/vibrator/1.3/IVibrator.h>
#include <android/hardware/vibrator/BnVibratorCallback.h>
#include <android/hardware/vibrator/IVibrator.h>
#include <hardware/vibrator.h>

#include <utils/Log.h>

#include <vibratorservice/VibratorCallbackScheduler.h>
#include <vibratorservice/VibratorHalWrapper.h>

using android::hardware::vibrator::CompositeEffect;
using android::hardware::vibrator::Effect;
using android::hardware::vibrator::EffectStrength;

using std::chrono::milliseconds;

namespace V1_0 = android::hardware::vibrator::V1_0;
namespace V1_1 = android::hardware::vibrator::V1_1;
namespace V1_2 = android::hardware::vibrator::V1_2;
namespace V1_3 = android::hardware::vibrator::V1_3;
namespace Aidl = android::hardware::vibrator;

namespace android {

namespace vibrator {

// -------------------------------------------------------------------------------------------------

template <class T>
HalResult<T> loadCached(const std::function<HalResult<T>()>& loadFn, std::optional<T>& cache) {
    if (cache.has_value()) {
        // Return copy of cached value.
        return HalResult<T>::ok(*cache);
    }
    HalResult<T> ret = loadFn();
    if (ret.isOk()) {
        // Cache copy of returned value.
        cache.emplace(ret.value());
    }
    return ret;
}

template <class T>
bool isStaticCastValid(Effect effect) {
    T castEffect = static_cast<T>(effect);
    auto iter = hardware::hidl_enum_range<T>();
    return castEffect >= *iter.begin() && castEffect <= *std::prev(iter.end());
}

// -------------------------------------------------------------------------------------------------

template <typename T>
HalResult<T> HalResult<T>::ok(T value) {
    return HalResult(value);
}

template <typename T>
HalResult<T> HalResult<T>::failed() {
    return HalResult(/* unsupported= */ false);
}

template <typename T>
HalResult<T> HalResult<T>::unsupported() {
    return HalResult(/* unsupported= */ true);
}

template <typename T>
HalResult<T> HalResult<T>::fromStatus(binder::Status status, T data) {
    if (status.exceptionCode() == binder::Status::EX_UNSUPPORTED_OPERATION) {
        return HalResult<T>::unsupported();
    }
    if (status.isOk()) {
        return HalResult<T>::ok(data);
    }
    return HalResult<T>::failed();
}

template <typename T>
HalResult<T> HalResult<T>::fromStatus(V1_0::Status status, T data) {
    switch (status) {
        case V1_0::Status::OK:
            return HalResult<T>::ok(data);
        case V1_0::Status::UNSUPPORTED_OPERATION:
            return HalResult<T>::unsupported();
        default:
            return HalResult<T>::failed();
    }
}

template <typename T>
template <typename R>
HalResult<T> HalResult<T>::fromReturn(hardware::Return<R>& ret, T data) {
    return ret.isOk() ? HalResult<T>::ok(data) : HalResult<T>::failed();
}

template <typename T>
template <typename R>
HalResult<T> HalResult<T>::fromReturn(hardware::Return<R>& ret, V1_0::Status status, T data) {
    return ret.isOk() ? HalResult<T>::fromStatus(status, data) : HalResult<T>::failed();
}

// -------------------------------------------------------------------------------------------------

HalResult<void> HalResult<void>::ok() {
    return HalResult();
}

HalResult<void> HalResult<void>::failed() {
    return HalResult(/* failed= */ true);
}

HalResult<void> HalResult<void>::unsupported() {
    return HalResult(/* failed= */ false, /* unsupported= */ true);
}

HalResult<void> HalResult<void>::fromStatus(binder::Status status) {
    if (status.exceptionCode() == binder::Status::EX_UNSUPPORTED_OPERATION) {
        return HalResult<void>::unsupported();
    }
    if (status.isOk()) {
        return HalResult<void>::ok();
    }
    return HalResult<void>::failed();
}

HalResult<void> HalResult<void>::fromStatus(V1_0::Status status) {
    switch (status) {
        case V1_0::Status::OK:
            return HalResult<void>::ok();
        case V1_0::Status::UNSUPPORTED_OPERATION:
            return HalResult<void>::unsupported();
        default:
            return HalResult<void>::failed();
    }
}

template <typename R>
HalResult<void> HalResult<void>::fromReturn(hardware::Return<R>& ret) {
    return ret.isOk() ? HalResult<void>::ok() : HalResult<void>::failed();
}

// -------------------------------------------------------------------------------------------------

class HalCallbackWrapper : public Aidl::BnVibratorCallback {
public:
    HalCallbackWrapper(std::function<void()> completionCallback)
          : mCompletionCallback(completionCallback) {}

    binder::Status onComplete() override {
        mCompletionCallback();
        return binder::Status::ok();
    }

private:
    const std::function<void()> mCompletionCallback;
};

// -------------------------------------------------------------------------------------------------

HalResult<void> AidlHalWrapper::ping() {
    return IInterface::asBinder(mHandle)->pingBinder() ? HalResult<void>::ok()
                                                       : HalResult<void>::failed();
}

HalResult<void> AidlHalWrapper::on(milliseconds timeout,
                                   const std::function<void()>& completionCallback) {
    HalResult<Capabilities> capabilities = getCapabilities();
    bool supportsCallback = capabilities.isOk() &&
            static_cast<int32_t>(capabilities.value() & Capabilities::ON_CALLBACK);
    auto cb = supportsCallback ? new HalCallbackWrapper(completionCallback) : nullptr;

    auto ret = HalResult<void>::fromStatus(mHandle->on(timeout.count(), cb));
    if (!supportsCallback && ret.isOk()) {
        mCallbackScheduler->schedule(completionCallback, timeout);
    }

    return ret;
}

HalResult<void> AidlHalWrapper::off() {
    return HalResult<void>::fromStatus(mHandle->off());
}

HalResult<void> AidlHalWrapper::setAmplitude(int32_t amplitude) {
    float convertedAmplitude = static_cast<float>(amplitude) / std::numeric_limits<uint8_t>::max();
    return HalResult<void>::fromStatus(mHandle->setAmplitude(convertedAmplitude));
}

HalResult<void> AidlHalWrapper::setExternalControl(bool enabled) {
    return HalResult<void>::fromStatus(mHandle->setExternalControl(enabled));
}

HalResult<void> AidlHalWrapper::alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) {
    return HalResult<void>::fromStatus(mHandle->alwaysOnEnable(id, effect, strength));
}

HalResult<void> AidlHalWrapper::alwaysOnDisable(int32_t id) {
    return HalResult<void>::fromStatus(mHandle->alwaysOnDisable(id));
}

HalResult<Capabilities> AidlHalWrapper::getCapabilities() {
    std::lock_guard<std::mutex> lock(mCapabilitiesMutex);
    return loadCached<Capabilities>(std::bind(&AidlHalWrapper::getCapabilitiesInternal, this),
                                    mCapabilities);
}

HalResult<std::vector<Effect>> AidlHalWrapper::getSupportedEffects() {
    std::lock_guard<std::mutex> lock(mSupportedEffectsMutex);
    return loadCached<std::vector<Effect>>(std::bind(&AidlHalWrapper::getSupportedEffectsInternal,
                                                     this),
                                           mSupportedEffects);
}

HalResult<milliseconds> AidlHalWrapper::performEffect(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    HalResult<Capabilities> capabilities = getCapabilities();
    bool supportsCallback = capabilities.isOk() &&
            static_cast<int32_t>(capabilities.value() & Capabilities::PERFORM_CALLBACK);
    auto cb = supportsCallback ? new HalCallbackWrapper(completionCallback) : nullptr;

    int32_t lengthMs;
    auto result = mHandle->perform(effect, strength, cb, &lengthMs);
    milliseconds length = milliseconds(lengthMs);

    auto ret = HalResult<milliseconds>::fromStatus(result, length);
    if (!supportsCallback && ret.isOk()) {
        mCallbackScheduler->schedule(completionCallback, length);
    }

    return ret;
}

HalResult<void> AidlHalWrapper::performComposedEffect(
        const std::vector<CompositeEffect>& primitiveEffects,
        const std::function<void()>& completionCallback) {
    // This method should always support callbacks, so no need to double check.
    auto cb = new HalCallbackWrapper(completionCallback);
    return HalResult<void>::fromStatus(mHandle->compose(primitiveEffects, cb));
}

HalResult<Capabilities> AidlHalWrapper::getCapabilitiesInternal() {
    int32_t capabilities = 0;
    auto result = mHandle->getCapabilities(&capabilities);
    return HalResult<Capabilities>::fromStatus(result, static_cast<Capabilities>(capabilities));
}

HalResult<std::vector<Effect>> AidlHalWrapper::getSupportedEffectsInternal() {
    std::vector<Effect> supportedEffects;
    auto result = mHandle->getSupportedEffects(&supportedEffects);
    return HalResult<std::vector<Effect>>::fromStatus(result, supportedEffects);
}

// -------------------------------------------------------------------------------------------------

HalResult<void> HidlHalWrapperV1_0::ping() {
    auto result = mHandleV1_0->ping();
    return HalResult<void>::fromReturn(result);
}

HalResult<void> HidlHalWrapperV1_0::on(milliseconds timeout,
                                       const std::function<void()>& completionCallback) {
    auto result = mHandleV1_0->on(timeout.count());
    auto ret = HalResult<void>::fromStatus(result.withDefault(V1_0::Status::UNKNOWN_ERROR));
    if (ret.isOk()) {
        mCallbackScheduler->schedule(completionCallback, timeout);
    }
    return ret;
}

HalResult<void> HidlHalWrapperV1_0::off() {
    auto result = mHandleV1_0->off();
    return HalResult<void>::fromStatus(result.withDefault(V1_0::Status::UNKNOWN_ERROR));
}

HalResult<void> HidlHalWrapperV1_0::setAmplitude(int32_t amplitude) {
    auto result = mHandleV1_0->setAmplitude(static_cast<uint8_t>(amplitude));
    return HalResult<void>::fromStatus(result.withDefault(V1_0::Status::UNKNOWN_ERROR));
}

HalResult<void> HidlHalWrapperV1_0::setExternalControl(bool) {
    ALOGV("Skipped setExternalControl because Vibrator HAL does not support it");
    return HalResult<void>::unsupported();
}

HalResult<void> HidlHalWrapperV1_0::alwaysOnEnable(int32_t, Effect, EffectStrength) {
    ALOGV("Skipped alwaysOnEnable because Vibrator HAL AIDL is not available");
    return HalResult<void>::unsupported();
}

HalResult<void> HidlHalWrapperV1_0::alwaysOnDisable(int32_t) {
    ALOGV("Skipped alwaysOnDisable because Vibrator HAL AIDL is not available");
    return HalResult<void>::unsupported();
}

HalResult<Capabilities> HidlHalWrapperV1_0::getCapabilities() {
    std::lock_guard<std::mutex> lock(mCapabilitiesMutex);
    return loadCached<Capabilities>(std::bind(&HidlHalWrapperV1_0::getCapabilitiesInternal, this),
                                    mCapabilities);
}

HalResult<std::vector<Effect>> HidlHalWrapperV1_0::getSupportedEffects() {
    ALOGV("Skipped getSupportedEffects because Vibrator HAL AIDL is not available");
    return HalResult<std::vector<Effect>>::unsupported();
}

HalResult<milliseconds> HidlHalWrapperV1_0::performEffect(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    if (isStaticCastValid<V1_0::Effect>(effect)) {
        return performInternalV1_0(effect, strength, completionCallback);
    }

    ALOGV("Skipped performEffect because Vibrator HAL does not support effect %s",
          Aidl::toString(effect).c_str());
    return HalResult<milliseconds>::unsupported();
}

HalResult<void> HidlHalWrapperV1_0::performComposedEffect(const std::vector<CompositeEffect>&,
                                                          const std::function<void()>&) {
    ALOGV("Skipped composed effect because Vibrator HAL AIDL is not available");
    return HalResult<void>::unsupported();
}

HalResult<Capabilities> HidlHalWrapperV1_0::getCapabilitiesInternal() {
    hardware::Return<bool> result = mHandleV1_0->supportsAmplitudeControl();
    Capabilities capabilities =
            result.withDefault(false) ? Capabilities::AMPLITUDE_CONTROL : Capabilities::NONE;
    return HalResult<Capabilities>::fromReturn(result, capabilities);
}

template <class I, class T>
HalResult<milliseconds> HidlHalWrapperV1_0::performInternal(
        perform_fn<I, T> performFn, sp<I> handle, T effect, EffectStrength strength,
        const std::function<void()>& completionCallback) {
    V1_0::Status status;
    int32_t lengthMs;
    auto effectCallback = [&status, &lengthMs](V1_0::Status retStatus, uint32_t retLengthMs) {
        status = retStatus;
        lengthMs = retLengthMs;
    };

    V1_0::EffectStrength effectStrength = static_cast<V1_0::EffectStrength>(strength);
    auto result = std::invoke(performFn, handle, effect, effectStrength, effectCallback);
    milliseconds length = milliseconds(lengthMs);

    auto ret = HalResult<milliseconds>::fromReturn(result, status, length);
    if (ret.isOk()) {
        mCallbackScheduler->schedule(completionCallback, length);
    }

    return ret;
}

HalResult<milliseconds> HidlHalWrapperV1_0::performInternalV1_0(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    V1_0::Effect e = static_cast<V1_0::Effect>(effect);
    return performInternal(&V1_0::IVibrator::perform, mHandleV1_0, e, strength, completionCallback);
}

// -------------------------------------------------------------------------------------------------

HalResult<milliseconds> HidlHalWrapperV1_1::performEffect(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    if (isStaticCastValid<V1_0::Effect>(effect)) {
        return performInternalV1_0(effect, strength, completionCallback);
    }
    if (isStaticCastValid<V1_1::Effect_1_1>(effect)) {
        return performInternalV1_1(effect, strength, completionCallback);
    }

    ALOGV("Skipped performEffect because Vibrator HAL does not support effect %s",
          Aidl::toString(effect).c_str());
    return HalResult<milliseconds>::unsupported();
}

HalResult<milliseconds> HidlHalWrapperV1_1::performInternalV1_1(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    V1_1::Effect_1_1 e = static_cast<V1_1::Effect_1_1>(effect);
    return performInternal(&V1_1::IVibrator::perform_1_1, mHandleV1_1, e, strength,
                           completionCallback);
}

// -------------------------------------------------------------------------------------------------

HalResult<milliseconds> HidlHalWrapperV1_2::performEffect(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    if (isStaticCastValid<V1_0::Effect>(effect)) {
        return performInternalV1_0(effect, strength, completionCallback);
    }
    if (isStaticCastValid<V1_1::Effect_1_1>(effect)) {
        return performInternalV1_1(effect, strength, completionCallback);
    }
    if (isStaticCastValid<V1_2::Effect>(effect)) {
        return performInternalV1_2(effect, strength, completionCallback);
    }

    ALOGV("Skipped performEffect because Vibrator HAL does not support effect %s",
          Aidl::toString(effect).c_str());
    return HalResult<milliseconds>::unsupported();
}

HalResult<milliseconds> HidlHalWrapperV1_2::performInternalV1_2(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    V1_2::Effect e = static_cast<V1_2::Effect>(effect);
    return performInternal(&V1_2::IVibrator::perform_1_2, mHandleV1_2, e, strength,
                           completionCallback);
}

// -------------------------------------------------------------------------------------------------

HalResult<void> HidlHalWrapperV1_3::setExternalControl(bool enabled) {
    auto result = mHandleV1_3->setExternalControl(static_cast<uint32_t>(enabled));
    return HalResult<void>::fromStatus(result.withDefault(V1_0::Status::UNKNOWN_ERROR));
}

HalResult<milliseconds> HidlHalWrapperV1_3::performEffect(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    if (isStaticCastValid<V1_0::Effect>(effect)) {
        return performInternalV1_0(effect, strength, completionCallback);
    }
    if (isStaticCastValid<V1_1::Effect_1_1>(effect)) {
        return performInternalV1_1(effect, strength, completionCallback);
    }
    if (isStaticCastValid<V1_2::Effect>(effect)) {
        return performInternalV1_2(effect, strength, completionCallback);
    }
    if (isStaticCastValid<V1_3::Effect>(effect)) {
        return performInternalV1_3(effect, strength, completionCallback);
    }

    ALOGV("Skipped performEffect because Vibrator HAL does not support effect %s",
          Aidl::toString(effect).c_str());
    return HalResult<milliseconds>::unsupported();
}

HalResult<Capabilities> HidlHalWrapperV1_3::getCapabilitiesInternal() {
    HalResult<Capabilities> parentResult = HidlHalWrapperV1_2::getCapabilitiesInternal();
    if (!parentResult.isOk()) {
        // Loading for previous HAL versions already failed, so propagate failure.
        return parentResult;
    }

    Capabilities capabilities = parentResult.value();
    auto result = mHandleV1_3->supportsExternalControl();
    capabilities |= result.withDefault(false) ? Capabilities::EXTERNAL_CONTROL : Capabilities::NONE;
    return HalResult<Capabilities>::fromReturn(result, capabilities);
}

HalResult<milliseconds> HidlHalWrapperV1_3::performInternalV1_3(
        Effect effect, EffectStrength strength, const std::function<void()>& completionCallback) {
    V1_3::Effect e = static_cast<V1_3::Effect>(effect);
    return performInternal(&V1_3::IVibrator::perform_1_3, mHandleV1_3, e, strength,
                           completionCallback);
}

// -------------------------------------------------------------------------------------------------

}; // namespace vibrator

}; // namespace android
