/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define LOG_TAG "VtsHalEqualizerTest"

#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_interface_utils.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <gtest/gtest.h>

#include <Utils.h>
#include <aidl/android/hardware/audio/effect/IEffect.h>
#include <aidl/android/hardware/audio/effect/IFactory.h>

#include "AudioHalBinderServiceUtil.h"
#include "EffectHelper.h"
#include "TestUtils.h"
#include "effect-impl/EffectUUID.h"

using namespace android;

using aidl::android::hardware::audio::effect::Capability;
using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::Equalizer;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::IFactory;
using aidl::android::hardware::audio::effect::kEqualizerTypeUUID;
using aidl::android::hardware::audio::effect::Parameter;

/**
 * Here we focus on specific effect (equalizer) parameter checking, general IEffect interfaces
 * testing performed in VtsAudioEfectTargetTest.
 */

enum ParamName { PARAM_INSTANCE_NAME, PARAM_BAND_LEVEL };
using EqualizerParamTestParam =
        std::tuple<std::pair<std::shared_ptr<IFactory>, Descriptor::Identity>, int>;

/*
Testing parameter range, assuming the parameter supported by effect is in this range.
This range is verified with IEffect.getDescriptor(), for any index supported vts expect EX_NONE
from IEffect.setParameter(), otherwise expect EX_ILLEGAL_ARGUMENT.
*/
const std::vector<int> kBandLevels = {0, -10, 10};  // needs update with implementation

