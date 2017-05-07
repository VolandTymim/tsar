//===---- tsar_dbg_output.h - Output functions for debugging ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines a set of output functions for various bits of information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DBG_OUTPUT_H
#define TSAR_DBG_OUTPUT_H

#include <llvm/IR/DebugInfoMetadata.h>

namespace llvm {
class raw_ostream;
class Function;
class Value;
class LoopInfo;
}

namespace tsar {
/// \brief Prints information available from a source code for the
/// specified memory location.
///
/// \pre At this moment location can be represented as a sequence of 'load' or
/// 'getelementptr' instructions ending 'alloca' instruction or global variable.
/// \par Example
/// \code
///    ...
/// 1: int *p;
/// 2: *p = 5;
/// \endcode
/// \code
/// %p = alloca i32*, align 4
/// %0 = load i32*, i32** %p, align 4
/// \endcode
/// If debug information is available the result for
/// %0 will be p[0] otherwise it will be *(%p = alloca i32*, align 4).
void printLocationSource(llvm::raw_ostream &o, const llvm::Value *Loc);

/// \brief Print description of a type from a source code.
///
/// \param [in] DITy Meta information for a type.
void printDIType(llvm::raw_ostream &o, const llvm::DITypeRef &DITy);

/// \brief Print description of a variable from a source code.
///
/// \param [in] DIVar Meta information for a variable.
void printDIVariable(llvm::raw_ostream &o, llvm:: DIVariable *DIVar);

/// \brief Prints loop tree which is calculated by the LoopInfo pass.
///
/// \param [in] LI Information about natural loops identified
/// by the LoopInfor pass.
void printLoops(llvm::raw_ostream &o, const llvm::LoopInfo &LI);
}

#endif//TSAR_DBG_OUTPUT_H
