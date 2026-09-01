// zk_stubs_ios.cpp
// Stub implementations for mobile NodeCore builds.
//
// Provides no-op stubs for subsystems not available on iOS:
// - dinero::zk (confidential tx, stealth addresses) — needs secp256k1-zkp
// - dinero::gpu (GPU mining) — no GPU mining on mobile
// - bulletproofs FFI (bp_*) — Rust Dalek library not built for iOS
//
// All undefined symbols found via: nm -u libnodecore-*.a | grep -E '(dinero::(zk|gpu)|_bp_)'

#if defined(IOS_BUILD) || defined(ANDROID_NODECORE_BUILD)

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>

// =============================================================================
// Bulletproofs FFI stubs (C linkage)
// =============================================================================

extern "C" {

int bp_init(void) { return 0; }
int bp_is_initialized(void) { return 0; }

int bp_generate(uint64_t, const uint8_t*, uint8_t*, size_t*) { return -1; }

int bp_verify(const uint8_t*, const uint8_t*, size_t) { return -1; }

int bp_verify_batch(const uint8_t**, const uint8_t**, const size_t*, size_t) { return -1; }

int bp_generate_with_nonce(uint64_t, const uint8_t*, const uint8_t*, uint8_t*, size_t*) { return -1; }

int bp_rewind(const uint8_t*, const uint8_t*, size_t, const uint8_t*, uint64_t*, uint8_t*) { return -1; }

size_t bp_max_proof_size(size_t) { return 0; }
const char* bp_version(void) { return "stub-mobile"; }

int commitment_add(const uint8_t*, const uint8_t*, uint8_t*) { return -1; }
int commitment_sub(const uint8_t*, const uint8_t*, uint8_t*) { return -1; }
int commitment_from_value(uint64_t, uint8_t*) { return -1; }
int commitment_create(uint64_t, const uint8_t*, uint8_t*) { return -1; }
int commitment_is_identity(const uint8_t*) { return -1; }
int generate_random_blinding(uint8_t*) { return -1; }

} // extern "C"

// =============================================================================
// dinero::zk stubs
// =============================================================================

