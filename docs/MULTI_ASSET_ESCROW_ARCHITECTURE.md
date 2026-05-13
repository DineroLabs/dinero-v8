# Multi-Asset Escrow Architecture Diagram

## Current System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          DineroCoin Escrow System                            │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────┐         ┌──────────────────────────────────────┐
│   ESCROW SUBSYSTEM          │         │   BRIDGE/ROUTING SUBSYSTEM           │
│   (Single-Asset DIN)        │         │   (Multi-Asset Conversions)          │
│                             │         │                                      │
│  ┌─────────────────────┐    │         │  ┌──────────────────────────┐        │
│  │ EscrowContract      │    │         │  │ FiatBridgeProvider       │        │
│  │ ├─ contract_id      │    │         │  │ (Abstract Base)          │        │
│  │ ├─ keys: EscrowKeys │    │         │  │ ├─ convert()             │        │
│  │ ├─ amount (DIN)     │    │         │  │ ├─ get_rate()            │        │
│  │ ├─ redeem_script    │    │         │  │ └─ name()                │        │
│  │ ├─ p2sh_address     │    │         │  └──────────────────────────┘        │
│  │ ├─ lock_txid        │    │         │           ▲                          │
│  │ └─ status           │    │         │           │                          │
│  └─────────────────────┘    │         │  ┌────────┴─────────┬──────────┐    │
│                             │         │  │                  │          │    │
│  ┌─────────────────────┐    │         │  ▼                  ▼          ▼    │
│  │ ContractRegistry    │    │         │ DexProvider    HybridProvider Custodial│
│  │ ├─ storeContract()  │    │         │ (Li.Fi, etc)   (SimpleSwap)  (Coinbase)
│  │ ├─ getContract()    │    │         │                                      │
│  │ └─ listContracts()  │    │         │  ┌──────────────────────────┐        │
│  └─────────────────────┘    │         │  │ FiatBridgeManager        │        │
│                             │         │  │ ├─ register_provider()   │        │
│  ┌─────────────────────┐    │         │  │ ├─ convert()             │        │
│  │ EscrowManager       │    │         │  │ ├─ get_rate()            │        │
│  │ ├─ createEscrow()   │    │         │  │ └─ get_all_routes()      │        │
│  │ ├─ releaseEscrow()  │    │         │  └──────────────────────────┘        │
│  │ └─ refundEscrow()   │    │         │           ▲                          │
│  └─────────────────────┘    │         │           │                          │
│                             │         │  ┌────────┴──────────┐               │
│  ┌─────────────────────┐    │         │  │                   │               │
│  │EscrowContractBuilder│    │         │  ▼                   ▼               │
│  │├─buildContract()    │    │         │ RouteHop      ConversionRoute       │
│  │├─buildRedeemScript()│    │         │ ├─from_asset  ├─hops[]             │
│  │├─createP2SHAddress()│    │         │ ├─to_asset    ├─total_rate         │
│  │└─createReleaseRx()  │    │         │ ├─rate        └─effective_rate()   │
│  └─────────────────────┘    │         │ └─provider                          │
│                             │         │                                      │
│  Bitcoin Script (P2SH):     │         │  ┌──────────────────────────┐        │
│  ┌─────────────────────┐    │         │  │ RoutingEngine            │        │
│  │ IF (release path):  │    │         │  │ ├─find_best_route()      │        │
│  │   2-of-3 multisig   │    │         │  │ ├─find_all_routes()      │        │
│  │ ELSE (refund path): │    │         │  │ └─build_rate_graph()     │        │
│  │   timelock + 1-of-1 │    │         │  └──────────────────────────┘        │
│  │ ENDIF               │    │         │  (Dijkstra pathfinding)              │
│  └─────────────────────┘    │         │                                      │
└─────────────────────────────┘         └──────────────────────────────────────┘

                                 │
                                 ├────────► RPC Methods
                                 │
                                 ├─ contract.createescrow
                                 ├─ contract.status
                                 ├─ contract.release
                                 ├─ contract.refund
                                 ├─ contract.list
                                 ├─ bridge.getrate
                                 ├─ bridge.convert
                                 └─ bridge.providers
