#pragma once

#include <string>
#include "primitives/transaction.h"

namespace dinero {
namespace consensus {

/**
 * Thin compatibility wrapper around the canonical primitives transaction codec.
 */
class TransactionParser {
public:
    /**
     * Parse a hex-encoded transaction string into Transaction struct
     * 
     * @param hex_str Hex-encoded transaction (e.g., from block.transactions[i])
     * @param tx Output parameter for parsed transaction
     * @param error Output parameter for error message
     * @return true if parsing succeeded
     */
    static bool ParseTransaction(const std::string& hex_str, Transaction& tx, std::string& error);
    
    /**
     * Parse a coinbase transaction
     * Coinbase has special format: null prevout, scriptSig with height
     * 
     * @param hex_str Hex-encoded coinbase transaction
     * @param tx Output parameter for parsed transaction
     * @param error Output parameter for error message
     * @return true if parsing succeeded
     */
    static bool ParseCoinbaseTransaction(const std::string& hex_str, Transaction& tx, std::string& error);
    
    /**
     * Serialize a transaction back to canonical hex, including witness data.
     * 
     * @param tx The transaction to serialize
     * @return Hex-encoded transaction string
     */
    static std::string SerializeTransaction(const Transaction& tx);
    
    /**
     * Calculate the canonical transaction ID.
     * 
     * @param tx The transaction
     * @return Transaction ID as hex string
     */
    static std::string CalculateTxId(const Transaction& tx);
};

} // namespace consensus
} // namespace dinero
