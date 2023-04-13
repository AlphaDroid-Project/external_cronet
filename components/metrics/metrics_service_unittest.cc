// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/metrics/metrics_service.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <string>

#include "base/bind.h"
#include "base/containers/contains.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/raw_ptr.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_snapshot_manager.h"
#include "base/metrics/metrics_hashes.h"
#include "base/metrics/statistics_recorder.h"
#include "base/metrics/user_metrics.h"
#include "base/test/bind.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_simple_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "components/metrics/clean_exit_beacon.h"
#include "components/metrics/client_info.h"
#include "components/metrics/environment_recorder.h"
#include "components/metrics/log_decoder.h"
#include "components/metrics/metrics_features.h"
#include "components/metrics/metrics_log.h"
#include "components/metrics/metrics_pref_names.h"
#include "components/metrics/metrics_state_manager.h"
#include "components/metrics/metrics_upload_scheduler.h"
#include "components/metrics/stability_metrics_helper.h"
#include "components/metrics/test/test_enabled_state_provider.h"
#include "components/metrics/test/test_metrics_provider.h"
#include "components/metrics/test/test_metrics_service_client.h"
#include "components/metrics/unsent_log_store_metrics_impl.h"
#include "components/prefs/testing_pref_service.h"
#include "components/variations/active_field_trials.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/metrics_proto/chrome_user_metrics_extension.pb.h"
#include "third_party/metrics_proto/system_profile.pb.h"
#include "third_party/zlib/google/compression_utils.h"

namespace metrics {
namespace {

const char kTestPrefName[] = "TestPref";

class TestUnsentLogStore : public UnsentLogStore {
 public:
  explicit TestUnsentLogStore(PrefService* service)
      : UnsentLogStore(std::make_unique<UnsentLogStoreMetricsImpl>(),
                       service,
                       kTestPrefName,
                       nullptr,
                       /*min_log_count=*/3,
                       /*min_log_bytes=*/1,
                       /*max_log_size=*/0,
                       /*signing_key=*/std::string(),
                       /*logs_event_manager=*/nullptr) {}
  ~TestUnsentLogStore() override = default;

  TestUnsentLogStore(const TestUnsentLogStore&) = delete;
  TestUnsentLogStore& operator=(const TestUnsentLogStore&) = delete;

  static void RegisterPrefs(PrefRegistrySimple* registry) {
    registry->RegisterListPref(kTestPrefName);
  }
};

void YieldUntil(base::Time when) {
  while (base::Time::Now() <= when)
    base::PlatformThread::YieldCurrentThread();
}

// Returns true if |id| is present in |proto|'s collection of FieldTrials.
bool IsFieldTrialPresent(const SystemProfileProto& proto,
                         const std::string& trial_name,
                         const std::string& group_name) {
  const variations::ActiveGroupId id =
      variations::MakeActiveGroupId(trial_name, group_name);

  for (const auto& trial : proto.field_trial()) {
    if (trial.name_id() == id.name && trial.group_id() == id.group)
      return true;
  }
  return false;
}

class TestMetricsService : public MetricsService {
 public:
  TestMetricsService(MetricsStateManager* state_manager,
                     MetricsServiceClient* client,
                     PrefService* local_state)
      : MetricsService(state_manager, client, local_state) {}

  TestMetricsService(const TestMetricsService&) = delete;
  TestMetricsService& operator=(const TestMetricsService&) = delete;

  ~TestMetricsService() override = default;

  using MetricsService::INIT_TASK_SCHEDULED;
  using MetricsService::RecordCurrentEnvironmentHelper;
  using MetricsService::SENDING_LOGS;
  using MetricsService::state;

  // MetricsService:
  void SetPersistentSystemProfile(const std::string& serialized_proto,
                                  bool complete) override {
    persistent_system_profile_provided_ = true;
    persistent_system_profile_complete_ = complete;
  }

  bool persistent_system_profile_provided() const {
    return persistent_system_profile_provided_;
  }
  bool persistent_system_profile_complete() const {
    return persistent_system_profile_complete_;
  }

 private:
  bool persistent_system_profile_provided_ = false;
  bool persistent_system_profile_complete_ = false;
};

class TestMetricsLog : public MetricsLog {
 public:
  TestMetricsLog(const std::string& client_id,
                 int session_id,
                 MetricsServiceClient* client)
      : MetricsLog(client_id, session_id, MetricsLog::ONGOING_LOG, client) {}

  TestMetricsLog(const TestMetricsLog&) = delete;
  TestMetricsLog& operator=(const TestMetricsLog&) = delete;

  ~TestMetricsLog() override {}
};

const char kOnDidCreateMetricsLogHistogramName[] = "Test.OnDidCreateMetricsLog";

class TestMetricsProviderForOnDidCreateMetricsLog : public TestMetricsProvider {
 public:
  TestMetricsProviderForOnDidCreateMetricsLog() = default;
  ~TestMetricsProviderForOnDidCreateMetricsLog() override = default;

  void OnDidCreateMetricsLog() override {
    base::UmaHistogramBoolean(kOnDidCreateMetricsLogHistogramName, true);
  }
};

const char kProvideHistogramsHistogramName[] = "Test.ProvideHistograms";

class TestMetricsProviderForProvideHistograms : public TestMetricsProvider {
 public:
  TestMetricsProviderForProvideHistograms() = default;
  ~TestMetricsProviderForProvideHistograms() override = default;

  bool ProvideHistograms() override {
    base::UmaHistogramBoolean(kProvideHistogramsHistogramName, true);
    return true;
  }

  void ProvideCurrentSessionData(
      ChromeUserMetricsExtension* uma_proto) override {
    MetricsProvider::ProvideCurrentSessionData(uma_proto);
  }
};

class TestMetricsProviderForProvideHistogramsEarlyReturn
    : public TestMetricsProviderForProvideHistograms {
 public:
  TestMetricsProviderForProvideHistogramsEarlyReturn() = default;
  ~TestMetricsProviderForProvideHistogramsEarlyReturn() override = default;

  void OnDidCreateMetricsLog() override {}
};

class TestIndependentMetricsProvider : public MetricsProvider {
 public:
  TestIndependentMetricsProvider() = default;
  ~TestIndependentMetricsProvider() override = default;

  // MetricsProvider:
  bool HasIndependentMetrics() override {
    // Only return true the first time this is called (i.e., we only have one
    // independent log to provide).
    if (!has_independent_metrics_called_) {
      has_independent_metrics_called_ = true;
      return true;
    }
    return false;
  }
  void ProvideIndependentMetrics(
      base::OnceCallback<void(bool)> done_callback,
      ChromeUserMetricsExtension* uma_proto,
      base::HistogramSnapshotManager* snapshot_manager) override {
    provide_independent_metrics_called_ = true;
    uma_proto->set_client_id(123);
    std::move(done_callback).Run(true);
  }

