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

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <logwrap/logwrap.h>
#include <odrefresh/odrefresh.h>

#include "CertUtils.h"
#include "FakeCompOs.h"
#include "KeystoreKey.h"
#include "VerityUtils.h"

#include "odsign_info.pb.h"

using android::base::ErrnoError;
using android::base::Error;
using android::base::Result;
using android::base::SetProperty;

using OdsignInfo = ::odsign::proto::OdsignInfo;

const std::string kSigningKeyCert = "/data/misc/odsign/key.cert";
const std::string kOdsignInfo = "/data/misc/odsign/odsign.info";
const std::string kOdsignInfoSignature = "/data/misc/odsign/odsign.info.signature";

const std::string kArtArtifactsDir = "/data/misc/apexdata/com.android.art/dalvik-cache";

static const char* kOdrefreshPath = "/apex/com.android.art/bin/odrefresh";

static const char* kFsVerityProcPath = "/proc/sys/fs/verity";

static const bool kForceCompilation = false;
static const bool kUseCompOs = false;  // STOPSHIP if true

static const char* kVirtApexPath = "/apex/com.android.virt";
const std::string kCompOsCert = "/data/misc/odsign/compos_key.cert";
const std::string kCompOsPublicKey = "/data/misc/odsign/compos_key.pubkey";
const std::string kCompOsKeyBlob = "/data/misc/odsign/compos_key.blob";
const std::string kCompOsPendingDir = "/data/misc/apexdata/com.android.art/compos-pending";

static const char* kOdsignVerificationDoneProp = "odsign.verification.done";
static const char* kOdsignKeyDoneProp = "odsign.key.done";

static const char* kOdsignVerificationStatusProp = "odsign.verification.success";
static const char* kOdsignVerificationStatusValid = "1";
static const char* kOdsignVerificationStatusError = "0";

static void writeBytesToFile(const std::vector<uint8_t>& bytes, const std::string& path) {
    std::string str(bytes.begin(), bytes.end());
    android::base::WriteStringToFile(str, path);
}

static std::vector<uint8_t> readBytesFromFile(const std::string& path) {
    std::string str;
    android::base::ReadFileToString(path, &str);
    return std::vector<uint8_t>(str.begin(), str.end());
}

static int removeDirectory(const std::string& directory) {
    std::error_code ec;
    auto num_removed = std::filesystem::remove_all(directory, ec);
    if (ec) {
        LOG(ERROR) << "Can't remove " << directory << ": " << ec.message();
        return 0;
    } else {
        if (num_removed > 0) {
            LOG(INFO) << "Removed " << num_removed << " entries from " << directory;
        }
        return num_removed;
    }
}

static bool directoryHasContent(const std::string& directory) {
    std::error_code ec;
    return std::filesystem::is_directory(directory, ec) &&
           !std::filesystem::is_empty(directory, ec);
}

art::odrefresh::ExitCode compileArtifacts(bool force) {
    const char* const argv[] = {kOdrefreshPath, force ? "--force-compile" : "--compile"};
    const int exit_code =
        logwrap_fork_execvp(arraysize(argv), argv, nullptr, false, LOG_ALOG, false, nullptr);
    return static_cast<art::odrefresh::ExitCode>(exit_code);
}

art::odrefresh::ExitCode checkArtifacts() {
    const char* const argv[] = {kOdrefreshPath, "--check"};
    const int exit_code =
        logwrap_fork_execvp(arraysize(argv), argv, nullptr, false, LOG_ALOG, false, nullptr);
    return static_cast<art::odrefresh::ExitCode>(exit_code);
}

static std::string toHex(const std::vector<uint8_t>& digest) {
    std::stringstream ss;
    for (auto it = digest.begin(); it != digest.end(); ++it) {
        ss << std::setfill('0') << std::setw(2) << std::hex << static_cast<unsigned>(*it);
    }
    return ss.str();
}

bool compOsPresent() {
    return access(kVirtApexPath, F_OK) == 0;
}