```

---

## Data Flow: Creating Multi-Asset Escrow

```
┌─────────┐
│  User   │
└────┬────┘
     │ RPC: multiasset.createescrow
     │ params: buyer_pk, seller_pk, asset="USDT", amount=100
     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ RPC Handler: multiasset_createescrow_impl()                                │
│                                                                            │
│  1. Parse parameters                                                       │
│  2. Validate asset_id in FiatBridgeManager                                │
│  3. Call MultiAssetEscrowBuilder::buildMultiAssetContract()               │
└───────────┬──────────────────────────────────────────────────────────────┘
            │
            ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ MultiAssetEscrowBuilder::buildMultiAssetContract()                          │
│                                                                            │
│  1. Create EscrowKeys from public keys                                     │
│  2. Extend EscrowContractBuilder for asset type:                          │
│     - Script includes asset metadata                                       │
│  3. Build redeem script (IF/ELSE branches)                                │
│  4. Hash script → P2SH address                                            │
│  5. Create AssetEscrowContract:                                           │
│     {                                                                      │
│       contract_id: "contract_...",                                        │
│       asset_id: "USDT",                                                   │
│       amount: 100.0,                                                      │
│       p2sh_address: "din1q...",                                           │
│       redeem_script: "6352...",                                           │
│       status: "pending"                                                   │
│     }                                                                      │
└───────────┬──────────────────────────────────────────────────────────────┘
            │
            ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ MultiAssetContractRegistry::storeContract()                                 │
│                                                                            │
│  Stores in map:                                                           │
│  contracts_by_asset["USDT"][contract_id] = contract                      │
│                                                                            │
│  Returns: {"contract_id": "...", "p2sh_address": "...", ...}             │
└───────────┬──────────────────────────────────────────────────────────────┘
            │
            ▼
         [Return to user]
         ┌─────────────┐
         │ User sends  │
         │ 100 USDT to │
         │ P2SH address│
         └─────────────┘
```

---

## Data Flow: Releasing with Automatic Conversion

```
┌──────────┐
│  Seller  │
└────┬─────┘
     │ RPC: multiasset.releasetoasset
     │ params: escrow_id, buyer_addr, target_asset="EUR"
     ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ RPC Handler: multiasset_releasetoasset_impl()                              │
│                                                                            │
│  1. Load contract from registry                                            │
│  2. Verify signatures (buyer + seller)                                    │
│  3. Call BridgedEscrowManager::releaseWithConversion()                    │
└───────────┬──────────────────────────────────────────────────────────────┘
            │
            ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ BridgedEscrowManager::releaseWithConversion()                              │
│                                                                            │
│  Input: escrow_id, buyer_addr, target_asset="EUR"                        │
│  Escrow holds: USDT                                                       │
│                                                                            │
│  1. Spend USDT from P2SH to temporary address                            │
│  2. Query RoutingEngine for best route:                                   │
│     RoutingEngine::find_best_route("USDT", "EUR", providers)             │
│  3. Returns ConversionRoute:                                              │
│     {                                                                      │
│       hops: [                                                              │
│         {from: "USDT", to: "BTC", rate: 0.000023, provider: "dex"},      │
│         {from: "BTC", to: "EUR", rate: 35000, provider: "coinbase"}      │
│       ],                                                                   │
│       total_rate: 0.805,     # USDT to EUR                               │
│       total_fee_bps: 300,    # 3% total fees                             │
│       description: "USDT→BTC→EUR via dex+coinbase"                       │
│     }                                                                      │
└───────────┬──────────────────────────────────────────────────────────────┘
            │
            ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ FiatBridgeManager::convert()                                                │