  bool has_independent_metrics_called() const {
    return has_independent_metrics_called_;
  }

  bool provide_independent_metrics_called() const {
    return provide_independent_metrics_called_;
  }

 private:
  bool has_independent_metrics_called_ = false;
  bool provide_independent_metrics_called_ = false;
};

class MetricsServiceTest : public testing::Test {
 public:
  MetricsServiceTest()
      : task_runner_(new base::TestSimpleTaskRunner),
        task_runner_handle_(task_runner_),
        enabled_state_provider_(new TestEnabledStateProvider(false, false)) {
    base::SetRecordActionTaskRunner(task_runner_);
    MetricsService::RegisterPrefs(testing_local_state_.registry());
  }

  MetricsServiceTest(const MetricsServiceTest&) = delete;
  MetricsServiceTest& operator=(const MetricsServiceTest&) = delete;

  ~MetricsServiceTest() override {}

  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  MetricsStateManager* GetMetricsStateManager(
      const base::FilePath& user_data_dir = base::FilePath(),
      StartupVisibility startup_visibility = StartupVisibility::kUnknown) {
    // Lazy-initialize the metrics_state_manager so that it correctly reads the
    // stability state from prefs after tests have a chance to initialize it.
    if (!metrics_state_manager_) {
      metrics_state_manager_ = MetricsStateManager::Create(
          GetLocalState(), enabled_state_provider_.get(), std::wstring(),
          user_data_dir, startup_visibility);
      metrics_state_manager_->InstantiateFieldTrialList();
    }
    return metrics_state_manager_.get();
  }

  std::unique_ptr<TestUnsentLogStore> InitializeTestLogStoreAndGet() {
    TestUnsentLogStore::RegisterPrefs(testing_local_state_.registry());
    return std::make_unique<TestUnsentLogStore>(GetLocalState());
  }

  PrefService* GetLocalState() { return &testing_local_state_; }

  // Sets metrics reporting as enabled for testing.
  void EnableMetricsReporting() { SetMetricsReporting(true); }

  // Sets metrics reporting for testing.
  void SetMetricsReporting(bool enabled) {
    enabled_state_provider_->set_consent(enabled);
    enabled_state_provider_->set_enabled(enabled);
  }

  // Finds a histogram with the specified |name_hash| in |histograms|.
  const base::HistogramBase* FindHistogram(
      const base::StatisticsRecorder::Histograms& histograms,
      uint64_t name_hash) {
    for (const base::HistogramBase* histogram : histograms) {
      if (name_hash == base::HashMetricName(histogram->histogram_name()))
        return histogram;
    }
    return nullptr;
  }

  // Checks whether |uma_log| contains any histograms that are not flagged
  // with kUmaStabilityHistogramFlag. Stability logs should only contain such
  // histograms.
  void CheckForNonStabilityHistograms(
      const ChromeUserMetricsExtension& uma_log) {
    const int kStabilityFlags = base::HistogramBase::kUmaStabilityHistogramFlag;
    const base::StatisticsRecorder::Histograms histograms =
        base::StatisticsRecorder::GetHistograms();
    for (int i = 0; i < uma_log.histogram_event_size(); ++i) {
      const uint64_t hash = uma_log.histogram_event(i).name_hash();

      const base::HistogramBase* histogram = FindHistogram(histograms, hash);
      EXPECT_TRUE(histogram) << hash;

      EXPECT_EQ(kStabilityFlags, histogram->flags() & kStabilityFlags) << hash;
    }
  }

  // Returns the number of samples logged to the specified histogram or 0 if
  // the histogram was not found.
  int GetHistogramSampleCount(const ChromeUserMetricsExtension& uma_log,
                              base::StringPiece histogram_name) {
    const auto histogram_name_hash = base::HashMetricName(histogram_name);
    int samples = 0;
    for (int i = 0; i < uma_log.histogram_event_size(); ++i) {
      const auto& histogram = uma_log.histogram_event(i);
      if (histogram.name_hash() == histogram_name_hash) {
        for (int j = 0; j < histogram.bucket_size(); ++j) {
          const auto& bucket = histogram.bucket(j);
          // Per proto comments, count field not being set means 1 sample.
          samples += (!bucket.has_count() ? 1 : bucket.count());
        }
      }
    }
    return samples;
  }

  // Returns the sampled count of the |kOnDidCreateMetricsLogHistogramName|
  // histogram in the currently staged log in |test_log_store|.
  int GetSampleCountOfOnDidCreateLogHistogram(MetricsLogStore* test_log_store) {
    ChromeUserMetricsExtension log;
    EXPECT_TRUE(DecodeLogDataToProto(test_log_store->staged_log(), &log));
    return GetHistogramSampleCount(log, kOnDidCreateMetricsLogHistogramName);
  }

  int GetNumberOfUserActions(MetricsLogStore* test_log_store) {
    ChromeUserMetricsExtension log;
    EXPECT_TRUE(DecodeLogDataToProto(test_log_store->staged_log(), &log));
    return log.user_action_event_size();
  }

  const base::FilePath user_data_dir_path() { return temp_dir_.GetPath(); }

 protected:
  scoped_refptr<base::TestSimpleTaskRunner> task_runner_;
  base::ThreadTaskRunnerHandle task_runner_handle_;
  base::test::ScopedFeatureList feature_list_;

