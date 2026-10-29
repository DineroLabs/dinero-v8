# Shared mining job policy

CreateJob now uses the same captured candidate ranking and common policy filter as CreateNewBlock. Its actual selection helper takes one immutable pool selection, applies configured CT/VWU/scheme limits and ancestor preservation, and totals only accepted fees before building the coinbase. Intelligent mode captures the same bounded extra candidate budget used by RPC templates. The operation retains its existing service lifetime owner.

Three benign patched cases build real jobs from actual signed pool entries: VWU parent filtering and retry in both modes, shared-parent inclusion once, and exact transaction-weight boundary. No pool mutation or policy relaxation. These tests do not qualify confidential/Orchard proofs, networking, global assembler concurrency, continuing chain authorization, or independently validated selected-parent activation. Mainnet stays unset and provider/release gates remain open.
