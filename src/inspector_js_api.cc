#include "base_object-inl.h"
#include "inspector_agent.h"
#include "inspector_io.h"
#include "memory_tracker-inl.h"
#include "util-inl.h"
#include "v8.h"
#include "v8-inspector.h"

#include <memory>

namespace node {
namespace inspector {
namespace {

using v8::Boolean;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Global;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::NewStringType;
using v8::Object;
using v8::String;
using v8::Uint32;
using v8::Value;

using v8_inspector::StringBuffer;
using v8_inspector::StringView;

std::unique_ptr<StringBuffer> ToProtocolString(Isolate* isolate,
                                               Local<Value> value) {
  TwoByteValue buffer(isolate, value);
  return StringBuffer::create(StringView(*buffer, buffer.length()));
}

class JSBindingsConnection : public AsyncWrap {
 public:
  class JSBindingsSessionDelegate : public InspectorSessionDelegate {
   public:
    JSBindingsSessionDelegate(Environment* env,
                              JSBindingsConnection* connection)
                              : env_(env),
                                connection_(connection) {
    }

    void SendMessageToFrontend(const v8_inspector::StringView& message)
        override {
      Isolate* isolate = env_->isolate();
      HandleScope handle_scope(isolate);
      Context::Scope context_scope(env_->context());
      MaybeLocal<String> v8string =
          String::NewFromTwoByte(isolate, message.characters16(),
                                 NewStringType::kNormal, message.length());
      Local<Value> argument = v8string.ToLocalChecked().As<Value>();
      connection_->OnMessage(argument);
    }

   private:
    Environment* env_;
    JSBindingsConnection* connection_;
  };

  JSBindingsConnection(Environment* env,
                       Local<Object> wrap,
                       Local<Function> callback)
                       : AsyncWrap(env, wrap, PROVIDER_INSPECTORJSBINDING),
                         callback_(env->isolate(), callback) {
    Agent* inspector = env->inspector_agent();
    session_ = inspector->Connect(std::make_unique<JSBindingsSessionDelegate>(
        env, this), false);
  }

  void OnMessage(Local<Value> value) {
    MakeCallback(callback_.Get(env()->isolate()), 1, &value);
  }

  static void New(const FunctionCallbackInfo<Value>& info) {
    Environment* env = Environment::GetCurrent(info);
    CHECK(info[0]->IsFunction());
    Local<Function> callback = info[0].As<Function>();
    new JSBindingsConnection(env, info.This(), callback);
  }

  void Disconnect() {
    session_.reset();
    delete this;
  }

  static void Disconnect(const FunctionCallbackInfo<Value>& info) {
    JSBindingsConnection* session;
    ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
    session->Disconnect();
  }

  static void Dispatch(const FunctionCallbackInfo<Value>& info) {
    Environment* env = Environment::GetCurrent(info);
    JSBindingsConnection* session;
    ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
    CHECK(info[0]->IsString());

    if (session->session_) {
      session->session_->Dispatch(
          ToProtocolString(env->isolate(), info[0])->string());
    }
  }

  void MemoryInfo(MemoryTracker* tracker) const override {
    tracker->TrackField("callback", callback_);
    tracker->TrackFieldWithSize(
        "session", sizeof(*session_), "InspectorSession");
  }

  SET_MEMORY_INFO_NAME(JSBindingsConnection)
  SET_SELF_SIZE(JSBindingsConnection)