Result<void> verifyExistingRootCert(const SigningKey& key) {
    if (access(kSigningKeyCert.c_str(), F_OK) < 0) {
        return ErrnoError() << "Key certificate not found: " << kSigningKeyCert;
    }
    auto trustedPublicKey = key.getPublicKey();
    if (!trustedPublicKey.ok()) {
        return Error() << "Failed to retrieve signing public key: " << trustedPublicKey.error();
    }

    auto publicKeyFromExistingCert = extractPublicKeyFromX509(kSigningKeyCert);
    if (!publicKeyFromExistingCert.ok()) {
        return publicKeyFromExistingCert.error();
    }
    if (publicKeyFromExistingCert.value() != trustedPublicKey.value()) {
        return Error() << "Public key of existing certificate at " << kSigningKeyCert
                       << " does not match signing public key.";
    }

    // At this point, we know the cert is for our key; it's unimportant whether it's
    // actually self-signed.
    return {};
}

Result<void> createX509RootCert(const SigningKey& key, const std::string& outPath) {
    auto publicKey = key.getPublicKey();

    if (!publicKey.ok()) {
        return publicKey.error();
    }

    auto keySignFunction = [&](const std::string& to_be_signed) { return key.sign(to_be_signed); };
    return createSelfSignedCertificate(*publicKey, keySignFunction, outPath);
}

Result<std::vector<uint8_t>> extractPublicKeyFromLeafCert(const SigningKey& key,
                                                          const std::string& certPath,
                                                          const std::string& expectedCn) {
    if (access(certPath.c_str(), F_OK) < 0) {
        return ErrnoError() << "Certificate not found: " << certPath;
    }
    auto trustedPublicKey = key.getPublicKey();
    if (!trustedPublicKey.ok()) {
        return Error() << "Failed to retrieve signing public key: " << trustedPublicKey.error();
    }

    auto existingCertInfo = verifyAndExtractCertInfoFromX509(certPath, trustedPublicKey.value());
    if (!existingCertInfo.ok()) {
        return Error() << "Failed to verify certificate at " << certPath << ": "
                       << existingCertInfo.error();
    }

    auto& actualCn = existingCertInfo.value().subjectCn;
    if (actualCn != expectedCn) {
        return Error() << "CN of existing certificate at " << certPath << " is " << actualCn
                       << ", should be " << expectedCn;
    }

    return existingCertInfo.value().subjectKey;
}

Result<std::vector<uint8_t>> verifyOrGenerateCompOsKey(const SigningKey& signingKey) {
    auto compOsStatus = FakeCompOs::newInstance();
    if (!compOsStatus.ok()) {
        return Error() << "Failed to start CompOs: " << compOsStatus.error();
    }

    FakeCompOs* compOs = compOsStatus.value().get();

    std::vector<uint8_t> keyBlob;
    std::vector<uint8_t> publicKey;
    bool haveKey = false;

    if (access(kCompOsPublicKey.c_str(), F_OK) == 0 && access(kCompOsKeyBlob.c_str(), F_OK) == 0) {
        // We have a purported key, but not a valid signature for it.
        // If compOs can verify it, we can sign it now.
        keyBlob = readBytesFromFile(kCompOsKeyBlob);
        publicKey = readBytesFromFile(kCompOsPublicKey);

        auto response = compOs->loadAndVerifyKey(keyBlob, publicKey);
        if (response.ok()) {
            LOG(INFO) << "Verified existing CompOs key";
            haveKey = true;
        } else {
            LOG(WARNING) << "Failed to verify existing CompOs key: " << response.error();
        }
    }

    if (!haveKey) {
        // If we don't have a key, or it doesn't verify, then we need a new one.
        auto keyData = compOs->generateKey();
        if (!keyData.ok()) {
            return Error() << "Failed to generate key: " << keyData.error();
        }
        auto publicKeyStatus = extractPublicKeyFromX509(keyData.value().cert);
        if (!publicKeyStatus.ok()) {
            return Error() << "Failed to extract CompOs public key" << publicKeyStatus.error();
        }

        keyBlob = std::move(keyData.value().blob);
        publicKey = std::move(publicKeyStatus.value());

        writeBytesToFile(keyBlob, kCompOsKeyBlob);
        writeBytesToFile(publicKey, kCompOsPublicKey);
    }

    auto signFunction = [&](const std::string& to_be_signed) {
        return signingKey.sign(to_be_signed);
    };
    auto certStatus = createLeafCertificate(kCompOsSubject, publicKey, signFunction,
                                            kSigningKeyCert, kCompOsCert);
    if (!certStatus.ok()) {
        return Error() << "Failed to create CompOs cert: " << certStatus.error();
    }

    return publicKey;
}

