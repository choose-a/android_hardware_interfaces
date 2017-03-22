/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define LOG_TAG "VtsHalGnssV1_0TargetTest"
#include <android/hardware/gnss/1.0/IGnss.h>
#include <android/log.h>

#include <VtsHalHidlTargetTestBase.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

using android::hardware::Return;
using android::hardware::Void;

using android::hardware::gnss::V1_0::GnssLocation;
using android::hardware::gnss::V1_0::GnssLocationFlags;
using android::hardware::gnss::V1_0::IGnss;
using android::hardware::gnss::V1_0::IGnssCallback;
using android::hardware::gnss::V1_0::IGnssDebug;
using android::hardware::gnss::V1_0::IGnssMeasurement;
using android::sp;

#define TIMEOUT_SEC 3  // for basic commands/responses

// Set these false for release, true for stronger test
#define GNSS_SIGNAL_IS_PRESENT false
#define GNSS_ASSISTANCE_IS_PRESENT false

// The main test class for GNSS HAL.
class GnssHalTest : public ::testing::VtsHalHidlTargetTestBase {
 public:
  virtual void SetUp() override {
    // Clean between tests
    capabilities_called_count_ = 0;
    location_called_count_ = 0;
    info_called_count_ = 0;

    gnss_hal_ = ::testing::VtsHalHidlTargetTestBase::getService<IGnss>();
    ASSERT_NE(gnss_hal_, nullptr);

    gnss_cb_ = new GnssCallback(*this);
    ASSERT_NE(gnss_cb_, nullptr);

    auto result = gnss_hal_->setCallback(gnss_cb_);
    if (!result.isOk()) {
      ALOGE("result of failed setCallback %s", result.description().c_str());
    }

    ASSERT_TRUE(result.isOk());
    ASSERT_TRUE(result);

    /*
     * At least one callback should trigger - it may be capabilites, or
     * system info first, so wait again if capabilities not received.
     */
    EXPECT_EQ(std::cv_status::no_timeout, wait(TIMEOUT_SEC));
    if (capabilities_called_count_ == 0) {
      EXPECT_EQ(std::cv_status::no_timeout, wait(TIMEOUT_SEC));
    }

    /*
     * Generally should be 1 capabilites callback -
     * or possibly 2 in some recovery cases (default cached & refreshed)
     */
    EXPECT_GE(capabilities_called_count_, 1);
    EXPECT_LE(capabilities_called_count_, 2);

    /*
     * Clear notify/waiting counter, allowing up till the timeout after
     * the last reply for final startup messages to arrive (esp. system
     * info.)
     */
    while (wait(TIMEOUT_SEC) == std::cv_status::no_timeout) {
    }
  }

  virtual void TearDown() override {
    if (gnss_hal_ != nullptr) {
      gnss_hal_->cleanup();
    }
  }

  /* Used as a mechanism to inform the test that a callback has occurred */
  inline void notify() {
    std::unique_lock<std::mutex> lock(mtx_);
    count++;
    cv_.notify_one();
  }

  /* Test code calls this function to wait for a callback */
  inline std::cv_status wait(int timeoutSeconds) {
    std::unique_lock<std::mutex> lock(mtx_);

    std::cv_status status = std::cv_status::no_timeout;
    auto now = std::chrono::system_clock::now();
    while (count == 0) {
      status = cv_.wait_until(lock, now + std::chrono::seconds(timeoutSeconds));
      if (status == std::cv_status::timeout) return status;
    }
    count--;
    return status;
  }

  /* Callback class for data & Event. */
  class GnssCallback : public IGnssCallback {
   public:
    GnssHalTest& parent_;

    GnssCallback(GnssHalTest& parent) : parent_(parent){};

    virtual ~GnssCallback() = default;

