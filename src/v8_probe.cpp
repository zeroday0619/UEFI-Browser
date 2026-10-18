#include "libplatform/libplatform.h"
#include "v8.h"
#include <memory>
extern "C" void graphics_log(const char *);
extern "C" bool firmware_entropy(unsigned char *, size_t);
extern "C" int run_v8_probe() {
  graphics_log("ENGINE: V8 initialization begin\n");
  v8::V8::SetFlagsFromString("--jitless --single-threaded --no-lazy");
  v8::V8::SetEntropySource(firmware_entropy);
  auto platform = v8::platform::NewSingleThreadedDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  if (!v8::V8::Initialize())
    return 1;
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams parameters;
  parameters.array_buffer_allocator = allocator.get();
  parameters.constraints.set_max_old_generation_size_in_bytes(64 * 1024 * 1024);
  parameters.constraints.set_max_young_generation_size_in_bytes(8 * 1024 *
                                                                1024);
  v8::Isolate *isolate = v8::Isolate::New(parameters);
  if (!isolate)
    return 2;
  bool success = false;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    v8::TryCatch exceptions(isolate);
    const char *source =
        "globalThis.completed = 0; Promise.resolve(42).then(v => completed=v); "
        "([1,2,3].map(v => v*v).reduce((a,b)=>a+b,0) === 14) && (21n*2n === "
        "42n) && (/^한글$/u.test('한글'))";
    v8::Local<v8::String> text;
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    success = v8::String::NewFromUtf8(isolate, source).ToLocal(&text) &&
              v8::Script::Compile(context, text).ToLocal(&script) &&
              script->Run(context).ToLocal(&result) && result->IsTrue();
    isolate->PerformMicrotaskCheckpoint();
    auto key = v8::String::NewFromUtf8Literal(isolate, "completed");
    v8::Local<v8::Value> completed;
    success = success &&
              context->Global()->Get(context, key).ToLocal(&completed) &&
              completed->Int32Value(context).FromMaybe(0) == 42;
    if (exceptions.HasCaught()) {
      v8::String::Utf8Value error(isolate, exceptions.Exception());
      if (*error)
        graphics_log(*error);
    }
    isolate->LowMemoryNotification();
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  graphics_log(success
                   ? "PASS: V8 JavaScript, BigInt, Unicode, Promise and GC\n"
                   : "FAIL: V8 evaluation\n");
  return success ? 0 : 3;
}
