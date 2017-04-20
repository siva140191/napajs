#include "zone-wrap.h"

#include "node-async-handler.h"

#include <napa.h>
#include <napa/v8-helpers.h>

#include <sstream>
#include <vector>

using namespace napa::binding;
using namespace napa::v8_helpers;

NAPA_DEFINE_PERSISTENT_CONSTRUCTOR(ZoneWrap);

// Forward declaration.
static v8::Local<v8::Object> CreateResponseObject(const napa::ExecuteResponse& response);
template <typename Func>
static void CreateRequestAndExecute(v8::Local<v8::Object> obj, Func&& func);

ZoneWrap::ZoneWrap(std::unique_ptr<napa::ZoneProxy> zoneProxy) : _zoneProxy(std::move(zoneProxy)) {}

void ZoneWrap::Init(v8::Isolate* isolate) {
    // Prepare constructor template.
    v8::Local<v8::FunctionTemplate> functionTemplate = v8::FunctionTemplate::New(isolate, NewCallback);
    functionTemplate->SetClassName(MakeV8String(isolate, _exportName));
    functionTemplate->InstanceTemplate()->SetInternalFieldCount(1);

    // Prototypes.
    NAPA_SET_PROTOTYPE_METHOD(functionTemplate, "broadcast", Broadcast);
    NAPA_SET_PROTOTYPE_METHOD(functionTemplate, "broadcastSync", BroadcastSync);
    NODE_SET_PROTOTYPE_METHOD(functionTemplate, "execute", Execute);
    NODE_SET_PROTOTYPE_METHOD(functionTemplate, "executeSync", ExecuteSync);

    // Set persistent constructor into V8.
    NAPA_SET_PERSISTENT_CONSTRUCTOR(_exportName, functionTemplate->GetFunction());
}

void ZoneWrap::NewInstance(ConstructorType type, const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    int argc = args.Length() + 1;
    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(argc);
    argv.emplace_back(v8::Uint32::NewFromUnsigned(isolate, static_cast<uint32_t>(type)));
    for (int i = 1; i < argc; ++i) {
        argv.emplace_back(args[i-1]);
    }

    auto constructor = NAPA_GET_PERSISTENT_CONSTRUCTOR(_exportName, ZoneWrap);
    auto context = isolate->GetCurrentContext();
    auto instance = constructor->NewInstance(context, argc, argv.data()).ToLocalChecked();

    args.GetReturnValue().Set(instance);
}

void ZoneWrap::NewCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = v8::Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    JS_ASSERT(isolate, args.IsConstructCall(), "Only constructor call is allowed");

    std::unique_ptr<napa::ZoneProxy> zoneProxy;
    auto constructorType = static_cast<ConstructorType>(args[0]->Uint32Value(context).FromJust());
    if (constructorType == ConstructorType::CREATE) {
        CHECK_ARG(isolate, args[1]->IsString(), "first argument to createZone must be a string");
        v8::String::Utf8Value zoneId(args[1]->ToString());

        std::stringstream ss;
        if (args.Length() > 2) {
            CHECK_ARG(isolate, args[2]->IsObject(), "second argument to createZone must be an object");
            v8::Local<v8::Object> settingsObj = args[2]->ToObject(context).ToLocalChecked();

            auto settingsMap = napa::v8_helpers::V8ObjectToMap<std::string>(isolate, settingsObj);

            for (const auto& kv : settingsMap) {
                ss << " --" << kv.first << " " << kv.second;
            }
        }

        zoneProxy = std::make_unique<napa::ZoneProxy>(*zoneId, ss.str());
    } else { // ConstructorType::GET
        CHECK_ARG(isolate, args[1]->IsString(), "first argument to getZone must be a string");
        v8::String::Utf8Value zoneId(args[1]->ToString());

        zoneProxy = napa::ZoneProxy::Get(*zoneId);
    }

    auto obj = new ZoneWrap(std::move(zoneProxy));

    obj->Wrap(args.This());
    args.GetReturnValue().Set(args.This());
}

void ZoneWrap::Broadcast(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = v8::Isolate::GetCurrent();

    CHECK_ARG(isolate, args[0]->IsString(), "first argument to zone.broadcast must be the javascript source");
    CHECK_ARG(isolate, args[1]->IsFunction(), "second argument to zone.broadcast must be the callback");
    
    v8::String::Utf8Value source(args[0]->ToString());
    auto callback = v8::Local<v8::Function>::Cast(args[1]);

    auto handler = NodeAsyncHandler<NapaResponseCode>::New(
        isolate,
        callback,
        [isolate](const auto& responseCode) {
            std::vector<v8::Local<v8::Value>> res;
            res.push_back(v8::Uint32::NewFromUnsigned(isolate, responseCode));
            return res;
        }
    );

    auto wrap = ObjectWrap::Unwrap<ZoneWrap>(args.Holder());
    wrap->_zoneProxy->Broadcast(*source, [handler](NapaResponseCode responseCode) {
        handler->DispatchCallback(responseCode);
    });
}

