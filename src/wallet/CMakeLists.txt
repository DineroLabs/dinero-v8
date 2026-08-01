# Dinero wallet library.
#
# This target owns user wallet state and signing logic. It intentionally links
# SQLite for user data and does not link RocksDB directly.

# Core wallet library (real implementations only)
# NOTE: src/wallet/hd_wallet.cpp is LEGACY and intentionally NOT compiled.
#       Active implementation: src/wallet/hd_wallet.cpp (uses BIP32Deriver)
add_library(dinero_wallet STATIC
  ${CMAKE_SOURCE_DIR}/src/common/logger.cpp         # Logger used by wallet code
  ${CMAKE_CURRENT_SOURCE_DIR}/address.cpp
  ${CMAKE_SOURCE_DIR}/src/common/AddressCodec.cpp  # Taproot address encoding (P2TR)
  ${CMAKE_SOURCE_DIR}/src/address/addr_codec.cpp    # Taproot address decoding and scriptPubKey creation
  ${CMAKE_SOURCE_DIR}/src/address/addr_types.cpp    # Address type definitions
  ${CMAKE_CURRENT_SOURCE_DIR}/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
  ${CMAKE_CURRENT_SOURCE_DIR}/key_identity.cpp    # Week 1: KeyID-based wallet identity
  ${CMAKE_CURRENT_SOURCE_DIR}/key_origin.cpp      # Week 1: KeyOriginInfo for descriptor wallet
  ${CMAKE_CURRENT_SOURCE_DIR}/script_ownership.cpp # Week 1: IsMine script ownership
  ${CMAKE_CURRENT_SOURCE_DIR}/hd_wallet.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/bip32_deriver.cpp   # BIP32 derivation helper (deduplicates lambdas)
  ${CMAKE_CURRENT_SOURCE_DIR}/shielded_derivation.cpp  # Phase 5 Wave 1: Sapling-shape sk → ask/nsk → ak/nk → ivk
  ${CMAKE_CURRENT_SOURCE_DIR}/bip39.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/key_store_impl.cpp  # Phase 3A: BIP32 HD keystore (replaces HdKeyStoreAdapter)
  ${CMAKE_CURRENT_SOURCE_DIR}/psbt.cpp
  ${CMAKE_SOURCE_DIR}/src/core/wallet/psbt.cpp    # PSBTv2 helpers (din:: namespace, add_global_output_count etc.)
  ${CMAKE_CURRENT_SOURCE_DIR}/psbt_signer.cpp  # PSBT signing with BIP86 guardrails
  ${CMAKE_CURRENT_SOURCE_DIR}/psbt_taproot_validator.cpp  # BIP86 PSBT guardrails (key-path only validation)
  ${CMAKE_CURRENT_SOURCE_DIR}/descriptor_checksum.cpp  # Phase 1: Bitcoin-compatible descriptor checksums
  ${CMAKE_CURRENT_SOURCE_DIR}/bip84_descriptor.cpp     # BIP84 descriptor factory (P2WPKH)
  ${CMAKE_CURRENT_SOURCE_DIR}/bip86_descriptor.cpp     # BIP86 descriptor factory (P2TR Taproot)
  ${CMAKE_CURRENT_SOURCE_DIR}/descriptor_activation.cpp  # Phase 2 Step 2: Descriptor activation gate (security-critical)
  ${CMAKE_CURRENT_SOURCE_DIR}/descriptor_psbt.cpp      # Phase 2 Step 3: Descriptor-based PSBT creation
  ${CMAKE_CURRENT_SOURCE_DIR}/descriptor_store.cpp     # Phase 3C: Descriptor persistence (storage + selection layer ONLY)
  # Phase W.1.1: Rescan logic moved to wallet_worker.cpp (reuses existing block processing)
  # src/wallet/transaction.cpp REMOVED: duplicate of src/primitives/transaction.cpp (now in dinero_consensus)
  # Wallet uses Transaction from dinero_consensus (already linked)
  ${CMAKE_CURRENT_SOURCE_DIR}/transaction_deserializer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/coin_selection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/bip143_signer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/transaction_builder.cpp  # Phase 33: Transaction building for wallet RPC
  ${CMAKE_CURRENT_SOURCE_DIR}/schnorr_signer.cpp  # BIP-340 Schnorr signatures for Taproot
  ${CMAKE_CURRENT_SOURCE_DIR}/taproot_keys.cpp  # BIP340 Schnorr key generation and signing
  ${CMAKE_CURRENT_SOURCE_DIR}/taproot_address.cpp  # BIP86 Taproot address generation (P2TR key-path)
  ${CMAKE_CURRENT_SOURCE_DIR}/taproot_tx_signer.cpp  # BIP341 Taproot transaction signing (BIP340 Schnorr)
  ${CMAKE_CURRENT_SOURCE_DIR}/intent_descriptor.cpp  # B3: Intent descriptor for SigHash v1 ext_commitment
  ${CMAKE_CURRENT_SOURCE_DIR}/policy_descriptor.cpp  # B2: PolicyDescriptor + policy_id computation
  ${CMAKE_CURRENT_SOURCE_DIR}/taproot_template_builder.cpp  # B2: Taproot template tree builder (PROTECTED/ESCROW)
  ${CMAKE_CURRENT_SOURCE_DIR}/covenant_profile.cpp  # Covenant profile-v1 construction + recovery descriptors
  ${CMAKE_CURRENT_SOURCE_DIR}/safety_profile.cpp  # B4: Safety profile management
  ${CMAKE_CURRENT_SOURCE_DIR}/protected_output_builder.cpp  # B4: Protected output construction (HDWallet + template builder)
  ${CMAKE_CURRENT_SOURCE_DIR}/protected_spend.cpp  # B4: Script-path spending for panic/recovery leaves
  ${CMAKE_CURRENT_SOURCE_DIR}/recovery_qr.cpp  # B4: Encrypted recovery QR envelope (AES-256-GCM)
  ${CMAKE_CURRENT_SOURCE_DIR}/escrow_descriptor.cpp  # B5: Escrow descriptor + trade ID computation
  ${CMAKE_CURRENT_SOURCE_DIR}/receipt_bundle.cpp  # B5: Attestor receipt bundle + Schnorr verification
  ${CMAKE_CURRENT_SOURCE_DIR}/escrow_state_machine.cpp  # B5: Escrow state machine (NEGOTIATING->FUNDED->RELEASED)
  ${CMAKE_CURRENT_SOURCE_DIR}/tx_classifier.cpp          # B7: UTXO classification + extended tx details
  ${CMAKE_CURRENT_SOURCE_DIR}/lp_router.cpp              # B6a: LP router — provider quote aggregation
  # Hardware wallet files added conditionally below (not available on iOS)
  # Trezor files conditionally added below (ENABLE_TREZOR)
  ${CMAKE_CURRENT_SOURCE_DIR}/recipient_descriptor.cpp  # DecodePaymentTarget — structured address resolution
  ${CMAKE_CURRENT_SOURCE_DIR}/wallet_sync_status.cpp  # Phase W.2.1: Truthful wallet sync UX
  ${CMAKE_CURRENT_SOURCE_DIR}/shielded_wallet_runtime.cpp  # v5 shielded note runtime + block sync (wallet-side only)
  ${CMAKE_CURRENT_SOURCE_DIR}/sync_eta_estimator.cpp  # Phase W.2.2: Honest ETA estimation
  ${CMAKE_CURRENT_SOURCE_DIR}/sync_progress_tracker.cpp  # Phase W.2.2: Phase-aware progress tracking
  ${CMAKE_CURRENT_SOURCE_DIR}/reorg_detector.cpp  # Phase W.2.4: Reorg detection and visibility
  ${CMAKE_CURRENT_SOURCE_DIR}/slow_reason_analyzer.cpp  # Phase W.2.5: Slow reason analyzer
  ${CMAKE_CURRENT_SOURCE_DIR}/rbf_cpfp_detector.cpp  # Phase W.4.2: RBF & CPFP capability detection
  ${CMAKE_CURRENT_SOURCE_DIR}/fee_bump_engine.cpp  # Phase W.4.3: Fee bump recommendation engine
  ${CMAKE_CURRENT_SOURCE_DIR}/wallet_fee_bump_hooks.cpp  # Phase W.4.5: Wallet fee bump UX hooks
  # Phase 33: Full Wallet Activation
  ${CMAKE_CURRENT_SOURCE_DIR}/wallet_features.cpp     # Wallet feature flags (all features enabled)
  # Milestone 12.2: Formal UTXO State Model (v0.12.0)
  ${CMAKE_CURRENT_SOURCE_DIR}/wallet_utxo_state.cpp    # UTXO state machine with validated transitions
  ${CMAKE_CURRENT_SOURCE_DIR}/wallet_utxo_tracker.cpp  # Per-wallet UTXO tracking with formal state model
  # Milestone 12.4: Unsigned Transaction Construction (v0.12.0)
  ${CMAKE_CURRENT_SOURCE_DIR}/unsigned_tx_builder.cpp  # Pure transaction builder (no side effects)
  # Milestone 12.5: Transaction Signing & Local Validation (v0.12.0)
  ${CMAKE_CURRENT_SOURCE_DIR}/transaction_signer.cpp   # Sign unsigned tx + local validation
  # Milestone 12.6: Wallet ↔ Mempool Submission (v0.12.0)
  ${CMAKE_CURRENT_SOURCE_DIR}/wallet_mempool_adapter.cpp  # Adapter between wallet and mempool
  # Milestone 12.7: Transaction Batching (v0.12.0 - FINAL)
  ${CMAKE_CURRENT_SOURCE_DIR}/batch_transaction_builder.cpp  # Pay multiple recipients in one tx
  # Phase 30: Taproot Asset Layer
  ${CMAKE_SOURCE_DIR}/src/assets/asset_id.cpp           # Asset ID computation and metadata
  ${CMAKE_SOURCE_DIR}/src/assets/asset_state.cpp        # Asset UTXO and state tracking
  ${CMAKE_SOURCE_DIR}/src/assets/asset_proof.cpp        # Merkle proofs for asset operations
  ${CMAKE_SOURCE_DIR}/src/assets/asset_transfer.cpp     # Asset transfer protocol
  ${CMAKE_SOURCE_DIR}/src/assets/asset_registry.cpp     # Global asset registry (SQLite)
  ${CMAKE_SOURCE_DIR}/src/crypto/pbkdf2.cpp
  ${CMAKE_SOURCE_DIR}/src/crypto/wallet_crypto.cpp  # Wallet encryption (Argon2id + AES-256-GCM)
  # Wallet UX Abstractions (Phase CT.2 - Unified API layer)
  ${CMAKE_CURRENT_SOURCE_DIR}/unified_balance_service.cpp # UnifiedBalanceService: combined transparent + CT balance
  ${CMAKE_CURRENT_SOURCE_DIR}/address_factory.cpp         # AddressFactory: generate any address type
)

