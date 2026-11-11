#include "daemon/mempool.h"
#include <iostream>

int main() {
    dinero::Mempool pool(nullptr);
    if (pool.size() != 0) {
        std::cerr << "Fresh standalone mempool is not empty\n";
        return 1;
    }
    std::cout << "Whole-archive mempool consumer ran\n";
    return 0;
}