    // Dummy callback handlers
    Return<void> gnssStatusCb(
        const IGnssCallback::GnssStatusValue /* status */) override {
      return Void();
    }
    Return<void> gnssSvStatusCb(
        const IGnssCallback::GnssSvStatus& /* svStatus */) override {
      return Void();
    }
    Return<void> gnssNmeaCb(
        int64_t /* timestamp */,
        const android::hardware::hidl_string& /* nmea */) override {
      return Void();
    }
    Return<void> gnssAcquireWakelockCb() override { return Void(); }
    Return<void> gnssReleaseWakelockCb() override { return Void(); }
    Return<void> gnssRequestTimeCb() override { return Void(); }

    // Actual (test) callback handlers
    Return<void> gnssLocationCb(const GnssLocation& location) override {
      ALOGI("Location received");
      parent_.location_called_count_++;
      parent_.last_location_ = location;
      parent_.notify();
      return Void();
    }

    Return<void> gnssSetCapabilitesCb(uint32_t capabilities) override {
      ALOGI("Capabilities received %d", capabilities);
      parent_.capabilities_called_count_++;
      parent_.last_capabilities_ = capabilities;
      parent_.notify();
      return Void();
    }

    Return<void> gnssSetSystemInfoCb(
        const IGnssCallback::GnssSystemInfo& info) override {
      ALOGI("Info received, year %d", info.yearOfHw);
      parent_.info_called_count_++;
      parent_.last_info_ = info;
      parent_.notify();
      return Void();
    }
  };

  sp<IGnss> gnss_hal_;         // GNSS HAL to call into
  sp<IGnssCallback> gnss_cb_;  // Primary callback interface

  /* Count of calls to set the following items, and the latest item (used by
   * test.)
   */
  int capabilities_called_count_;
  uint32_t last_capabilities_;

  int location_called_count_;
  GnssLocation last_location_;

  int info_called_count_;
  IGnssCallback::GnssSystemInfo last_info_;

 private:
  std::mutex mtx_;
  std::condition_variable cv_;
  int count;
};

/*
 * SetCallbackCapabilitiesCleanup:
 * Sets up the callback, awaits the capabilities, and calls cleanup
 *
 * Since this is just the basic operation of SetUp() and TearDown(),
 * the function definition is intentionally empty
 */
TEST_F(GnssHalTest, SetCallbackCapabilitiesCleanup) {}

/*
 * CheckLocation:
 * Helper function to vet Location fields
 */

