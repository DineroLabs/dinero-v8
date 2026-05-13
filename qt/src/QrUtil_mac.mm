#include "QrUtil.h"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QPainter>

#import <CoreImage/CoreImage.h>
#import <AppKit/AppKit.h>

namespace QrUtil {
QImage makeMacQr(const QString& text, int sizePx, int margin, int ecLevel);
}

namespace {

NSString* correctionLevel(int ecLevel) {
    switch (ecLevel) {
        case 0: return @"L";
        case 1: return @"M";
        case 2: return @"Q";
        case 3: return @"H";
        default: return @"M";
    }
}

bool moduleIsDark(NSBitmapImageRep* rep, NSInteger x, NSInteger y) {
    NSColor* color = [rep colorAtX:x y:y];
    if (!color) {
        return false;
    }

    NSColor* gray = [color colorUsingColorSpace:[NSColorSpace genericGrayColorSpace]];
    if (!gray) {
        return false;
    }

    return [gray whiteComponent] < 0.5;
}

}

QImage QrUtil::makeMacQr(const QString& text, int sizePx, int margin, int ecLevel) {
    if (text.isEmpty() || sizePx <= 0 || margin < 0) {
        return QImage();
    }

    @autoreleasepool {
        QByteArray bytes = text.toUtf8();
        NSData* payload = [NSData dataWithBytes:bytes.constData()
                                         length:static_cast<NSUInteger>(bytes.size())];

        CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
        if (!filter) {
            return QImage();
        }

        [filter setValue:payload forKey:@"inputMessage"];
        [filter setValue:correctionLevel(ecLevel) forKey:@"inputCorrectionLevel"];

        CIImage* ciImage = filter.outputImage;
        if (!ciImage) {
            return QImage();
        }

        CGRect extent = CGRectIntegral(ciImage.extent);
        const int modules = static_cast<int>(CGRectGetWidth(extent));
        if (modules <= 0) {
            return QImage();
        }

        CIContext* context = [CIContext contextWithOptions:nil];
        CGImageRef cgImage = [context createCGImage:ciImage fromRect:extent];
        if (!cgImage) {
            return QImage();
        }

        NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:cgImage];
        CGImageRelease(cgImage);
        if (!bitmap) {
            return QImage();
        }

        QImage image(sizePx, sizePx, QImage::Format_RGB32);
        image.fill(Qt::white);

        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);

        const int totalModules = modules + 2 * margin;
        const double scale = static_cast<double>(sizePx) / static_cast<double>(totalModules);

        for (int y = 0; y < modules; ++y) {
            for (int x = 0; x < modules; ++x) {
                if (!moduleIsDark(bitmap, x, y)) {
                    continue;
                }

                painter.drawRect(QRectF((x + margin) * scale,
                                        (y + margin) * scale,
                                        scale,
                                        scale));
            }
        }

        return image;
    }
}
