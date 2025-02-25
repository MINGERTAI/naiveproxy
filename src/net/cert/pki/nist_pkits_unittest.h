// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_CERT_PKI_NIST_PKITS_UNITTEST_H_
#define NET_CERT_PKI_NIST_PKITS_UNITTEST_H_

#include <set>

#include "net/cert/pki/test_helpers.h"
#include "net/der/parse_values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

// Describes the inputs and outputs (other than the certificates) for
// the PKITS tests.
struct PkitsTestInfo {
  // Default construction results in the "default settings".
  PkitsTestInfo();
  PkitsTestInfo(const PkitsTestInfo& other);
  ~PkitsTestInfo();

  // Sets |initial_policy_set| to the specified policies. The
  // policies are described as comma-separated symbolic strings like
  // "anyPolicy" and "NIST-test-policy-1".
  //
  // If this isn't called, the default is "anyPolicy".
  void SetInitialPolicySet(const char* const policy_names);

  // Sets |user_constrained_policy_set| to the specified policies. The
  // policies are described as comma-separated symbolic strings like
  // "anyPolicy" and "NIST-test-policy-1".
  //
  // If this isn't called, the default is "NIST-test-policy-1".
  void SetUserConstrainedPolicySet(const char* const policy_names);

  void SetInitialExplicitPolicy(bool b);
  void SetInitialPolicyMappingInhibit(bool b);
  void SetInitialInhibitAnyPolicy(bool b);

  // ----------------
  // Info
  // ----------------

  // The PKITS test number. For example, "4.1.1".
  const char* test_number = nullptr;

  // ----------------
  // Inputs
  // ----------------

  // A set of policy OIDs to use for "initial-policy-set".
  std::set<der::Input> initial_policy_set;

  // The value of "initial-explicit-policy".
  InitialExplicitPolicy initial_explicit_policy = InitialExplicitPolicy::kFalse;

  // The value of "initial-policy-mapping-inhibit".
  InitialPolicyMappingInhibit initial_policy_mapping_inhibit =
      InitialPolicyMappingInhibit::kFalse;

  // The value of "initial-inhibit-any-policy".
  InitialAnyPolicyInhibit initial_inhibit_any_policy =
      InitialAnyPolicyInhibit::kFalse;

  // This is the time when PKITS was published.
  der::GeneralizedTime time = {2011, 4, 15, 0, 0, 0};

  // ----------------
  // Expected outputs
  // ----------------

  // Whether path validation should succeed.
  bool should_validate = false;

  std::set<der::Input> user_constrained_policy_set;
};

// Parameterized test class for PKITS tests.
// The instantiating code should define a PkitsTestDelegate with an appropriate
// static RunTest method, and then INSTANTIATE_TYPED_TEST_SUITE_P for each
// testcase (each TYPED_TEST_SUITE_P in pkits_testcases-inl.h).
template <typename PkitsTestDelegate>
class PkitsTest : public ::testing::Test {
 public:
  template <size_t num_certs, size_t num_crls>
  void RunTest(const char* const (&cert_names)[num_certs],
               const char* const (&crl_names)[num_crls],
               const PkitsTestInfo& info) {
    std::vector<std::string> cert_ders;
    for (const std::string& s : cert_names)
      cert_ders.push_back(net::ReadTestFileToString(
          "net/third_party/nist-pkits/certs/" + s + ".crt"));
    std::vector<std::string> crl_ders;
    for (const std::string& s : crl_names)
      crl_ders.push_back(net::ReadTestFileToString(
          "net/third_party/nist-pkits/crls/" + s + ".crl"));

    base::StringPiece test_number = info.test_number;

    // Some of the PKITS tests are intentionally given different expectations
    // from PKITS.pdf.
    //
    // Empty user_constrained_policy_set due to short-circuit on invalid
    // signatures:
    //
    //   4.1.2 - Invalid CA Signature Test2
    //   4.1.3 - Invalid EE Signature Test3
    //   4.1.6 - Invalid DSA Signature Test6
    //
    // Expected to fail because DSA signatures are not supported:
    //
    //   4.1.4 - Valid DSA Signatures Test4
    //   4.1.5 - Valid DSA Parameter Inheritance Test5
    //
    // Expected to fail because Name constraints on rfc822Names are not
    // supported:
    //
    //   4.13.21 - Valid RFC822 nameConstraints Test21
    //   4.13.23 - Valid RFC822 nameConstraints Test23
    //   4.13.25 - Valid RFC822 nameConstraints Test25
    //   4.13.27 - Valid DN and RFC822 nameConstraints Test27
    //
    // Expected to fail because Name constraints on
    // uniformResourceIdentifiers are not supported:
    //
    //   4.13.34 - Valid URI nameConstraints Test34
    //   4.13.36 - Valid URI nameConstraints Test36
    if (test_number == "4.1.2" || test_number == "4.1.3" ||
        test_number == "4.1.6") {
      PkitsTestInfo modified_info = info;
      modified_info.user_constrained_policy_set = {};
      PkitsTestDelegate::RunTest(cert_ders, crl_ders, modified_info);
    } else if (test_number == "4.1.4" || test_number == "4.1.5") {
      PkitsTestInfo modified_info = info;
      modified_info.user_constrained_policy_set = {};
      modified_info.should_validate = false;
      PkitsTestDelegate::RunTest(cert_ders, crl_ders, modified_info);
    } else if (test_number == "4.13.21" || test_number == "4.13.23" ||
               test_number == "4.13.25" || test_number == "4.13.27" ||
               test_number == "4.13.34" || test_number == "4.13.36") {
      PkitsTestInfo modified_info = info;
      modified_info.should_validate = false;
      PkitsTestDelegate::RunTest(cert_ders, crl_ders, modified_info);
    } else {
      PkitsTestDelegate::RunTest(cert_ders, crl_ders, info);
    }
  }
};

// Inline the generated test code:
#include "net/third_party/nist-pkits/pkits_testcases-inl.h"

}  // namespace net

#endif  // NET_CERT_PKI_NIST_PKITS_UNITTEST_H_
