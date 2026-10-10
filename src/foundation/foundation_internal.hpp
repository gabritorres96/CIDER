#pragma once
// CIDER's Foundation subset: host-implemented classes registered in the Objective-C runtime (see docs/FOUNDATION.md). This is NOT Apple's
// Foundation and does not reuse any of its code: it is written from the public behaviour of the classes, covers only what is listed in
// the documentation, and traps (ends the guest with a message) where Apple's Foundation would raise an exception.
//
// Instances of the payload classes (NSString, NSNumber, NSArray, NSDictionary...) are ordinary Objective-C objects: [isa][payload id]
// followed by the ivars of a guest subclass; the payload (std::string, vector...) lives in a host table and is released by -dealloc.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cider/objc.hpp"

namespace cider::foundation {

using objc::Addr;
using objc::Class;
using objc::Runtime;

struct Payload { virtual ~Payload() = default; };
struct StringPayload : Payload { std::string utf8; };
struct NumberPayload : Payload {
    enum class Kind { Bool, Int, Double } kind = Kind::Int;
    int64_t i = 0;
    double d = 0;
};
struct ArrayPayload : Payload { std::vector<Addr> items; };
struct DictPayload : Payload {
    struct Entry { uint64_t hash; Addr key, value; };
    std::vector<Entry> entries;        // insertion order; lookup is a hash-filtered linear scan (documented O(n))
};
struct PoolPayload : Payload { Addr token = 0; };
struct DataPayload : Payload { std::vector<uint8_t> bytes; };
struct DatePayload : Payload { double since_reference = 0; };          // seconds since 2001-01-01 00:00:00 UTC
struct URLPayload : Payload { std::string absolute, scheme, host, path, query; };
struct ErrorPayload : Payload { Addr domain = 0, userinfo = 0; int64_t code = 0; };

struct Classes {
    Class *object = nullptr, *string = nullptr, *constant_string = nullptr, *number = nullptr, *array = nullptr, *marray = nullptr,
          *dict = nullptr, *mdict = nullptr, *pool = nullptr, *mstring = nullptr, *data = nullptr, *mdata = nullptr, *date = nullptr, *url = nullptr,
          *error = nullptr, *filemgr = nullptr;
    std::vector<Class*> extra_payload;      // further classes whose instances keep a payload id at +8
};
Classes& classes();
Runtime& rt();

// ---- payload store -----------------------------------------------------------------------------------------------------------------------
uint64_t store_add(std::unique_ptr<Payload> p);
Payload* store_get(uint64_t id);
void store_drop(uint64_t id);
size_t store_size();

// ---- objects ---------------------------------------------------------------------------------------------------------------------------------
Addr make_object(Class* cls, std::unique_ptr<Payload> payload);   // new instance, retain count 1
Addr autoreleased(Addr obj);
Payload* payload_of(Addr obj);                                    // nullptr when `obj` is nil, not live, or has no payload yet
uint64_t* payload_slot(Addr obj);                                 // address of the payload id inside the object (nullptr if invalid)
bool is_kind(Addr obj, Class* cls);

// strings
bool string_view_of(Addr obj, std::string_view& out);             // NSString, NSConstantString (and subclasses created here)
Addr new_string(std::string utf8);                                // retained (+1)
Addr new_string_autoreleased(std::string utf8);
size_t utf16_length(std::string_view utf8);
std::u16string to_u16(std::string_view utf8);
std::string from_u16(const std::u16string& units);
bool valid_utf8(std::string_view s);
std::string describe(Addr obj);                                   // -description of any object as UTF-8 ("(null)" for nil)

// numbers
bool number_value(Addr obj, NumberPayload& out);
Addr new_number(const NumberPayload& n, bool autorelease);

// arrays / dictionaries
ArrayPayload* array_of(Addr obj);
DictPayload* dict_of(Addr obj);

Addr new_array(std::vector<Addr> items, bool mutable_, bool retain_items);   // autoreleased; retain_items=false adopts the +1 the caller holds

// errors: Apple raises an NSException; CIDER ends the guest with the message
void g_rt_set(Runtime* rt);
void raise(const std::string& name, const std::string& reason);   // traps the guest, returns so the native method can return 0

void install_nsobject(Runtime& rt);
void install_nsstring(Runtime& rt);
void install_nsnumber(Runtime& rt);
void install_nsarray(Runtime& rt);
void install_nsdictionary(Runtime& rt);
void install_misc(Runtime& rt);
void install_nsextras(Runtime& rt);       // NSMutableString, NSData, NSMutableData, NSDate, NSURL, NSError, NSFileManager

bool format_with_objects(const std::string& format, abi::ArgReader& args, std::string& out);   // printf with %@ (NSLog, stringWithFormat:)
Addr immortal_string(const std::string& utf8);   // NSString in the runtime arena that is never released (framework constants)

}  // namespace cider::foundation