class EqualizerTest : public ::testing::TestWithParam<EqualizerParamTestParam>,
                      public EffectHelper {
  public:
    EqualizerTest() : mBandLevel(std::get<PARAM_BAND_LEVEL>(GetParam())) {
        std::tie(mFactory, mIdentity) = std::get<PARAM_INSTANCE_NAME>(GetParam());
    }

    void SetUp() override {
        ASSERT_NE(nullptr, mFactory);
        ASSERT_NO_FATAL_FAILURE(create(mFactory, mEffect, mIdentity));

        Parameter::Specific specific = getDefaultParamSpecific();
        Parameter::Common common = EffectHelper::createParamCommon(
                0 /* session */, 1 /* ioHandle */, 44100 /* iSampleRate */, 44100 /* oSampleRate */,
                kInputFrameCount /* iFrameCount */, kOutputFrameCount /* oFrameCount */);
        IEffect::OpenEffectReturn ret;
        ASSERT_NO_FATAL_FAILURE(open(mEffect, common, specific, &ret, EX_NONE));
        ASSERT_NE(nullptr, mEffect);
        ASSERT_NO_FATAL_FAILURE(setTagRange());
    }
    void TearDown() override {
        ASSERT_NO_FATAL_FAILURE(close(mEffect));
        ASSERT_NO_FATAL_FAILURE(destroy(mFactory, mEffect));
    }

    std::pair<int, int> setPresetIndexRange(const Equalizer::Capability& cap) const {
        const auto [min, max] =
                std::minmax_element(cap.presets.begin(), cap.presets.end(),
                                    [](const auto& a, const auto& b) { return a.index < b.index; });
        return {min->index, max->index};
    }
    std::pair<int, int> setBandIndexRange(const Equalizer::Capability& cap) const {
        const auto [min, max] =
                std::minmax_element(cap.bandFrequencies.begin(), cap.bandFrequencies.end(),
                                    [](const auto& a, const auto& b) { return a.index < b.index; });
        return {min->index, max->index};
    }
    void setTagRange() {
        Descriptor desc;
        ASSERT_STATUS(EX_NONE, mEffect->getDescriptor(&desc));
        Equalizer::Capability& eqCap = desc.capability.get<Capability::equalizer>();
        mPresetIndex = setPresetIndexRange(eqCap);
        mBandIndex = setBandIndexRange(eqCap);
    }

    static const long kInputFrameCount = 0x100, kOutputFrameCount = 0x100;
    std::shared_ptr<IFactory> mFactory;
    std::shared_ptr<IEffect> mEffect;
    Descriptor::Identity mIdentity;
    std::pair<int, int> mPresetIndex;
    std::pair<int, int> mBandIndex;
    const int mBandLevel;
    Descriptor mDesc;

    void SetAndGetEqualizerParameters() {
        ASSERT_NE(nullptr, mEffect);
        for (auto& it : mTags) {
            auto& tag = it.first;
            auto& eq = it.second;

            // validate parameter
            const bool valid = isTagInRange(it.first, it.second);
            const binder_exception_t expected = valid ? EX_NONE : EX_ILLEGAL_ARGUMENT;

            // set
            Parameter expectParam;
            Parameter::Specific specific;
            specific.set<Parameter::Specific::equalizer>(eq);
            expectParam.set<Parameter::specific>(specific);
            EXPECT_STATUS(expected, mEffect->setParameter(expectParam))
                    << expectParam.toString() << "\n"
                    << mDesc.toString();

            // only get if parameter in range and set success
            if (expected == EX_NONE) {
                Parameter getParam;
                Parameter::Id id;
                Equalizer::Id eqId;
                eqId.set<Equalizer::Id::commonTag>(tag);
                id.set<Parameter::Id::equalizerTag>(eqId);
                // if set success, then get should match
                EXPECT_STATUS(expected, mEffect->getParameter(id, &getParam));
                EXPECT_TRUE(isEqParameterExpected(expectParam, getParam))
                        << "\nexpect:" << expectParam.toString()
                        << "\ngetParam:" << getParam.toString();
            }
        }
    }

    bool isEqParameterExpected(const Parameter& expect, const Parameter& target) {
        // if parameter same, then for sure they are matched
        if (expect == target) return true;

        // if not, see if target include the expect parameter, and others all default (0).
        /*
         * This is to verify the case of client setParameter to a single bandLevel ({3, -1} for
         * example), and return of getParameter must be [{0, 0}, {1, 0}, {2, 0}, {3, -1}, {4, 0}]
         */
        EXPECT_EQ(expect.getTag(), Parameter::specific);
        EXPECT_EQ(target.getTag(), Parameter::specific);

        Parameter::Specific expectSpec = expect.get<Parameter::specific>(),
                            targetSpec = target.get<Parameter::specific>();
        EXPECT_EQ(expectSpec.getTag(), Parameter::Specific::equalizer);
        EXPECT_EQ(targetSpec.getTag(), Parameter::Specific::equalizer);

        Equalizer expectEq = expectSpec.get<Parameter::Specific::equalizer>(),
                  targetEq = targetSpec.get<Parameter::Specific::equalizer>();
        EXPECT_EQ(expectEq.getTag(), targetEq.getTag());

        auto eqTag = targetEq.getTag();
        switch (eqTag) {
            case Equalizer::bandLevels: {
                auto expectBl = expectEq.get<Equalizer::bandLevels>();
                std::sort(expectBl.begin(), expectBl.end(),
                          [](const auto& a, const auto& b) { return a.index < b.index; });
                expectBl.erase(std::unique(expectBl.begin(), expectBl.end()), expectBl.end());
                auto targetBl = targetEq.get<Equalizer::bandLevels>();
                return std::includes(targetBl.begin(), targetBl.end(), expectBl.begin(),
                                     expectBl.end());
            }
            case Equalizer::preset: {
                return expectEq.get<Equalizer::preset>() == targetEq.get<Equalizer::preset>();
            }
            default:
                return false;
        }
        return false;
    }

    void addPresetParam(int preset) {
        Equalizer eq;
        eq.set<Equalizer::preset>(preset);
        mTags.push_back({Equalizer::preset, eq});
    }

    void addBandLevelsParam(std::vector<Equalizer::BandLevel>& bandLevels) {
        Equalizer eq;
        eq.set<Equalizer::bandLevels>(bandLevels);
        mTags.push_back({Equalizer::bandLevels, eq});
    }

    bool isTagInRange(const Equalizer::Tag& tag, const Equalizer& eq) const {
        switch (tag) {
            case Equalizer::preset: {
                int index = eq.get<Equalizer::preset>();
                return index >= mPresetIndex.first && index <= mPresetIndex.second;
            }
            case Equalizer::bandLevels: {
                auto& bandLevel = eq.get<Equalizer::bandLevels>();
                return isBandInRange(bandLevel);
            }
            default:
                return false;
        }
        return false;
    }

    bool isBandInRange(const std::vector<Equalizer::BandLevel>& bandLevel) const {
        for (auto& it : bandLevel) {
            if (it.index < mBandIndex.first || it.index > mBandIndex.second) return false;
        }
        return true;
    }

    Parameter::Specific getDefaultParamSpecific() {
        Equalizer eq = Equalizer::make<Equalizer::preset>(0);
        Parameter::Specific specific =
                Parameter::Specific::make<Parameter::Specific::equalizer>(eq);
        return specific;
    }

  private:
    std::vector<std::pair<Equalizer::Tag, Equalizer>> mTags;

    bool validCapabilityTag(Capability& cap) { return cap.getTag() == Capability::equalizer; }

    void CleanUp() { mTags.clear(); }
};

