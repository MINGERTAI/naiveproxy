// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/pki/cert_error_params.h"

#include <memory>

#include "base/check.h"
#include "base/strings/string_number_conversions.h"
#include "net/der/input.h"

namespace net {

namespace {

// Parameters subclass for describing (and pretty-printing) 1 or 2 DER
// blobs. It makes a copy of the der::Inputs.
class CertErrorParams2Der : public CertErrorParams {
 public:
  CertErrorParams2Der(const char* name1,
                      const der::Input& der1,
                      const char* name2,
                      const der::Input& der2)
      : name1_(name1),
        der1_(der1.AsString()),
        name2_(name2),
        der2_(der2.AsString()) {}

  CertErrorParams2Der(const CertErrorParams2Der&) = delete;
  CertErrorParams2Der& operator=(const CertErrorParams2Der&) = delete;

  std::string ToDebugString() const override {
    std::string result;
    AppendDer(name1_, der1_, &result);
    if (name2_) {
      result += "\n";
      AppendDer(name2_, der2_, &result);
    }
    return result;
  }

 private:
  static void AppendDer(const char* name,
                        const std::string& der,
                        std::string* out) {
    *out += name;
    *out += ": " + base::HexEncode(der.data(), der.size());
  }

  const char* name1_;
  std::string der1_;

  const char* name2_;
  std::string der2_;
};

// Parameters subclass for describing (and pretty-printing) a single size_t.
class CertErrorParams1SizeT : public CertErrorParams {
 public:
  CertErrorParams1SizeT(const char* name, size_t value)
      : name_(name), value_(value) {}

  CertErrorParams1SizeT(const CertErrorParams1SizeT&) = delete;
  CertErrorParams1SizeT& operator=(const CertErrorParams1SizeT&) = delete;

  std::string ToDebugString() const override {
    return name_ + std::string(": ") + base::NumberToString(value_);
  }

 private:
  const char* name_;
  size_t value_;
};

// Parameters subclass for describing (and pretty-printing) two size_t
// values.
class CertErrorParams2SizeT : public CertErrorParams {
 public:
  CertErrorParams2SizeT(const char* name1,
                        size_t value1,
                        const char* name2,
                        size_t value2)
      : name1_(name1), value1_(value1), name2_(name2), value2_(value2) {}

  CertErrorParams2SizeT(const CertErrorParams2SizeT&) = delete;
  CertErrorParams2SizeT& operator=(const CertErrorParams2SizeT&) = delete;

  std::string ToDebugString() const override {
    return name1_ + std::string(": ") + base::NumberToString(value1_) + "\n" +
           name2_ + std::string(": ") + base::NumberToString(value2_);
  }

 private:
  const char* name1_;
  size_t value1_;
  const char* name2_;
  size_t value2_;
};

}  // namespace

CertErrorParams::CertErrorParams() = default;
CertErrorParams::~CertErrorParams() = default;

std::unique_ptr<CertErrorParams> CreateCertErrorParams1Der(
    const char* name,
    const der::Input& der) {
  DCHECK(name);
  return std::make_unique<CertErrorParams2Der>(name, der, nullptr,
                                               der::Input());
}

std::unique_ptr<CertErrorParams> CreateCertErrorParams2Der(
    const char* name1,
    const der::Input& der1,
    const char* name2,
    const der::Input& der2) {
  DCHECK(name1);
  DCHECK(name2);
  return std::make_unique<CertErrorParams2Der>(name1, der1, name2, der2);
}

std::unique_ptr<CertErrorParams> CreateCertErrorParams1SizeT(const char* name,
                                                             size_t value) {
  DCHECK(name);
  return std::make_unique<CertErrorParams1SizeT>(name, value);
}

NET_EXPORT std::unique_ptr<CertErrorParams> CreateCertErrorParams2SizeT(
    const char* name1,
    size_t value1,
    const char* name2,
    size_t value2) {
  DCHECK(name1);
  DCHECK(name2);
  return std::make_unique<CertErrorParams2SizeT>(name1, value1, name2, value2);
}

}  // namespace net
