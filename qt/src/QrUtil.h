#pragma once

#include <QImage>
#include <QString>

namespace QrUtil {
    /**
     * @brief Generate a QR code image from text
     * @param text The text to encode (typically a Dinero address or URI)
     * @param sizePx Final image size in pixels (square)
     * @param margin Quiet zone modules (usually 4)
     * @param ecLevel Error correction level (0=L, 1=M, 2=Q, 3=H). Default M.
     * @return QImage containing the QR code
     */
    QImage makeQr(const QString& text, int sizePx=256, int margin=4, int ecLevel=1);
    
    /**
     * @brief Generate a QR code image with a center logo overlay
     * @param text The text to encode
     * @param logoPath Path to the logo image file
     * @param sizePx Final image size in pixels (square)
     * @param margin Quiet zone modules (usually 4)
     * @return QImage containing the QR code with logo
     */
    QImage makeQrWithLogo(const QString& text, const QString& logoPath, int sizePx=256, int margin=4);
}
