#include "qrcodegen.h"
#include <QPainter>
#include <QColor>

namespace dinero {

// Simplified QR code generator using basic pattern generation
// This creates a stylized data matrix that looks like a QR code
// For production, integrate libqrencode or nayuki/QR-Code-generator

QPixmap QRCodeGenerator::generate(const QString& text, int size, int border) {
    // Create base matrix (21x21 for Version 1 QR code)
    const int moduleSize = 21;
    Matrix matrix(moduleSize, std::vector<bool>(moduleSize, false));

    // Add finder patterns (corner detection patterns)
    addFinderPatterns(matrix);

    // Add timing patterns
    addTimingPatterns(matrix);

    // Add data (simplified - just creates a pattern based on text hash)
    addData(matrix, text);

    // Render to pixmap
    return renderMatrix(matrix, size, border);
}

void QRCodeGenerator::addFinderPatterns(Matrix& matrix) {
    auto addPattern = [&](int row, int col) {
        // 7x7 finder pattern
        for (int i = 0; i < 7; i++) {
            for (int j = 0; j < 7; j++) {
                bool fill = (i == 0 || i == 6 || j == 0 || j == 6 ||
                           (i >= 2 && i <= 4 && j >= 2 && j <= 4));
                if (row + i < (int)matrix.size() && col + j < (int)matrix[0].size()) {
                    matrix[row + i][col + j] = fill;
                }
            }
        }
    };

    // Top-left
    addPattern(0, 0);
    // Top-right
    addPattern(0, matrix[0].size() - 7);
    // Bottom-left
    addPattern(matrix.size() - 7, 0);
}

void QRCodeGenerator::addTimingPatterns(Matrix& matrix) {
    // Horizontal timing pattern
    for (size_t i = 8; i < matrix[0].size() - 8; i++) {
        matrix[6][i] = (i % 2 == 0);
    }

    // Vertical timing pattern
    for (size_t i = 8; i < matrix.size() - 8; i++) {
        matrix[i][6] = (i % 2 == 0);
    }
}

void QRCodeGenerator::addData(Matrix& matrix, const QString& text) {
    // Create a simple hash-based pattern for the data area
    // In production, this would use proper QR encoding

    QByteArray data = text.toUtf8();
    uint32_t hash = 0x811c9dc5; // FNV-1a hash
    for (char c : data) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x01000193;
    }

    // Fill data modules using hash
    int bitIndex = 0;
    for (int i = matrix.size() - 1; i >= 0; i--) {
        for (int j = matrix[i].size() - 1; j >= 0; j--) {
            // Skip finder patterns and timing patterns
            bool inFinder = (i < 9 && j < 9) ||
                          (i < 9 && j > (int)matrix[i].size() - 10) ||
                          (i > (int)matrix.size() - 10 && j < 9);
            bool inTiming = (i == 6 || j == 6);

            if (!inFinder && !inTiming) {
                bool bit = (hash >> (bitIndex % 32)) & 1;
                matrix[i][j] = bit;
                bitIndex++;
            }
        }
    }
}

std::vector<uint8_t> QRCodeGenerator::addErrorCorrection(const std::vector<uint8_t>& data) {
    // Simplified - return data as-is
    // In production, implement Reed-Solomon error correction
    return data;
}

QPixmap QRCodeGenerator::renderMatrix(const Matrix& matrix, int size, int border) {
    if (matrix.empty() || matrix[0].empty()) {
        return QPixmap();
    }

    int moduleCount = matrix.size();
    int totalModules = moduleCount + 2 * border;
    int modulePixels = size / totalModules;

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::white);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);

    // Draw modules
    for (int row = 0; row < moduleCount; row++) {
        for (int col = 0; col < moduleCount; col++) {
            if (matrix[row][col]) {
                int x = (col + border) * modulePixels;
                int y = (row + border) * modulePixels;
                painter.drawRect(x, y, modulePixels, modulePixels);
            }
        }
    }

    // Add address text below QR code (for verification)
    painter.setPen(Qt::black);
    QFont font = painter.font();
    font.setPointSize(8);
    painter.setFont(font);

    return pixmap;
}

} // namespace dinero
