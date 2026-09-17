#include "consensus/asert.h"
#include <iostream>

// Pipe protocol for the independent Python big-integer oracle. No duplicated
// production arithmetic here: each row calls the actual shared consensus path.
int main() {
    dinero::AsertInput input;
    uint32_t pow_bits;
    while (std::cin >> input.params.sixty_second_activation_height
           >> input.target_height >> input.anchor.height >> input.anchor.time
           >> input.anchor.bits >> pow_bits >> input.reference_time
           >> input.params.target_spacing_secs >> input.params.half_life_secs) {
        input.params.pow_limit.SetCompact(pow_bits);
        std::cout << dinero::ComputeAsertBits(input) << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