void ZoneWrap::BroadcastSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = v8::Isolate::GetCurrent();

    CHECK_ARG(isolate, args[0]->IsString(), "first argument to zone.broadcastSync must be the javascript source");

    v8::String::Utf8Value source(args[0]->ToString());

    auto wrap = ObjectWrap::Unwrap<ZoneWrap>(args.Holder());
    auto responseCode = wrap->_zoneProxy->BroadcastSync(*source);

    args.GetReturnValue().Set(v8::Uint32::NewFromUnsigned(isolate, responseCode));
}

void ZoneWrap::Execute(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = v8::Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    CHECK_ARG(isolate, args[0]->IsObject(), "first argument to zone.execute must be the execution request object");
    CHECK_ARG(isolate, args[1]->IsFunction(), "second argument to zone.execute must be the callback");

    auto callback = v8::Local<v8::Function>::Cast(args[1]);

    auto handler = NodeAsyncHandler<napa::ExecuteResponse>::New(isolate, callback, [](const auto& response) {
        std::vector<v8::Local<v8::Value>> res;
        res.push_back(CreateResponseObject(response));
        return res;
    });

    auto wrap = ObjectWrap::Unwrap<ZoneWrap>(args.Holder());

    CreateRequestAndExecute(args[0]->ToObject(), [wrap, handler](const napa::ExecuteRequest& request) {
        wrap->_zoneProxy->Execute(request, [handler](napa::ExecuteResponse response) {
            handler->DispatchCallback(std::move(response));
        });
    });
}

void ZoneWrap::ExecuteSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = v8::Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    CHECK_ARG(isolate, args[0]->IsObject(), "first argument to zone.execute must be the execution request object");

    auto wrap = ObjectWrap::Unwrap<ZoneWrap>(args.Holder());

    napa::ExecuteResponse response;
    CreateRequestAndExecute(args[0]->ToObject(), [wrap, &response, &args](const napa::ExecuteRequest& request) {
        response = wrap->_zoneProxy->ExecuteSync(request);
        args.GetReturnValue().Set(CreateResponseObject(response));
    });
}

static v8::Local<v8::Object> CreateResponseObject(const napa::ExecuteResponse& response) {
    auto isolate = v8::Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    auto responseObject = v8::Object::New(isolate);

    (void)responseObject->CreateDataProperty(
        context,
        MakeV8String(isolate, "code"),
        v8::Uint32::NewFromUnsigned(isolate, response.code));

    (void)responseObject->CreateDataProperty(
        context,
        MakeV8String(isolate, "errorMessage"),
        MakeV8String(isolate, response.errorMessage));

    v8::Local<v8::Value> returnValue;
    if (response.returnValue.empty()) {
        returnValue = v8::String::Empty(isolate);
    } else {
        // If parse fails, return the string as is.
        returnValue = MakeV8String(isolate, response.returnValue);

        v8::TryCatch tryCatch;
        returnValue = v8::JSON::Parse(isolate, v8::Local<v8::String>::Cast(returnValue)).FromMaybe(returnValue);
    }

    (void)responseObject->CreateDataProperty(
        context,
        MakeV8String(isolate, "returnValue"),
        returnValue);

    // TODO @asib: Set the transport context to the JS response

    return responseObject;
}

template <typename Func>
static void CreateRequestAndExecute(v8::Local<v8::Object> obj, Func&& func) {
    auto isolate = v8::Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();
    
    napa::ExecuteRequest request;

    // module property is optional in a request
    Utf8String module;
    auto maybe = obj->Get(context, MakeV8String(isolate, "module"));
    if (!maybe.IsEmpty()) {
        module = Utf8String(maybe.ToLocalChecked());
        request.module = NAPA_STRING_REF_WITH_SIZE(module.Data(), module.Length());
    }

    // function property is mandatory in a request
    maybe = obj->Get(context, MakeV8String(isolate, "function"));
    CHECK_ARG(isolate, !maybe.IsEmpty(), "function property is missing in execution request object");
    auto functionValue = maybe.ToLocalChecked();
    CHECK_ARG(isolate, functionValue->IsString(), "function property in execution request object must be a string");
    v8::String::Utf8Value function(functionValue->ToString());
    request.function = NAPA_STRING_REF_WITH_SIZE(*function, static_cast<size_t>(function.length()));

    // arguments are optional in a request
    maybe = obj->Get(context, MakeV8String(isolate, "arguments"));
    if (!maybe.IsEmpty()) {
        auto arguments = V8ArrayToVector<Utf8String>(isolate, v8::Local<v8::Array>::Cast(maybe.ToLocalChecked()));

        request.arguments.reserve(arguments.size());
        for (const auto& arg : arguments) {
            request.arguments.emplace_back(NAPA_STRING_REF_WITH_SIZE(arg.Data(), arg.Length()));
        }
    }

    // timeout argument is optional
    maybe = obj->Get(context, MakeV8String(isolate, "timeout"));
    if (!maybe.IsEmpty()) {
        request.timeout = maybe.ToLocalChecked()->Uint32Value(context).FromJust();
    }

    // TODO @asib: Get the transport context from JS request object and set in request transport context

    // Execute
    func(request);
}