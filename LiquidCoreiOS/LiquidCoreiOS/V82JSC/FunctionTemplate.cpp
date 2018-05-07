//
//  FunctionTemplate.cpp
//  LiquidCoreiOS
//
//  Created by Eric Lange on 2/9/18.
//  Copyright © 2018 LiquidPlayer. All rights reserved.
//

#include "V82JSC.h"

using namespace v8;

#define PROXY_CALL_MAGIC_NUMBER ((void*)0x0133707eel)

Local<Signature> Signature::New(Isolate* isolate,
                                Local<FunctionTemplate> receiver)
{
    SignatureImpl * signature = static_cast<SignatureImpl *>(HeapAllocator::Alloc(reinterpret_cast<IsolateImpl*>(isolate), sizeof(SignatureImpl)));
    signature->m_template = V82JSC::ToImpl<FunctionTemplateImpl>(receiver);
    
    return V82JSC::MakeLocal<Signature>(isolate, signature);
}

Local<AccessorSignature> AccessorSignature::New(Isolate* isolate,
                                                Local<FunctionTemplate> receiver)
{
    SignatureImpl * signature = static_cast<SignatureImpl *>(HeapAllocator::Alloc(reinterpret_cast<IsolateImpl*>(isolate), sizeof(SignatureImpl)));
    signature->m_template = V82JSC::ToImpl<FunctionTemplateImpl>(receiver);
    
    return V82JSC::MakeLocal<AccessorSignature>(isolate, signature);
}

/** Get a template included in the snapshot by index. */
MaybeLocal<FunctionTemplate> FunctionTemplate::FromSnapshot(Isolate* isolate,
                                                            size_t index)
{
    return MaybeLocal<FunctionTemplate>();
}

Local<FunctionTemplate> FunctionTemplate::New(Isolate* isolate, FunctionCallback callback,
                                          Local<Value> data,
                                          Local<Signature> signature, int length,
                                          ConstructorBehavior behavior)
{
    Local<Context> context = V82JSC::OperatingContext(isolate);
    FunctionTemplateImpl *templ = (FunctionTemplateImpl*) TemplateImpl::New(isolate, sizeof(FunctionTemplateImpl));
    V82JSC::Map(templ)->set_instance_type(v8::internal::FUNCTION_TEMPLATE_INFO_TYPE);

    templ->m_name = std::string();
    templ->m_callback = callback;
    templ->m_signature = nullptr;
    if (*signature) {
        templ->m_signature = V82JSC::ToImpl<SignatureImpl>(signature);
    }
    templ->m_behavior = behavior;
    templ->m_length = length;
    if (!*data) {
        data = Undefined(isolate);
    }
    templ->m_data = V82JSC::ToJSValueRef<Value>(data, isolate);
    JSValueProtect(V82JSC::ToContextRef(context), templ->m_data);
    templ->m_functions = std::map<const ContextImpl*, JSObjectRef>();
    
    return V82JSC::MakeLocal<FunctionTemplate>(isolate, templ);
}

/**
 * Creates a function template backed/cached by a private property.
 */
Local<FunctionTemplate> FunctionTemplate::NewWithCache(
                                            Isolate* isolate, FunctionCallback callback,
                                            Local<Private> cache_property, Local<Value> data,
                                            Local<Signature> signature, int length)
{
    return New(isolate, callback, data, signature, length);
}

/*
 * \code
 *   FunctionTemplate Parent  -> Parent() . prototype -> { }
 *     ^                                                  ^
 *     | Inherit(Parent)                                  | .__proto__
 *     |                                                  |
 *   FunctionTemplate Child   -> Child()  . prototype -> { }
 * \endcode
 *
 * A FunctionTemplate 'Child' inherits from 'Parent', the prototype
 * object of the Child() function has __proto__ pointing to the
 * Parent() function's prototype object. An instance of the Child
 * function has all properties on Parent's instance templates.
 */

