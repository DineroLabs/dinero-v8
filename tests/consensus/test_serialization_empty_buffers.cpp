// Empty fields are valid in stored UTXOs. Their zero-length storage must never
// be passed to memcpy, which requires non-null pointers even for a zero count.
#include "common/serialization.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename F>
void RequireRejected(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, message);
}
} // namespace

int main() try {
    // Independent literal: empty bytes, nonempty bytes (including zero), empty
    // string, nonempty string, empty bytes. Empty fields still have a length byte.
    const std::string expected("\x00\x03\x11\x00\x22\x00\x02ok\x00", 10);
    dinero::Reader reader(expected);
    Require(reader.readBytes().empty(), "empty bytes were not decoded");
    Require(reader.position() == 1, "empty field consumed the next field");
    Require(reader.readBytes() == std::vector<uint8_t>({0x11, 0x00, 0x22}),
            "nonempty bytes changed");
    Require(reader.readString().empty(), "empty string was not decoded");
    Require(reader.readString() == "ok", "nonempty string changed");
    Require(reader.readBytes().empty() && reader.eof(), "trailing empty field changed");
    reader.read(nullptr, 0);
    Require(reader.position() == expected.size(), "zero read advanced at EOF");

    dinero::VectorWriter writer;
    writer.write(nullptr, 0);
    Require(writer.data().empty(), "empty raw write emitted bytes");
    writer.writeBytes({});
    writer.writeBytes({0x11, 0x00, 0x22});
    writer.writeString("");
    writer.writeString("ok");
    writer.writeBytes({});
    writer.write(nullptr, 0);
    Require(writer.release_string() == expected, "serialized field bytes changed");

    dinero::Reader empty(std::vector<uint8_t>{});
    empty.read(nullptr, 0);
    Require(empty.position() == 0 && empty.eof(), "empty raw read advanced");
    uint8_t sentinel = 0x5a;
    empty.read(&sentinel, 0);
    Require(sentinel == 0x5a, "zero read changed destination");
    RequireRejected([&] { empty.read(&sentinel, 1); }, "nonempty read past EOF accepted");
    Require(sentinel == 0x5a && empty.position() == 0, "failed read mutated state");
    RequireRejected([&] { empty.readBytes(); }, "missing length accepted as empty field");

    dinero::Reader truncated(std::string("\x01", 1));
    RequireRejected([&] { truncated.readBytes(); }, "truncated nonempty field accepted");
    dinero::Reader invalid_position(std::string("\x00", 1));
    invalid_position.setPosition(2);
    RequireRejected([&] { invalid_position.read(nullptr, 0); },
                    "zero read bypassed the existing position bound");
    std::cout << "PASS empty fields, exact bytes, cursor and malformed-field rejection\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