namespace dinero {
namespace zk {

using BlindingFactor = std::array<uint8_t, 32>;
using CommitmentBytes = std::array<uint8_t, 33>;
using PrivateKey = std::array<uint8_t, 32>;
using PublicKey = std::array<uint8_t, 33>;
using SharedSecret = std::array<uint8_t, 32>;

struct PedersenCommitment {
    uint8_t commitment_data[64];
    uint64_t value;
    BlindingFactor blinding_factor;
    CommitmentBytes Serialize() const;
    bool Deserialize(const CommitmentBytes&);
};

struct RangeProof {
    std::vector<uint8_t> proof;
    uint64_t min_value;
    uint64_t max_value;
    std::array<uint8_t, 32> nonce;
};

struct ConfidentialInput {
    PedersenCommitment commitment;
    BlindingFactor blinding_factor;
};

struct ConfidentialOutput {
    PedersenCommitment commitment;
    RangeProof range_proof;
    CommitmentBytes ephemeral_key;
};

struct StealthKeyPair {
    PrivateKey view_private;
    PublicKey view_public;
    PrivateKey spend_private;
    PublicKey spend_public;
};

struct StealthAddress {
    PublicKey view_public;
    PublicKey spend_public;
    std::string Encode() const;
    bool Decode(const std::string&);
    bool IsValid() const;
};

struct EphemeralKey {
    PrivateKey secret;
    PublicKey public_key;
};

struct StealthOutput {
    PublicKey destination;
    PublicKey ephemeral_public;
};

enum class ZKError { OK = 0 };
template<typename T> struct ZKResult {
    T value; ZKError error; std::string error_message;
    ZKResult() : error(ZKError::OK) {}
};

class Secp256k1Context {
public:
    Secp256k1Context();
    ~Secp256k1Context();
};

class ConfidentialTxBuilder {
public:
    ConfidentialTxBuilder();
    ~ConfidentialTxBuilder();
    bool AddInput(uint64_t, const BlindingFactor&);
    ZKResult<BlindingFactor> AddOutput(uint64_t, const BlindingFactor* = nullptr);
    bool BalanceBlindingFactors();
    bool GenerateRangeProofs();
    bool VerifyRangeProofs() const;
    bool VerifyBalance() const;
    uint64_t GetTotalInputValue() const;
    uint64_t GetTotalOutputValue() const;
    const std::vector<ConfidentialInput>& GetInputs() const;
    const std::vector<ConfidentialOutput>& GetOutputs() const;
    bool RewindRangeProof(const ConfidentialOutput&, const uint8_t[32], uint64_t*, BlindingFactor*) const;
    void Clear();
private:
    std::unique_ptr<Secp256k1Context> ctx_;
    std::vector<ConfidentialInput> inputs_;
    std::vector<ConfidentialOutput> outputs_;
};

class ConfidentialTxValidator {
public:
    ConfidentialTxValidator();
    ~ConfidentialTxValidator();
    bool Verify(const std::vector<ConfidentialInput>&, const std::vector<ConfidentialOutput>&);
    bool VerifyRangeProof(const ConfidentialOutput&);
    bool VerifyCommitmentBalance(const std::vector<ConfidentialInput>&, const std::vector<ConfidentialOutput>&);
private:
    std::unique_ptr<Secp256k1Context> ctx_;
};

class StealthAddressGenerator {
public:
    StealthAddressGenerator();
    ~StealthAddressGenerator();
    StealthKeyPair GenerateKeyPair();
    StealthKeyPair DeriveKeyPair(const PrivateKey&, uint32_t = 0, uint32_t = 0);
    StealthAddress CreateAddress(const StealthKeyPair&);
    EphemeralKey GenerateEphemeralKey();
    StealthOutput ComputeStealthOutput(const EphemeralKey&, const StealthAddress&);
    std::optional<SharedSecret> CheckOwnership(const StealthOutput&, const StealthKeyPair&);
    PrivateKey DeriveSpendingKey(const SharedSecret&, const StealthKeyPair&);
    SharedSecret ComputeSharedSecret(const EphemeralKey&, const PublicKey&);
    PrivateKey HashToScalar(const SharedSecret&);
};

// util namespace
namespace util {
std::string PublicKeyToHex(const PublicKey&);
bool PublicKeyFromHex(const std::string&, PublicKey&);
std::string PrivateKeyToHex(const PrivateKey&);
bool PrivateKeyFromHex(const std::string&, PrivateKey&);
}

// --- Implementations ---

Secp256k1Context::Secp256k1Context() {}
Secp256k1Context::~Secp256k1Context() {}

CommitmentBytes PedersenCommitment::Serialize() const { return {}; }
bool PedersenCommitment::Deserialize(const CommitmentBytes&) { return false; }

ConfidentialTxBuilder::ConfidentialTxBuilder() {}
ConfidentialTxBuilder::~ConfidentialTxBuilder() {}
bool ConfidentialTxBuilder::AddInput(uint64_t, const BlindingFactor&) { return false; }
ZKResult<BlindingFactor> ConfidentialTxBuilder::AddOutput(uint64_t, const BlindingFactor*) { return {}; }
bool ConfidentialTxBuilder::BalanceBlindingFactors() { return false; }
bool ConfidentialTxBuilder::GenerateRangeProofs() { return false; }
bool ConfidentialTxBuilder::VerifyRangeProofs() const { return false; }
bool ConfidentialTxBuilder::VerifyBalance() const { return false; }
uint64_t ConfidentialTxBuilder::GetTotalInputValue() const { return 0; }
uint64_t ConfidentialTxBuilder::GetTotalOutputValue() const { return 0; }
const std::vector<ConfidentialInput>& ConfidentialTxBuilder::GetInputs() const { return inputs_; }
const std::vector<ConfidentialOutput>& ConfidentialTxBuilder::GetOutputs() const { return outputs_; }
bool ConfidentialTxBuilder::RewindRangeProof(const ConfidentialOutput&, const uint8_t[32], uint64_t*, BlindingFactor*) const { return false; }
void ConfidentialTxBuilder::Clear() {}

ConfidentialTxValidator::ConfidentialTxValidator() {}
ConfidentialTxValidator::~ConfidentialTxValidator() {}
bool ConfidentialTxValidator::Verify(const std::vector<ConfidentialInput>&, const std::vector<ConfidentialOutput>&) { return false; }
bool ConfidentialTxValidator::VerifyRangeProof(const ConfidentialOutput&) { return false; }
bool ConfidentialTxValidator::VerifyCommitmentBalance(const std::vector<ConfidentialInput>&, const std::vector<ConfidentialOutput>&) { return false; }

std::string StealthAddress::Encode() const { return ""; }
bool StealthAddress::Decode(const std::string&) { return false; }
bool StealthAddress::IsValid() const { return false; }

StealthAddressGenerator::StealthAddressGenerator() {}
StealthAddressGenerator::~StealthAddressGenerator() {}
StealthKeyPair StealthAddressGenerator::GenerateKeyPair() { return {}; }
StealthKeyPair StealthAddressGenerator::DeriveKeyPair(const PrivateKey&, uint32_t, uint32_t) { return {}; }
StealthAddress StealthAddressGenerator::CreateAddress(const StealthKeyPair&) { return {}; }
EphemeralKey StealthAddressGenerator::GenerateEphemeralKey() { return {}; }
StealthOutput StealthAddressGenerator::ComputeStealthOutput(const EphemeralKey&, const StealthAddress&) { return {}; }
std::optional<SharedSecret> StealthAddressGenerator::CheckOwnership(const StealthOutput&, const StealthKeyPair&) { return std::nullopt; }
PrivateKey StealthAddressGenerator::DeriveSpendingKey(const SharedSecret&, const StealthKeyPair&) { return {}; }
SharedSecret StealthAddressGenerator::ComputeSharedSecret(const EphemeralKey&, const PublicKey&) { return {}; }
PrivateKey StealthAddressGenerator::HashToScalar(const SharedSecret&) { return {}; }

namespace util {
std::string PublicKeyToHex(const PublicKey&) { return ""; }
bool PublicKeyFromHex(const std::string&, PublicKey&) { return false; }
std::string PrivateKeyToHex(const PrivateKey&) { return ""; }
bool PrivateKeyFromHex(const std::string&, PrivateKey&) { return false; }
}

} // namespace zk

// =============================================================================
// dinero::gpu stubs
// =============================================================================

namespace gpu {

enum class BackendType { NONE, OPENCL, CUDA, METAL };

struct GPUDevice {
    BackendType backend;
    uint32_t device_id;
    std::string name;
    std::string vendor;
    size_t global_memory_mb;
    uint32_t compute_units;
    uint32_t max_clock_mhz;
    bool available;
    GPUDevice() : backend(BackendType::NONE), device_id(0), global_memory_mb(0),
                  compute_units(0), max_clock_mhz(0), available(false) {}
};

class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;
};

std::unique_ptr<IComputeBackend> createBackend(BackendType) { return nullptr; }

class GPUDeviceManager {
public:
    GPUDeviceManager();
    ~GPUDeviceManager();
    std::vector<GPUDevice> detectAllDevices();
    BackendType getBestAvailableBackend() const;
    bool hasGPU() const;
    size_t getDeviceCount() const;
    GPUDevice getDevice(uint32_t) const;
private:
    std::vector<GPUDevice> detected_devices_;
};

GPUDeviceManager::GPUDeviceManager() {}
GPUDeviceManager::~GPUDeviceManager() {}
std::vector<GPUDevice> GPUDeviceManager::detectAllDevices() { return {}; }
BackendType GPUDeviceManager::getBestAvailableBackend() const { return BackendType::NONE; }
bool GPUDeviceManager::hasGPU() const { return false; }
size_t GPUDeviceManager::getDeviceCount() const { return 0; }
GPUDevice GPUDeviceManager::getDevice(uint32_t) const { return {}; }

} // namespace gpu