Result<std::map<std::string, std::string>> computeDigests(const std::string& path) {
    std::error_code ec;
    std::map<std::string, std::string> digests;

    auto it = std::filesystem::recursive_directory_iterator(path, ec);
    auto end = std::filesystem::recursive_directory_iterator();

    while (!ec && it != end) {
        if (it->is_regular_file()) {
            auto digest = createDigest(it->path());
            if (!digest.ok()) {
                return Error() << "Failed to compute digest for " << it->path() << ": "
                               << digest.error();
            }
            digests[it->path()] = toHex(*digest);
        }
        ++it;
    }
    if (ec) {
        return Error() << "Failed to iterate " << path << ": " << ec;
    }

    return digests;
}

Result<void> verifyDigests(const std::map<std::string, std::string>& digests,
                           const std::map<std::string, std::string>& trusted_digests) {
    for (const auto& path_digest : digests) {
        auto path = path_digest.first;
        auto digest = path_digest.second;
        if ((trusted_digests.count(path) == 0)) {
            return Error() << "Couldn't find digest for " << path;
        }
        if (trusted_digests.at(path) != digest) {
            return Error() << "Digest mismatch for " << path;
        }
    }

    // All digests matched!
    if (digests.size() > 0) {
        LOG(INFO) << "All root hashes match.";
    }
    return {};
}

Result<void> verifyIntegrityFsVerity(const std::map<std::string, std::string>& trusted_digests) {
    // Just verify that the files are in verity, and get their digests
    auto result = verifyAllFilesInVerity(kArtArtifactsDir);
    if (!result.ok()) {
        return result.error();
    }

    return verifyDigests(*result, trusted_digests);
}

Result<void> verifyIntegrityNoFsVerity(const std::map<std::string, std::string>& trusted_digests) {
    // On these devices, just compute the digests, and verify they match the ones we trust
    auto result = computeDigests(kArtArtifactsDir);
    if (!result.ok()) {
        return result.error();
    }

    return verifyDigests(*result, trusted_digests);
}

Result<OdsignInfo> getOdsignInfo(const SigningKey& key) {
    std::string persistedSignature;
    OdsignInfo odsignInfo;

    if (!android::base::ReadFileToString(kOdsignInfoSignature, &persistedSignature)) {
        return ErrnoError() << "Failed to read " << kOdsignInfoSignature;
    }

    std::fstream odsign_info(kOdsignInfo, std::ios::in | std::ios::binary);
    if (!odsign_info) {
        return Error() << "Failed to open " << kOdsignInfo;
    }
    odsign_info.seekg(0);
    // Verify the hash
    std::string odsign_info_str((std::istreambuf_iterator<char>(odsign_info)),
                                std::istreambuf_iterator<char>());

    auto publicKey = key.getPublicKey();
    auto signResult = verifySignature(odsign_info_str, persistedSignature, *publicKey);
    if (!signResult.ok()) {
        return Error() << kOdsignInfoSignature << " does not match.";
    } else {
        LOG(INFO) << kOdsignInfoSignature << " matches.";
    }

    odsign_info.seekg(0);
    if (!odsignInfo.ParseFromIstream(&odsign_info)) {
        return Error() << "Failed to parse " << kOdsignInfo;
    }

    LOG(INFO) << "Loaded " << kOdsignInfo;
    return odsignInfo;
}

