/*
 * Copyright 2019 The Android Open Source Project
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

// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "RefreshRateConfigs.h"
#include <android-base/stringprintf.h>
#include <utils/Trace.h>
#include <chrono>
#include <cmath>

using namespace std::chrono_literals;

namespace android::scheduler {

using AllRefreshRatesMapType = RefreshRateConfigs::AllRefreshRatesMapType;
using RefreshRate = RefreshRateConfigs::RefreshRate;

const RefreshRate& RefreshRateConfigs::getRefreshRateForContent(
        const std::vector<LayerRequirement>& layers) const {
    std::lock_guard lock(mLock);
    int contentFramerate = 0;
    int explicitContentFramerate = 0;
    for (const auto& layer : layers) {
        const auto desiredRefreshRateRound = round<int>(layer.desiredRefreshRate);
        if (layer.vote == LayerVoteType::ExplicitDefault ||
            layer.vote == LayerVoteType::ExplicitExactOrMultiple) {
            if (desiredRefreshRateRound > explicitContentFramerate) {
                explicitContentFramerate = desiredRefreshRateRound;
            }
        } else {
            if (desiredRefreshRateRound > contentFramerate) {
                contentFramerate = desiredRefreshRateRound;
            }
        }
    }

    if (explicitContentFramerate != 0) {
        contentFramerate = explicitContentFramerate;
    } else if (contentFramerate == 0) {
        contentFramerate = round<int>(mMaxSupportedRefreshRate->fps);
    }
    ATRACE_INT("ContentFPS", contentFramerate);

    // Find the appropriate refresh rate with minimal error
    auto iter = min_element(mAvailableRefreshRates.cbegin(), mAvailableRefreshRates.cend(),
                            [contentFramerate](const auto& lhs, const auto& rhs) -> bool {
                                return std::abs(lhs->fps - contentFramerate) <
                                        std::abs(rhs->fps - contentFramerate);
                            });

    // Some content aligns better on higher refresh rate. For example for 45fps we should choose
    // 90Hz config. However we should still prefer a lower refresh rate if the content doesn't
    // align well with both
    const RefreshRate* bestSoFar = *iter;
    constexpr float MARGIN = 0.05f;
    float ratio = (*iter)->fps / contentFramerate;
    if (std::abs(std::round(ratio) - ratio) > MARGIN) {
        while (iter != mAvailableRefreshRates.cend()) {
            ratio = (*iter)->fps / contentFramerate;

            if (std::abs(std::round(ratio) - ratio) <= MARGIN) {
                bestSoFar = *iter;
                break;
            }
            ++iter;
        }
    }

    return *bestSoFar;
}

const RefreshRate& RefreshRateConfigs::getRefreshRateForContentV2(
        const std::vector<LayerRequirement>& layers) const {
    constexpr nsecs_t MARGIN = std::chrono::nanoseconds(800us).count();
    ATRACE_CALL();
    ALOGV("getRefreshRateForContent %zu layers", layers.size());

    std::lock_guard lock(mLock);

    int noVoteLayers = 0;
    int minVoteLayers = 0;
    int maxVoteLayers = 0;
    int explicitDefaultVoteLayers = 0;
    int explicitExactOrMultipleVoteLayers = 0;
    for (const auto& layer : layers) {
        if (layer.vote == LayerVoteType::NoVote)
            noVoteLayers++;
        else if (layer.vote == LayerVoteType::Min)
            minVoteLayers++;
        else if (layer.vote == LayerVoteType::Max)
            maxVoteLayers++;
        else if (layer.vote == LayerVoteType::ExplicitDefault)
            explicitDefaultVoteLayers++;
        else if (layer.vote == LayerVoteType::ExplicitExactOrMultiple)
            explicitExactOrMultipleVoteLayers++;
    }

    // Only if all layers want Min we should return Min
    if (noVoteLayers + minVoteLayers == layers.size()) {
        return *mAvailableRefreshRates.front();
    }

    // If we have some Max layers and no Explicit we should return Max
    if (maxVoteLayers > 0 && explicitDefaultVoteLayers + explicitExactOrMultipleVoteLayers == 0) {
        return *mAvailableRefreshRates.back();
    }

    // Find the best refresh rate based on score
    std::vector<std::pair<const RefreshRate*, float>> scores;
    scores.reserve(mAvailableRefreshRates.size());

    for (const auto refreshRate : mAvailableRefreshRates) {
        scores.emplace_back(refreshRate, 0.0f);
    }

    for (const auto& layer : layers) {
        ALOGV("Calculating score for %s (type: %d)", layer.name.c_str(), layer.vote);
        if (layer.vote == LayerVoteType::NoVote || layer.vote == LayerVoteType::Min ||
            layer.vote == LayerVoteType::Max) {
            continue;
        }

        // Adjust the weight in case we have explicit layers. The priority is:
        //  - ExplicitExactOrMultiple
        //  - ExplicitDefault
        //  - Heuristic
        auto weight = layer.weight;
        if (explicitExactOrMultipleVoteLayers + explicitDefaultVoteLayers > 0) {
            if (layer.vote == LayerVoteType::Heuristic) {
                weight /= 2.f;
            }
        }

        if (explicitExactOrMultipleVoteLayers > 0) {
            if (layer.vote == LayerVoteType::Heuristic ||
                layer.vote == LayerVoteType::ExplicitDefault) {
                weight /= 2.f;
            }
        }

        for (auto& [refreshRate, overallScore] : scores) {
            const auto displayPeriod = refreshRate->vsyncPeriod;
            const auto layerPeriod = round<nsecs_t>(1e9f / layer.desiredRefreshRate);

            // Calculate how many display vsyncs we need to present a single frame for this layer
            auto [displayFramesQuot, displayFramesRem] = std::div(layerPeriod, displayPeriod);
            if (displayFramesRem <= MARGIN ||
                std::abs(displayFramesRem - displayPeriod) <= MARGIN) {
                displayFramesQuot++;
                displayFramesRem = 0;
            }

            float layerScore;
            static constexpr size_t MAX_FRAMES_TO_FIT = 10; // Stop calculating when score < 0.1
            if (displayFramesRem == 0) {
                // Layer desired refresh rate matches the display rate.
                layerScore = weight * 1.0f;
            } else if (displayFramesQuot == 0) {
                // Layer desired refresh rate is higher the display rate.
                layerScore = weight *
                        (static_cast<float>(layerPeriod) / static_cast<float>(displayPeriod)) *
                        (1.0f / (MAX_FRAMES_TO_FIT + 1));
            } else {
                // Layer desired refresh rate is lower the display rate. Check how well it fits the
                // cadence
                auto diff = std::abs(displayFramesRem - (displayPeriod - displayFramesRem));
                int iter = 2;
                while (diff > MARGIN && iter < MAX_FRAMES_TO_FIT) {
                    diff = diff - (displayPeriod - diff);
                    iter++;
                }

                layerScore = weight * (1.0f / iter);
            }

            ALOGV("%s (weight %.2f) %.2fHz gives %s score of %.2f", layer.name.c_str(), weight,
                  1e9f / layerPeriod, refreshRate->name.c_str(), layerScore);
            overallScore += layerScore;
        }
    }

    float max = 0;
    const RefreshRate* bestRefreshRate = nullptr;
    for (const auto [refreshRate, score] : scores) {
        ALOGV("%s scores %.2f", refreshRate->name.c_str(), score);

        ATRACE_INT(refreshRate->name.c_str(), round<int>(score * 100));

        if (score > max) {
            max = score;
            bestRefreshRate = refreshRate;
        }
    }

    return bestRefreshRate == nullptr ? *mCurrentRefreshRate : *bestRefreshRate;
}

const AllRefreshRatesMapType& RefreshRateConfigs::getAllRefreshRates() const {
    return mRefreshRates;
}

const RefreshRate& RefreshRateConfigs::getMinRefreshRateByPolicy() const {
    std::lock_guard lock(mLock);
    return *mAvailableRefreshRates.front();
}

const RefreshRate& RefreshRateConfigs::getMaxRefreshRateByPolicy() const {
    std::lock_guard lock(mLock);
        return *mAvailableRefreshRates.back();
}

const RefreshRate& RefreshRateConfigs::getCurrentRefreshRate() const {
    std::lock_guard lock(mLock);
    return *mCurrentRefreshRate;
}

void RefreshRateConfigs::setCurrentConfigId(HwcConfigIndexType configId) {
    std::lock_guard lock(mLock);
    mCurrentRefreshRate = &mRefreshRates.at(configId);
}

RefreshRateConfigs::RefreshRateConfigs(const std::vector<InputConfig>& configs,
                                       HwcConfigIndexType currentHwcConfig) {
    init(configs, currentHwcConfig);
}

RefreshRateConfigs::RefreshRateConfigs(
        const std::vector<std::shared_ptr<const HWC2::Display::Config>>& configs,
        HwcConfigIndexType currentConfigId) {
    std::vector<InputConfig> inputConfigs;
    for (size_t configId = 0; configId < configs.size(); ++configId) {
        auto configGroup = HwcConfigGroupType(configs[configId]->getConfigGroup());
        inputConfigs.push_back({HwcConfigIndexType(static_cast<int>(configId)), configGroup,
                                configs[configId]->getVsyncPeriod()});
    }
    init(inputConfigs, currentConfigId);
}

status_t RefreshRateConfigs::setPolicy(HwcConfigIndexType defaultConfigId, float minRefreshRate,
                                       float maxRefreshRate, bool* outPolicyChanged) {
    std::lock_guard lock(mLock);
    bool policyChanged = defaultConfigId != mDefaultConfig ||
            minRefreshRate != mMinRefreshRateFps || maxRefreshRate != mMaxRefreshRateFps;
    if (outPolicyChanged) {
        *outPolicyChanged = policyChanged;
    }
    if (!policyChanged) {
        return NO_ERROR;
    }
    // defaultConfigId must be a valid config ID, and within the given refresh rate range.
    if (mRefreshRates.count(defaultConfigId) == 0) {
        return BAD_VALUE;
    }
    const RefreshRate& refreshRate = mRefreshRates.at(defaultConfigId);
    if (!refreshRate.inPolicy(minRefreshRate, maxRefreshRate)) {
        return BAD_VALUE;
    }
    mDefaultConfig = defaultConfigId;
    mMinRefreshRateFps = minRefreshRate;
    mMaxRefreshRateFps = maxRefreshRate;
    constructAvailableRefreshRates();
    return NO_ERROR;
}

void RefreshRateConfigs::getPolicy(HwcConfigIndexType* defaultConfigId, float* minRefreshRate,
                                   float* maxRefreshRate) const {
    std::lock_guard lock(mLock);
    *defaultConfigId = mDefaultConfig;
    *minRefreshRate = mMinRefreshRateFps;
    *maxRefreshRate = mMaxRefreshRateFps;
}

bool RefreshRateConfigs::isConfigAllowed(HwcConfigIndexType config) const {
    std::lock_guard lock(mLock);
    for (const RefreshRate* refreshRate : mAvailableRefreshRates) {
        if (refreshRate->configId == config) {
            return true;
        }
    }
    return false;
}

void RefreshRateConfigs::getSortedRefreshRateList(
        const std::function<bool(const RefreshRate&)>& shouldAddRefreshRate,
        std::vector<const RefreshRate*>* outRefreshRates) {
    outRefreshRates->clear();
    outRefreshRates->reserve(mRefreshRates.size());
    for (const auto& [type, refreshRate] : mRefreshRates) {
        if (shouldAddRefreshRate(refreshRate)) {
            ALOGV("getSortedRefreshRateList: config %d added to list policy",
                  refreshRate.configId.value());
            outRefreshRates->push_back(&refreshRate);
        }
    }

    std::sort(outRefreshRates->begin(), outRefreshRates->end(),
              [](const auto refreshRate1, const auto refreshRate2) {
                  return refreshRate1->vsyncPeriod > refreshRate2->vsyncPeriod;
              });
}

void RefreshRateConfigs::constructAvailableRefreshRates() {
    // Filter configs based on current policy and sort based on vsync period
    HwcConfigGroupType group = mRefreshRates.at(mDefaultConfig).configGroup;
    ALOGV("constructAvailableRefreshRates: default %d group %d min %.2f max %.2f",
          mDefaultConfig.value(), group.value(), mMinRefreshRateFps, mMaxRefreshRateFps);
    getSortedRefreshRateList(
            [&](const RefreshRate& refreshRate) REQUIRES(mLock) {
                return refreshRate.configGroup == group &&
                        refreshRate.inPolicy(mMinRefreshRateFps, mMaxRefreshRateFps);
            },
            &mAvailableRefreshRates);

    std::string availableRefreshRates;
    for (const auto& refreshRate : mAvailableRefreshRates) {
        base::StringAppendF(&availableRefreshRates, "%s ", refreshRate->name.c_str());
    }

    ALOGV("Available refresh rates: %s", availableRefreshRates.c_str());
    LOG_ALWAYS_FATAL_IF(mAvailableRefreshRates.empty(),
                        "No compatible display configs for default=%d min=%.0f max=%.0f",
                        mDefaultConfig.value(), mMinRefreshRateFps, mMaxRefreshRateFps);
}

// NO_THREAD_SAFETY_ANALYSIS since this is called from the constructor
void RefreshRateConfigs::init(const std::vector<InputConfig>& configs,
                              HwcConfigIndexType currentHwcConfig) NO_THREAD_SAFETY_ANALYSIS {
    LOG_ALWAYS_FATAL_IF(configs.empty());
    LOG_ALWAYS_FATAL_IF(currentHwcConfig.value() >= configs.size());

    auto buildRefreshRate = [&](InputConfig config) -> RefreshRate {
        const float fps = 1e9f / config.vsyncPeriod;
        return RefreshRate(config.configId, config.vsyncPeriod, config.configGroup,
                           base::StringPrintf("%2.ffps", fps), fps);
    };

    for (const auto& config : configs) {
        mRefreshRates.emplace(config.configId, buildRefreshRate(config));
        if (config.configId == currentHwcConfig) {
            mCurrentRefreshRate = &mRefreshRates.at(config.configId);
        }
    }

    std::vector<const RefreshRate*> sortedConfigs;
    getSortedRefreshRateList([](const RefreshRate&) { return true; }, &sortedConfigs);
    mDefaultConfig = currentHwcConfig;
    mMinSupportedRefreshRate = sortedConfigs.front();
    mMaxSupportedRefreshRate = sortedConfigs.back();
    constructAvailableRefreshRates();
}

} // namespace android::scheduler