 private:
  std::unique_ptr<TestEnabledStateProvider> enabled_state_provider_;
  TestingPrefServiceSimple testing_local_state_;
  std::unique_ptr<MetricsStateManager> metrics_state_manager_;
  base::ScopedTempDir temp_dir_;
};

struct MetricsServiceTestWithFeaturesParams {
  bool emit_histograms_earlier;
  bool emit_for_independent_logs;
};

class MetricsServiceTestWithFeatures
    : public MetricsServiceTest,
      public ::testing::WithParamInterface<
          MetricsServiceTestWithFeaturesParams> {
 public:
  MetricsServiceTestWithFeatures() = default;
  ~MetricsServiceTestWithFeatures() override = default;

  bool ShouldEmitHistogramsEarlier() {
    return GetParam().emit_histograms_earlier;
  }
  bool ShouldEmitHistogramsForIndependentLogs() {
    return GetParam().emit_for_independent_logs;
  }

  void SetUp() override {
    MetricsServiceTest::SetUp();
    if (ShouldEmitHistogramsEarlier()) {
      const std::map<std::string, std::string> params = {
          {"emit_for_independent_logs",
           ShouldEmitHistogramsForIndependentLogs() ? "true" : "false"}};
      feature_list_.InitWithFeaturesAndParameters(
          {{features::kEmitHistogramsEarlier, params}}, {});
    } else {
      feature_list_.InitWithFeatures({}, {features::kEmitHistogramsEarlier});
    }
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

struct StartupVisibilityTestParams {
  const std::string test_name;
  metrics::StartupVisibility startup_visibility;
  bool emit_histograms_earlier;
  bool emit_for_independent_logs;
  bool expected_beacon_value;
};

class MetricsServiceTestWithStartupVisibility
    : public MetricsServiceTest,
      public ::testing::WithParamInterface<StartupVisibilityTestParams> {
 public:
  MetricsServiceTestWithStartupVisibility() = default;
  ~MetricsServiceTestWithStartupVisibility() override = default;

  bool ShouldEmitHistogramsEarlier() {
    return GetParam().emit_histograms_earlier;
  }
  bool ShouldEmitHistogramsForIndependentLogs() {
    return GetParam().emit_for_independent_logs;
  }

  void SetUp() override {
    MetricsServiceTest::SetUp();
    if (ShouldEmitHistogramsEarlier()) {
      const std::map<std::string, std::string> params = {
          {"emit_for_independent_logs",
           ShouldEmitHistogramsForIndependentLogs() ? "true" : "false"}};
      feature_list_.InitWithFeaturesAndParameters(
          {{features::kEmitHistogramsEarlier, params}}, {});
    } else {
      feature_list_.InitWithFeatures({}, {features::kEmitHistogramsEarlier});
    }
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

class ExperimentTestMetricsProvider : public TestMetricsProvider {
 public:
  explicit ExperimentTestMetricsProvider(
      base::FieldTrial* profile_metrics_trial,
      base::FieldTrial* session_data_trial)
      : profile_metrics_trial_(profile_metrics_trial),
        session_data_trial_(session_data_trial) {}

  ~ExperimentTestMetricsProvider() override = default;

  void ProvideSystemProfileMetrics(
      SystemProfileProto* system_profile_proto) override {
    TestMetricsProvider::ProvideSystemProfileMetrics(system_profile_proto);
    profile_metrics_trial_->Activate();
  }

  void ProvideCurrentSessionData(
      ChromeUserMetricsExtension* uma_proto) override {
    TestMetricsProvider::ProvideCurrentSessionData(uma_proto);
    session_data_trial_->Activate();
  }

 private:
  raw_ptr<base::FieldTrial> profile_metrics_trial_;
  raw_ptr<base::FieldTrial> session_data_trial_;
};

bool HistogramExists(base::StringPiece name) {
  return base::StatisticsRecorder::FindHistogram(name) != nullptr;
}

base::HistogramBase::Count GetHistogramDeltaTotalCount(base::StringPiece name) {
  return base::StatisticsRecorder::FindHistogram(name)
      ->SnapshotDelta()
      ->TotalCount();
}

}  // namespace

INSTANTIATE_TEST_SUITE_P(
    All,
    MetricsServiceTestWithFeatures,
    testing::Values(MetricsServiceTestWithFeaturesParams{true, true},
                    MetricsServiceTestWithFeaturesParams{true, false},
                    MetricsServiceTestWithFeaturesParams{false, false}));

TEST_P(MetricsServiceTestWithFeatures, InitialStabilityLogAfterCleanShutDown) {
  base::HistogramTester histogram_tester;
  EnableMetricsReporting();
  // Write a beacon file indicating that Chrome exited cleanly. Note that the
  // crash streak value is arbitrary.
  const base::FilePath beacon_file_path =
      user_data_dir_path().Append(kCleanExitBeaconFilename);
  ASSERT_LT(0,
            base::WriteFile(beacon_file_path,
                            CleanExitBeacon::CreateBeaconFileContentsForTesting(
                                /*exited_cleanly=*/true, /*crash_streak=*/1)
                                .data()));

  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(user_data_dir_path()),
                             &client, GetLocalState());

  TestMetricsProvider* test_provider = new TestMetricsProvider();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();

  // No initial stability log should be generated.
  EXPECT_FALSE(service.has_unsent_logs());

  // Ensure that HasPreviousSessionData() is always called on providers,
  // for consistency, even if other conditions already indicate their presence.
  EXPECT_TRUE(test_provider->has_initial_stability_metrics_called());

  // The test provider should not have been called upon to provide initial
  // stability nor regular stability metrics.
  EXPECT_FALSE(test_provider->provide_initial_stability_metrics_called());
  EXPECT_FALSE(test_provider->provide_stability_metrics_called());

  // As there wasn't an unclean shutdown, no browser crash samples should have
  // been emitted.
  histogram_tester.ExpectBucketCount("Stability.Counts2",
                                     StabilityEventType::kBrowserCrash, 0);
}

TEST_P(MetricsServiceTestWithFeatures, InitialStabilityLogAtProviderRequest) {
  base::HistogramTester histogram_tester;
  EnableMetricsReporting();

  // Save an existing system profile to prefs, to correspond to what would be
  // saved from a previous session.
  TestMetricsServiceClient client;
  TestMetricsLog log("client", 1, &client);
  DelegatingProvider delegating_provider;
  TestMetricsService::RecordCurrentEnvironmentHelper(&log, GetLocalState(),
                                                     &delegating_provider);

  // Record stability build time and version from previous session, so that
  // stability metrics (including exited cleanly flag) won't be cleared.
  EnvironmentRecorder(GetLocalState())
      .SetBuildtimeAndVersion(MetricsLog::GetBuildTime(),
                              client.GetVersionString());

  // Write a beacon file indicating that Chrome exited cleanly. Note that the
  // crash streak value is arbitrary.
  const base::FilePath beacon_file_path =
      user_data_dir_path().Append(kCleanExitBeaconFilename);
  ASSERT_LT(0,
            base::WriteFile(beacon_file_path,
                            CleanExitBeacon::CreateBeaconFileContentsForTesting(
                                /*exited_cleanly=*/true, /*crash_streak=*/1)
                                .data()));

  TestMetricsService service(GetMetricsStateManager(user_data_dir_path()),
                             &client, GetLocalState());
  // Add a metrics provider that requests a stability log.
  TestMetricsProvider* test_provider = new TestMetricsProvider();
  test_provider->set_has_initial_stability_metrics(true);
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();

  // The initial stability log should be generated and persisted in unsent logs.
  MetricsLogStore* test_log_store = service.LogStoreForTest();
  EXPECT_TRUE(test_log_store->has_unsent_logs());
  EXPECT_FALSE(test_log_store->has_staged_log());

  // Ensure that HasPreviousSessionData() is always called on providers,
  // for consistency, even if other conditions already indicate their presence.
  EXPECT_TRUE(test_provider->has_initial_stability_metrics_called());

  // The test provider should have been called upon to provide initial
  // stability and regular stability metrics.
  EXPECT_TRUE(test_provider->provide_initial_stability_metrics_called());
  EXPECT_TRUE(test_provider->provide_stability_metrics_called());

  // Stage the log and retrieve it.
  test_log_store->StageNextLog();
  EXPECT_TRUE(test_log_store->has_staged_log());

  ChromeUserMetricsExtension uma_log;
  EXPECT_TRUE(DecodeLogDataToProto(test_log_store->staged_log(), &uma_log));

  EXPECT_TRUE(uma_log.has_client_id());
  EXPECT_TRUE(uma_log.has_session_id());
  EXPECT_TRUE(uma_log.has_system_profile());
  EXPECT_EQ(0, uma_log.user_action_event_size());
  EXPECT_EQ(0, uma_log.omnibox_event_size());
  CheckForNonStabilityHistograms(uma_log);

  // As there wasn't an unclean shutdown, no browser crash samples should have
  // been emitted.
  histogram_tester.ExpectBucketCount("Stability.Counts2",
                                     StabilityEventType::kBrowserCrash, 0);
}

TEST_P(MetricsServiceTestWithFeatures, IndependentLogAtProviderRequest) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  // Create a a provider that will have one independent log to provide.
  auto* test_provider = new TestIndependentMetricsProvider();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();
  ASSERT_EQ(TestMetricsService::INIT_TASK_SCHEDULED, service.state());

  // Verify that the independent log provider has not yet been called, and emit
  // a histogram. This histogram should not be put into the independent log.
  EXPECT_FALSE(test_provider->has_independent_metrics_called());
  EXPECT_FALSE(test_provider->provide_independent_metrics_called());
  const std::string test_histogram = "Test.Histogram";
  base::UmaHistogramBoolean(test_histogram, true);

  // Run pending tasks to finish init task and complete the first ongoing log.
  // It should also have called the independent log provider (which should have
  // produced a log).
  task_runner_->RunPendingTasks();
  EXPECT_EQ(TestMetricsService::SENDING_LOGS, service.state());
  EXPECT_TRUE(test_provider->has_independent_metrics_called());
  EXPECT_TRUE(test_provider->provide_independent_metrics_called());

  MetricsLogStore* test_log_store = service.LogStoreForTest();

  // The currently staged log should be the independent log created by the
  // independent log provider. The log should have a client id of 123. It should
  // also not contain |test_histogram|.
  ASSERT_TRUE(test_log_store->has_staged_log());
  ChromeUserMetricsExtension uma_log;
  EXPECT_TRUE(DecodeLogDataToProto(test_log_store->staged_log(), &uma_log));
  EXPECT_EQ(uma_log.client_id(), 123UL);
  EXPECT_EQ(GetHistogramSampleCount(uma_log, test_histogram), 0);

  // Discard the staged log and stage the next one. It should be the first
  // ongoing log.
  test_log_store->DiscardStagedLog();
  ASSERT_TRUE(test_log_store->has_unsent_logs());
  test_log_store->StageNextLog();
  ASSERT_TRUE(test_log_store->has_staged_log());

  // Verify that the first ongoing log contains |test_histogram| (it should not
  // have been put into the independent log).
  EXPECT_TRUE(DecodeLogDataToProto(test_log_store->staged_log(), &uma_log));
  EXPECT_EQ(GetHistogramSampleCount(uma_log, test_histogram), 1);
}

TEST_P(MetricsServiceTestWithFeatures, OnDidCreateMetricsLogAtShutdown) {
  base::HistogramTester histogram_tester;
  EnableMetricsReporting();
  TestMetricsServiceClient client;

  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  // Create a provider that will log to |kOnDidCreateMetricsLogHistogramName|
  // in OnDidCreateMetricsLog().
  auto* test_provider = new TestMetricsProviderForOnDidCreateMetricsLog();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();

  // OnDidCreateMetricsLog() is called once when the first ongoing log is
  // created.
  histogram_tester.ExpectBucketCount(kOnDidCreateMetricsLogHistogramName, true,
                                     1);
  service.Stop();

  // If the feature kEmitHistogramsEarlier is enabled and parameter
  // kEmitHistogramsForIndependentLogs is set to true, OnDidCreateMetricsLog()
  // will be called during shutdown to emit histograms.
  histogram_tester.ExpectBucketCount(
      kOnDidCreateMetricsLogHistogramName, true,
      ShouldEmitHistogramsForIndependentLogs() ? 2 : 1);

  // Clean up histograms.
  base::StatisticsRecorder::ForgetHistogramForTesting(
      kOnDidCreateMetricsLogHistogramName);
}

TEST_P(MetricsServiceTestWithFeatures, ProvideHistograms) {
  base::HistogramTester histogram_tester;
  EnableMetricsReporting();
  TestMetricsServiceClient client;

  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  // Create a provider that will log to |kProvideHistogramsHistogramName|
  // in ProvideHistograms().
  auto* test_provider = new TestMetricsProviderForProvideHistograms();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();

  // If the feature kEmitHistogramsEarlier is enabled, ProvideHistograms() is
  // called in OnDidCreateMetricsLog().
  histogram_tester.ExpectBucketCount(kProvideHistogramsHistogramName, true,
                                     ShouldEmitHistogramsEarlier() ? 1 : 0);

  service.StageCurrentLogForTest();
  // Make sure if kEmitHistogramsEarlier is not set, ProvideHistograms() is
  // called in ProvideCurrentSessionData().
  histogram_tester.ExpectBucketCount(kProvideHistogramsHistogramName, true,
                                     ShouldEmitHistogramsEarlier() ? 2 : 1);

  service.Stop();

  // Clean up histograms.
  base::StatisticsRecorder::ForgetHistogramForTesting(
      kProvideHistogramsHistogramName);
}

TEST_P(MetricsServiceTestWithFeatures, ProvideHistogramsEarlyReturn) {
  base::HistogramTester histogram_tester;
  EnableMetricsReporting();
  TestMetricsServiceClient client;

  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  // Create a provider that will log to |kOnDidCreateMetricsLogHistogramName|
  // in OnDidCreateMetricsLog().
  auto* test_provider =
      new TestMetricsProviderForProvideHistogramsEarlyReturn();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();

  // Make sure no histogram is emitted when having an early return.
  histogram_tester.ExpectBucketCount(kProvideHistogramsHistogramName, true, 0);

  service.StageCurrentLogForTest();
  // ProvideHistograms() should be called in ProvideCurrentSessionData() if
  // histograms haven't been emitted.
  histogram_tester.ExpectBucketCount(kProvideHistogramsHistogramName, true, 1);

  // Try another log to make sure emission status is reset between logs.
  service.LogStoreForTest()->DiscardStagedLog();
  service.StageCurrentLogForTest();
  histogram_tester.ExpectBucketCount(kProvideHistogramsHistogramName, true, 2);

  service.Stop();

  // Clean up histograms.
  base::StatisticsRecorder::ForgetHistogramForTesting(
      kProvideHistogramsHistogramName);
}

INSTANTIATE_TEST_SUITE_P(
    All,
    MetricsServiceTestWithStartupVisibility,
    ::testing::Values(
        StartupVisibilityTestParams{
            .test_name = "UnknownVisibilityEarlyEmissionIndependentLogs",
            .startup_visibility = StartupVisibility::kUnknown,
            .emit_histograms_earlier = true,
            .emit_for_independent_logs = true,
            .expected_beacon_value = true},
        StartupVisibilityTestParams{
            .test_name = "BackgroundVisibilityEarlyEmissionIndependentLogs",
            .startup_visibility = StartupVisibility::kBackground,
            .emit_histograms_earlier = true,
            .emit_for_independent_logs = true,
            .expected_beacon_value = true},
        StartupVisibilityTestParams{
            .test_name = "ForegroundVisibilityEarlyEmissionIndependentLogs",
            .startup_visibility = StartupVisibility::kForeground,
            .emit_histograms_earlier = true,
            .emit_for_independent_logs = true,
            .expected_beacon_value = false},
        StartupVisibilityTestParams{
            .test_name = "UnknownVisibilityEarlyEmission",
            .startup_visibility = StartupVisibility::kUnknown,
            .emit_histograms_earlier = true,
            .emit_for_independent_logs = false,
            .expected_beacon_value = true},
        StartupVisibilityTestParams{
            .test_name = "BackgroundVisibilityEarlyEmission",
            .startup_visibility = StartupVisibility::kBackground,
            .emit_histograms_earlier = true,
            .emit_for_independent_logs = false,
            .expected_beacon_value = true},
        StartupVisibilityTestParams{
            .test_name = "ForegroundVisibilityEarlyEmission",
            .startup_visibility = StartupVisibility::kForeground,
            .emit_histograms_earlier = true,
            .emit_for_independent_logs = false,
            .expected_beacon_value = false},
        StartupVisibilityTestParams{
            .test_name = "UnknownVisibility",
            .startup_visibility = StartupVisibility::kUnknown,
            .emit_histograms_earlier = false,
            .emit_for_independent_logs = false,
            .expected_beacon_value = true},
        StartupVisibilityTestParams{
            .test_name = "BackgroundVisibility",
            .startup_visibility = StartupVisibility::kBackground,
            .emit_histograms_earlier = false,
            .emit_for_independent_logs = false,
            .expected_beacon_value = true},
        StartupVisibilityTestParams{
            .test_name = "ForegroundVisibility",
            .startup_visibility = StartupVisibility::kForeground,
            .emit_histograms_earlier = false,
            .emit_for_independent_logs = false,
            .expected_beacon_value = false}),
    [](const ::testing::TestParamInfo<StartupVisibilityTestParams>& params) {
      return params.param.test_name;
    });

TEST_P(MetricsServiceTestWithStartupVisibility, InitialStabilityLogAfterCrash) {
  base::HistogramTester histogram_tester;
  PrefService* local_state = GetLocalState();
  EnableMetricsReporting();

  // Write a beacon file indicating that Chrome exited uncleanly. Note that the
  // crash streak value is arbitrary.
  const base::FilePath beacon_file_path =
      user_data_dir_path().Append(kCleanExitBeaconFilename);
  ASSERT_LT(0,
            base::WriteFile(beacon_file_path,
                            CleanExitBeacon::CreateBeaconFileContentsForTesting(
                                /*exited_cleanly=*/false, /*crash_streak=*/1)
                                .data()));

  // Set up prefs to simulate restarting after a crash.

  // Save an existing system profile to prefs, to correspond to what would be
  // saved from a previous session.
  TestMetricsServiceClient client;
  const std::string kCrashedVersion = "4.0.321.0-64-devel";
  client.set_version_string(kCrashedVersion);
  TestMetricsLog log("client", 1, &client);
  DelegatingProvider delegating_provider;
  TestMetricsService::RecordCurrentEnvironmentHelper(&log, local_state,
                                                     &delegating_provider);

  // Record stability build time and version from previous session, so that
  // stability metrics (including exited cleanly flag) won't be cleared.
  EnvironmentRecorder(local_state)
      .SetBuildtimeAndVersion(MetricsLog::GetBuildTime(),
                              client.GetVersionString());

  const std::string kCurrentVersion = "5.0.322.0-64-devel";
  client.set_version_string(kCurrentVersion);

  StartupVisibilityTestParams params = GetParam();
  TestMetricsService service(
      GetMetricsStateManager(user_data_dir_path(), params.startup_visibility),
      &client, local_state);
  // Add a provider.
  TestMetricsProvider* test_provider = new TestMetricsProvider();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));
  service.InitializeMetricsRecordingState();

