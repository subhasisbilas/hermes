/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifdef HERMES_ENABLE_DEBUGGER

#include "JSLibInternal.h"

namespace hermes {
namespace vm {

#ifdef HERMES_ENABLE_DEBUGGER

/// DebuggerInternal.isDebuggerAttached getter.
static CallResult<HermesValue>
isDebuggerAttached(void *ctx, Runtime *runtime, NativeArgs args);

/// DebuggerInternal.shouldPauseOnThrow getter.
static CallResult<HermesValue>
shouldPauseOnThrow(void *ctx, Runtime *runtime, NativeArgs args);

Handle<JSObject> createDebuggerInternalObject(Runtime *runtime) {
  Handle<JSObject> intern = toHandle(runtime, JSObject::create(runtime));

  // Configurable property stored in the Debugger
  // To be used when a debugger transitions to an attached state.
  defineAccessor(
      runtime,
      intern,
      Predefined::getSymbolID(Predefined::isDebuggerAttached),
      nullptr,
      isDebuggerAttached,
      nullptr,
      true,
      false);

  // Configurable property to poll whether
  // the VM will pause the debugger on exceptions.
  defineAccessor(
      runtime,
      intern,
      Predefined::getSymbolID(Predefined::shouldPauseOnThrow),
      nullptr,
      shouldPauseOnThrow,
      nullptr,
      true,
      false);

  JSObject::preventExtensions(*intern);
  runtime->debuggerInternalObject_ = intern.getHermesValue();

  return intern;
}

static CallResult<HermesValue>
isDebuggerAttached(void *ctx, Runtime *runtime, NativeArgs args) {
  return HermesValue::encodeBoolValue(
      runtime->getDebugger().getIsDebuggerAttached());
}

static CallResult<HermesValue>
shouldPauseOnThrow(void *ctx, Runtime *runtime, NativeArgs args) {
  bool shouldPauseOnThrow = runtime->getDebugger().getPauseOnThrowMode() !=
      facebook::hermes::debugger::PauseOnThrowMode::None;
  return HermesValue::encodeBoolValue(shouldPauseOnThrow);
}

#endif // HERMES_ENABLE_DEBUGGER

} // namespace vm
} // namespace hermes

#endif // HERMES_ENABLE_DEBUGGER