Result<void> persistDigests(const std::map<std::string, std::string>& digests,
                            const SigningKey& key) {
    OdsignInfo signInfo;
    google::protobuf::Map<std::string, std::string> proto_hashes(digests.begin(), digests.end());
    auto map = signInfo.mutable_file_hashes();
    *map = proto_hashes;

    std::fstream odsign_info(kOdsignInfo,
                             std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary);
    if (!signInfo.SerializeToOstream(&odsign_info)) {
        return Error() << "Failed to persist root hashes in " << kOdsignInfo;
    }

    // Sign the signatures with our key itself, and write that to storage
    odsign_info.seekg(0, std::ios::beg);
    std::string odsign_info_str((std::istreambuf_iterator<char>(odsign_info)),
                                std::istreambuf_iterator<char>());
    auto signResult = key.sign(odsign_info_str);
    if (!signResult.ok()) {
        return Error() << "Failed to sign " << kOdsignInfo;
    }
    android::base::WriteStringToFile(*signResult, kOdsignInfoSignature);
    return {};
}

static Result<void> verifyArtifacts(const SigningKey& key, bool supportsFsVerity) {
    auto signInfo = getOdsignInfo(key);
    // Tell init we're done with the key; this is a boot time optimization
    // in particular for the no fs-verity case, where we need to do a
    // costly verification. If the files haven't been tampered with, which
    // should be the common path, the verification will succeed, and we won't
    // need the key anymore. If it turns out the artifacts are invalid (eg not
    // in fs-verity) or the hash doesn't match, we won't be able to generate
    // new artifacts without the key, so in those cases, remove the artifacts,
    // and use JIT zygote for the current boot. We should recover automatically
    // by the next boot.
    SetProperty(kOdsignKeyDoneProp, "1");
    if (!signInfo.ok()) {
        return signInfo.error();
    }
    std::map<std::string, std::string> trusted_digests(signInfo->file_hashes().begin(),
                                                       signInfo->file_hashes().end());
    Result<void> integrityStatus;

    if (supportsFsVerity) {
        integrityStatus = verifyIntegrityFsVerity(trusted_digests);
    } else {
        integrityStatus = verifyIntegrityNoFsVerity(trusted_digests);
    }
    if (!integrityStatus.ok()) {
        return integrityStatus.error();
    }

    return {};
}

Result<std::vector<uint8_t>> addCompOsCertToFsVerityKeyring(const SigningKey& signingKey) {
    std::vector<uint8_t> compos_key;
    auto existing_key =
        extractPublicKeyFromLeafCert(signingKey, kCompOsCert, kCompOsSubject.commonName);
    if (existing_key.ok()) {
        LOG(INFO) << "Found and verified existing CompOs public key certificate: " << kCompOsCert;
        compos_key = std::move(existing_key.value());
    } else {
        LOG(WARNING) << existing_key.error();

        auto new_key = verifyOrGenerateCompOsKey(signingKey);
        if (!new_key.ok()) {
            return new_key.error();
        } else {
            LOG(INFO) << "Generated new CompOs public key certificate";
            compos_key = std::move(new_key.value());
        }
    }

    auto cert_add_result = addCertToFsVerityKeyring(kCompOsCert, "fsv_compos");
    if (!cert_add_result.ok()) {
        // Best efforts only - nothing we can do if deletion fails.
        unlink(kCompOsCert.c_str());
        return Error() << "Failed to add CompOs certificate to fs-verity keyring: "
                       << cert_add_result.error();
    }

    return compos_key;
}

