#pragma once

#include <string>
#include <vector>

namespace dinero {

// BIP-39 English wordlist (2048 words)
// Source: https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt
class BIP39Wordlist {
public:
    static const std::vector<std::string>& get_english_wordlist();
    static int find_word_index(const std::string& word);
    static bool is_valid_word(const std::string& word);
    
private:
    static const std::vector<std::string> ENGLISH_WORDS;
};

} // namespace dinero
