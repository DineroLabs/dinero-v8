# Dinero CLI Fish Completion

# Global options
complete -c dinero-cli -l rpc-url -d "RPC URL" -x
complete -c dinero-cli -l rpc-user -d "RPC username" -x
complete -c dinero-cli -l rpc-pass -d "RPC password" -x
complete -c dinero-cli -l wallet -d "Wallet name" -x
complete -c dinero-cli -l datadir -d "Data directory" -F
complete -c dinero-cli -s j -l json -d "JSON output"
complete -c dinero-cli -s v -l verbose -d "Verbose output"
complete -c dinero-cli -s h -l help -d "Show help"

# Top-level commands
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "blockchain" -d "Blockchain operations"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "miner" -d "Mining operations"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "wallet" -d "Wallet operations"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "addr" -d "Address tools"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "tx" -d "Transactions"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "height" -d "Quick blockchain height"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "besthash" -d "Best block hash"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "stop" -d "Stop daemon"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "version" -d "Show version"
complete -c dinero-cli -f -n "__fish_use_subcommand" -a "help" -d "Show help"

# Blockchain subcommands
complete -c dinero-cli -f -n "__fish_seen_subcommand_from blockchain" -a "height" -d "Show current block height"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from blockchain" -a "getblockhash" -d "Get block hash by height"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from blockchain" -a "listunspent" -d "List unspent outputs"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from blockchain" -a "stop" -d "Stop the daemon"

# Miner subcommands
complete -c dinero-cli -f -n "__fish_seen_subcommand_from miner" -a "start" -d "Start mining"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from miner" -a "stop" -d "Stop mining"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from miner" -a "status" -d "Show mining status"

# Miner start options
complete -c dinero-cli -f -n "__fish_seen_subcommand_from miner; and __fish_seen_subcommand_from start" -l threads -d "Number of threads" -a "1 2 4 8 12 16"

# Wallet subcommands
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet" -a "create" -d "Create wallet"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet" -a "list" -d "List loaded wallets"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet" -a "load" -d "Load wallet by name"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet" -a "lock" -d "Lock wallet"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet" -a "passphrase" -d "Unlock wallet"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet" -a "balance" -d "Show wallet balance"

# Wallet passphrase options
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet; and __fish_seen_subcommand_from passphrase" -l timeout -d "Timeout in seconds" -a "60 300 600 1800 3600"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from wallet; and __fish_seen_subcommand_from passphrase" -l stdin -d "Read from stdin"

# Address subcommands
complete -c dinero-cli -f -n "__fish_seen_subcommand_from addr" -a "new" -d "Generate new address"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from addr" -a "validate" -d "Validate address"

# Address new options
complete -c dinero-cli -f -n "__fish_seen_subcommand_from addr; and __fish_seen_subcommand_from new" -l type -d "Address type" -a "bech32 p2pkh"

# Transaction subcommands
complete -c dinero-cli -f -n "__fish_seen_subcommand_from tx" -a "send" -d "Send coins"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from tx" -a "list" -d "List transactions"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from tx" -a "raw" -d "Raw transaction tools"

# Transaction send options
complete -c dinero-cli -f -n "__fish_seen_subcommand_from tx; and __fish_seen_subcommand_from send" -l subtractfee -d "Subtract fee from amount"

# Transaction list options
complete -c dinero-cli -f -n "__fish_seen_subcommand_from tx; and __fish_seen_subcommand_from list" -l count -d "Number of transactions" -a "10 25 50 100"

# Raw transaction subcommands
complete -c dinero-cli -f -n "__fish_seen_subcommand_from tx; and __fish_seen_subcommand_from raw" -a "create" -d "Create raw transaction"
complete -c dinero-cli -f -n "__fish_seen_subcommand_from tx; and __fish_seen_subcommand_from raw" -a "send" -d "Broadcast raw transaction"
