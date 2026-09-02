#include "QrUtil.h"
#include <QPainter>
#include <QPainterPath>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

#ifdef DIN_HAVE_QRENCODE
#include <qrencode.h>
#endif

#if !defined(DIN_HAVE_QRENCODE) && defined(Q_OS_MACOS)
namespace QrUtil {
    QImage makeMacQr(const QString& text, int sizePx, int margin, int ecLevel);
}
#endif

namespace {

QImage makePlainQr(const QString& text, int sizePx, int margin, int ecLevel) {
#ifndef DIN_HAVE_QRENCODE
#ifdef Q_OS_MACOS
    QImage nativeQr = QrUtil::makeMacQr(text, sizePx, margin, ecLevel);
    if (!nativeQr.isNull()) {
        return nativeQr;
    }
#endif

    // Last-resort fallback: explicit failure image if no native QR backend exists.
    QImage img(sizePx, sizePx, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setPen(Qt::black);
    p.drawRect(0, 0, sizePx - 1, sizePx - 1);
    p.drawText(img.rect(), Qt::AlignCenter | Qt::TextWordWrap,
               "QR backend unavailable");
    return img;
#else
    // Convert QString to UTF-8 bytes for qrencode
    QByteArray utf8 = text.toUtf8();

    // Map int ecLevel to qrencode enum (0=L, 1=M, 2=Q, 3=H)
    QRecLevel qrEc = QR_ECLEVEL_M;
    switch (ecLevel) {
        case 0: qrEc = QR_ECLEVEL_L; break;
        case 1: qrEc = QR_ECLEVEL_M; break;
        case 2: qrEc = QR_ECLEVEL_Q; break;
        case 3: qrEc = QR_ECLEVEL_H; break;
    }

    // Encode the text as QR code
    QRcode* code = QRcode_encodeString(utf8.constData(), 0, qrEc, QR_MODE_8, 1);
    if (!code) {
        // Create error image if encoding failed
        QImage err(sizePx, sizePx, QImage::Format_ARGB32_Premultiplied);
        err.fill(Qt::white);
        QPainter p(&err);
        p.setPen(Qt::red);
        p.drawText(err.rect(), Qt::AlignCenter, "QR encode failed");
        return err;
    }
    
    // Calculate dimensions
    const int modules = code->width > 0 ? code->width : 1;
    const int totalModules = modules + 2 * margin;
    const double scale = double(sizePx) / double(totalModules);
    
    // Create the output image
    QImage img(sizePx, sizePx, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    
    QPainter qp(&img);
    qp.setRenderHint(QPainter::Antialiasing, false);
    qp.setPen(Qt::NoPen);
    qp.setBrush(Qt::black);
    
    // Draw QR code modules
    const unsigned char* data = code->data;
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            const bool on = (*data & 0x01) != 0;
            if (on) {
                const QRectF r(
                    (x + margin) * scale,
                    (y + margin) * scale,
                    scale, scale
                );
                qp.drawRect(r);
            }
            ++data;
        }
    }
    
    // Clean up
    QRcode_free(code);
    
    return img;
#endif
}

QString bundledLogoPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        appDir + QStringLiteral("/../Resources/Dinero-Coin.png"),
        appDir + QStringLiteral("/Dinero-Coin.png"),
        QDir::currentPath() + QStringLiteral("/Dinero-Coin.png")
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return QDir::cleanPath(candidate);
    }
    return {};
}

} // namespace

QImage QrUtil::makeQrWithLogo(const QString& text, const QString& logoPath, int sizePx, int margin) {
    // Generate QR with HIGH error correction (30% damage tolerance) for logo overlay
    QImage qrImage = makePlainQr(text, sizePx, margin, /*ecLevel=*/3);

    // Load the logo image
    QImage logo(logoPath);
    if (logo.isNull()) {
        return qrImage;
    }

    // Logo = ~22% of QR size (matches DineroDPI iOS), padding = 8px
    double logoSize = sizePx * 0.22;
    double padding = 8.0;
    double bgRadius = (logoSize + 2.0 * padding) / 2.0;

    // Force square scaling (ignore source aspect ratio)
    logo = logo.scaled(static_cast<int>(logoSize), static_cast<int>(logoSize),
                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QPainter painter(&qrImage);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Exact center in floating point
    double cx = sizePx / 2.0;
    double cy = sizePx / 2.0;

    // White circle background with subtle shadow
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 30));
    painter.drawEllipse(QPointF(cx, cy + 1.0), bgRadius + 1.0, bgRadius + 1.0);

    painter.setBrush(Qt::white);
    painter.drawEllipse(QPointF(cx, cy), bgRadius, bgRadius);

    // Draw logo precisely centered using floating-point rect
    QRectF logoRect(cx - logoSize / 2.0, cy - logoSize / 2.0, logoSize, logoSize);

    QPainterPath clipPath;
    clipPath.addEllipse(QPointF(cx, cy), logoSize / 2.0, logoSize / 2.0);
    painter.setClipPath(clipPath);
    painter.drawImage(logoRect, logo);

    return qrImage;
}

QImage QrUtil::makeQr(const QString& text, int sizePx, int margin, int ecLevel) {
    const QString logoPath = bundledLogoPath();
    if (!logoPath.isEmpty()) {
        // A center mark requires high error correction regardless of the caller's
        // previous default. This keeps every Qt QR branded and still scannable.
        return makeQrWithLogo(text, logoPath, sizePx, margin);
    }
    return makePlainQr(text, sizePx, margin, ecLevel);
}
