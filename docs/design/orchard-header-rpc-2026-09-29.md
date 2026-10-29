# Owned Orchard header RPC reads

Locally qualified. The actual context-aware getblockheader obtains an owned typed-or-historical header snapshot with checked stored height and chainwork and optional current index status under the existing selected-service lock, taking the global index lock only while copying index status. Existing display/raw Utreexo root fields and status flags are retained. It requires the existing archival body, as the previous handler did; it does not grant header-only availability, admission or canonicality.

Three patched-path cases cover actual mined shield header/work/flags, DB reopen and retained disconnected body; real historical flatfile and malformed requests; unavailable typed storage and invalid profile followed by successful retry. OFF missing owner and stored active-profile header refuse. No broader global index lifetime/race guarantee, HTTP/P2P, production provider or release claim. Mainnet unset.
