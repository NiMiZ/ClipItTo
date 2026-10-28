// Text recognition with Apple's Vision framework (built in, offline). Which languages it reads
// depends on the macOS version; from macOS 13 it detects the language by itself.
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include "Ocr.h"

#include <QCoreApplication>

namespace clipitto::ocr {

bool available(QString*)
{
    return true; // Vision's text recognition is part of macOS 10.15 and later (the app needs 11)
}

std::optional<QString> recognize(const QByteArray& png, QString* error)
{
    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:png.constData() length:NSUInteger(png.size())];
        VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
        request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        request.usesLanguageCorrection = YES;
        if (@available(macOS 13.0, *)) {
            request.automaticallyDetectsLanguage = YES;
        }
        VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithData:data options:@{}];
        NSError* failure = nil;
        if (![handler performRequests:@[ request ] error:&failure]) {
            if (error) {
                *error = failure ? QString::fromNSString(failure.localizedDescription)
                                 : QCoreApplication::translate("clipitto::ocr", "Text recognition failed.");
            }
            return std::nullopt;
        }
        // Lines top to bottom: Vision's origin is the bottom-left corner.
        NSArray<VNRecognizedTextObservation*>* found =
            [request.results sortedArrayUsingComparator:^NSComparisonResult(VNRecognizedTextObservation* a,
                                                                           VNRecognizedTextObservation* b) {
                const CGFloat ya = CGRectGetMaxY(a.boundingBox);
                const CGFloat yb = CGRectGetMaxY(b.boundingBox);
                if (ya > yb + 0.01) {
                    return NSOrderedAscending;
                }
                if (ya < yb - 0.01) {
                    return NSOrderedDescending;
                }
                return CGRectGetMinX(a.boundingBox) < CGRectGetMinX(b.boundingBox) ? NSOrderedAscending
                                                                                   : NSOrderedDescending;
            }];
        QStringList lines;
        for (VNRecognizedTextObservation* observation in found) {
            VNRecognizedText* top = [observation topCandidates:1].firstObject;
            if (top) {
                lines << QString::fromNSString(top.string);
            }
        }
        return lines.join(QLatin1Char('\n')).trimmed();
    }
}

} // namespace clipitto::ocr
