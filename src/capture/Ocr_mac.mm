// Text recognition with Apple's Vision framework (built in, offline). Which languages it reads
// depends on the macOS version; from macOS 13 it detects the language by itself.
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include "Ocr.h"

#include <QCoreApplication>

namespace clipitto::ocr {

bool builtIn()
{
    return true;
}

bool available(QString*)
{
    return true; // Vision's text recognition is part of macOS 10.15 and later (the app needs 11)
}

namespace {

// Vision wants region tags: "he" stays, "en" -> "en-US", "pt" -> "pt-BR", "zh" -> "zh-Hans" ...
NSString* visionTag(const QString& tag)
{
    static NSDictionary<NSString*, NSString*>* map = @{
        @"en" : @"en-US", @"fr" : @"fr-FR", @"de" : @"de-DE", @"es" : @"es-ES", @"it" : @"it-IT", @"pt" : @"pt-BR",
        @"zh" : @"zh-Hans", @"ja" : @"ja-JP", @"ko" : @"ko-KR", @"ru" : @"ru-RU", @"uk" : @"uk-UA",
        @"eng" : @"en-US", @"fra" : @"fr-FR", @"deu" : @"de-DE", @"spa" : @"es-ES", @"ita" : @"it-IT",
        @"por" : @"pt-BR", @"jpn" : @"ja-JP", @"kor" : @"ko-KR", @"rus" : @"ru-RU", @"ukr" : @"uk-UA"
    };
    NSString* mapped = map[tag.toLower().toNSString()];
    return mapped ? mapped : tag.toNSString();
}

} // namespace

std::optional<QString> recognize(const QByteArray& png, const QStringList& languages, QString* error)
{
    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:png.constData() length:NSUInteger(png.size())];
        VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
        request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        request.usesLanguageCorrection = YES;
        if (!languages.isEmpty()) {
            NSMutableArray<NSString*>* tags = [NSMutableArray array];
            for (const QString& tag : languages) {
                [tags addObject:visionTag(tag)];
            }
            request.recognitionLanguages = tags;
            if (@available(macOS 13.0, *)) {
                request.automaticallyDetectsLanguage = NO;
            }
        } else if (@available(macOS 13.0, *)) {
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
