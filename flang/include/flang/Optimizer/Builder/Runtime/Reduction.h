//===-- Reduction.h -- generate calls to reduction runtime API --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The runtime routines generated in this file are generally storing the result
// in a descriptor (represented as a `box` in FIR). Some function might
// have a specialization where the value is returned as a scalar value, e.g.
// `genAll` is a specialization of `genAllDescriptor`.
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_OPTIMIZER_BUILDER_RUNTIME_REDUCTION_H
#define FORTRAN_OPTIMIZER_BUILDER_RUNTIME_REDUCTION_H

#include "mlir/Dialect/StandardOps/IR/Ops.h"

namespace fir {
class ExtendedValue;
class FirOpBuilder;
} // namespace fir

namespace fir::runtime {

/// Generate call to `all` runtime routine.
/// This calls the descriptor based runtime call implementation of the `all`
/// intrinsic.
void genAllDescriptor(fir::FirOpBuilder &builder, mlir::Location loc,
                      mlir::Value resultBox, mlir::Value maskBox,
                      mlir::Value dim);

/// Generate call to `any` runtime routine.
/// This calls the descriptor based runtime call implementation of the `any`
/// intrinsic.
void genAnyDescriptor(fir::FirOpBuilder &builder, mlir::Location loc,
                      mlir::Value resultBox, mlir::Value maskBox,
                      mlir::Value dim);

/// Generate call to `all` runtime routine. This version of `all` is specialized
/// for rank 1 mask arguments.
/// This calls the version that returns a scalar logical value.
mlir::Value genAll(fir::FirOpBuilder &builder, mlir::Location loc,
                   mlir::Value maskBox, mlir::Value dim);

/// Generate call to `any` runtime routine. This version of `any` is specialized
/// for rank 1 mask arguments.
/// This calls the version that returns a scalar logical value.
mlir::Value genAny(fir::FirOpBuilder &builder, mlir::Location loc,
                   mlir::Value maskBox, mlir::Value dim);

/// Generate call to `count` runtime routine. This routine is a specialized
/// version when mask is a rank one array or the dim argument is not
/// specified by the user.
mlir::Value genCount(fir::FirOpBuilder &builder, mlir::Location loc,
                     mlir::Value maskBox, mlir::Value dim);

/// Generate call to general CountDim runtime routine. This routine has a
/// descriptor result.
void genCountDim(fir::FirOpBuilder &builder, mlir::Location loc,
                 mlir::Value resultBox, mlir::Value maskBox, mlir::Value dim,
                 mlir::Value kind);

/// Generate call to `dot_product` intrinsic runtime routine.
mlir::Value genDotProduct(fir::FirOpBuilder &builder, mlir::Location loc,
                          mlir::Value vectorABox, mlir::Value vectorBBox,
                          mlir::Value resultBox);

/// Generate call to `maxloc` intrinsic runtime routine. This is the version
/// that does not take a dim argument.
void genMaxloc(fir::FirOpBuilder &builder, mlir::Location loc,
               mlir::Value resultBox, mlir::Value arrayBox, mlir::Value maskBox,
               mlir::Value kind, mlir::Value back);

/// Generate call to `maxloc` intrinsic runtime routine. This is the version
/// that takes a dim argument.
void genMaxlocDim(fir::FirOpBuilder &builder, mlir::Location loc,
                  mlir::Value resultBox, mlir::Value arrayBox, mlir::Value dim,
                  mlir::Value maskBox, mlir::Value kind, mlir::Value back);

/// Generate call to `minloc` intrinsic runtime routine. This is the version
/// that does not take a dim argument.
void genMinloc(fir::FirOpBuilder &builder, mlir::Location loc,
               mlir::Value resultBox, mlir::Value arrayBox, mlir::Value maskBox,
               mlir::Value kind, mlir::Value back);

/// Generate call to `minloc` intrinsic runtime routine. This is the version
/// that takes a dim argument.
void genMinlocDim(fir::FirOpBuilder &builder, mlir::Location loc,
                  mlir::Value resultBox, mlir::Value arrayBox, mlir::Value dim,
                  mlir::Value maskBox, mlir::Value kind, mlir::Value back);

/// Generate call to `maxval` intrinsic runtime routine. This is the version
/// that does not take a dim argument.
mlir::Value genMaxval(fir::FirOpBuilder &builder, mlir::Location loc,
                      mlir::Value arrayBox, mlir::Value maskBox);

/// Generate call to `maxval` intrinsic runtime routine. This is the version
/// that that handles 1 dimensional character arrays  with no DIM argument.
void genMaxvalChar(fir::FirOpBuilder &builder, mlir::Location loc,
                   mlir::Value resultBox, mlir::Value arrayBox,
                   mlir::Value maskBox);

/// Generate call to `maxval` intrinsic runtime routine. This is the version
/// that takes arrays of any rank with a dim argument specified.
void genMaxvalDim(fir::FirOpBuilder &builder, mlir::Location loc,
                  mlir::Value resultBox, mlir::Value arrayBox, mlir::Value dim,
                  mlir::Value maskBox);

/// Generate call to `minval` intrinsic runtime routine. This is the version
/// that does not take a dim argument.
mlir::Value genMinval(fir::FirOpBuilder &builder, mlir::Location loc,
                      mlir::Value arrayBox, mlir::Value maskBox);

/// Generate call to `minval` intrinsic runtime routine. This is the version
/// that that handles 1 dimensional character arrays with no DIM argument.
void genMinvalChar(fir::FirOpBuilder &builder, mlir::Location loc,
                   mlir::Value resultBox, mlir::Value arrayBox,
                   mlir::Value maskBox);

/// Generate call to `minval` intrinsic runtime routine. This is the version
/// that takes arrays of any rank with a dim argument specified.
void genMinvalDim(fir::FirOpBuilder &builder, mlir::Location loc,
                  mlir::Value resultBox, mlir::Value arrayBox, mlir::Value dim,
                  mlir::Value maskBox);

/// Generate call to `product` intrinsic runtime routine. This is the version
/// that does not take a dim argument.
mlir::Value genProduct(fir::FirOpBuilder &builder, mlir::Location loc,
                       mlir::Value arrayBox, mlir::Value maskBox,
                       mlir::Value resultBox);

/// Generate call to `product` intrinsic runtime routine. This is the version
/// that takes arrays of any rank with a dim argument specified.
void genProductDim(fir::FirOpBuilder &builder, mlir::Location loc,
                   mlir::Value resultBox, mlir::Value arrayBox, mlir::Value dim,
                   mlir::Value maskBox);

/// Generate call to `sum` intrinsic runtime routine. This is the version
/// that does not take a dim argument.
mlir::Value genSum(fir::FirOpBuilder &builder, mlir::Location loc,
                   mlir::Value arrayBox, mlir::Value maskBox,
                   mlir::Value resultBox);

/// Generate call to `sum` intrinsic runtime routine. This is the version
/// that takes arrays of any rank with a dim argument specified.
void genSumDim(fir::FirOpBuilder &builder, mlir::Location loc,
               mlir::Value resultBox, mlir::Value arrayBox, mlir::Value dim,
               mlir::Value maskBox);

} // namespace fir::runtime

#endif // FORTRAN_OPTIMIZER_BUILDER_RUNTIME_REDUCTION_H