  // Verify that Chrome is (or is not) watching for crashes by checking the
  // beacon value.
  std::string beacon_file_contents;
  ASSERT_TRUE(base::ReadFileToString(beacon_file_path, &beacon_file_contents));
  std::string partial_expected_contents;
#if BUILDFLAG(IS_ANDROID)
  // Whether Chrome is watching for crashes after
  // InitializeMetricsRecordingState() depends on the type of Android Chrome
  // session. See the comments in MetricsService::InitializeMetricsState() for
  // more details.
  const std::string beacon_value =
      params.expected_beacon_value ? "true" : "false";
  partial_expected_contents = "exited_cleanly\":" + beacon_value;
#else
  partial_expected_contents = "exited_cleanly\":false";
#endif  // BUILDFLAG(IS_ANDROID)
  EXPECT_TRUE(base::Contains(beacon_file_contents, partial_expected_contents));

  // The initial stability log should be generated and persisted in unsent logs.
  MetricsLogStore* test_log_store = service.LogStoreForTest();
  EXPECT_TRUE(test_log_store->has_unsent_logs());
  EXPECT_FALSE(test_log_store->has_staged_log());

  // Ensure that HasPreviousSessionData() is always called on providers,
  // for consistency, even if other conditions already indicate their presence.
  EXPECT_TRUE(test_provider->has_initial_stability_metrics_called());

