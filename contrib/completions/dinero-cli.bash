#!/bin/bash
# Bash completion for dinero-cli

_dinero_cli() {
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Global flags
    local global_flags="
        --network --regtest --testnet --mainnet
        --wallet -w --transport --http-only
        --datadir --nodeinfo --rpc-url --cookie-file
        --format --json --json-schema --pretty --no-pretty
        --color --no-color --timeout --retries --wait-ready
        --dry-run --curl --verbose -v --version
        --limit --offset --cursor --filter --since --until --all
        --min-conf --address --type --label --min-amount --max-amount
        --confirmed-only --state --min-version --min-fee-rate --txid
        --profile --accept-insecure-cookie --connect-timeout-ms --read-timeout-ms
    "

    # Main commands
    local commands="
        status nodeinfo doctor
        chain net mempool tx
        wallet addr send mining
        rpc rpc-parity profile
    "

    # Subcommands
    local chain_cmds="tip info count getblockhash getblock"
    local net_cmds="info peers connections"
    local mempool_cmds="info raw list"
    local tx_cmds="get decode"
    local wallet_cmds="create load info balance history utxos addresses newaddress encrypt lock unlock change-passphrase backup export"
    local addr_cmds="validate"
    local mining_cmds="info setaddress getaddress start stop setthreads generatetoaddress"
    local profile_cmds="list show set-default"

    # Handle subcommands
    if [[ ${#COMP_WORDS[@]} -gt 2 ]]; then
        case "${COMP_WORDS[1]}" in
            chain)
                COMPREPLY=($(compgen -W "${chain_cmds}" -- ${cur}))
                return 0
                ;;
            net)
                COMPREPLY=($(compgen -W "${net_cmds}" -- ${cur}))
                return 0
                ;;
            mempool)
                COMPREPLY=($(compgen -W "${mempool_cmds}" -- ${cur}))
                return 0
                ;;
            tx)
                COMPREPLY=($(compgen -W "${tx_cmds}" -- ${cur}))
                return 0
                ;;
            wallet)
                COMPREPLY=($(compgen -W "${wallet_cmds}" -- ${cur}))
                return 0
                ;;
            addr)
                COMPREPLY=($(compgen -W "${addr_cmds}" -- ${cur}))
                return 0
                ;;
            mining)
                COMPREPLY=($(compgen -W "${mining_cmds}" -- ${cur}))
                return 0
                ;;
            profile)
                COMPREPLY=($(compgen -W "${profile_cmds}" -- ${cur}))
                return 0
                ;;
        esac
    fi

    # Handle flag values
    case "${prev}" in
        --network)
            COMPREPLY=($(compgen -W "mainnet testnet regtest" -- ${cur}))
            return 0
            ;;
        --format)
            COMPREPLY=($(compgen -W "table json plain" -- ${cur}))
            return 0
            ;;
        --transport)
            COMPREPLY=($(compgen -W "auto http ws" -- ${cur}))
            return 0
            ;;
        --profile)
            # Try to read profiles from config file
            if [[ -f ~/.dinero-cli/profiles.json ]]; then
                local profiles=$(jq -r '.profiles | keys[]' ~/.dinero-cli/profiles.json 2>/dev/null)
                COMPREPLY=($(compgen -W "${profiles}" -- ${cur}))
            fi
            return 0
            ;;
    esac

    # Complete with commands and flags
    if [[ ${cur} == -* ]]; then
        COMPREPLY=($(compgen -W "${global_flags}" -- ${cur}))
    else
        COMPREPLY=($(compgen -W "${commands}" -- ${cur}))
    fi
}

complete -F _dinero_cli dinero-cli
