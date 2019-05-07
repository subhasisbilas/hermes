// RUN: %hermesc -Xflow-parser -dump-bytecode %s | %FileCheck %s --match-full-lines
// REQUIRES: flowparser

function *loop(x) {
  var i = 0;
  while (y) {
    yield x[i++];
  }
  return 'DONE LOOPING';
}

// CHECK-LABEL: NCFunction<loop>(2 params, 5 registers, 2 symbols):
// CHECK-NEXT:     CreateEnvironment r0
// CHECK-NEXT:     LoadParam         r1, 1
// CHECK-NEXT:     LoadConstUndefined r2
// CHECK-NEXT:     StoreNPToEnvironment r0, 0, r2
// CHECK-NEXT:     StoreToEnvironment r0, 1, r1
// CHECK-NEXT:     CreateGenerator   r3, r0, 2
// CHECK-NEXT:     Ret               r3

// CHECK-LABEL: Function<?anon_0_loop>(2 params, 15 registers, 2 symbols):
// CHECK-NEXT: Offset in debug table: src 0x7, vars 0x0
// CHECK-NEXT:     CreateEnvironment r0
// CHECK-NEXT:     LoadParam         r1, 1
// CHECK-NEXT:     LoadConstUndefined r2
// CHECK-NEXT:     LoadConstZero     r3
// CHECK-NEXT:     LoadConstUInt8    r4, 1
// CHECK-NEXT:     LoadConstString   r5, "DONE LOOPING"
// CHECK-NEXT:     GetGlobalObject   r6
// CHECK-NEXT:     StartGenerator
// CHECK-NEXT:     ResumeGenerator   r8, r7
// CHECK-NEXT:     Mov               r9, r7
// CHECK-NEXT:     JmpTrue           L1, r9
// CHECK-NEXT:     StoreNPToEnvironment r0, 0, r2
// CHECK-NEXT:     StoreToEnvironment r0, 1, r1
// CHECK-NEXT:     StoreNPToEnvironment r0, 0, r3
// CHECK-NEXT:     TryGetById        r7, r6, 1, "y"
// CHECK-NEXT:     JmpFalse          L2, r7
// CHECK-NEXT: L5:
// CHECK-NEXT:     LoadFromEnvironment r7, r0, 1
// CHECK-NEXT:     LoadFromEnvironment r9, r0, 0
// CHECK-NEXT:     ToNumber          r10, r9
// CHECK-NEXT:     AddN              r11, r10, r4
// CHECK-NEXT:     StoreToEnvironment r0, 0, r11
// CHECK-NEXT:     GetByVal          r12, r7, r10
// CHECK-NEXT:     SaveGenerator     L3
// CHECK-NEXT:     Ret               r12
// CHECK-NEXT: L3:
// CHECK-NEXT:     ResumeGenerator   r7, r13
// CHECK-NEXT:     Mov               r9, r13
// CHECK-NEXT:     JmpTrue           L4, r9
// CHECK-NEXT:     TryGetById        r9, r6, 1, "y"
// CHECK-NEXT:     JmpTrue           L5, r9
// CHECK-NEXT: L2:
// CHECK-NEXT:     CompleteGenerator
// CHECK-NEXT:     Ret               r5
// CHECK-NEXT: L4:
// CHECK-NEXT:     CompleteGenerator
// CHECK-NEXT:     Ret               r7
// CHECK-NEXT: L1:
// CHECK-NEXT:     CompleteGenerator
// CHECK-NEXT:     Ret               r8