  // The test provider should have been called upon to provide initial
  // stability and regular stability metrics.
  EXPECT_TRUE(test_provider->provide_initial_stability_metrics_called());
  EXPECT_TRUE(test_provider->provide_stability_metrics_called());

  // The test provider should have been called when the initial stability log
  // was closed.
  EXPECT_TRUE(test_provider->record_initial_histogram_snapshots_called());

  // Stage the log and retrieve it.
  test_log_store->StageNextLog();
  EXPECT_TRUE(test_log_store->has_staged_log());

  ChromeUserMetricsExtension uma_log;
  EXPECT_TRUE(DecodeLogDataToProto(test_log_store->staged_log(), &uma_log));

  EXPECT_TRUE(uma_log.has_client_id());
  EXPECT_TRUE(uma_log.has_session_id());
  EXPECT_TRUE(uma_log.has_system_profile());
  EXPECT_EQ(0, uma_log.user_action_event_size());
  EXPECT_EQ(0, uma_log.omnibox_event_size());
  CheckForNonStabilityHistograms(uma_log);

  // Verify that the histograms emitted by the test provider made it into the
  // log.
  EXPECT_EQ(GetHistogramSampleCount(uma_log, "TestMetricsProvider.Initial"), 1);
  EXPECT_EQ(GetHistogramSampleCount(uma_log, "TestMetricsProvider.Regular"), 1);

