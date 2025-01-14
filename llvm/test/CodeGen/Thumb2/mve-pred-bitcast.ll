; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=thumbv8.1m.main-none-none-eabi -mattr=+mve -verify-machineinstrs %s -o - | FileCheck %s --check-prefix=CHECK-LE
; RUN: llc -mtriple=thumbebv8.1m.main-none-none-eabi -mattr=+mve -verify-machineinstrs %s -o - | FileCheck %s --check-prefix=CHECK-BE

define arm_aapcs_vfpcc <4 x i32> @bitcast_to_v4i1(i4 %b, <4 x i32> %a) {
; CHECK-LE-LABEL: bitcast_to_v4i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    and r0, r0, #15
; CHECK-LE-NEXT:    vmov.i8 q1, #0x0
; CHECK-LE-NEXT:    vmov.i8 q2, #0xff
; CHECK-LE-NEXT:    vmsr p0, r0
; CHECK-LE-NEXT:    vpsel q1, q2, q1
; CHECK-LE-NEXT:    vmov.u8 r0, q1[2]
; CHECK-LE-NEXT:    vmov.u8 r1, q1[0]
; CHECK-LE-NEXT:    vmov q2[2], q2[0], r1, r0
; CHECK-LE-NEXT:    vmov.u8 r0, q1[3]
; CHECK-LE-NEXT:    vmov.u8 r1, q1[1]
; CHECK-LE-NEXT:    vmov.i32 q1, #0x0
; CHECK-LE-NEXT:    vmov q2[3], q2[1], r1, r0
; CHECK-LE-NEXT:    vcmp.i32 ne, q2, zr
; CHECK-LE-NEXT:    vpsel q0, q0, q1
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_to_v4i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    rbit r0, r0
; CHECK-BE-NEXT:    vmov.i8 q1, #0x0
; CHECK-BE-NEXT:    vmov.i8 q2, #0xff
; CHECK-BE-NEXT:    lsrs r0, r0, #28
; CHECK-BE-NEXT:    vmsr p0, r0
; CHECK-BE-NEXT:    vpsel q1, q2, q1
; CHECK-BE-NEXT:    vmov.u8 r0, q1[2]
; CHECK-BE-NEXT:    vmov.u8 r1, q1[0]
; CHECK-BE-NEXT:    vmov q2[2], q2[0], r1, r0
; CHECK-BE-NEXT:    vmov.u8 r0, q1[3]
; CHECK-BE-NEXT:    vmov.u8 r1, q1[1]
; CHECK-BE-NEXT:    vrev64.32 q1, q0
; CHECK-BE-NEXT:    vmov q2[3], q2[1], r1, r0
; CHECK-BE-NEXT:    vmov.i32 q0, #0x0
; CHECK-BE-NEXT:    vcmp.i32 ne, q2, zr
; CHECK-BE-NEXT:    vpsel q1, q1, q0
; CHECK-BE-NEXT:    vrev64.32 q0, q1
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = bitcast i4 %b to <4 x i1>
  %s = select <4 x i1> %c, <4 x i32> %a, <4 x i32> zeroinitializer
  ret <4 x i32> %s
}