/** Returns the unique function instance in the current execution context.*/
MaybeLocal<Function> FunctionTemplate::GetFunction(Local<Context> context)
{
    FunctionTemplateImpl *impl = V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(this);
    const ContextImpl *ctx = V82JSC::ToContextImpl(context);
    IsolateImpl* iso = V82JSC::ToIsolateImpl(ctx);

    JSObjectRef function = impl->m_functions[ctx];
    if (function) {
        return ValueImpl::New(ctx, function).As<Function>();
    }

    JSStringRef generic_function_name = JSStringCreateWithUTF8CString("generic_function");
    JSStringRef empty_body = JSStringCreateWithUTF8CString("");

    JSValueRef excp = nullptr;
    JSObjectRef generic_function = JSObjectMakeFunction(V82JSC::ToContextRef(context),
                                    generic_function_name, 0, 0, empty_body, 0, 0, &excp);
    assert(excp == nullptr);
    JSStringRelease(generic_function_name);
    JSStringRelease(empty_body);
    JSValueRef generic_function_prototype = V82JSC::exec(V82JSC::ToContextRef(context),
                                                         "return Object.getPrototypeOf(_1)", 1,
                                                         &generic_function);

    TemplateWrap *wrap = new TemplateWrap();
    wrap->m_template = impl;
    wrap->m_isolate = iso;
    
    JSClassDefinition function_def = kJSClassDefinitionEmpty;
    function_def.callAsFunction = TemplateImpl::callAsFunctionCallback;
    function_def.className = "function_proxy";
    JSClassRef function_class = JSClassCreate(&function_def);
    JSObjectRef function_proxy = JSObjectMake(ctx->m_ctxRef, function_class, (void*)wrap);
    JSObjectSetPrototype(ctx->m_ctxRef, function_proxy, generic_function_prototype);
    JSClassRelease(function_class);

    JSClassDefinition constructor_def = kJSClassDefinitionEmpty;
    constructor_def.callAsFunction = FunctionTemplateImpl::callAsConstructorCallback;
    constructor_def.className = "constructor_proxy";
    JSClassRef constructor_class = JSClassCreate(&constructor_def);
    JSObjectRef constructor_proxy = JSObjectMake(ctx->m_ctxRef, constructor_class, (void*)wrap);
    JSObjectSetPrototype(ctx->m_ctxRef, constructor_proxy, generic_function_prototype);
    JSClassRelease(constructor_class);
    
    static const char *proxy_function_template =
    "function name () { "
    "    if (new.target) { "
    "        return name_ctor.call(this, new.target == name, ...arguments); "
    "    } else { "
    "        return name_func.call(this, ...arguments); "
    "    } "
    "}; "
    "if (name_length) { "
    "    Object.defineProperty(name, 'length', {value: name_length}); "
    "} "
    "return name; ";
    
    auto ReplaceStringInPlace = [](std::string& subject, const std::string& search,
                              const std::string& replace) {
        size_t pos = 0;
        while((pos = subject.find(search, pos)) != std::string::npos) {
            subject.replace(pos, search.length(), replace);
            pos += replace.length();
        }
    };
    
    std::string proxy_function_body = proxy_function_template;
    const char *sname = impl->m_name.length() ? impl->m_name.c_str() : "Function";
    ReplaceStringInPlace(proxy_function_body, "name", sname);
    
    JSStringRef name = JSStringCreateWithUTF8CString("proxy_function");
    JSStringRef body = JSStringCreateWithUTF8CString(proxy_function_body.c_str());
    JSStringRef paramNames[] = {
        JSStringCreateWithUTF8CString(std::string(std::string(sname) + "_func").c_str()),
        JSStringCreateWithUTF8CString(std::string(std::string(sname) + "_ctor").c_str()),
        JSStringCreateWithUTF8CString(std::string(std::string(sname) + "_length").c_str()),
    };
    JSValueRef exp = nullptr;
    JSObjectRef get_proxy = JSObjectMakeFunction(V82JSC::ToContextRef(context),
                                                 name,
                                                 sizeof paramNames / sizeof (JSStringRef),
                                                 paramNames,
                                                 body, 0, 0, &exp);
    assert(exp==nullptr);
    JSStringRelease(name);
    JSStringRelease(body);
    for (int i=0; i < sizeof paramNames / sizeof (JSStringRef); i++) {
        JSStringRelease(paramNames[i]);
    }
    JSValueRef params[] = {
        function_proxy,
        constructor_proxy,
        JSValueMakeNumber(ctx->m_ctxRef, impl->m_length)
    };
    function = (JSObjectRef) JSObjectCallAsFunction(V82JSC::ToContextRef(context),
                                                    get_proxy, 0, sizeof params / sizeof (JSValueRef), params, &exp);
    assert(exp==nullptr);
    
    LocalException exception(iso);

    MaybeLocal<Object> thizo = impl->InitInstance(context, function, exception);
    if (thizo.IsEmpty()) {
        return MaybeLocal<Function>();
    }
    JSStringRef sprototype = JSStringCreateWithUTF8CString("prototype");
    JSValueRef prototype_property = 0;
    if (impl->m_prototype_template || impl->m_parent) {
        Local<ObjectTemplate> prototype_template = V82JSC::MakeLocal<FunctionTemplate>(iso, impl)->PrototypeTemplate();
        MaybeLocal<Object> prototype = prototype_template->NewInstance(context);
        if (prototype.IsEmpty()) {
            return MaybeLocal<Function>();
        }
        prototype_property = V82JSC::ToJSValueRef(prototype.ToLocalChecked(), context);
        JSObjectSetProperty(V82JSC::ToContextRef(context), function, sprototype,
                            prototype_property, kJSPropertyAttributeDontEnum/*|kJSPropertyAttributeReadOnly*/, 0);
    }
    if (impl->m_parent) {
        MaybeLocal<Function> parentFunc = V82JSC::MakeLocal<FunctionTemplate>(iso, impl->m_parent)->GetFunction(context);
        if (parentFunc.IsEmpty()) {
            JSStringRelease(sprototype);
            return MaybeLocal<Function>();
        }
        JSValueRef parentFuncRef = V82JSC::ToJSValueRef<Function>(parentFunc.ToLocalChecked(), context);
        JSValueRef parentFuncPrototype = JSObjectGetProperty(ctx->m_ctxRef, (JSObjectRef)parentFuncRef, sprototype, 0);
        if (prototype_property) {
            JSObjectSetPrototype(ctx->m_ctxRef, (JSObjectRef)prototype_property, parentFuncPrototype);
        }
    }
    JSStringRelease(sprototype);

    impl->m_functions[ctx] = function;
    JSValueProtect(ctx->m_ctxRef, function);
    return ValueImpl::New(ctx, function).As<Function>();
}