  EXPECT_EQ(kCrashedVersion, uma_log.system_profile().app_version());
  EXPECT_EQ(kCurrentVersion,
            uma_log.system_profile().log_written_by_app_version());

  histogram_tester.ExpectBucketCount("Stability.Counts2",
                                     StabilityEventType::kBrowserCrash, 1);
}

TEST_P(MetricsServiceTestWithFeatures,
       InitialLogsHaveOnDidCreateMetricsLogHistograms) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  // Create a provider that will log to |kOnDidCreateMetricsLogHistogramName|
  // in OnDidCreateMetricsLog()
  auto* test_provider = new TestMetricsProviderForOnDidCreateMetricsLog();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();
  ASSERT_EQ(TestMetricsService::INIT_TASK_SCHEDULED, service.state());

  // Run pending tasks to finish init task and complete the first ongoing log.
  // Also verify that the test provider was called when closing the log.
  task_runner_->RunPendingTasks();
  ASSERT_EQ(TestMetricsService::SENDING_LOGS, service.state());
  EXPECT_TRUE(test_provider->record_histogram_snapshots_called());

  MetricsLogStore* test_log_store = service.LogStoreForTest();

  // Stage the next log, which should be the first ongoing log.
  // Check that it has one sample in |kOnDidCreateMetricsLogHistogramName|.
  test_log_store->StageNextLog();
  EXPECT_EQ(1, GetSampleCountOfOnDidCreateLogHistogram(test_log_store));

  // Discard the staged log and close and stage the next log, which is the
  // second "ongoing log".
  // Check that it has one sample in |kOnDidCreateMetricsLogHistogramName|.
  // Also verify that the test provider was called when closing the new log.
  test_provider->set_record_histogram_snapshots_called(false);
  test_log_store->DiscardStagedLog();
  service.StageCurrentLogForTest();
  EXPECT_EQ(1, GetSampleCountOfOnDidCreateLogHistogram(test_log_store));
  EXPECT_TRUE(test_provider->record_histogram_snapshots_called());

  // Check one more log for good measure.
  test_provider->set_record_histogram_snapshots_called(false);
  test_log_store->DiscardStagedLog();
  service.StageCurrentLogForTest();
  EXPECT_EQ(1, GetSampleCountOfOnDidCreateLogHistogram(test_log_store));
  EXPECT_TRUE(test_provider->record_histogram_snapshots_called());

  service.Stop();

  // Clean up histograms.
  base::StatisticsRecorder::ForgetHistogramForTesting(
      kOnDidCreateMetricsLogHistogramName);
}

TEST_P(MetricsServiceTestWithFeatures, MarkCurrentHistogramsAsReported) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  // Emit to histogram |Test.Before.Histogram|.
  ASSERT_FALSE(HistogramExists("Test.Before.Histogram"));
  base::UmaHistogramBoolean("Test.Before.Histogram", true);
  ASSERT_TRUE(HistogramExists("Test.Before.Histogram"));

  // Mark histogram data that has been collected until now (in particular, the
  // |Test.Before.Histogram| sample) as reported.
  service.MarkCurrentHistogramsAsReported();

  // Emit to histogram |Test.After.Histogram|.
  ASSERT_FALSE(HistogramExists("Test.After.Histogram"));
  base::UmaHistogramBoolean("Test.After.Histogram", true);
  ASSERT_TRUE(HistogramExists("Test.After.Histogram"));

  // Verify that the |Test.Before.Histogram| sample was marked as reported, and
  // is not included in the next snapshot.
  EXPECT_EQ(0, GetHistogramDeltaTotalCount("Test.Before.Histogram"));
  // Verify that the |Test.After.Histogram| sample was not marked as reported,
  // and is included in the next snapshot.
  EXPECT_EQ(1, GetHistogramDeltaTotalCount("Test.After.Histogram"));

  // Clean up histograms.
  base::StatisticsRecorder::ForgetHistogramForTesting("Test.Before.Histogram");
  base::StatisticsRecorder::ForgetHistogramForTesting("Test.After.Histogram");
}

TEST_P(MetricsServiceTestWithFeatures, LogHasUserActions) {
  // This test verifies that user actions are properly captured in UMA logs.
  // In particular, it checks that the first log has actions, a behavior that
  // was buggy in the past, plus additional checks for subsequent logs with
  // different numbers of actions.
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  service.InitializeMetricsRecordingState();

  // Start() will create an initial log.
  service.Start();
  ASSERT_EQ(TestMetricsService::INIT_TASK_SCHEDULED, service.state());

  base::RecordAction(base::UserMetricsAction("TestAction"));
  base::RecordAction(base::UserMetricsAction("TestAction"));
  base::RecordAction(base::UserMetricsAction("DifferentAction"));

  // Run pending tasks to finish init task and complete the first ongoing log.
  task_runner_->RunPendingTasks();
  ASSERT_EQ(TestMetricsService::SENDING_LOGS, service.state());

  MetricsLogStore* test_log_store = service.LogStoreForTest();

  // Stage the next log, which should be the initial metrics log.
  test_log_store->StageNextLog();
  EXPECT_EQ(3, GetNumberOfUserActions(test_log_store));

  // Log another action.
  base::RecordAction(base::UserMetricsAction("TestAction"));
  test_log_store->DiscardStagedLog();
  service.StageCurrentLogForTest();
  EXPECT_EQ(1, GetNumberOfUserActions(test_log_store));

  // Check a log with no actions.
  test_log_store->DiscardStagedLog();
  service.StageCurrentLogForTest();
  EXPECT_EQ(0, GetNumberOfUserActions(test_log_store));

  // And another one with a couple.
  base::RecordAction(base::UserMetricsAction("TestAction"));
  base::RecordAction(base::UserMetricsAction("TestAction"));
  test_log_store->DiscardStagedLog();
  service.StageCurrentLogForTest();
  EXPECT_EQ(2, GetNumberOfUserActions(test_log_store));
}

