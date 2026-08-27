/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_synthetic_fixture.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "xenia/cpu/guest_invocation_runner.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {

TEST_CASE("synthetic invocation fixture closes each supported host granule",
          "[guest-invocation-synthetic-fixture]") {
  for (uint32_t host_page_size : {4096u, 8192u, 16384u, 65536u}) {
    INFO("host page size " << host_page_size);
    SyntheticGuestInvocationFixture fixture;
    std::string error;
    REQUIRE(BuildSyntheticGuestInvocationFixture(host_page_size, 123, &fixture,
                                                 &error));
    REQUIRE(error.empty());
    REQUIRE(fixture.host_page_size == host_page_size);
    REQUIRE(fixture.captured_host_code_size == 123);

    const size_t closure_page_count = host_page_size / JitCorpus::kPageSize;
    REQUIRE(fixture.corpus.page_addresses().size() == closure_page_count);
    REQUIRE(fixture.corpus.functions().size() == 1);
    REQUIRE(fixture.corpus.functions().front().address ==
            SyntheticGuestInvocationFixture::kCodeAddress);
    REQUIRE(fixture.corpus.functions().front().end_address ==
            SyntheticGuestInvocationFixture::kCodeAddress + 12);
    REQUIRE(fixture.corpus.functions().front().host_code_size == 123);

    REQUIRE(fixture.valid_invocation.input_data_pages.size() ==
            closure_page_count);
    REQUIRE(fixture.valid_invocation.expected_dirty_pages.size() == 1);
    REQUIRE(fixture.valid_invocation.input_data_pages.front().data[3] == 41);
    REQUIRE(fixture.valid_invocation.expected_dirty_pages.front().data[3] ==
            42);

    GuestInvocationReplayPlan plan;
    REQUIRE(BuildGuestInvocationReplayPlan(fixture.valid_invocation,
                                           fixture.corpus, host_page_size,
                                           &plan, &error));
    REQUIRE(plan.supplied_page_addresses.size() == closure_page_count * 2);
    REQUIRE(plan.reset_page_addresses.size() == closure_page_count);
    REQUIRE(plan.protection_granules.size() == 2);

    REQUIRE(fixture.omitted_page_fault_invocation.input_data_pages.empty());
    REQUIRE(fixture.omitted_page_fault_invocation.input.gpr[3] ==
            SyntheticGuestInvocationFixture::kDataAddress);
    REQUIRE(BuildGuestInvocationReplayPlan(
        fixture.omitted_page_fault_invocation, fixture.corpus, host_page_size,
        &plan, &error));
    REQUIRE(plan.supplied_page_addresses.size() == closure_page_count);
    REQUIRE(plan.reset_page_addresses.empty());

    REQUIRE(fixture.address_7f_fault_invocation.input_data_pages.empty());
    REQUIRE(fixture.address_7f_fault_invocation.input.gpr[3] ==
            SyntheticGuestInvocationFixture::kAddress7F);
    REQUIRE(BuildGuestInvocationReplayPlan(fixture.address_7f_fault_invocation,
                                           fixture.corpus, host_page_size,
                                           &plan, &error));
    REQUIRE(plan.supplied_page_addresses.size() == closure_page_count);
    REQUIRE(plan.reset_page_addresses.empty());

    ppc::GuestInvocationArtifact artifact;
    artifact.capture_build_sha256.fill(1);
    artifact.code_corpus_sha256.fill(2);
    artifact.replay_config_sha256.fill(3);
    artifact.invocations = {fixture.valid_invocation};
    std::vector<uint8_t> encoded_artifact;
    REQUIRE(ppc::GuestInvocationArtifactCodec::Encode(
        artifact, &encoded_artifact, &error));
    ppc::GuestInvocationArtifact decoded_artifact;
    REQUIRE(ppc::GuestInvocationArtifactCodec::Decode(
        encoded_artifact, &decoded_artifact, &error));
    REQUIRE(decoded_artifact == artifact);

    artifact.invocations = {fixture.omitted_page_fault_invocation};
    REQUIRE(ppc::GuestInvocationArtifactCodec::Encode(
        artifact, &encoded_artifact, &error));
    REQUIRE(ppc::GuestInvocationArtifactCodec::Decode(
        encoded_artifact, &decoded_artifact, &error));
    REQUIRE(decoded_artifact == artifact);

    artifact.invocations = {fixture.address_7f_fault_invocation};
    REQUIRE(ppc::GuestInvocationArtifactCodec::Encode(
        artifact, &encoded_artifact, &error));
    REQUIRE(ppc::GuestInvocationArtifactCodec::Decode(
        encoded_artifact, &decoded_artifact, &error));
    REQUIRE(decoded_artifact == artifact);
  }
}

TEST_CASE("synthetic invocation fixture rejects unsupported host granules",
          "[guest-invocation-synthetic-fixture]") {
  SyntheticGuestInvocationFixture fixture;
  fixture.host_page_size = 123;
  std::string error;
  REQUIRE_FALSE(
      BuildSyntheticGuestInvocationFixture(8193, 1, &fixture, &error));
  REQUIRE(error == "synthetic fixture host page size is unsupported");
  REQUIRE(fixture.host_page_size == 0);

  REQUIRE_FALSE(BuildSyntheticGuestInvocationFixture(4096, 1, nullptr, &error));
  REQUIRE(error == "synthetic fixture output is null");

  REQUIRE_FALSE(
      BuildSyntheticGuestInvocationFixture(4096, 0, &fixture, &error));
  REQUIRE(error == "synthetic fixture is missing the captured host code size");
  REQUIRE(fixture.host_page_size == 0);
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