/**
 * Similar to Context::NewRemoteContext, this creates an instance that
 * isn't backed by an actual object.
 *
 * The InstanceTemplate of this FunctionTemplate must have access checks with
 * handlers installed.
 */
MaybeLocal<Object> FunctionTemplate::NewRemoteInstance()
{
    assert(0);
    return MaybeLocal<Object>();
}

/**
 * Set the call-handler callback for a FunctionTemplate.  This
 * callback is called whenever the function created from this
 * FunctionTemplate is called.
 */
void FunctionTemplate::SetCallHandler(FunctionCallback callback,
                    Local<Value> data)
{
    FunctionTemplateImpl *impl =  V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(this);
    IsolateImpl* iso = V82JSC::ToIsolateImpl(impl);
    impl->m_callback = callback;
    if (!*data) {
        data = Undefined(V82JSC::ToIsolate(iso));
    }
    impl->m_data = V82JSC::ToJSValueRef(data, V82JSC::ToIsolate(iso));
}

/** Set the predefined length property for the FunctionTemplate. */
void FunctionTemplate::SetLength(int length)
{
    FunctionTemplateImpl *impl =  V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(this);
    impl->m_length = length;
}

/** Get the InstanceTemplate. */
Local<ObjectTemplate> FunctionTemplate::InstanceTemplate()
{
    FunctionTemplateImpl *impl =  V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(this);
    IsolateImpl* iso = V82JSC::ToIsolateImpl(impl);
    Local<ObjectTemplate> instance_template;
    if (!impl->m_instance_template) {
        instance_template = ObjectTemplate::New(V82JSC::ToIsolate(iso));
        impl->m_instance_template = V82JSC::ToImpl<ObjectTemplateImpl>(instance_template);
        impl->m_instance_template->m_constructor_template = impl;
        impl->m_instance_template->m_parent = impl;
    } else {
        instance_template = V82JSC::MakeLocal<ObjectTemplate>(iso, impl->m_instance_template);
    }
    return instance_template;
}

/**
 * Causes the function template to inherit from a parent function template.
 * This means the the function's prototype.__proto__ is set to the parent
 * function's prototype.
 **/
void FunctionTemplate::Inherit(Local<FunctionTemplate> parent)
{
    FunctionTemplateImpl *impl = V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(this);
    impl->m_parent = V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(*parent);
}

/**
 * A PrototypeTemplate is the template used to create the prototype object
 * of the function created by this template.
 */
