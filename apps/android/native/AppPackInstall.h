#pragma once

#include <jni.h>

#include <string>

struct android_app;

// Phase 22 (apppack, ADR-0005 §2): app.vdpk (our texts, art and font: pack/AppPack.h) travels in the APK's
// assets and is copied to the app's internal storage when it is missing there or when the APK changed (its
// versionCode, or its install time: a development APK keeps its versionCode). Returns the copy's path, empty
// when the APK has no app.vdpk or it cannot be copied (logged: the game then shows the keys of our texts and
// no icons of ours).
std::string installAppPack(android_app* app, JNIEnv* env);
