// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// This file is included inside namespace net by the platform trust adapter.
class MozillaSystemTrustStore final : public SystemTrustStore {
 public:
  MozillaSystemTrustStore() {
    CertificateList certificates = X509Certificate::CreateCertificateListFromBytes(
        base::as_byte_span(kUefiMozillaRootCertificates),
        X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
    CHECK_EQ(certificates.size(), kUefiMozillaRootCertificateCount);
    for (const auto& certificate : certificates) {
      bssl::CertErrors errors;
      auto parsed = x509_util::CreateParsedCertificate(
          bssl::UpRef(certificate->cert_buffer()), &errors);
      CHECK(parsed) << errors.ToDebugString();
      roots_.AddTrustAnchorWithConstraints(std::move(parsed));
    }
  }
  bssl::TrustStore* GetTrustStore() override { return &roots_; }
  bool IsKnownRoot(const bssl::ParsedCertificate* certificate) const override {
    return roots_.Contains(certificate);
  }
  bool IsKnownMtcAnchor(const bssl::MTCAnchor*) const override { return false; }
  PlatformTrustStore* GetPlatformTrustStore() override { return nullptr; }
  bool IsLocallyTrustedRoot(const bssl::ParsedCertificate*) override {
    return false;
  }
  int64_t chrome_root_store_version() const override { return 0; }
  std::optional<base::Time> signer_set_timestamp() const override {
    return std::nullopt;
  }
  std::optional<base::Time> mtc_metadata_update_time() const override {
    return std::nullopt;
  }
  base::span<const ChromeRootCertConstraints> GetChromeRootConstraints(
      const bssl::CertPathBuilderResultPath*) const override { return {}; }
  const TrustStoreChrome::MtcAnchorExtraData* GetMTCAnchorData(
      base::span<const uint8_t>) const override { return nullptr; }
  std::optional<bssl::VerifyCertificateChainDelegate::MTCCosigner>
  GetMtcMirrorKey(base::span<const uint8_t>) const override {
    return std::nullopt;
  }
  bool IsMtcCosignerPolicySatisfied(
      const bssl::ParsedCertificate&, base::Time, const bssl::MTCAnchor*,
      base::span<const std::vector<uint8_t>>,
      const NetLogWithSource&) const override { return false; }
  std::optional<int32_t> GetCrsRootIdForCert(
      const bssl::CertPathBuilderResultPath*) const override {
    return std::nullopt;
  }
  bssl::TrustStore* eutl_trust_store() override { return &empty_eutl_; }
 private:
  // Both stores are immutable after construction. Only certificates from the
  // bundled Mozilla CA input are trusted; no MTC or EUTL anchors are added.
  bssl::TrustStoreInMemory roots_;
  bssl::TrustStoreInMemory empty_eutl_;
};

std::unique_ptr<SystemTrustStore> CreateSslSystemTrustStoreChromeRoot(
    std::unique_ptr<TrustStoreChrome>) {
  // The interface name is upstream API; its Chrome root input is deliberately
  // excluded from this build's Mozilla trust-anchor policy.
  return std::make_unique<MozillaSystemTrustStore>();
}
