/*
 * Copyright (c) 2006-2007, Apple Inc. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer. 
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution. 
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission. 
 * 
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include <st.h>
#include "rbosa.h"

static void 
rbosa_app_name_signature (CFURLRef URL, VALUE *name, VALUE *signature)
{
    CFBundleRef     bundle;
    CFDictionaryRef info;
    CFStringRef     str;

    bundle = CFBundleCreate (kCFAllocatorDefault, URL);
    info = CFBundleGetInfoDictionary (bundle);
    
    if (NIL_P(*name)) {
        str = CFDictionaryGetValue (info, CFSTR ("CFBundleName"));
        if (str == NULL) {
            /* Try 'CFBundleExecutable' if 'CFBundleName' does not exist (which is a bug). */
            str = CFDictionaryGetValue (info, CFSTR ("CFBundleExecutable"));
        }
        *name = str != NULL ? CSTR2RVAL (CFStringGetCStringPtr (str, CFStringGetFastestEncoding (str))) : Qnil;
    }
    if (NIL_P(*signature)) {
        str = CFDictionaryGetValue (info, CFSTR ("CFBundleSignature"));
        *signature = str != NULL ? CSTR2RVAL (CFStringGetCStringPtr (str, CFStringGetFastestEncoding (str))) : Qnil;
    }

    CFRelease (bundle);
}

static bool 
rbosa_translate_app (VALUE criterion, VALUE value, VALUE *app_signature, VALUE *app_name, FSRef *fs_ref, const char **error)
{
    OSStatus    err;
    CFURLRef    URL;

    *app_name = Qnil;
    *app_signature = Qnil;
    err = noErr;
 
    if (criterion == ID2SYM (rb_intern ("signature"))) {
        err = LSFindApplicationForInfo (RVAL2FOURCHAR (value), NULL, NULL, fs_ref, &URL);
        *app_signature = value; /* Don't need to get the app signature, we already have it. */
    }
    else { 
        CFMutableStringRef  str;
        
        str = CFStringCreateMutable (kCFAllocatorDefault, 0);
        CFStringAppendCString (str, RVAL2CSTR (value), kCFStringEncodingUTF8);

        if (criterion == ID2SYM (rb_intern ("path"))) {
            err = FSPathMakeRef ((const UInt8 *)RVAL2CSTR (value), fs_ref, NULL);
            if (err == noErr) {
                URL = CFURLCreateWithFileSystemPath (kCFAllocatorDefault, str, kCFURLPOSIXPathStyle, FALSE);
            }
        }
        else if (criterion == ID2SYM (rb_intern ("name"))) {
            CFStringRef dot_app;
            
            dot_app = CFSTR (".app");
            if (!CFStringHasSuffix (str, dot_app))
                CFStringAppend (str, dot_app);
   
            err = LSFindApplicationForInfo (kLSUnknownCreator, NULL, str, fs_ref, &URL);
            *app_name = value; /* Don't need to get the app name, we already have it. */
        }
        else if (criterion == ID2SYM (rb_intern ("bundle_id"))) {
            err = LSFindApplicationForInfo (kLSUnknownCreator, str, NULL, fs_ref, &URL);
        }
        else {
            *error = "Invalid criterion";
            CFRelease (str);
            return FALSE;
        }
        
        CFRelease (str);
    }
    
    if (err != noErr) {
        *error = "Can't locate the target bundle on the file system";
        return FALSE;
    }

    rbosa_app_name_signature (URL, app_name, app_signature);
    
    CFRelease (URL);

    if (NIL_P (*app_signature)) {
        *error = "Can't get the target bundle signature";
        return FALSE;
    }
 
    return TRUE;
}

static inline VALUE
__get_criterion (VALUE hash, const char *str, VALUE *psym)
{
    VALUE   sym;
    VALUE   val;

    sym = ID2SYM (rb_intern (str));
    val = rb_hash_delete (hash, sym);

    if (!NIL_P (val)) {
        if (TYPE (val) != T_STRING)
            rb_raise (rb_eArgError, "argument '%s' must have a String value", RVAL2CSTR (sym));
        if (psym != NULL)
            *psym = sym;
    }

    return val;     
}

VALUE
rbosa_scripting_info (VALUE self, VALUE hash)
{
    const char *    error;
    VALUE           criterion;
    VALUE           value;
    VALUE           remote;
    VALUE           ary;
    VALUE           name;  
    VALUE           signature;
    FSRef           fs;
    OSAError        osa_error;
    CFDataRef       sdef_data;

    Check_Type (hash, T_HASH);

    criterion = Qnil;
    value = __get_criterion (hash, "name", &criterion);
    if (NIL_P (value))
        value = __get_criterion (hash, "path", &criterion);
    if (NIL_P (value))
        value = __get_criterion (hash, "bundle_id", &criterion);
    if (NIL_P (value))
        value = __get_criterion (hash, "signature", &criterion);
    if (NIL_P (value))
        rb_raise (rb_eArgError, "expected :name, :path, :bundle_id or :signature key/value");

    remote = __get_criterion (hash, "machine", NULL);
    if (!NIL_P (remote)) {
        VALUE username;
        VALUE password;
        char buf[128];

        if (NIL_P (value) || criterion != ID2SYM (rb_intern ("name")))
            rb_raise (rb_eArgError, ":machine argument requires :name");

        username = __get_criterion (hash, "username", NULL);
        password = __get_criterion (hash, "password", NULL);

        if (NIL_P (username)) {
            if (!NIL_P (password))
                rb_raise (rb_eArgError, ":password argument requires :username");
            snprintf (buf, sizeof buf, "eppc://%s/%s", RVAL2CSTR (remote), RVAL2CSTR (value));
        }
        else {
            if (NIL_P (password))
                snprintf (buf, sizeof buf, "eppc://%s@%s/%s", RVAL2CSTR (username), RVAL2CSTR (remote), RVAL2CSTR (value));
            else
                snprintf (buf, sizeof buf, "eppc://%s:%s@%s/%s", RVAL2CSTR (username), RVAL2CSTR (password), RVAL2CSTR (remote), RVAL2CSTR (value));
        }

        remote = CSTR2RVAL (buf);
    } 

    if (RHASH (hash)->tbl->num_entries > 0) {
        VALUE   keys;

        keys = rb_funcall (hash, rb_intern ("keys"), 0);
        rb_raise (rb_eArgError, "inappropriate argument(s): %s", RSTRING (rb_inspect (keys))->ptr);
    }

    // FIXME: we currently don't have a way to retrieve the sdef of a remote application.
 
    if (!rbosa_translate_app (criterion, value, &signature, &name, &fs, &error))
        rb_raise (rb_eRuntimeError, error);

    osa_error = OSACopyScriptingDefinition (&fs, kOSAModeNull, &sdef_data);
    if (osa_error != noErr)
        rb_raise (rb_eRuntimeError, "Cannot get scripting definition : error %d", osa_error);

    ary = rb_ary_new3 (3, name, NIL_P (remote) ? signature : remote, 
                       rb_str_new ((const char *)CFDataGetBytePtr (sdef_data), 
                                   CFDataGetLength (sdef_data)));

    CFRelease (sdef_data);

    return ary;
}