void CheckLocation(GnssLocation& location, bool checkAccuracies) {
  EXPECT_TRUE(location.gnssLocationFlags & GnssLocationFlags::HAS_LAT_LONG);
  EXPECT_TRUE(location.gnssLocationFlags & GnssLocationFlags::HAS_ALTITUDE);
  EXPECT_TRUE(location.gnssLocationFlags & GnssLocationFlags::HAS_SPEED);
  EXPECT_TRUE(location.gnssLocationFlags &
              GnssLocationFlags::HAS_HORIZONTAL_ACCURACY);
  // New uncertainties available in O must be provided,
  // at least when paired with modern hardware (2017+)
  if (checkAccuracies) {
    EXPECT_TRUE(location.gnssLocationFlags &
                GnssLocationFlags::HAS_VERTICAL_ACCURACY);
    EXPECT_TRUE(location.gnssLocationFlags &
                GnssLocationFlags::HAS_SPEED_ACCURACY);
    if (location.gnssLocationFlags & GnssLocationFlags::HAS_BEARING) {
      EXPECT_TRUE(location.gnssLocationFlags &
                  GnssLocationFlags::HAS_BEARING_ACCURACY);
    }
  }
  EXPECT_GE(location.latitudeDegrees, -90.0);
  EXPECT_LE(location.latitudeDegrees, 90.0);
  EXPECT_GE(location.longitudeDegrees, -180.0);
  EXPECT_LE(location.longitudeDegrees, 180.0);
  EXPECT_GE(location.altitudeMeters, -1000.0);
  EXPECT_LE(location.altitudeMeters, 30000.0);
  EXPECT_GE(location.speedMetersPerSec, 0.0);
  EXPECT_LE(location.speedMetersPerSec, 5.0);  // VTS tests are stationary.

  // Non-zero speeds must be reported with an associated bearing
  if (location.speedMetersPerSec > 0.0) {
    EXPECT_TRUE(location.gnssLocationFlags & GnssLocationFlags::HAS_BEARING);
  }

  /*
   * Tolerating some especially high values for accuracy estimate, in case of
   * first fix with especially poor geometry (happens occasionally)
   */
  EXPECT_GT(location.horizontalAccuracyMeters, 0.0);
  EXPECT_LE(location.horizontalAccuracyMeters, 250.0);

  /*
   * Some devices may define bearing as -180 to +180, others as 0 to 360.
   * Both are okay & understandable.
   */
  if (location.gnssLocationFlags & GnssLocationFlags::HAS_BEARING) {
    EXPECT_GE(location.bearingDegrees, -180.0);
    EXPECT_LE(location.bearingDegrees, 360.0);
  }
  if (location.gnssLocationFlags & GnssLocationFlags::HAS_VERTICAL_ACCURACY) {
    EXPECT_GT(location.verticalAccuracyMeters, 0.0);
    EXPECT_LE(location.verticalAccuracyMeters, 500.0);
  }
  if (location.gnssLocationFlags & GnssLocationFlags::HAS_SPEED_ACCURACY) {
    EXPECT_GT(location.speedAccuracyMetersPerSecond, 0.0);
    EXPECT_LE(location.speedAccuracyMetersPerSecond, 50.0);
  }
  if (location.gnssLocationFlags & GnssLocationFlags::HAS_BEARING_ACCURACY) {
    EXPECT_GT(location.bearingAccuracyDegrees, 0.0);
    EXPECT_LE(location.bearingAccuracyDegrees, 360.0);
  }

  // Check timestamp > 1.48e12 (47 years in msec - 1970->2017+)
  EXPECT_GT(location.timestamp, 1.48e12);
}

/*
 * GetLocation:
 * Turns on location, waits 45 second for at least 5 locations,
 * and checks them for reasonable validity.
 */
TEST_F(GnssHalTest, GetLocation) {
#define MIN_INTERVAL_MSEC 500
#define PREFERRED_ACCURACY 0   // Ideally perfect (matches GnssLocationProvider)
#define PREFERRED_TIME_MSEC 0  // Ideally immediate

#if GNSS_ASSISTANCE_IS_PRESENT
#define LOCATION_TIMEOUT_FIRST_SEC 15
#else
#define LOCATION_TIMEOUT_FIRST_SEC 45
#endif

#define LOCATION_TIMEOUT_SUBSEQUENT_SEC 3
#define LOCATIONS_TO_CHECK 5

  bool checkMoreAccuracies =
      (info_called_count_ > 0 && last_info_.yearOfHw >= 2017);

  auto result = gnss_hal_->setPositionMode(
      IGnss::GnssPositionMode::MS_BASED,
      IGnss::GnssPositionRecurrence::RECURRENCE_PERIODIC, MIN_INTERVAL_MSEC,
      PREFERRED_ACCURACY, PREFERRED_TIME_MSEC);

  ASSERT_TRUE(result.isOk());
  EXPECT_TRUE(result);

  result = gnss_hal_->start();

  ASSERT_TRUE(result.isOk());
  EXPECT_TRUE(result);

  /*
   * GPS signals initially optional for this test, so don't expect no timeout
   * yet
   */
  wait(LOCATION_TIMEOUT_FIRST_SEC);
  if (GNSS_SIGNAL_IS_PRESENT) {
    ASSERT_GT(location_called_count_, 0);
  }
  if (location_called_count_ > 0) {
    CheckLocation(last_location_, checkMoreAccuracies);
  }

  for (int i = 1; i < LOCATIONS_TO_CHECK; i++) {
    wait(LOCATION_TIMEOUT_SUBSEQUENT_SEC);
    if (location_called_count_ > 0) {
      CheckLocation(last_location_, checkMoreAccuracies);
    }
  }

  result = gnss_hal_->stop();

  ASSERT_TRUE(result.isOk());
  ASSERT_TRUE(result);
}