define arm_aapcs_vfpcc <8 x i16> @bitcast_to_v8i1(i8 %b, <8 x i16> %a) {
; CHECK-LE-LABEL: bitcast_to_v8i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    uxtb r0, r0
; CHECK-LE-NEXT:    vmov.i8 q1, #0x0
; CHECK-LE-NEXT:    vmov.i8 q2, #0xff
; CHECK-LE-NEXT:    vmsr p0, r0
; CHECK-LE-NEXT:    vpsel q2, q2, q1
; CHECK-LE-NEXT:    vmov.u8 r0, q2[0]
; CHECK-LE-NEXT:    vmov.16 q1[0], r0
; CHECK-LE-NEXT:    vmov.u8 r0, q2[1]
; CHECK-LE-NEXT:    vmov.16 q1[1], r0
; CHECK-LE-NEXT:    vmov.u8 r0, q2[2]
; CHECK-LE-NEXT:    vmov.16 q1[2], r0
; CHECK-LE-NEXT:    vmov.u8 r0, q2[3]
; CHECK-LE-NEXT:    vmov.16 q1[3], r0
; CHECK-LE-NEXT:    vmov.u8 r0, q2[4]
; CHECK-LE-NEXT:    vmov.16 q1[4], r0
; CHECK-LE-NEXT:    vmov.u8 r0, q2[5]
; CHECK-LE-NEXT:    vmov.16 q1[5], r0
; CHECK-LE-NEXT:    vmov.u8 r0, q2[6]
; CHECK-LE-NEXT:    vmov.16 q1[6], r0
; CHECK-LE-NEXT:    vmov.u8 r0, q2[7]
; CHECK-LE-NEXT:    vmov.16 q1[7], r0
; CHECK-LE-NEXT:    vcmp.i16 ne, q1, zr
; CHECK-LE-NEXT:    vmov.i32 q1, #0x0
; CHECK-LE-NEXT:    vpsel q0, q0, q1
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_to_v8i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    uxtb r0, r0
; CHECK-BE-NEXT:    vmov.i8 q1, #0x0
; CHECK-BE-NEXT:    rbit r0, r0
; CHECK-BE-NEXT:    vmov.i8 q2, #0xff
; CHECK-BE-NEXT:    lsrs r0, r0, #24
; CHECK-BE-NEXT:    vmsr p0, r0
; CHECK-BE-NEXT:    vpsel q2, q2, q1
; CHECK-BE-NEXT:    vmov.u8 r0, q2[0]
; CHECK-BE-NEXT:    vmov.16 q1[0], r0
; CHECK-BE-NEXT:    vmov.u8 r0, q2[1]
; CHECK-BE-NEXT:    vmov.16 q1[1], r0
; CHECK-BE-NEXT:    vmov.u8 r0, q2[2]
; CHECK-BE-NEXT:    vmov.16 q1[2], r0
; CHECK-BE-NEXT:    vmov.u8 r0, q2[3]
; CHECK-BE-NEXT:    vmov.16 q1[3], r0
; CHECK-BE-NEXT:    vmov.u8 r0, q2[4]
; CHECK-BE-NEXT:    vmov.16 q1[4], r0
; CHECK-BE-NEXT:    vmov.u8 r0, q2[5]
; CHECK-BE-NEXT:    vmov.16 q1[5], r0
; CHECK-BE-NEXT:    vmov.u8 r0, q2[6]
; CHECK-BE-NEXT:    vmov.16 q1[6], r0
; CHECK-BE-NEXT:    vmov.u8 r0, q2[7]
; CHECK-BE-NEXT:    vmov.16 q1[7], r0
; CHECK-BE-NEXT:    vcmp.i16 ne, q1, zr
; CHECK-BE-NEXT:    vrev64.16 q1, q0
; CHECK-BE-NEXT:    vmov.i32 q0, #0x0
; CHECK-BE-NEXT:    vrev32.16 q0, q0
; CHECK-BE-NEXT:    vpsel q1, q1, q0
; CHECK-BE-NEXT:    vrev64.16 q0, q1
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = bitcast i8 %b to <8 x i1>
  %s = select <8 x i1> %c, <8 x i16> %a, <8 x i16> zeroinitializer
  ret <8 x i16> %s
}

define arm_aapcs_vfpcc <16 x i8> @bitcast_to_v16i1(i16 %b, <16 x i8> %a) {
; CHECK-LE-LABEL: bitcast_to_v16i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    vmsr p0, r0
; CHECK-LE-NEXT:    vmov.i32 q1, #0x0
; CHECK-LE-NEXT:    vpsel q0, q0, q1
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_to_v16i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    uxth r0, r0
; CHECK-BE-NEXT:    vrev64.8 q1, q0
; CHECK-BE-NEXT:    rbit r0, r0
; CHECK-BE-NEXT:    vmov.i32 q0, #0x0
; CHECK-BE-NEXT:    vrev32.8 q0, q0
; CHECK-BE-NEXT:    lsrs r0, r0, #16
; CHECK-BE-NEXT:    vmsr p0, r0
; CHECK-BE-NEXT:    vpsel q1, q1, q0
; CHECK-BE-NEXT:    vrev64.8 q0, q1
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = bitcast i16 %b to <16 x i1>
  %s = select <16 x i1> %c, <16 x i8> %a, <16 x i8> zeroinitializer
  ret <16 x i8> %s
}

