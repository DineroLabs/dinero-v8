#pragma once

#include <QPixmap>
#include <QString>
#include <vector>

namespace dinero {

/**
 * @brief Simple QR Code generator for addresses
 *
 * This is a lightweight QR code generator that creates QR codes
 * suitable for cryptocurrency addresses without external dependencies.
 *
 * Based on the QR Code specification ISO/IEC 18004:2015
 * Supports alphanumeric mode for cryptocurrency addresses
 */
class QRCodeGenerator {
public:
    /**
     * @brief Generate QR code image for given text
     * @param text Text to encode (e.g., Dinero address)
     * @param size Size of output image in pixels (default: 300)
     * @param border Border size in modules (default: 4)
     * @return QPixmap containing the QR code image
     */
    static QPixmap generate(const QString& text, int size = 300, int border = 4);

private:
    // QR Code module matrix
    using Matrix = std::vector<std::vector<bool>>;

    // Generate QR code matrix
    static Matrix generateMatrix(const QString& text);

    // Render matrix to QPixmap
    static QPixmap renderMatrix(const Matrix& matrix, int size, int border);

    // Add finder patterns (corner squares)
    static void addFinderPatterns(Matrix& matrix);

    // Add timing patterns
    static void addTimingPatterns(Matrix& matrix);

    // Add data
    static void addData(Matrix& matrix, const QString& text);

    // Simple Reed-Solomon error correction
    static std::vector<uint8_t> addErrorCorrection(const std::vector<uint8_t>& data);
};

} // namespace dinero
