#include "wallet/descriptor_checksum.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string desc;
    if (argc > 1) {
        desc = argv[1];
    } else {
        std::getline(std::cin, desc);
    }
    std::cout << din::DescriptorChecksum::AddChecksum(desc) << std::endl;
    return 0;
}