// =============================================================================
// dinero::hw stubs (hardware wallet — needs hidapi, not available on iOS)
// =============================================================================

namespace hw {

enum class DeviceType {
    LEDGER_NANO_S, LEDGER_NANO_X, LEDGER_NANO_S_PLUS,
    TREZOR_ONE, TREZOR_MODEL_T,
    COLDCARD_MK3, COLDCARD_MK4,
    KEEPKEY, BITBOX02, KEYSTONE, PASSPORT,
    AIRGAP_VAULT, GENERIC_PSBT, GENERIC_USB, UNKNOWN
};

enum class TransportType { USB, BLUETOOTH, NFC, FILE_SYSTEM };

struct DeviceCapabilities {
    bool supports_taproot = false;
    bool supports_psbt_v2 = false;
    bool supports_multisig = false;
};

struct DeviceInfo {
    DeviceType type = DeviceType::UNKNOWN;
    TransportType transport = TransportType::FILE_SYSTEM;
    std::string device_id;
    std::string manufacturer;
    std::string model;
    std::string serial_number;
    std::string firmware_version;
    bool initialized = false;
    bool bootloader_mode = false;
    bool pin_cached = false;
    DeviceCapabilities capabilities;
};

template<typename T>
struct HWResult {
    bool success = false;
    T value;
    std::string error_message;
    int error_code = 0;
    static HWResult<T> Err(const std::string& msg, int code = -1) {
        HWResult<T> r; r.success = false; r.error_message = msg; r.error_code = code; return r;
    }
};

using ProgressCallback = std::function<void(int, const std::string&)>;
using PinCallback = std::function<std::string()>;
using PassphraseCallback = std::function<std::string()>;
using ConfirmCallback = std::function<bool(const std::string&)>;

struct PSBT {
    std::vector<uint8_t> data;
};

class IHardwareWallet {
public:
    virtual ~IHardwareWallet() = default;
    virtual HWResult<std::vector<DeviceInfo>> EnumerateDevices() = 0;
    virtual HWResult<bool> Connect(const std::string& device_id = "") = 0;
    virtual HWResult<bool> Disconnect() = 0;
    virtual HWResult<DeviceInfo> GetDeviceInfo() = 0;
    virtual bool IsConnected() const = 0;
    virtual HWResult<PSBT> SignPSBT(const PSBT&, const std::vector<std::string>& = {}, ProgressCallback = {}) = 0;
    virtual HWResult<bool> DisplayAddress(const std::string&, const std::string&) = 0;
    virtual HWResult<std::string> GetPublicKey(const std::string&) = 0;
    virtual HWResult<uint32_t> GetMasterFingerprint() = 0;
    virtual void SetPinCallback(PinCallback callback) { pin_callback_ = callback; }
    virtual void SetPassphraseCallback(PassphraseCallback callback) { passphrase_callback_ = callback; }
    virtual void SetConfirmCallback(ConfirmCallback callback) { confirm_callback_ = callback; }
protected:
    PinCallback pin_callback_;
    PassphraseCallback passphrase_callback_;
    ConfirmCallback confirm_callback_;
};

class USBHardwareWallet : public IHardwareWallet {
public:
    USBHardwareWallet(DeviceType type);
    HWResult<std::vector<DeviceInfo>> EnumerateDevices() override;
    HWResult<bool> Connect(const std::string& = "") override;
    HWResult<bool> Disconnect() override;
    HWResult<DeviceInfo> GetDeviceInfo() override;
    bool IsConnected() const override { return false; }
    HWResult<PSBT> SignPSBT(const PSBT&, const std::vector<std::string>& = {}, ProgressCallback = {}) override;
    HWResult<bool> DisplayAddress(const std::string&, const std::string&) override;
    HWResult<std::string> GetPublicKey(const std::string&) override;
    HWResult<uint32_t> GetMasterFingerprint() override;
private:
    DeviceType device_type_;
    bool connected_ = false;
    DeviceInfo device_info_;
};

USBHardwareWallet::USBHardwareWallet(DeviceType type) : device_type_(type) {}
HWResult<std::vector<DeviceInfo>> USBHardwareWallet::EnumerateDevices() { return HWResult<std::vector<DeviceInfo>>::Err("Not available on iOS"); }
HWResult<bool> USBHardwareWallet::Connect(const std::string&) { return HWResult<bool>::Err("Not available on iOS"); }
HWResult<bool> USBHardwareWallet::Disconnect() { return HWResult<bool>::Err("Not available on iOS"); }
HWResult<DeviceInfo> USBHardwareWallet::GetDeviceInfo() { return HWResult<DeviceInfo>::Err("Not available on iOS"); }
HWResult<PSBT> USBHardwareWallet::SignPSBT(const PSBT&, const std::vector<std::string>&, ProgressCallback) { return HWResult<PSBT>::Err("Not available on iOS"); }
HWResult<bool> USBHardwareWallet::DisplayAddress(const std::string&, const std::string&) { return HWResult<bool>::Err("Not available on iOS"); }
HWResult<std::string> USBHardwareWallet::GetPublicKey(const std::string&) { return HWResult<std::string>::Err("Not available on iOS"); }
HWResult<uint32_t> USBHardwareWallet::GetMasterFingerprint() { return HWResult<uint32_t>::Err("Not available on iOS"); }

} // namespace hw
} // namespace dinero

// Forward declaration — DaemonContext is in the global namespace
class DaemonContext;

// =============================================================================
// dinero::grpc_server stubs (gRPC wallet server — not needed on iOS)
// =============================================================================

namespace dinero {
namespace grpc_server {

class SocketWalletServer {
public:
    explicit SocketWalletServer(DaemonContext*, const std::string& = "127.0.0.1:50051");
    ~SocketWalletServer();
    bool Start();
    void Stop();
    bool IsRunning() const { return false; }
    std::string GetAddress() const { return m_address; }
private:
    std::string m_address;
};

SocketWalletServer::SocketWalletServer(DaemonContext*, const std::string& address) : m_address(address) {}
SocketWalletServer::~SocketWalletServer() {}
bool SocketWalletServer::Start() { return false; }
void SocketWalletServer::Stop() {}

} // namespace grpc_server

} // namespace dinero

#endif // IOS_BUILD || ANDROID_NODECORE_BUILD