 private:
  std::unique_ptr<InspectorSession> session_;
  Global<Function> callback_;
};

static bool InspectorEnabled(Environment* env) {
  Agent* agent = env->inspector_agent();
  return agent->IsActive();
}

void SetConsoleExtensionInstaller(const FunctionCallbackInfo<Value>& info) {
  auto env = Environment::GetCurrent(info);

  CHECK_EQ(info.Length(), 1);
  CHECK(info[0]->IsFunction());

  env->set_inspector_console_extension_installer(info[0].As<Function>());
}

void CallAndPauseOnStart(const FunctionCallbackInfo<v8::Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK_GT(args.Length(), 1);
  CHECK(args[0]->IsFunction());
  SlicedArguments call_args(args, /* start */ 2);
  env->inspector_agent()->PauseOnNextJavascriptStatement("Break on start");
  v8::MaybeLocal<v8::Value> retval =
      args[0].As<v8::Function>()->Call(env->context(), args[1],
                                       call_args.length(), call_args.out());
  if (!retval.IsEmpty()) {
    args.GetReturnValue().Set(retval.ToLocalChecked());
  }
}

void V8CallAndPauseOnStart(const FunctionCallbackInfo<v8::Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK_GT(args.Length(), 1);
  // CHECK(args[0]->IsFunction());


  // Create a string containing the JavaScript source code.
  // v8::Local<v8::String> source =
  //     v8::String::NewFromUtf8(env->isolate(), args[0],
  //                             v8::NewStringType::kNormal)
  //         .ToLocalChecked();


  SlicedArguments call_args(args, /* start */ 2);


  env->inspector_agent()->PauseOnNextJavascriptStatement("Break on start");

  // Compile the source code.
  v8::Local<v8::Script> script =
      v8::Script::Compile(env->context(), args[0].As<v8::String>()).ToLocalChecked();

  // Run the script to get the result.
  v8::MaybeLocal<v8::Value> retval = script->Run(env->context()).ToLocalChecked();

  // v8::MaybeLocal<v8::Value> retval =
  //     args[0].As<v8::Function>()->Call(env->context(), args[1],
  //                                      call_args.length(), call_args.out());

  if (!retval.IsEmpty()) {
    args.GetReturnValue().Set(retval.ToLocalChecked());
  }
}

void InspectorConsoleCall(const FunctionCallbackInfo<Value>& info) {
  Environment* env = Environment::GetCurrent(info);
  Isolate* isolate = env->isolate();
  Local<Context> context = isolate->GetCurrentContext();
  CHECK_GE(info.Length(), 2);
  SlicedArguments call_args(info, /* start */ 2);
  if (InspectorEnabled(env)) {
    Local<Value> inspector_method = info[0];
    CHECK(inspector_method->IsFunction());
    if (!env->is_in_inspector_console_call()) {
      env->set_is_in_inspector_console_call(true);
      MaybeLocal<Value> ret =
          inspector_method.As<Function>()->Call(context,
                                                info.Holder(),
                                                call_args.length(),
                                                call_args.out());
      env->set_is_in_inspector_console_call(false);
      if (ret.IsEmpty())
        return;
    }
  }

  Local<Value> node_method = info[1];
  CHECK(node_method->IsFunction());
  node_method.As<Function>()->Call(context,
                                   info.Holder(),
                                   call_args.length(),
                                   call_args.out()).FromMaybe(Local<Value>());
}

static void* GetAsyncTask(int64_t asyncId) {
  // The inspector assumes that when other clients use its asyncTask* API,
  // they use real pointers, or at least something aligned like real pointer.
  // In general it means that our task_id should always be even.
  //
  // On 32bit platforms, the 64bit asyncId would get truncated when converted
  // to a 32bit pointer. However, the javascript part will never enable
  // the async_hook on 32bit platforms, therefore the truncation will never
  // happen in practice.
  return reinterpret_cast<void*>(asyncId << 1);
}

template <void (Agent::*asyncTaskFn)(void*)>
static void InvokeAsyncTaskFnWithId(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsNumber());
  int64_t task_id = args[0]->IntegerValue(env->context()).FromJust();
  (env->inspector_agent()->*asyncTaskFn)(GetAsyncTask(task_id));
}

static void AsyncTaskScheduledWrapper(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsString());
  Local<String> task_name = args[0].As<String>();
  String::Value task_name_value(args.GetIsolate(), task_name);
  StringView task_name_view(*task_name_value, task_name_value.length());

  CHECK(args[1]->IsNumber());
  int64_t task_id = args[1]->IntegerValue(env->context()).FromJust();
  void* task = GetAsyncTask(task_id);

  CHECK(args[2]->IsBoolean());
  bool recurring = args[2]->BooleanValue(args.GetIsolate());

