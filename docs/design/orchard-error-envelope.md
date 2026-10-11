# Orchard structured failures across HTTP and Qt

Candidate, not qualified. Orchard string failures remain top-level JSON-RPC errors. The HTTP dispatcher retains only typed error_code, proof_state and reservation_retained under error.data.orchard for canonical Orchard methods and aliases. Other result fields are not exported. Existing well-formed error objects keep their existing data.

RpcClient passes error.data through one detailed failure signal, retaining the old code/message signal for existing consumers. Orchard subscribes only to the detailed route. Its existing wallet/generation/request guards run before a bounded typed failure is passed to the state machine. Non-null malformed JSON-RPC errors cannot become successful result notifications. Network/parse errors also use the detailed route without inventing proof or reservation state.

Offline tests use a private reply-only constructor with no connection discovery, cookies, timer or network manager. The real client parser and real signals run; widget requests remain passive. This is not socket/IPC, installed desktop, full payment or release qualification. Fresh daemon/Qt builds, backend ON/OFF qualification, measured sanitizer scope and safe copied-original controls remain required. Mainnet activation and UI defaults are unchanged.