define arm_aapcs_vfpcc <2 x i64> @bitcast_to_v2i1(i2 %b, <2 x i64> %a) {
; CHECK-LE-LABEL: bitcast_to_v2i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    and r0, r0, #3
; CHECK-LE-NEXT:    vmov.i8 q1, #0x0
; CHECK-LE-NEXT:    vmov.i8 q2, #0xff
; CHECK-LE-NEXT:    vmsr p0, r0
; CHECK-LE-NEXT:    vpsel q1, q2, q1
; CHECK-LE-NEXT:    vmov.u8 r0, q1[1]
; CHECK-LE-NEXT:    vmov.u8 r1, q1[0]
; CHECK-LE-NEXT:    vmov q1[2], q1[0], r1, r0
; CHECK-LE-NEXT:    vmov q1[3], q1[1], r1, r0
; CHECK-LE-NEXT:    vcmp.i32 ne, q1, zr
; CHECK-LE-NEXT:    vmov.i32 q1, #0x0
; CHECK-LE-NEXT:    vpsel q0, q0, q1
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_to_v2i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    rbit r0, r0
; CHECK-BE-NEXT:    vmov.i8 q1, #0x0
; CHECK-BE-NEXT:    vmov.i8 q2, #0xff
; CHECK-BE-NEXT:    lsrs r0, r0, #30
; CHECK-BE-NEXT:    vmsr p0, r0
; CHECK-BE-NEXT:    vpsel q1, q2, q1
; CHECK-BE-NEXT:    vmov.u8 r0, q1[1]
; CHECK-BE-NEXT:    vmov.u8 r1, q1[0]
; CHECK-BE-NEXT:    vmov q1[2], q1[0], r1, r0
; CHECK-BE-NEXT:    vmov q1[3], q1[1], r1, r0
; CHECK-BE-NEXT:    vcmp.i32 ne, q1, zr
; CHECK-BE-NEXT:    vmov.i32 q1, #0x0
; CHECK-BE-NEXT:    vpsel q0, q0, q1
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = bitcast i2 %b to <2 x i1>
  %s = select <2 x i1> %c, <2 x i64> %a, <2 x i64> zeroinitializer
  ret <2 x i64> %s
}


define arm_aapcs_vfpcc i4 @bitcast_from_v4i1(<4 x i32> %a) {
; CHECK-LE-LABEL: bitcast_from_v4i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    vcmp.i32 eq, q0, zr
; CHECK-LE-NEXT:    vmrs r1, p0
; CHECK-LE-NEXT:    and r0, r1, #1
; CHECK-LE-NEXT:    rsbs r2, r0, #0
; CHECK-LE-NEXT:    movs r0, #0
; CHECK-LE-NEXT:    bfi r0, r2, #0, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #4, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #1, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #8, #1
; CHECK-LE-NEXT:    ubfx r1, r1, #12, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #2, #1
; CHECK-LE-NEXT:    rsbs r1, r1, #0
; CHECK-LE-NEXT:    bfi r0, r1, #3, #1
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_from_v4i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    vrev64.32 q1, q0
; CHECK-BE-NEXT:    vcmp.i32 eq, q1, zr
; CHECK-BE-NEXT:    vmrs r1, p0
; CHECK-BE-NEXT:    ubfx r0, r1, #12, #1
; CHECK-BE-NEXT:    rsbs r2, r0, #0
; CHECK-BE-NEXT:    movs r0, #0
; CHECK-BE-NEXT:    bfi r0, r2, #0, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #8, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #1, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #4, #1
; CHECK-BE-NEXT:    and r1, r1, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #2, #1
; CHECK-BE-NEXT:    rsbs r1, r1, #0
; CHECK-BE-NEXT:    bfi r0, r1, #3, #1
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = icmp eq <4 x i32> %a, zeroinitializer
  %b = bitcast <4 x i1> %c to i4
  ret i4 %b
}

define arm_aapcs_vfpcc i8 @bitcast_from_v8i1(<8 x i16> %a) {
; CHECK-LE-LABEL: bitcast_from_v8i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    vcmp.i16 eq, q0, zr
; CHECK-LE-NEXT:    vmrs r1, p0
; CHECK-LE-NEXT:    and r0, r1, #1
; CHECK-LE-NEXT:    rsbs r2, r0, #0
; CHECK-LE-NEXT:    movs r0, #0
; CHECK-LE-NEXT:    bfi r0, r2, #0, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #2, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #1, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #4, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #2, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #6, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #3, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #8, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #4, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #10, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #5, #1
; CHECK-LE-NEXT:    ubfx r2, r1, #12, #1
; CHECK-LE-NEXT:    ubfx r1, r1, #14, #1
; CHECK-LE-NEXT:    rsbs r2, r2, #0
; CHECK-LE-NEXT:    bfi r0, r2, #6, #1
; CHECK-LE-NEXT:    rsbs r1, r1, #0
; CHECK-LE-NEXT:    bfi r0, r1, #7, #1
; CHECK-LE-NEXT:    uxtb r0, r0
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_from_v8i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    vrev64.16 q1, q0
; CHECK-BE-NEXT:    vcmp.i16 eq, q1, zr
; CHECK-BE-NEXT:    vmrs r1, p0
; CHECK-BE-NEXT:    ubfx r0, r1, #14, #1
; CHECK-BE-NEXT:    rsbs r2, r0, #0
; CHECK-BE-NEXT:    movs r0, #0
; CHECK-BE-NEXT:    bfi r0, r2, #0, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #12, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #1, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #10, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #2, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #8, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #3, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #6, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #4, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #4, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #5, #1
; CHECK-BE-NEXT:    ubfx r2, r1, #2, #1
; CHECK-BE-NEXT:    and r1, r1, #1
; CHECK-BE-NEXT:    rsbs r2, r2, #0
; CHECK-BE-NEXT:    bfi r0, r2, #6, #1
; CHECK-BE-NEXT:    rsbs r1, r1, #0
; CHECK-BE-NEXT:    bfi r0, r1, #7, #1
; CHECK-BE-NEXT:    uxtb r0, r0
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = icmp eq <8 x i16> %a, zeroinitializer
  %b = bitcast <8 x i1> %c to i8
  ret i8 %b
}

