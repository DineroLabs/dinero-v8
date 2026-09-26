#include "consensus/orchard_resources.h"
#include "consensus/limits.h"
#include "consensus/script_validation.h"
namespace dinero::consensus {
namespace {
using Error = OrchardResourceErrorCode;
[[noreturn]] void Reject(Error code) { throw OrchardResourceError(code); }
void Charge(uint32_t &value, uint64_t extra, uint32_t limit, Error error) {
  if (value > limit || extra > limit - value)
    Reject(error);
  value += static_cast<uint32_t>(extra);
}
void Check(const OrchardResourceUsage &usage) {
  if (usage.bundles > kOrchardMaxBlockBundles)
    Reject(Error::Bundles);
  if (usage.actions > kOrchardMaxBlockActions)
    Reject(Error::Actions);
  if (usage.sigops > kOrchardMaxBlockSigops)
    Reject(Error::Sigops);
}
uint32_t InputCost(const std::vector<std::vector<uint8_t>> &witness,
                   const UTXOEntry &coin) {
  switch (DetectScriptType(coin.scriptPubKey)) {
  case ScriptType::P2PKH:
  case ScriptType::P2WPKH:
  case ScriptType::P2MR:
    return 1;
  case ScriptType::P2TR: {
    size_t count = witness.size();
    if (count >= 2 && !witness.back().empty() && witness.back()[0] == 0x50)
      --count;
    if (count == 1)
      return 1;
    if (count < 2)
      Reject(Error::InputShape);
    // Control block is last; script is penultimate after annex removal.
    return CountOrchardProfileScriptSigops(witness[count - 2], true, true);
  }
  default:
    // Unknown scripts do not pass the separate authoritative script gate.
    return 0;
  }
}
} // namespace
static_assert(kOrchardMaxBlockSigops == MAX_BLOCK_SIGOPS_COST);
static_assert(kOrchardMaxBlockActions >= orchard::kMaxActionsV1);
uint32_t CountOrchardProfileScriptSigops(std::span<const uint8_t> script,
                                         bool accurate, bool tapscript) {
  uint32_t cost = 0;
  uint8_t previous = 0xff;
  size_t offset = 0;
  while (offset < script.size()) {
    const uint8_t op = script[offset++];
    if (op <= 0x4e) {
      uint64_t length = op;
      if (op >= 0x4c) {
        const size_t width = size_t(1) << (op - 0x4c);
        if (width > script.size() - offset)
          break;
        length = 0;
        for (size_t i = 0; i < width; ++i)
          length |= uint64_t(script[offset++]) << (8 * i);
      }
      if (length > script.size() - offset)
        break;
      offset += static_cast<size_t>(length);
    } else if (op == 0xac || op == 0xad || (tapscript && op == 0xba)) {
      Charge(cost, 1, kOrchardMaxBlockSigops, Error::Sigops);
    } else if (op == 0xae || op == 0xaf) {
      const auto n = accurate && previous >= 0x51 && previous <= 0x60
                         ? previous - 0x50
                         : 20;
      Charge(cost, n, kOrchardMaxBlockSigops, Error::Sigops);
    }
    previous = op;
  }
  return cost;
}
void AccumulateOrchardTransactionResources(const ParsedTransaction &parsed,
                                           OrchardResourceUsage &usage) {
  Check(usage);
  auto next = usage;
  const auto script = [&](const std::vector<uint8_t> &bytes, bool accurate) {
    Charge(next.sigops, CountOrchardProfileScriptSigops(bytes, accurate, false),
           kOrchardMaxBlockSigops, Error::Sigops);
  };
  if (parsed.IsOrchard()) {
    const auto &tx = parsed.Orchard();
    Charge(next.bundles, 1, kOrchardMaxBlockBundles, Error::Bundles);
    Charge(next.actions, tx.UnverifiedFacts().action_count,
           kOrchardMaxBlockActions, Error::Actions);
    for (const auto &out : tx.Outputs())
      script(out.script_pub_key, true);
  } else {
    const auto &tx = parsed.Historical();
    for (const auto &in : tx.vin)
      script(in.scriptSig, false);
    for (const auto &out : tx.vout)
      script(out.scriptPubKey, true);
  }
  usage = next;
}
void AccumulateOrchardInputResources(const ParsedTransaction &parsed,
                                     std::span<const UTXOEntry> coins,
                                     OrchardResourceUsage &usage) {
  Check(usage);
  auto next = usage;
  const auto inputs = parsed.IsOrchard() ? parsed.Orchard().Inputs().size()
                                         : parsed.Historical().vin.size();
  if (coins.size() != inputs ||
      (!parsed.IsOrchard() && parsed.Historical().IsCoinbase()))
    Reject(Error::InputShape);
  for (size_t i = 0; i < inputs; ++i) {
    const auto &witness = parsed.IsOrchard()
                              ? parsed.Orchard().Inputs()[i].witness
                              : parsed.Historical().vin[i].witness;
    Charge(next.sigops, InputCost(witness, coins[i]), kOrchardMaxBlockSigops,
           Error::Sigops);
  }
  usage = next;
}
} // namespace dinero::consensus