TEST_P(MetricsServiceTestWithFeatures, FirstLogCreatedBeforeUnsentLogsSent) {
  // This test checks that we will create and serialize the first ongoing log
  // before starting to send unsent logs from the past session. The latter is
  // simulated by injecting some fake ongoing logs into the MetricsLogStore.
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();
  ASSERT_EQ(TestMetricsService::INIT_TASK_SCHEDULED, service.state());

  MetricsLogStore* test_log_store = service.LogStoreForTest();

  // Set up the log store with an existing fake log entry. The string content
  // is never deserialized to proto, so we're just passing some dummy content.
  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(0u, test_log_store->ongoing_log_count());
  test_log_store->StoreLog("blah_blah", MetricsLog::ONGOING_LOG, LogMetadata());
  // Note: |initial_log_count()| refers to initial stability logs, so the above
  // log is counted an ongoing log (per its type).
  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(1u, test_log_store->ongoing_log_count());

  // Run pending tasks to finish init task and complete the first ongoing log.
  task_runner_->RunPendingTasks();
  ASSERT_EQ(TestMetricsService::SENDING_LOGS, service.state());
  // When the init task is complete, the first ongoing log should be created
  // and added to the ongoing logs.
  EXPECT_EQ(0u, test_log_store->initial_log_count());
  EXPECT_EQ(2u, test_log_store->ongoing_log_count());
}

TEST_P(MetricsServiceTestWithFeatures,
       MetricsProviderOnRecordingDisabledCalledOnInitialStop) {
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  TestMetricsProvider* test_provider = new TestMetricsProvider();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();
  service.Stop();

  EXPECT_TRUE(test_provider->on_recording_disabled_called());
}

TEST_P(MetricsServiceTestWithFeatures, MetricsProvidersInitialized) {
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  TestMetricsProvider* test_provider = new TestMetricsProvider();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();

  EXPECT_TRUE(test_provider->init_called());
}

// Verify that FieldTrials activated by a MetricsProvider are reported by the
// FieldTrialsProvider.
TEST_P(MetricsServiceTestWithFeatures, ActiveFieldTrialsReported) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  // Set up FieldTrials.
  const std::string trial_name1 = "CoffeeExperiment";
  const std::string group_name1 = "Free";
  base::FieldTrial* trial1 =
      base::FieldTrialList::CreateFieldTrial(trial_name1, group_name1);

  const std::string trial_name2 = "DonutExperiment";
  const std::string group_name2 = "MapleBacon";
  base::FieldTrial* trial2 =
      base::FieldTrialList::CreateFieldTrial(trial_name2, group_name2);

  service.RegisterMetricsProvider(
      std::make_unique<ExperimentTestMetricsProvider>(trial1, trial2));

  service.InitializeMetricsRecordingState();
  service.Start();
  service.StageCurrentLogForTest();

  MetricsLogStore* test_log_store = service.LogStoreForTest();
  ChromeUserMetricsExtension uma_log;
  EXPECT_TRUE(DecodeLogDataToProto(test_log_store->staged_log(), &uma_log));

  // Verify that the reported FieldTrial IDs are for the trial set up by this
  // test.
  EXPECT_TRUE(
      IsFieldTrialPresent(uma_log.system_profile(), trial_name1, group_name1));
  EXPECT_TRUE(
      IsFieldTrialPresent(uma_log.system_profile(), trial_name2, group_name2));
}

TEST_P(MetricsServiceTestWithFeatures,
       SystemProfileDataProvidedOnEnableRecording) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  TestMetricsProvider* test_provider = new TestMetricsProvider();
  service.RegisterMetricsProvider(
      std::unique_ptr<MetricsProvider>(test_provider));

  service.InitializeMetricsRecordingState();

  // ProvideSystemProfileMetrics() shouldn't be called initially.
  EXPECT_FALSE(test_provider->provide_system_profile_metrics_called());
  EXPECT_FALSE(service.persistent_system_profile_provided());

  service.Start();

  // Start should call ProvideSystemProfileMetrics().
  EXPECT_TRUE(test_provider->provide_system_profile_metrics_called());
  EXPECT_TRUE(service.persistent_system_profile_provided());
  EXPECT_FALSE(service.persistent_system_profile_complete());
}

TEST_P(MetricsServiceTestWithFeatures, SplitRotation) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());
  service.InitializeMetricsRecordingState();
  service.Start();
  // Rotation loop should create a log and mark state as idle.
  // Upload loop should start upload or be restarted.
  // The independent-metrics upload job will be started and always be a task.
  task_runner_->RunPendingTasks();
  // Rotation loop should terminated due to being idle.
  // Upload loop should start uploading if it isn't already.
  task_runner_->RunPendingTasks();
  EXPECT_TRUE(client.uploader()->is_uploading());
  EXPECT_EQ(1U, task_runner_->NumPendingTasks());
  service.OnApplicationNotIdle();
  EXPECT_TRUE(client.uploader()->is_uploading());
  EXPECT_EQ(2U, task_runner_->NumPendingTasks());
  // Log generation should be suppressed due to unsent log.
  // Idle state should not be reset.
  task_runner_->RunPendingTasks();
  EXPECT_TRUE(client.uploader()->is_uploading());
  EXPECT_EQ(2U, task_runner_->NumPendingTasks());
  // Make sure idle state was not reset.
  task_runner_->RunPendingTasks();
  EXPECT_TRUE(client.uploader()->is_uploading());
  EXPECT_EQ(2U, task_runner_->NumPendingTasks());
  // Upload should not be rescheduled, since there are no other logs.
  client.uploader()->CompleteUpload(200);
  EXPECT_FALSE(client.uploader()->is_uploading());
  EXPECT_EQ(2U, task_runner_->NumPendingTasks());
  // Running should generate a log, restart upload loop, and mark idle.
  task_runner_->RunPendingTasks();
  EXPECT_FALSE(client.uploader()->is_uploading());
  EXPECT_EQ(3U, task_runner_->NumPendingTasks());
  // Upload should start, and rotation loop should idle out.
  task_runner_->RunPendingTasks();
  EXPECT_TRUE(client.uploader()->is_uploading());
  EXPECT_EQ(1U, task_runner_->NumPendingTasks());
}

