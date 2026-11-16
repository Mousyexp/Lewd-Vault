// Starts Chat's Nearby (LvNearby.swift) when the app loads: the Swift side then waits for the web view.
// An Objective-C +load because it runs without anything calling it, so the app's own code (and the
// Rust library) needs no link to the Swift.
#import <Foundation/Foundation.h>
#import <objc/message.h>

@interface LvNearbyLoad : NSObject
@end

@implementation LvNearbyLoad
+ (void)load {
  dispatch_async(dispatch_get_main_queue(), ^{
    Class c = NSClassFromString(@"LvNearbyInstaller");
    SEL s = NSSelectorFromString(@"begin");
    if (c && [c respondsToSelector:s]) ((void (*)(id, SEL))objc_msgSend)(c, s);
  });
}
@end
