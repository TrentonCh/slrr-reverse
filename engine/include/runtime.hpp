#pragma once

#include "natives.hpp"

#include <cstdint>

namespace inv {

// Minimal string object for the rewrite host (not stock layout yet).
struct InvString {
  const char* utf8;
};

InvObject* string_new(const char* utf8);
const char* string_cstr(InvObject* obj);
// Fork: true only for registered string objects: those made by string_new
// and the shells Lang.cpp makes in lang_string_from_cstr (string_cstr on any
// other object reads its first field as a char* and is not a type test).
bool string_is(InvObject* obj);
// Fork: add / remove a string shell that was not made by string_new. The exe
// has one interned String instance per text (PE 940 JVM_String_from_cstr
// FUN_0055b980 -> find instance FUN_00561020), so `token == "\n"` is an
// identity test there; the host keeps separate objects and compares
// registered strings by text (vmthread_soft_value_eq).
void string_register(InvObject* obj);
void string_unregister(InvObject* obj);

// Registry-only extras not always present in Java inventory.
int32_t java_lang_Object_hashCode(InvObject* self);

void time_init();
float time_current();
float time_sim();
float time_warp(float m);
void time_sync_game(float t);
int32_t time_game_day_seconds();
bool exit_requested();
void request_exit();

}  // namespace inv