│                                                                            │
│  For each hop in ConversionRoute:                                         │
│                                                                            │
│  Hop 1: USDT → BTC (via DEX)                                             │
│  └─→ DexProvider::convert()                                               │
│      ├─ Execute swap: 100 USDT → 0.0023 BTC                             │
│      └─ Returns: {txid: "tx1...", received: 0.0023, rate: 0.000023}      │
│                                                                            │
│  Hop 2: BTC → EUR (via Coinbase)                                         │
│  └─→ CustodialProvider::convert()                                         │
│      ├─ Send 0.0023 BTC to exchange                                       │
│      ├─ Trade for EUR                                                     │
│      └─ Returns: {txid: "tx2...", received: 80.35, rate: 35000}          │
│                                                                            │
│  Result: Buyer receives 80.35 EUR (after fees)                           │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Component Relationships

```
┌─────────────────────┐
│  RPC Methods        │
│  ├─ contract.*      │
│  └─ multiasset.*    │
└────────┬────────────┘
         │ calls
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ BUILDERS & MANAGERS                                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  MultiAssetEscrowBuilder                                        │
│    ├─ extends EscrowContractBuilder                            │
│    ├─ adds asset_id tracking                                   │
│    └─ returns AssetEscrowContract                              │
│                                                                   │
│  BridgedEscrowManager                                           │
│    ├─ extends EscrowManager                                    │
│    ├─ integrates RoutingEngine                                 │
│    └─ returns ConversionResult                                 │
│                                                                   │
│  MultiAssetContractRegistry                                     │
│    ├─ extends ContractRegistry                                 │
│    ├─ indexes by asset_id                                      │
│    └─ queries getTotalLockedByAsset()                          │
└────────┬────────────────────────────────────────────────────────┘
         │ uses
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ CONVERSION INFRASTRUCTURE                                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  FiatBridgeManager                                              │
│    ├─ register_provider()                                      │
│    ├─ convert() ────────────┬──► FiatBridgeProvider[]          │
│    └─ get_rate()            │                                  │
│                             │                                  │
│  RoutingEngine                                                  │
│    ├─ find_best_route() ────┤                                  │
│    └─ find_all_routes() ────┘                                  │
│                                                                   │
│  ConversionRoute                                                │
│    ├─ hops: RouteHop[]                                         │
│    ├─ total_rate                                               │
│    ├─ total_fee_bps                                            │
│    └─ description()                                            │
└─────────────────────────────────────────────────────────────────┘
```

---

## Multi-Asset Escrow Release Sequence

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Scenario: Release USDT escrow, convert to EUR                              │
└─────────────────────────────────────────────────────────────────────────────┘

Time    Event                                       Component
────    ─────────────────────────────────────────────────────────────────────
0       User calls: multiasset.releasetoasset()    User/RPC
        └─ params: escrow_id, target_asset="EUR"

1       Load contract from registry                ContractRegistry
        └─ contract: {asset_id: "USDT", amount: 100}

2       Verify signatures                          EscrowContractBuilder
        └─ sig_buyer + sig_seller = valid

3       Query routing engine                       FiatBridgeManager
        └─ find_best_route("USDT", "EUR")

4a      Build rate graph from providers            RoutingEngine
        └─ discover USDT→BTC→EUR path

4b      Run Dijkstra pathfinding                   RoutingEngine
        └─ best_route: USDT→BTC→EUR (via dex+coinbase)
           effective_rate: 0.805 (after fees/slippage)

5       Create conversion request                  ConversionRequest
        {
          from_asset: "USDT",
          to_asset: "EUR",
          amount: 100,
          dest_address: buyer_addr
        }

6       Execute hop 1: USDT → BTC                  DexProvider
        └─ txid: "hop1_tx...", received: 0.0023 BTC

7       Execute hop 2: BTC → EUR                   CustodialProvider
        └─ txid: "hop2_tx...", received: 80.35 EUR

