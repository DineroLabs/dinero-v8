#include "bundledsnapshotpolicy.h"

#include <iostream>
#include <stdexcept>

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "selection case required");
        const std::string name = argv[1];
        using dinero::qt::SelectBundledSnapshots;
        if (name == "FreshPrimary") {
            const auto selected = SelectBundledSnapshots({"150521-v6-full.dat", "148375.dat", "99677.dat", "65300.dat"});
            Require(selected.primary == "150521-v6-full.dat", "newest snapshot was not selected");
            Require(selected.fallbacks == std::vector<std::string>{"148375.dat", "99677.dat", "65300.dat"}, "old recovery bases were lost");
        } else if (name == "MissingPrimary") {
            const auto selected = SelectBundledSnapshots({"", "148375.dat", "99677.dat", "65300.dat"});
            Require(selected.primary == "148375.dat", "available recovery base was not selected");
            Require(selected.fallbacks == std::vector<std::string>{"99677.dat", "65300.dat"}, "older recovery bases were lost");
        } else if (name == "OldestOnly") {
            const auto selected = SelectBundledSnapshots({"", "", "", "65300.dat"});
            Require(selected.primary == "65300.dat" && selected.fallbacks.empty(), "oldest-only install refused");
        } else if (name == "None") {
            const auto selected = SelectBundledSnapshots({"", "", "", ""});
            Require(selected.primary.empty() && selected.fallbacks.empty(), "missing payloads invented a selection");
        } else if (name == "Deduplicate") {
            const auto selected = SelectBundledSnapshots({"same.dat", "same.dat", "old.dat", "old.dat"});
            Require(selected.primary == "same.dat" && selected.fallbacks == std::vector<std::string>{"old.dat"}, "duplicate paths were armed");
        } else if (name == "DelimitedFallback") {
            const auto selected = SelectBundledSnapshots({"new.dat", "unsafe;old.dat", "older.dat"});
            Require(selected.fallbacks == std::vector<std::string>{"older.dat"}, "ambiguous fallback delimiter accepted");
        } else if (name == "WindowsAndUnicodePaths") {
            const std::string first = "C:\\Program Files\\Dinero\\148375.dat";
            const std::string second = "C:\\Users\\Žiga\\99677.dat";
            const auto selected = SelectBundledSnapshots({first, second});
            Require(selected.primary == first && selected.fallbacks == std::vector<std::string>{second}, "path bytes changed");
        } else {
            throw std::runtime_error("unknown selection case");
        }
        std::cout << "PASS " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