/*
 * InjectDelete:
 * Ensures that calls to inject and/or delete information state are handled.
 * Better tests await GPS signal
 */
TEST_F(GnssHalTest, InjectDelete) {
  // confidently, well north of Alaska
  auto result = gnss_hal_->injectLocation(80.0, -170.0, 1000.0);

  // TODO: full self-diff including TODO's :)
  ASSERT_TRUE(result.isOk());
  EXPECT_TRUE(result);

  // fake time, but generally reasonable values (time in Aug. 2018)
  result = gnss_hal_->injectTime(1534567890123L, 123456L, 10000L);

  ASSERT_TRUE(result.isOk());
  EXPECT_TRUE(result);

  auto resultVoid = gnss_hal_->deleteAidingData(IGnss::GnssAidingData::DELETE_ALL);

  ASSERT_TRUE(resultVoid.isOk());
}

/*
 * GetAllExtentions:
 * Tries getting all optional extensions, and ensures a valid return
 *   null or actual extension, no crash.
 * Confirms year-based required extensions (Measurement & Debug) are present
 */
TEST_F(GnssHalTest, GetAllExtensions) {
  // Basic call-is-handled checks
  auto gnssXtra = gnss_hal_->getExtensionXtra();
  ASSERT_TRUE(gnssXtra.isOk());

  auto gnssRil = gnss_hal_->getExtensionAGnssRil();
  ASSERT_TRUE(gnssRil.isOk());

  auto gnssAgnss = gnss_hal_->getExtensionAGnss();
  ASSERT_TRUE(gnssAgnss.isOk());

  auto gnssNi = gnss_hal_->getExtensionGnssNi();
  ASSERT_TRUE(gnssNi.isOk());

  auto gnssNavigationMessage = gnss_hal_->getExtensionGnssNavigationMessage();
  ASSERT_TRUE(gnssNavigationMessage.isOk());

  auto gnssConfiguration = gnss_hal_->getExtensionGnssConfiguration();
  ASSERT_TRUE(gnssConfiguration.isOk());

  auto gnssGeofencing = gnss_hal_->getExtensionGnssGeofencing();
  ASSERT_TRUE(gnssGeofencing.isOk());

  auto gnssBatching = gnss_hal_->getExtensionGnssBatching();
  ASSERT_TRUE(gnssBatching.isOk());

  // Verifying, in some cases, that these return actual extensions
  auto gnssMeasurement = gnss_hal_->getExtensionGnssMeasurement();
  ASSERT_TRUE(gnssMeasurement.isOk());
  if (last_capabilities_ & IGnssCallback::Capabilities::MEASUREMENTS) {
    sp<IGnssMeasurement> iGnssMeas = gnssMeasurement;
    EXPECT_NE(iGnssMeas, nullptr);
  }

  auto gnssDebug = gnss_hal_->getExtensionGnssDebug();
  ASSERT_TRUE(gnssDebug.isOk());
  if (info_called_count_ > 0 && last_info_.yearOfHw >= 2017) {
    sp<IGnssDebug> iGnssDebug = gnssDebug;
    EXPECT_NE(iGnssDebug, nullptr);
  }
}

/*
 * MeasurementCapabilities:
 * Verifies that modern hardware supports measurement capabilities.
 */
TEST_F(GnssHalTest, MeasurementCapabilites) {
  if (info_called_count_ > 0 && last_info_.yearOfHw >= 2016) {
    EXPECT_TRUE(last_capabilities_ & IGnssCallback::Capabilities::MEASUREMENTS);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  int status = RUN_ALL_TESTS();
  ALOGI("Test result = %d", status);
  return status;
}