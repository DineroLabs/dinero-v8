# In-process Orchard node/Qt contract qualification

The optional `DINERO_TEST_ORCHARD_QT_CONTRACT` test build links Qt Core into the existing replay fixture only. It leaves desktop UI enablement and production targets unchanged. No HTTP listener, socket, desktop app or node process is launched by these cases.

Qt request builders serialize real payment and read requests. The actual RPC registry checks wallet bindings and executes the real account owner. Its JSON responses are serialized and passed to the production Qt parsers in a separate translation unit, which returns parsed fields for assertions. No success responses are supplied by the adapter.

Five cases are defined for backend ON: absent-owner refusal; account/balance/activation/history reads with reopen and stale binding; private transfer and unshield with stored-ID completion, admission, confirmation and undo; and automatic shield selection with a real received note. The absent-owner case also runs with backend OFF. Counts are definitions until executed. Existing fixtures and their assertions are unchanged.

This is not desktop transport, RpcClient dispatch, widget behavior, installed-package, whole-node restart/reindex, platform or release qualification. The test uses isolated existing regtest fixtures and their existing in-process proof worker. The production activation height stays unset. The UI stays off by default. Public CI dispatch remains held.
