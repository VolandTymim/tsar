//===---- CopyPropagation.h - Copy Propagation (Clang) -----------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to replace the occurrences of variables with
// direct assignments.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/CopyPropagation.h"
#include "tsar_dbg_output.h"
#include "tsar_query.h"
#include "tsar_matcher.h"
#include "SourceUnparserUtils.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/CallSite.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar.h>
#include <bcl/tagged.h>
#include <stack>
#include <tuple>
#include <vector>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-copy-propagation"

char ClangCopyPropagation::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangCopyPropagation, "clang-copy-propagation",
  "Copy Propagation (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(ClangCopyPropagation, "clang-copy-propagation",
  "Copy Propagation (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

FunctionPass * createClangCopyPropagation() {
  return new ClangCopyPropagation();
}

void ClangCopyPropagation::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredID(std::unique_ptr<FunctionPass>(createSROAPass())->getPassID());
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.setPreservesAll();
}

namespace {
struct Use {};
struct Def {};

class DefUseVisitor : public RecursiveASTVisitor<DefUseVisitor> {
public:
  /// Map from source string to possible a replacement string.
  using ReplacementT = StringMap<SmallString<16>>;

private:
  /// Map from instruction which uses a memory location to a definition which
  /// can be propagated to replace operand in this instruction.
  using UseLocationMap = DenseMap<
    DILocation *, ReplacementT, DILocationMapInfo,
    TaggedDenseMapPair<
      bcl::tagged<DILocation *, Use>,
      bcl::tagged<ReplacementT, Def>>>;

  /// A statement in AST which correspond to an instruction which contains
  /// uses that can be replaced.
  struct TargetStmt {
    /// Definitions which can be used for replacement.
    UseLocationMap::mapped_type &PropagatedDefs;
    /// Root which corresponds to a some key in a UseLocationMap.
    Stmt *Root;
  };

public:
  explicit DefUseVisitor(TransformationContext &TfmCtx) :
    mRewriter(TfmCtx.getRewriter()),
    mSrcMgr(TfmCtx.getRewriter().getSourceMgr()) {}

  /// Return set of replacements in subtrees of a tree which represents
  /// expression at a specified location (create empty set if it does not
  /// exist).
  ///
  /// Note, that replacement for a subtree overrides a replacement for a tree.
  ReplacementT & getReplacement(DebugLoc UseLoc) {
    assert(UseLoc && UseLoc.get() && "Use location must not be null!");
    auto UseItr = mUseLocs.try_emplace(UseLoc.get()).first;
    return UseItr->get<Def>();
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    // Do not replace variables in increment/decrement because this operators
    // change an accessed variable:
    // `X = I; ++X; return I;` is not equivalent `X = I; ++I; return I`
    if (auto *UOp = dyn_cast<UnaryOperator>(S))
      if (UOp->isIncrementDecrementOp())
        return true;
    mParents.push(S);
    auto Loc = isa<Expr>(S) ? cast<Expr>(S)->getExprLoc() : S->getLocStart();
    bool Res = false;
    if (Loc.isValid() && Loc.isFileID()) {
      auto PLoc = mSrcMgr.getPresumedLoc(Loc);
      auto UseItr = mUseLocs.find_as(PLoc);
      if (UseItr != mUseLocs.end()) {
        LLVM_DEBUG(
            dbgs() << "[COPY PROPAGATION]: traverse propagation target at ";
            Loc.dump(mSrcMgr); dbgs() << "\n");
        mCurrUses.push(TargetStmt{ UseItr->get<Def>(), S });
        Res = RecursiveASTVisitor::TraverseStmt(S);
        mCurrUses.pop();
      } else {
        Res = RecursiveASTVisitor::TraverseStmt(S);
      }
    } else {
      Res = RecursiveASTVisitor::TraverseStmt(S);
    }
    mParents.pop();
    return Res;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Ref) {
    if (mCurrUses.empty())
      return true;
    auto ND = Ref->getFoundDecl();
    if (!ND->getDeclName().isIdentifier())
      return true;
    auto &Candidates = mCurrUses.top().PropagatedDefs;
    auto ReplacementItr = Candidates.find(ND->getName());
    if (ReplacementItr == Candidates.end())
       return true;
    LLVM_DEBUG(dbgs() << "[COPY PROPAGATION]: replace variable in [";
                 Ref->getLocStart().dump(mSrcMgr); dbgs() << ", ";
                 Ref->getLocEnd().dump(mSrcMgr);
                 dbgs() << "] with '" << ReplacementItr->second << "'\n");
    mRewriter.ReplaceText(
      SourceRange(Ref->getLocStart(), Ref->getLocEnd()),
      ReplacementItr->second);
    return true;
  }

private:
  Rewriter &mRewriter;
  SourceManager &mSrcMgr;
  UseLocationMap mUseLocs;