art::odrefresh::ExitCode checkCompOsPendingArtifacts(const std::vector<uint8_t>& compos_key,
                                                     const SigningKey& signingKey,
                                                     bool* digests_verified) {
    if (!directoryHasContent(kCompOsPendingDir)) {
        return art::odrefresh::ExitCode::kCompilationRequired;
    }

    // CompOs has generated some artifacts that may, or may not, match the
    // current state.  But if there are already valid artifacts present the
    // CompOs ones are redundant.
    art::odrefresh::ExitCode odrefresh_status = checkArtifacts();
    if (odrefresh_status != art::odrefresh::ExitCode::kCompilationRequired) {
        if (odrefresh_status == art::odrefresh::ExitCode::kOkay) {
            LOG(INFO) << "Current artifacts are OK, deleting pending artifacts";
            removeDirectory(kCompOsPendingDir);
        }
        return odrefresh_status;
    }

    // No useful current artifacts, lets see if the CompOs ones are ok
    LOG(INFO) << "Current artifacts are out of date, switching to pending artifacts";
    removeDirectory(kArtArtifactsDir);
    std::error_code ec;
    std::filesystem::rename(kCompOsPendingDir, kArtArtifactsDir, ec);
    if (ec) {
        LOG(ERROR) << "Can't rename " << kCompOsPendingDir << " to " << kArtArtifactsDir << ": "
                   << ec.message();
        removeDirectory(kCompOsPendingDir);
        return art::odrefresh::ExitCode::kCompilationRequired;
    }
    // TODO: Make sure that we check here that the contents of the artifacts
    // correspond to their filenames (and extensions) - the CompOs signatures
    // can't guarantee that.
    odrefresh_status = checkArtifacts();
    if (odrefresh_status != art::odrefresh::ExitCode::kOkay) {
        LOG(WARNING) << "Pending artifacts are not OK";
        return odrefresh_status;
    }

    // The artifacts appear to be up to date - but we haven't
    // verified that they are genuine yet.
    Result<std::map<std::string, std::string>> digests =
        verifyAllFilesUsingCompOs(kArtArtifactsDir, compos_key);

    if (digests.ok()) {
        auto persisted = persistDigests(digests.value(), signingKey);

        // Having signed the digests (or failed to), we're done with the signing key.
        SetProperty(kOdsignKeyDoneProp, "1");

        if (persisted.ok()) {
            *digests_verified = true;
            LOG(INFO) << "Pending artifacts successfully verified.";
            return art::odrefresh::ExitCode::kOkay;
        } else {
            LOG(WARNING) << persisted.error();
        }
    } else {
        LOG(WARNING) << "Pending artifact verification failed: " << digests.error();
    }

    // We can't use the existing artifacts, so we will need to generate new
    // ones.
    removeDirectory(kArtArtifactsDir);
    return art::odrefresh::ExitCode::kCompilationRequired;
}

