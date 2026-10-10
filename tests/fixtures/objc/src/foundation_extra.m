/* Second Foundation batch from guest code (ARC): NSMutableString, NSData/NSMutableData, NSDate, NSURL, NSError, NSFileManager.
   Needs `--root <dir>` (an empty directory) for the file part. Exits with the number of failed checks. */
#include "Foundation/Foundation.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static double fabs_d(double x) { return x < 0 ? -x : x; }

int main(void) {
    @autoreleasepool {
        /* ---- NSMutableString ---- */
        NSMutableString *m = [NSMutableString stringWithCapacity:16];
        [m appendString:@"hello"];
        [m appendFormat:@", %@ %d", @"world", 42];
        CHECK([m isEqualToString:@"hello, world 42"]);
        [m insertString:@"[" atIndex:0];
        [m appendString:@"]"];
        CHECK([m isEqualToString:@"[hello, world 42]"]);
        [m deleteCharactersInRange:NSMakeRange(1, 7)];
        CHECK([m isEqualToString:@"[world 42]"]);
        [m replaceCharactersInRange:NSMakeRange(1, 5) withString:@"W"];
        CHECK([m isEqualToString:@"[W 42]"] && [m length] == 6);
        NSString *snapshot = [m copy];
        [m setString:@"changed"];
        CHECK([snapshot isEqualToString:@"[W 42]"] && [m isEqualToString:@"changed"]);      /* copy of a mutable string is a snapshot */
        NSMutableString *mc = [snapshot mutableCopy];
        [mc appendString:@"!"];
        CHECK([mc isEqualToString:@"[W 42]!"] && [snapshot length] == 6);
        NSMutableString *uni = [NSMutableString stringWithString:@"hé"];
        [uni insertString:@"\U0001F600" atIndex:1];                                        /* index is in UTF-16 units */
        CHECK([uni length] == 4 && [[uni substringFromIndex:3] isEqualToString:@"é"]);
        CHECK([@"/a/b/file.txt" hasSuffix:@".txt"] && [[@"/a/b/file.txt" lastPathComponent] isEqualToString:@"file.txt"]);
        CHECK([[@"/a/b/file.txt" pathExtension] isEqualToString:@"txt"] && [[@"/a/b/file.txt" stringByDeletingLastPathComponent] isEqualToString:@"/a/b"]);
        CHECK([[@"/a/b" stringByAppendingPathComponent:@"c.txt"] isEqualToString:@"/a/b/c.txt"] && [@"/x" isAbsolutePath] && ![@"x" isAbsolutePath]);

        /* ---- NSData / NSMutableData ---- */
        const unsigned char raw[] = { 1, 2, 3, 4, 5, 0xab, 0xcd };
        NSData *d = [NSData dataWithBytes:raw length:sizeof raw];
        CHECK([d length] == 7 && ((const unsigned char *)[d bytes])[5] == 0xab);
        CHECK([[d description] isEqualToString:@"<01020304 05abcd>"]);
        CHECK([[NSData data] length] == 0 && [[[NSData data] description] isEqualToString:@"<>"]);
        unsigned char back[3];
        [d getBytes:back length:3];
        CHECK(back[0] == 1 && back[2] == 3);
        NSData *sub = [d subdataWithRange:NSMakeRange(2, 3)];
        CHECK([sub length] == 3 && ((const unsigned char *)[sub bytes])[0] == 3);
        CHECK([d isEqualToData:[NSData dataWithData:d]] && ![d isEqualToData:sub] && [d isEqual:[d copy]] && [d hash] == [[d copy] hash]);
        NSMutableData *md = [NSMutableData dataWithLength:2];
        CHECK([md length] == 2 && ((unsigned char *)[md mutableBytes])[1] == 0);
        [md appendBytes:raw length:3];
        [md appendData:sub];
        CHECK([md length] == 8);
        [md setLength:4];
        CHECK([md length] == 4 && ((unsigned char *)[md mutableBytes])[2] == 1);
        const unsigned char patch[] = { 9, 9 };
        [md replaceBytesInRange:NSMakeRange(1, 2) withBytes:patch];
        CHECK(((unsigned char *)[md mutableBytes])[1] == 9 && ((unsigned char *)[md mutableBytes])[2] == 9);
        NSData *frozen = [md copy];
        [md appendBytes:raw length:1];
        CHECK([frozen length] == 4 && [md length] == 5);

        /* ---- NSDate ---- */
        NSDate *epoch = [NSDate dateWithTimeIntervalSince1970:0];
        CHECK([epoch timeIntervalSince1970] == 0 && [epoch timeIntervalSinceReferenceDate] == -978307200.0);
        CHECK([[epoch description] isEqualToString:@"1970-01-01 00:00:00 +0000"]);
        NSDate *later = [epoch dateByAddingTimeInterval:86400.5];
        CHECK([later timeIntervalSinceDate:epoch] == 86400.5);
        CHECK([epoch compare:later] == NSOrderedAscending && [later compare:epoch] == NSOrderedDescending && [epoch compare:epoch] == NSOrderedSame);
        CHECK([[epoch laterDate:later] isEqualToDate:later] && [[epoch earlierDate:later] isEqualToDate:epoch]);
        NSDate *now = [NSDate date];
        CHECK([now timeIntervalSince1970] > 1.7e9);                                         /* after 2023 */
        CHECK(fabs_d([now timeIntervalSinceNow]) < 5.0);
        CHECK([[NSDate dateWithTimeIntervalSinceReferenceDate:0] timeIntervalSince1970] == 978307200.0);

        /* ---- NSURL ---- */
        NSURL *u = [NSURL URLWithString:@"https://example.com:8080/a%20b/c.txt?x=1#frag"];
        CHECK(u != nil && [[u scheme] isEqualToString:@"https"] && [[u host] isEqualToString:@"example.com:8080"]);
        CHECK([[u path] isEqualToString:@"/a b/c.txt"] && [[u query] isEqualToString:@"x=1"] && ![u isFileURL]);
        CHECK([[u lastPathComponent] isEqualToString:@"c.txt"] && [[u pathExtension] isEqualToString:@"txt"]);
        CHECK([NSURL URLWithString:@"has space"] == nil && [NSURL URLWithString:@""] == nil);
        NSURL *f = [NSURL fileURLWithPath:@"/tmp/some dir/x.txt"];
        CHECK([f isFileURL] && [[f absoluteString] isEqualToString:@"file:///tmp/some%20dir/x.txt"] && [[f path] isEqualToString:@"/tmp/some dir/x.txt"]);
        CHECK([[[f URLByAppendingPathComponent:@"y"] path] isEqualToString:@"/tmp/some dir/x.txt/y"]);
        CHECK([f isEqual:[NSURL fileURLWithPath:@"/tmp/some dir/x.txt"]] && ![f isEqual:u]);

        /* ---- NSError ---- */
        NSError *e = [NSError errorWithDomain:@"CIDERDomain" code:7 userInfo:@{NSLocalizedDescriptionKey: @"it broke"}];
        CHECK([[e domain] isEqualToString:@"CIDERDomain"] && [e code] == 7 && [[e localizedDescription] isEqualToString:@"it broke"]);
        CHECK([[e description] hasPrefix:@"Error Domain=CIDERDomain Code=7"]);
        NSError *plain = [[NSError alloc] initWithDomain:NSPOSIXErrorDomain code:2 userInfo:nil];
        CHECK([[plain domain] isEqualToString:@"NSPOSIXErrorDomain"] && [plain userInfo] == nil && [[plain localizedDescription] length] > 0);

        /* ---- NSFileManager (confined to --root) ---- */
        NSFileManager *fm = [NSFileManager defaultManager];
        CHECK(fm == [NSFileManager defaultManager] && [[fm currentDirectoryPath] isEqualToString:@"/"]);
        BOOL isDir = NO;
        CHECK([fm fileExistsAtPath:@"/" isDirectory:&isDir] && isDir);
        CHECK(![fm fileExistsAtPath:@"/work/a.txt"]);
        NSError *err = nil;
        CHECK([fm createDirectoryAtPath:@"/work/sub" withIntermediateDirectories:YES attributes:nil error:&err] && err == nil);
        CHECK([fm fileExistsAtPath:@"/work/sub" isDirectory:&isDir] && isDir);
        CHECK(![fm createDirectoryAtPath:@"/work/sub" withIntermediateDirectories:NO attributes:nil error:&err] && [err code] == 516);
        err = nil;
        CHECK([fm createFileAtPath:@"/work/a.txt" contents:d attributes:nil]);
        CHECK([fm fileExistsAtPath:@"/work/a.txt" isDirectory:&isDir] && !isDir);
        NSData *read = [fm contentsAtPath:@"/work/a.txt"];
        CHECK(read != nil && [read isEqualToData:d]);
        CHECK([fm contentsAtPath:@"/work/missing.txt"] == nil);
        CHECK([fm createFileAtPath:@"/work/empty.txt" contents:nil attributes:nil] && [[fm contentsAtPath:@"/work/empty.txt"] length] == 0);
        NSArray *names = [fm contentsOfDirectoryAtPath:@"/work" error:&err];
        CHECK(err == nil && [names count] == 3 && [names[0] isEqualToString:@"a.txt"] && [names[1] isEqualToString:@"empty.txt"] && [names[2] isEqualToString:@"sub"]);
        CHECK([fm contentsOfDirectoryAtPath:@"/nope" error:&err] == nil && [err code] == 4);
        err = nil;
        CHECK(![fm removeItemAtPath:@"/work/ghost" error:&err] && [err code] == 4 && [[err domain] isEqualToString:NSCocoaErrorDomain]);
        CHECK([[err userInfo][NSFilePathErrorKey] isEqualToString:@"/work/ghost"]);
        err = nil;
        CHECK([fm removeItemAtPath:@"/work/a.txt" error:&err] && ![fm fileExistsAtPath:@"/work/a.txt"]);
        CHECK([fm removeItemAtPath:@"/work" error:&err] && ![fm fileExistsAtPath:@"/work"]);     /* directories are removed recursively */
        CHECK(![fm removeItemAtPath:@"/" error:&err]);                                           /* never the root itself */
        CHECK(![fm fileExistsAtPath:@"/../../etc/passwd"]);                                      /* ".." cannot leave the root: resolves to /etc/passwd INSIDE it */

        NSLog(@"extra checks done: %d failures", fails);
    }
    return fails;
}