  env->inspector_agent()->AsyncTaskScheduled(task_name_view, task, recurring);
}

static void RegisterAsyncHookWrapper(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsFunction());
  Local<Function> enable_function = args[0].As<Function>();
  CHECK(args[1]->IsFunction());
  Local<Function> disable_function = args[1].As<Function>();
  env->inspector_agent()->RegisterAsyncHook(env->isolate(),
    enable_function, disable_function);
}

void IsEnabled(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  args.GetReturnValue().Set(InspectorEnabled(env));
}

void Open(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Agent* agent = env->inspector_agent();

  if (args.Length() > 0 && args[0]->IsUint32()) {
    uint32_t port = args[0].As<Uint32>()->Value();
    agent->host_port()->set_port(static_cast<int>(port));
  }

  if (args.Length() > 1 && args[1]->IsString()) {
    Utf8Value host(env->isolate(), args[1].As<String>());
    agent->host_port()->set_host(*host);
  }

  agent->StartIoThread();
}

void WaitForDebugger(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Agent* agent = env->inspector_agent();
  if (agent->IsActive())
    agent->WaitForConnect();
  args.GetReturnValue().Set(agent->IsActive());
}

void Url(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  std::string url = env->inspector_agent()->GetWsUrl();
  if (url.length() == 0) {
    return;
  }
  args.GetReturnValue().Set(OneByteString(env->isolate(), url.c_str()));
}

void Initialize(Local<Object> target, Local<Value> unused,
                Local<Context> context, void* priv) {
  Environment* env = Environment::GetCurrent(context);

  v8::Local<v8::Function> consoleCallFunc =
      env->NewFunctionTemplate(InspectorConsoleCall, v8::Local<v8::Signature>(),
                               v8::ConstructorBehavior::kThrow,
                               v8::SideEffectType::kHasSideEffect)
          ->GetFunction(context)
          .ToLocalChecked();
  auto name_string = FIXED_ONE_BYTE_STRING(env->isolate(), "consoleCall");
  target->Set(context, name_string, consoleCallFunc).Check();
  consoleCallFunc->SetName(name_string);

  env->SetMethod(
      target, "setConsoleExtensionInstaller", SetConsoleExtensionInstaller);
  env->SetMethod(target, "callAndPauseOnStart", CallAndPauseOnStart);
  env->SetMethod(target, "v8callAndPauseOnStart", V8CallAndPauseOnStart);
  env->SetMethod(target, "open", Open);
  env->SetMethodNoSideEffect(target, "url", Url);
  env->SetMethod(target, "waitForDebugger", WaitForDebugger);

  env->SetMethod(target, "asyncTaskScheduled", AsyncTaskScheduledWrapper);
  env->SetMethod(target, "asyncTaskCanceled",
      InvokeAsyncTaskFnWithId<&Agent::AsyncTaskCanceled>);
  env->SetMethod(target, "asyncTaskStarted",
      InvokeAsyncTaskFnWithId<&Agent::AsyncTaskStarted>);
  env->SetMethod(target, "asyncTaskFinished",
      InvokeAsyncTaskFnWithId<&Agent::AsyncTaskFinished>);

  env->SetMethod(target, "registerAsyncHook", RegisterAsyncHookWrapper);
  env->SetMethodNoSideEffect(target, "isEnabled", IsEnabled);

  auto conn_str = FIXED_ONE_BYTE_STRING(env->isolate(), "Connection");
  Local<FunctionTemplate> tmpl =
      env->NewFunctionTemplate(JSBindingsConnection::New);
  tmpl->InstanceTemplate()->SetInternalFieldCount(1);
  tmpl->SetClassName(conn_str);
  tmpl->Inherit(AsyncWrap::GetConstructorTemplate(env));
  env->SetProtoMethod(tmpl, "dispatch", JSBindingsConnection::Dispatch);
  env->SetProtoMethod(tmpl, "disconnect", JSBindingsConnection::Disconnect);
  target
      ->Set(env->context(), conn_str,
            tmpl->GetFunction(env->context()).ToLocalChecked())
      .ToChecked();
}

}  // namespace
}  // namespace inspector
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(inspector,
                                  node::inspector::Initialize)