int main(int /* argc */, char** /* argv */) {
    auto errorScopeGuard = []() {
        // In case we hit any error, remove the artifacts and tell Zygote not to use anything
        removeDirectory(kArtArtifactsDir);
        removeDirectory(kCompOsPendingDir);
        // Tell init we don't need to use our key anymore
        SetProperty(kOdsignKeyDoneProp, "1");
        // Tell init we're done with verification, and that it was an error
        SetProperty(kOdsignVerificationDoneProp, "1");
        SetProperty(kOdsignVerificationStatusProp, kOdsignVerificationStatusError);
    };
    auto scope_guard = android::base::make_scope_guard(errorScopeGuard);

    if (!android::base::GetBoolProperty("ro.apex.updatable", false)) {
        LOG(INFO) << "Device doesn't support updatable APEX, exiting.";
        return 0;
    }

    auto keystoreResult = KeystoreKey::getInstance();
    if (!keystoreResult.ok()) {
        LOG(ERROR) << "Could not create keystore key: " << keystoreResult.error();
        return -1;
    }
    SigningKey* key = keystoreResult.value();

    bool supportsFsVerity = access(kFsVerityProcPath, F_OK) == 0;
    if (!supportsFsVerity) {
        LOG(INFO) << "Device doesn't support fsverity. Falling back to full verification.";
    }

    bool supportsCompOs = kUseCompOs && supportsFsVerity && compOsPresent();

    if (supportsFsVerity) {
        auto existing_cert = verifyExistingRootCert(*key);
        if (!existing_cert.ok()) {
            LOG(WARNING) << existing_cert.error();

            // Try to create a new cert
            auto new_cert = createX509RootCert(*key, kSigningKeyCert);
            if (!new_cert.ok()) {
                LOG(ERROR) << "Failed to create X509 certificate: " << new_cert.error();
                // TODO apparently the key become invalid - delete the blob / cert
                return -1;
            }
        } else {
            LOG(INFO) << "Found and verified existing public key certificate: " << kSigningKeyCert;
        }
        auto cert_add_result = addCertToFsVerityKeyring(kSigningKeyCert, "fsv_ods");
        if (!cert_add_result.ok()) {
            LOG(ERROR) << "Failed to add certificate to fs-verity keyring: "
                       << cert_add_result.error();
            return -1;
        }
    }

    art::odrefresh::ExitCode odrefresh_status = art::odrefresh::ExitCode::kCompilationRequired;
    bool digests_verified = false;

    if (supportsCompOs) {
        auto compos_key = addCompOsCertToFsVerityKeyring(*key);
        if (!compos_key.ok()) {
            LOG(ERROR) << compos_key.error();
        } else {
            odrefresh_status =
                checkCompOsPendingArtifacts(compos_key.value(), *key, &digests_verified);
        }
    }

    if (odrefresh_status == art::odrefresh::ExitCode::kCompilationRequired) {
        odrefresh_status = compileArtifacts(kForceCompilation);
    }
    if (odrefresh_status == art::odrefresh::ExitCode::kOkay) {
        LOG(INFO) << "odrefresh said artifacts are VALID";
        if (!digests_verified) {
            // A post-condition of validating artifacts is that if the ones on /system
            // are used, kArtArtifactsDir is removed. Conversely, if kArtArtifactsDir
            // exists, those are artifacts that will be used, and we should verify them.
            int err = access(kArtArtifactsDir.c_str(), F_OK);
            // If we receive any error other than ENOENT, be suspicious
            bool artifactsPresent = (err == 0) || (err < 0 && errno != ENOENT);
            if (artifactsPresent) {
                auto verificationResult = verifyArtifacts(*key, supportsFsVerity);
                if (!verificationResult.ok()) {
                    LOG(ERROR) << verificationResult.error();
                    return -1;
                }
            }
        }
    } else if (odrefresh_status == art::odrefresh::ExitCode::kCompilationSuccess ||
               odrefresh_status == art::odrefresh::ExitCode::kCompilationFailed) {
        const bool compiled_all = odrefresh_status == art::odrefresh::ExitCode::kCompilationSuccess;
        LOG(INFO) << "odrefresh compiled " << (compiled_all ? "all" : "partial")
                  << " artifacts, returned " << odrefresh_status;
        Result<std::map<std::string, std::string>> digests;
        if (supportsFsVerity) {
            digests = addFilesToVerityRecursive(kArtArtifactsDir, *key);
        } else {
            // If we can't use verity, just compute the root hashes and store
            // those, so we can reverify them at the next boot.
            digests = computeDigests(kArtArtifactsDir);
        }
        if (!digests.ok()) {
            LOG(ERROR) << digests.error();
            return -1;
        }
        auto persistStatus = persistDigests(*digests, *key);
        if (!persistStatus.ok()) {
            LOG(ERROR) << persistStatus.error();
            return -1;
        }
    } else if (odrefresh_status == art::odrefresh::ExitCode::kCleanupFailed) {
        LOG(ERROR) << "odrefresh failed cleaning up existing artifacts";
        return -1;
    } else {
        LOG(ERROR) << "odrefresh exited unexpectedly, returned " << odrefresh_status;
        return -1;
    }

    LOG(INFO) << "On-device signing done.";

    scope_guard.Disable();
    // At this point, we're done with the key for sure
    SetProperty(kOdsignKeyDoneProp, "1");
    // And we did a successful verification
    SetProperty(kOdsignVerificationDoneProp, "1");
    SetProperty(kOdsignVerificationStatusProp, kOdsignVerificationStatusValid);

    return 0;
}