  std::stack<TargetStmt> mCurrUses;
  std::stack<Stmt *> mParents;
};
}

bool ClangCopyPropagation::unparseReplacement(
    const Value &Def, const DIMemoryLocation *DIDef,
    unsigned DWLang, const DIMemoryLocation &DIUse,
    SmallVectorImpl<char> &DefStr, SmallVectorImpl<char> &UseStr) {
  if (!unparseToString(DWLang, DIUse, UseStr))
    return false;
  if (auto *C = dyn_cast<Constant>(&Def)) {
    if (auto *CF = dyn_cast<Function>(&Def)) {
      auto *D = mTfmCtx->getDeclForMangledName(CF->getName());
      auto *ND = dyn_cast_or_null<NamedDecl>(D);
      if (!ND)
        return false;
      DefStr.assign(ND->getName().begin(), ND->getName().end());
    } else if (auto *CFP = dyn_cast<ConstantFP>(&Def)) {
      CFP->getValueAPF().toString(DefStr);
    } else if (auto *CInt = dyn_cast<ConstantInt>(&Def)) {
      auto *Ty = dyn_cast_or_null<DIBasicType>(
        DIUse.Var->getType().resolve());
      if (!Ty)
        return false;
      DefStr.clear();
      if (Ty->getEncoding() == dwarf::DW_ATE_signed)
        CInt->getValue().toStringSigned(DefStr);
      else if (Ty->getEncoding() == dwarf::DW_ATE_unsigned)
        CInt->getValue().toStringUnsigned(DefStr);
      else
        return false;
    }
    return true;
  } 
  if (!DIDef || !DIDef->isValid() || DIDef->Template || !DIDef->Loc)
    return false;
  if (!unparseToString(DWLang, *DIDef, DefStr))
    return false;
  if (StringRef(UseStr.data()) == DefStr.data())
    return false;
  return true;
}

bool ClangCopyPropagation::runOnFunction(Function &F) {
  auto *M = F.getParent();
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M->getContext().emitError("can not transform sources"
      ": transformation context is not available");
    return false;
  }
  auto FuncDecl = mTfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto DWLang = getLanguage(F);
  if (!DWLang)
    return false;
  auto &SrcMgr = mTfmCtx->getRewriter().getSourceMgr();
  if (SrcMgr.getFileCharacteristic(FuncDecl->getLocStart()) != SrcMgr::C_User)
    return false;
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  DefUseVisitor Visitor(*mTfmCtx);
  DenseSet<Value *> WorkSet;
  for (auto &I : instructions(F)) {
    auto DbgVal = dyn_cast<DbgValueInst>(&I);
    if (!DbgVal)
      continue;
    auto *Def = DbgVal->getValue();
    if (!Def || isa<UndefValue>(Def))
      continue;
    if (!WorkSet.insert(Def).second)
      continue;
    for (auto &U : Def->uses()) {
      if (!isa<Instruction>(U.getUser()))
        break;
      auto *UI = cast<Instruction>(U.getUser());
      if (!UI->getDebugLoc())
        continue;
      SmallVector<DIMemoryLocation, 4> DILocs;
      auto DIDef = findMetadata(Def, makeArrayRef(UI), *mDT, DILocs);
      if (DILocs.empty())
        continue;
      LLVM_DEBUG(dbgs() << "[COPY PROPAGATION]: remember instruction " << *UI
                        << "as a root for replacement at ";
                 UI->getDebugLoc().print(dbgs()); dbgs() << "\n");
      auto &Candidates = Visitor.getReplacement(UI->getDebugLoc());
      for (auto &DILoc : DILocs) {
        if (DILoc.Template)
          continue;
        SmallString<16> DefStr, UseStr;
        if (!unparseReplacement(*Def, DIDef ? &*DIDef : nullptr,
              *DWLang, DILoc, DefStr, UseStr))
          continue;
        LLVM_DEBUG(dbgs() << "[COPY PROPAGATION]: find source-level definition "
                          << DefStr << " for " << *Def << " to  replace ";
                   printDILocationSource(*DWLang, DILoc, dbgs());
                   dbgs() << "\n");
        //TODO (kaniandr@gmail.com): it is possible to propagate not only
        // variables, for example, accesses to members of a structure can be
        // also propagated. However, it is necessary to update processing of
        // AST in DefUseVisitor for members.
        Candidates.insert(std::make_pair(UseStr, DefStr));
      }
    }
  }
  Visitor.TraverseDecl(FuncDecl);
  return false;
}