define arm_aapcs_vfpcc i16 @bitcast_from_v16i1(<16 x i8> %a) {
; CHECK-LE-LABEL: bitcast_from_v16i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    vcmp.i8 eq, q0, zr
; CHECK-LE-NEXT:    vmrs r0, p0
; CHECK-LE-NEXT:    uxth r0, r0
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_from_v16i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    vrev64.8 q1, q0
; CHECK-BE-NEXT:    vcmp.i8 eq, q1, zr
; CHECK-BE-NEXT:    vmrs r0, p0
; CHECK-BE-NEXT:    rbit r0, r0
; CHECK-BE-NEXT:    lsrs r0, r0, #16
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = icmp eq <16 x i8> %a, zeroinitializer
  %b = bitcast <16 x i1> %c to i16
  ret i16 %b
}

define arm_aapcs_vfpcc i2 @bitcast_from_v2i1(<2 x i64> %a) {
; CHECK-LE-LABEL: bitcast_from_v2i1:
; CHECK-LE:       @ %bb.0: @ %entry
; CHECK-LE-NEXT:    .pad #4
; CHECK-LE-NEXT:    sub sp, #4
; CHECK-LE-NEXT:    vmov r0, r1, d0
; CHECK-LE-NEXT:    orrs r0, r1
; CHECK-LE-NEXT:    cset r0, eq
; CHECK-LE-NEXT:    cmp r0, #0
; CHECK-LE-NEXT:    mov.w r0, #0
; CHECK-LE-NEXT:    csetm r1, ne
; CHECK-LE-NEXT:    bfi r0, r1, #0, #1
; CHECK-LE-NEXT:    vmov r1, r2, d1
; CHECK-LE-NEXT:    orrs r1, r2
; CHECK-LE-NEXT:    cset r1, eq
; CHECK-LE-NEXT:    cmp r1, #0
; CHECK-LE-NEXT:    csetm r1, ne
; CHECK-LE-NEXT:    bfi r0, r1, #1, #1
; CHECK-LE-NEXT:    add sp, #4
; CHECK-LE-NEXT:    bx lr
;
; CHECK-BE-LABEL: bitcast_from_v2i1:
; CHECK-BE:       @ %bb.0: @ %entry
; CHECK-BE-NEXT:    .pad #4
; CHECK-BE-NEXT:    sub sp, #4
; CHECK-BE-NEXT:    vrev64.32 q1, q0
; CHECK-BE-NEXT:    vmov r0, r1, d3
; CHECK-BE-NEXT:    orrs r0, r1
; CHECK-BE-NEXT:    cset r0, eq
; CHECK-BE-NEXT:    cmp r0, #0
; CHECK-BE-NEXT:    mov.w r0, #0
; CHECK-BE-NEXT:    csetm r1, ne
; CHECK-BE-NEXT:    bfi r0, r1, #0, #1
; CHECK-BE-NEXT:    vmov r1, r2, d2
; CHECK-BE-NEXT:    orrs r1, r2
; CHECK-BE-NEXT:    cset r1, eq
; CHECK-BE-NEXT:    cmp r1, #0
; CHECK-BE-NEXT:    csetm r1, ne
; CHECK-BE-NEXT:    bfi r0, r1, #1, #1
; CHECK-BE-NEXT:    add sp, #4
; CHECK-BE-NEXT:    bx lr
entry:
  %c = icmp eq <2 x i64> %a, zeroinitializer
  %b = bitcast <2 x i1> %c to i2
  ret i2 %b
}