TEST_P(EqualizerTest, SetAndGetPresetOutOfLowerBound) {
    addPresetParam(mPresetIndex.second - 1);
    ASSERT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetPresetOutOfUpperBound) {
    addPresetParam(mPresetIndex.second + 1);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetPresetAtLowerBound) {
    addPresetParam(mPresetIndex.first);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetPresetAtHigherBound) {
    addPresetParam(mPresetIndex.second);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetPresetInBound) {
    addPresetParam((mPresetIndex.first + mPresetIndex.second) >> 1);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetBandOutOfLowerBound) {
    std::vector<Equalizer::BandLevel> bandLevels{{mBandIndex.first - 1, mBandLevel}};
    addBandLevelsParam(bandLevels);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetBandOutOfUpperBound) {
    std::vector<Equalizer::BandLevel> bandLevels{{mBandIndex.second + 1, mBandLevel}};
    addBandLevelsParam(bandLevels);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetBandAtLowerBound) {
    std::vector<Equalizer::BandLevel> bandLevels{{mBandIndex.first, mBandLevel}};
    addBandLevelsParam(bandLevels);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetBandAtHigherBound) {
    std::vector<Equalizer::BandLevel> bandLevels{{mBandIndex.second, mBandLevel}};
    addBandLevelsParam(bandLevels);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetBandInBound) {
    std::vector<Equalizer::BandLevel> bandLevels{
            {(mBandIndex.first + mBandIndex.second) >> 1, mBandLevel}};
    addBandLevelsParam(bandLevels);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetMultiBands) {
    addPresetParam(mPresetIndex.first);
    std::vector<Equalizer::BandLevel> bandLevels{
            {mBandIndex.first, mBandLevel},
            {mBandIndex.second, mBandLevel},
            {(mBandIndex.first + mBandIndex.second) >> 1, mBandLevel}};
    addBandLevelsParam(bandLevels);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

TEST_P(EqualizerTest, SetAndGetMultipleParams) {
    std::vector<Equalizer::BandLevel> bandLevels{
            {(mBandIndex.first + mBandIndex.second) >> 1, mBandLevel}};
    addBandLevelsParam(bandLevels);
    addPresetParam((mPresetIndex.first + mPresetIndex.second) >> 1);
    EXPECT_NO_FATAL_FAILURE(SetAndGetEqualizerParameters());
}

INSTANTIATE_TEST_SUITE_P(
        EqualizerTest, EqualizerTest,
        ::testing::Combine(testing::ValuesIn(EffectFactoryHelper::getAllEffectDescriptors(
                                   IFactory::descriptor, kEqualizerTypeUUID)),
                           testing::ValuesIn(kBandLevels)),
        [](const testing::TestParamInfo<EqualizerTest::ParamType>& info) {
            auto msSinceEpoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
            auto instance = std::get<PARAM_INSTANCE_NAME>(info.param);
            std::string bandLevel = std::to_string(std::get<PARAM_BAND_LEVEL>(info.param));
            std::ostringstream address;
            address << msSinceEpoch << "_factory_" << instance.first.get();
            std::string name = address.str() + "_UUID_timeLow_" +
                               ::android::internal::ToString(instance.second.uuid.timeLow) +
                               "_timeMid_" +
                               ::android::internal::ToString(instance.second.uuid.timeMid) +
                               "_bandLevel_" + bandLevel;
            std::replace_if(
                    name.begin(), name.end(), [](const char c) { return !std::isalnum(c); }, '_');
            return name;
        });
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(EqualizerTest);

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();
    return RUN_ALL_TESTS();
}