target_include_directories(dinero_wallet PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include  # ZK proof support
)

target_link_libraries(dinero_wallet PUBLIC
  dinero_tx_primitives # ← Transaction structures and Schnorr/Taproot signing
  dinero_crypto
  dinero_consensus  # ← Needed for CoinbaseMaturity::isCoinbaseMature()
  dinero_shielded
  argon2            # ← Wallet encryption (bundled Argon2id)
  OpenSSL::SSL
  OpenSSL::Crypto
  sqlite3           # ← ONLY SQLite, NO RocksDB
)

# Hardware wallet USB/HID transport — not available on iOS or when disabled.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND ENABLE_HARDWARE_WALLETS)
  target_link_libraries(dinero_wallet PUBLIC hidapi::hidapi)
  target_sources(dinero_wallet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware_wallet_interface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hid_transport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ledger_wallet.cpp
  )
  target_compile_definitions(dinero_wallet PUBLIC HAVE_USB_HWALLET=1)
endif()

# Trezor support (optional - requires protobuf and trezor-common protos).
if(ENABLE_TREZOR)
  target_sources(dinero_wallet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/trezor_transport.cpp  # Trezor wire protocol transport (Protobuf over HID)
    ${CMAKE_CURRENT_SOURCE_DIR}/trezor_wallet.cpp  # Trezor hardware wallet implementation (Protobuf messages)
  )
  target_link_libraries(dinero_wallet PUBLIC trezor_proto)
  target_compile_definitions(dinero_wallet PUBLIC ENABLE_TREZOR=1)
  message(STATUS "Trezor hardware wallet support: ENABLED")
else()
  message(STATUS "Trezor hardware wallet support: DISABLED (use -DENABLE_TREZOR=ON to enable)")
endif()