TEST_P(MetricsServiceTestWithFeatures, LastLiveTimestamp) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  base::Time initial_last_live_time =
      GetLocalState()->GetTime(prefs::kStabilityBrowserLastLiveTimeStamp);

  service.InitializeMetricsRecordingState();
  service.Start();

  task_runner_->RunPendingTasks();
  size_t num_pending_tasks = task_runner_->NumPendingTasks();

  service.StartUpdatingLastLiveTimestamp();

  // Starting the update sequence should not write anything, but should
  // set up for a later write.
  EXPECT_EQ(
      initial_last_live_time,
      GetLocalState()->GetTime(prefs::kStabilityBrowserLastLiveTimeStamp));
  EXPECT_EQ(num_pending_tasks + 1, task_runner_->NumPendingTasks());

  // To avoid flakiness, yield until we're over a microsecond threshold.
  YieldUntil(initial_last_live_time + base::Microseconds(2));

  task_runner_->RunPendingTasks();

  // Verify that the time has updated in local state.
  base::Time updated_last_live_time =
      GetLocalState()->GetTime(prefs::kStabilityBrowserLastLiveTimeStamp);
  EXPECT_LT(initial_last_live_time, updated_last_live_time);

  // Double check that an update schedules again...
  YieldUntil(updated_last_live_time + base::Microseconds(2));

  task_runner_->RunPendingTasks();
  EXPECT_LT(
      updated_last_live_time,
      GetLocalState()->GetTime(prefs::kStabilityBrowserLastLiveTimeStamp));
}

TEST_P(MetricsServiceTestWithFeatures, EnablementObserverNotification) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());
  service.InitializeMetricsRecordingState();

  absl::optional<bool> enabled;
  auto observer = [&enabled](bool notification) { enabled = notification; };

  auto subscription =
      service.AddEnablementObserver(base::BindLambdaForTesting(observer));

  service.Start();
  ASSERT_TRUE(enabled.has_value());
  EXPECT_TRUE(enabled.value());

  enabled.reset();

  service.Stop();
  ASSERT_TRUE(enabled.has_value());
  EXPECT_FALSE(enabled.value());
}

#if BUILDFLAG(IS_CHROMEOS_LACROS)
// ResetClientId is only enabled on certain targets.
TEST_P(MetricsServiceTestWithFeatures, SetClientIdToExternalId) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  const std::string client_id = "d92ad666-a420-4c73-8718-94311ae2ff5f";

  EXPECT_NE(service.GetClientId(), client_id);

  service.SetExternalClientId(client_id);
  // Reset will cause the client id to be regenerated. If an external client id
  // is provided, it should defer to using that id instead of creating its own.
  service.ResetClientId();

  EXPECT_EQ(service.GetClientId(), client_id);
}
#endif  //  BUILDFLAG(IS_CHROMEOS_LACROS)

#if BUILDFLAG(IS_CHROMEOS_ASH)
TEST_P(MetricsServiceTestWithFeatures,
       OngoingLogNotFlushedBeforeInitialLogWhenUserLogStoreSet) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();

  MetricsLogStore* test_log_store = service.LogStoreForTest();
  std::unique_ptr<TestUnsentLogStore> alternate_ongoing_log_store =
      InitializeTestLogStoreAndGet();
  TestUnsentLogStore* alternate_ongoing_log_store_ptr =
      alternate_ongoing_log_store.get();

  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(0u, test_log_store->ongoing_log_count());

  service.SetUserLogStore(std::move(alternate_ongoing_log_store));

  // Initial logs should not have been collected so the ongoing log being
  // recorded should not be flushed when a user log store is mounted.
  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(0u, test_log_store->ongoing_log_count());

  // Run pending tasks to finish init task and complete the first ongoing log.
  task_runner_->RunPendingTasks();
  ASSERT_EQ(TestMetricsService::SENDING_LOGS, service.state());
  // When the init task is complete, the first ongoing log should be created
  // in the alternate ongoing log store.
  EXPECT_EQ(0u, test_log_store->initial_log_count());
  EXPECT_EQ(0u, test_log_store->ongoing_log_count());
  EXPECT_EQ(1u, alternate_ongoing_log_store_ptr->size());
}

TEST_P(MetricsServiceTestWithFeatures,
       OngoingLogFlushedAfterInitialLogWhenUserLogStoreSet) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();

  MetricsLogStore* test_log_store = service.LogStoreForTest();
  std::unique_ptr<TestUnsentLogStore> alternate_ongoing_log_store =
      InitializeTestLogStoreAndGet();

  // Init state.
  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(0u, test_log_store->ongoing_log_count());

  // Run pending tasks to finish init task and complete the first ongoing log.
  task_runner_->RunPendingTasks();
  ASSERT_EQ(TestMetricsService::SENDING_LOGS, service.state());
  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(1u, test_log_store->ongoing_log_count());

  // User log store set post-init.
  service.SetUserLogStore(std::move(alternate_ongoing_log_store));

  // Another log should have been flushed from setting the user log store.
  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(2u, test_log_store->ongoing_log_count());
}

TEST_P(MetricsServiceTestWithFeatures,
       OngoingLogDiscardedAfterEarlyUnsetUserLogStore) {
  EnableMetricsReporting();
  TestMetricsServiceClient client;
  TestMetricsService service(GetMetricsStateManager(), &client,
                             GetLocalState());

  service.InitializeMetricsRecordingState();
  // Start() will create the first ongoing log.
  service.Start();
  ASSERT_EQ(TestMetricsService::INIT_TASK_SCHEDULED, service.state());

  MetricsLogStore* test_log_store = service.LogStoreForTest();
  std::unique_ptr<TestUnsentLogStore> alternate_ongoing_log_store =
      InitializeTestLogStoreAndGet();

  ASSERT_EQ(0u, test_log_store->initial_log_count());
  ASSERT_EQ(0u, test_log_store->ongoing_log_count());

  service.SetUserLogStore(std::move(alternate_ongoing_log_store));

  // Unset the user log store before we started sending logs.
  base::UmaHistogramBoolean("Test.Before.Histogram", true);
  service.UnsetUserLogStore();
  base::UmaHistogramBoolean("Test.After.Histogram", true);

  // Verify that the current log was discarded.
  EXPECT_FALSE(service.GetCurrentLogForTest());

  // Verify that histograms from before unsetting the user log store were
  // flushed.
  EXPECT_EQ(0, GetHistogramDeltaTotalCount("Test.Before.Histogram"));
  EXPECT_EQ(1, GetHistogramDeltaTotalCount("Test.After.Histogram"));

  // Clean up histograms.
  base::StatisticsRecorder::ForgetHistogramForTesting("Test.Before.Histogram");
  base::StatisticsRecorder::ForgetHistogramForTesting("Test.After.Histogram");
}
#endif  // BUILDFLAG(IS_CHROMEOS_LACROS)

}  // namespace metrics