Local<ObjectTemplate> FunctionTemplate::PrototypeTemplate()
{
    FunctionTemplateImpl *impl = V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(this);
    IsolateImpl* iso = V82JSC::ToIsolateImpl(impl);
    Local<ObjectTemplate> prototype_template;
    if (!impl->m_prototype_template) {
        prototype_template = ObjectTemplate::New(V82JSC::ToIsolate(iso));
        impl->m_prototype_template = V82JSC::ToImpl<ObjectTemplateImpl>(prototype_template);
    } else {
        prototype_template = V82JSC::MakeLocal<ObjectTemplate>(iso, impl->m_prototype_template);
    }
    return prototype_template;
}

/**
 * A PrototypeProviderTemplate is another function template whose prototype
 * property is used for this template. This is mutually exclusive with setting
 * a prototype template indirectly by calling PrototypeTemplate() or using
 * Inherit().
 **/
void FunctionTemplate::SetPrototypeProviderTemplate(Local<FunctionTemplate> prototype_provider)
{
    assert(0);
}

/**
 * Set the class name of the FunctionTemplate.  This is used for
 * printing objects created with the function created from the
 * FunctionTemplate as its constructor.
 */
void FunctionTemplate::SetClassName(Local<String> name)
{
    FunctionTemplateImpl *impl = V82JSC::ToImpl<FunctionTemplateImpl,FunctionTemplate>(this);
    String::Utf8Value str(name);
    impl->m_name = std::string(*str);
}

/**
 * When set to true, no access check will be performed on the receiver of a
 * function call.  Currently defaults to true, but this is subject to change.
 */
void FunctionTemplate::SetAcceptAnyReceiver(bool value)
{
    assert(0);
}

/**
 * Determines whether the __proto__ accessor ignores instances of
 * the function template.  If instances of the function template are
 * ignored, __proto__ skips all instances and instead returns the
 * next object in the prototype chain.
 *
 * Call with a value of true to make the __proto__ accessor ignore
 * instances of the function template.  Call with a value of false
 * to make the __proto__ accessor not ignore instances of the
 * function template.  By default, instances of a function template
 * are not ignored.
 */
void FunctionTemplate::SetHiddenPrototype(bool value)
{
    assert(0);
}

/**
 * Sets the ReadOnly flag in the attributes of the 'prototype' property
 * of functions created from this FunctionTemplate to true.
 */
void FunctionTemplate::ReadOnlyPrototype()
{
    assert(0);
}

/**
 * Removes the prototype property from functions created from this
 * FunctionTemplate.
 */
void FunctionTemplate::RemovePrototype()
{
    assert(0);
}

/**
 * Returns true if the given object is an instance of this function
 * template.
 */
bool FunctionTemplate::HasInstance(Local<Value> object)
{
    assert(0);
    return false;
}

#undef O
#define O(v) reinterpret_cast<v8::internal::Object*>(v)