8       Return ConversionResult                    ConversionResult
        {
          success: true,
          txid: "hop2_tx...",
          received_amount: 80.35,
          rate: 0.805,
          provider: "dex+coinbase",
          total_fee_bps: 300
        }

9       Broadcast release transaction              BridgedEscrowManager
        └─ spend P2SH with 2-of-3 multisig

10      Update contract status                     MultiAssetContractRegistry
        └─ status: "released", release_txid: "..."

11      Return to user                             User
        {
          success: true,
          received_amount: 80.35 EUR,
          conversion_route: "USDT→BTC→EUR via dex+coinbase",
          timestamp: ...
        }
```

---

## File Dependencies

```
include/contracts/
├─ escrow_contract.h ────┐
│  (EscrowContract,      │
│   EscrowContractBuilder)
│                        │
└─ contract_registry.h ──┼──► multiasset_escrow_contract.h (NEW)
                         │    ├─ AssetEscrowContract
                         │    └─ MultiAssetEscrowBuilder
                         │
include/p2p/             │
├─ escrow_manager.h ─────┼──► multiasset_escrow_manager.h (NEW)
│  (EscrowManager)       │    ├─ BridgedEscrowManager
│  (EscrowStatus)        │    └─ releaseWithConversion()
│                        │
include/bridge/          │
├─ fiat_bridge_manager.h ┤    multiasset_contract_registry.h (NEW)
├─ fiat_bridge_provider.h├───► ├─ MultiAssetContractRegistry
├─ routing_engine.h ─────┘     └─ listByAsset()
│
include/rpc/
├─ methods_contract.h ────────► methods_multiasset.h (NEW)
│  (RPC interface)              ├─ multiasset.createescrow
│                               ├─ multiasset.releasetoasset
                                └─ multiasset.listebyasset
```

---

## State Transition Diagram: Multi-Asset Escrow

```
                    ┌──────────────┐
                    │   CREATING   │
                    │              │
                    │ ├─ Building  │
                    │ │  contract  │
                    │ ├─ Storing   │
                    │ │  in DB     │
                    │ └─ P2SH addr │
                    └──────┬───────┘
                           │
                           ▼
                    ┌──────────────┐
                    │   PENDING    │
                    │              │
                    │ Waiting for  │
                    │ funding      │
                    └──────┬───────┘
                           │
                      [User funds]
                           │
                           ▼
                    ┌──────────────┐
                    │   LOCKED     │
                    │              │
                    │ Funds on     │
                    │ blockchain   │
                    │ (confirmed)  │
                    └──┬───────────┘
                       │
        ┌──────────────┼──────────────┐
        │              │              │
        │ (happy case) │ (timeout)    │ (dispute)
        │              │              │
        ▼              ▼              ▼
    ┌─────────┐   ┌────────────┐  ┌──────────────┐
    │RELEASING│   │  EXPIRING  │  │ DISPUTING    │
    │         │   │            │  │              │
    │ With    │   │ Timelock   │  │ Mediator     │
    │ routing │   │ reached    │  │ co-signs     │
    │ engine  │   │            │  │              │
    └────┬────┘   └─────┬──────┘  └──────┬───────┘
         │              │                 │
         ▼              ▼                 ▼
    ┌─────────────────────────────────────────┐
    │  CONVERSION (if target_asset != current)│
    │                                         │
    │  1. Query routing engine                │
    │  2. Find best route                     │
    │  3. Execute conversion hops             │
    │  4. Send to final destination           │
    └───────────┬─────────────────────────────┘
                │
    ┌───────────┴──────────────┬────────────┐
    │                          │            │
    ▼                          ▼            ▼
┌───────────┐         ┌────────────┐  ┌────────────┐
│ RELEASED  │         │ REFUNDED   │  │ DISPUTED   │
│           │         │            │  │            │
│ Funds sent│         │ Buyer gets │  │ Mediator   │
│ to buyer  │         │ refund     │  │ decided    │
└───────────┘         └────────────┘  └────────────┘
```

