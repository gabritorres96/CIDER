; A frame of ~20 KB in a function carrying the attribute Apple's clang puts on macOS functions with large frames: LLVM itself emits the call to
; ____chkstk_darwin (size in RAX, then `sub rax, rsp`). Written as IR because the available driver does not add the attribute on its own.
; 7 + 9 (first and last byte of the buffer, volatile) + 26 = 42.
target triple = "x86_64-apple-macosx10.15.0"

define i32 @big(i32 %seed) #0 {
  %buf = alloca [20000 x i8], align 16
  %p0 = getelementptr [20000 x i8], ptr %buf, i64 0, i64 0
  %pend = getelementptr [20000 x i8], ptr %buf, i64 0, i64 19999
  store volatile i8 7, ptr %p0
  store volatile i8 9, ptr %pend
  %a = load volatile i8, ptr %p0
  %b = load volatile i8, ptr %pend
  %az = zext i8 %a to i32
  %bz = zext i8 %b to i32
  %s = add i32 %az, %bz
  %r = add i32 %s, %seed
  ret i32 %r
}

define i32 @main() {
  %r = call i32 @big(i32 26)
  ret i32 %r
}

attributes #0 = { noinline nounwind "probe-stack"="___chkstk_darwin" }
