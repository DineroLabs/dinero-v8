#!/bin/bash
# Dinero CLI Bash Completion
# Install: sudo cp scripts/bash_completion.sh /usr/local/etc/bash_completion.d/dinero-cli
# Or: source scripts/bash_completion.sh

_dinero_cli() {
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    
    # Global options
    local global_opts="--rpc-url --rpc-user --rpc-pass --wallet --datadir --json --verbose --help"
    
    # Top-level commands
    local top_commands="blockchain miner wallet addr tx height besthash stop help"
    
    # Subcommands by category
    local blockchain_cmds="height getblockhash listunspent stop"
    local miner_cmds="start stop status"
    local wallet_cmds="create list load lock passphrase balance"
    local addr_cmds="new validate"
    local tx_cmds="send list raw"
    local raw_cmds="create send"
    
    case "$prev" in
        dinero-cli)
            COMPREPLY=( $(compgen -W "$top_commands $global_opts" -- "$cur") )
            return 0
            ;;
        blockchain)
            COMPREPLY=( $(compgen -W "$blockchain_cmds" -- "$cur") )
            return 0
            ;;
        miner)
            COMPREPLY=( $(compgen -W "$miner_cmds" -- "$cur") )
            return 0
            ;;
        wallet)
            COMPREPLY=( $(compgen -W "$wallet_cmds" -- "$cur") )
            return 0
            ;;
        addr)
            COMPREPLY=( $(compgen -W "$addr_cmds" -- "$cur") )
            return 0
            ;;
        tx)
            COMPREPLY=( $(compgen -W "$tx_cmds" -- "$cur") )
            return 0
            ;;
        raw)
            COMPREPLY=( $(compgen -W "$raw_cmds" -- "$cur") )
            return 0
            ;;
        --threads)
            COMPREPLY=( $(compgen -W "1 2 4 8 12 16" -- "$cur") )
            return 0
            ;;
        --type)
            COMPREPLY=( $(compgen -W "bech32 p2pkh" -- "$cur") )
            return 0
            ;;
        --timeout)
            COMPREPLY=( $(compgen -W "60 300 600 1800 3600" -- "$cur") )
            return 0
            ;;
        --count)
            COMPREPLY=( $(compgen -W "10 25 50 100" -- "$cur") )
            return 0
            ;;
        --rpc-url)
            COMPREPLY=( $(compgen -W "http://127.0.0.1:8332 http://localhost:8332" -- "$cur") )
            return 0
            ;;
        --wallet)
            # Could potentially list actual wallets from RPC, but keep simple for now
            COMPREPLY=( $(compgen -W "main default test" -- "$cur") )
            return 0
            ;;
    esac
    
    # Handle flags that might appear anywhere
    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "$global_opts" -- "$cur") )
        return 0
    fi
    
    return 0
}

# Register the completion function
complete -F _dinero_cli dinero-cli

# Also support the binary name without path
complete -F _dinero_cli ./dinero-cli
complete -F _dinero_cli build-release/bin/dinero-cli

echo "Dinero CLI bash completion loaded!"
echo "Usage: dinero-cli <TAB><TAB> to see available commands"