JSValueRef FunctionTemplateImpl::callAsConstructorCallback(JSContextRef ctx,
                                                           JSObjectRef constructor_function,
                                                           JSObjectRef instance,
                                                           size_t argumentCount,
                                                           const JSValueRef *arguments,
                                                           JSValueRef *exception)
{
    TemplateWrap *wrap = reinterpret_cast<TemplateWrap*>(JSObjectGetPrivate(constructor_function));
    IsolateImpl *isolateimpl = wrap->m_isolate;
    Isolate *isolate = V82JSC::ToIsolate(isolateimpl);
    Local<Context> context = ContextImpl::New(isolate, ctx);
    ContextImpl *ctximpl = V82JSC::ToContextImpl(context);
    
    assert(argumentCount>0);
    bool create_object = JSValueToBoolean(ctx, arguments[0]);
    argumentCount--;
    arguments++;

    FunctionTemplateImpl *ftempl = reinterpret_cast<FunctionTemplateImpl*>(const_cast<TemplateImpl*>(wrap->m_template));
    Local<FunctionTemplate> function_template = V82JSC::MakeLocal<FunctionTemplate>(isolate, ftempl);

    if (create_object) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.attributes = kJSClassAttributeNoAutomaticPrototype;
        const std::string& name = ftempl->m_name;
        def.className = name.length() ? name.c_str() : nullptr;
        JSClassRef claz = JSClassCreate(&def);
        instance = JSObjectMake(ctx, claz, 0);

        JSObjectRef function = (JSObjectRef) V82JSC::ToJSValueRef(function_template->GetFunction(context).ToLocalChecked(), context);
        JSStringRef sprototype = JSStringCreateWithUTF8CString("prototype");
        JSStringRef sconstructor = JSStringCreateWithUTF8CString("constructor");
        JSValueRef excp = 0;
        JSObjectRef prototype = (JSObjectRef) JSObjectGetProperty(ctx, function, sprototype, &excp);
        assert(excp == 0);
        JSObjectSetPrototype(ctx, instance, prototype);
        JSObjectSetProperty(ctx, instance, sconstructor, function, kJSPropertyAttributeDontEnum|kJSPropertyAttributeReadOnly, &excp);
        assert(excp == 0);
    }
    
    TryCatch try_catch(isolate);
    
    Local<ObjectTemplate> instance_template = function_template->InstanceTemplate();
    ObjectTemplateImpl* otempl = V82JSC::ToImpl<ObjectTemplateImpl>(*instance_template);
    Local<Object> thiz = otempl->NewInstance(context, instance).ToLocalChecked();

    if (try_catch.HasCaught()) {
        if (exception) {
            *exception = V82JSC::ToJSValueRef<Value>(try_catch.Exception(), context);
        }
        return 0;
    }

    Local<Value> data = ValueImpl::New(V82JSC::ToContextImpl(context), wrap->m_template->m_data);
    
    v8::internal::Object * implicit[] = {
        * reinterpret_cast<v8::internal::Object**>(*thiz),   // kHolderIndex = 0;
        O(isolateimpl),                                      // kIsolateIndex = 1;
        O(isolateimpl->i.roots.the_hole_value),              // kReturnValueDefaultValueIndex = 2;
        O(isolateimpl->i.roots.the_hole_value),              // kReturnValueIndex = 3;
        * reinterpret_cast<v8::internal::Object**>(*data),   // kDataIndex = 4;
        nullptr /*deprecated*/,                              // kCalleeIndex = 5;
        nullptr, // FIXME                                    // kContextSaveIndex = 6;
        * reinterpret_cast<v8::internal::Object**>(*thiz)    // kNewTargetIndex = 7;
    };
    v8::internal::Object * values_[argumentCount + 1];
    v8::internal::Object ** values = values_ + argumentCount - 1;
    *(values + 1) = * reinterpret_cast<v8::internal::Object**>(*thiz);
    for (size_t i=0; i<argumentCount; i++) {
        Local<Value> arg = ValueImpl::New(ctximpl, arguments[i]);
        *(values-i) = * reinterpret_cast<v8::internal::Object**>(*arg);
    }
    
    FunctionCallbackImpl info(implicit, values, (int) argumentCount);
    
    //internal::Object* held_exception = isolateimpl->i.ii.thread_local_top()->scheduled_exception_;
    isolateimpl->i.ii.thread_local_top()->scheduled_exception_ = *isolateimpl->i.roots.the_hole_value;

    if (wrap->m_template->m_callback) {
        wrap->m_template->m_callback(info);
    }
    
    if (try_catch.HasCaught()) {
        *exception = V82JSC::ToJSValueRef(try_catch.Exception(), context);
    } else if (isolateimpl->i.ii.thread_local_top()->scheduled_exception_ != *isolateimpl->i.roots.the_hole_value) {
        InternalObjectImpl* i = reinterpret_cast<InternalObjectImpl*>(
                                reinterpret_cast<intptr_t>(isolateimpl->i.ii.thread_local_top()->scheduled_exception_ - internal::kHeapObjectTag));
        Local<Value> excp = V82JSC::MakeLocal<Value>(isolateimpl, i);
        *exception = V82JSC::ToJSValueRef(excp, context);
        isolateimpl->i.ii.thread_local_top()->scheduled_exception_ = reinterpret_cast<v8::internal::Object*>(isolateimpl->i.roots.the_hole_value);
    }
    //isolateimpl->i.ii.thread_local_top()->scheduled_exception_ = held_exception;

    if (implicit[3] == O(isolateimpl->i.roots.the_hole_value)) {
        return V82JSC::ToJSValueRef<Object>(thiz, context);
    }

    Local<Value> ret = info.GetReturnValue().Get();
    return (JSObjectRef) V82JSC::ToJSValueRef<Value>(ret, isolate);
}

