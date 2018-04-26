#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#include <llvm/IR/InstVisitor.h>
#include <llvm/Analysis/LoopInfo.h>
#include "Intrinsics.h"
#include "Registrator.h"
#include "tsar_instrumentation.h"
#include "CanonicalLoop.h"
#include "DFRegionInfo.h"
#include <sstream>

class Instrumentation :public llvm::InstVisitor<Instrumentation> {
public:
  enum BaseTypeID { VoidTy = 0, HalfTy, FloatTy, DoubleTy, X86_FP80Ty, FP128Ty,
    PPC_FP128Ty, LabelTy, MetadataTy, X86_MMXTy, TokenTy, IntegerTy };

  static const unsigned maxIntBitWidth = 64;

  Instrumentation(llvm::Module& M, llvm::InstrumentationPass* const I);
  ~Instrumentation() {delete &mLoopInfo; delete &mRegionInfo;}

  void visitAllocaInst(llvm::AllocaInst &I);
  void visitLoadInst(llvm::LoadInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitCallInst(llvm::CallInst &I);
  void visitInvokeInst(llvm::InvokeInst &I);
  void visitReturnInst(llvm::ReturnInst &I);
  void visitFunction(llvm::Function &F);
  
  unsigned getTypeId(const llvm::Type& T);
private:
  Registrator mRegistrator;
  llvm::LoopInfo& mLoopInfo;
  llvm::DFRegionInfo* mRegionInfo;
  tsar::CanonicalLoopSet* mCanonicalLoop;
  llvm::InstrumentationPass* const mInstrPass;

  //visitCallInst and visiInvokeInst have completely the same code
  //so template for them
  //
  //NOTE: Instead of template it was possible to overload visitCallSite which 
  //is for both calls and invokes. Maybe i'll change it later. 
  template<class T>
  void FunctionCallInst(T &I) {
    //not llvm function
    if(I.getCalledFunction()->isIntrinsic())
      return;
    //not tsar function
    tsar::IntrinsicId Id;
    if(getTsarLibFunc(I.getCalledFunction()->getName(), Id)) {
      return;
    }
    std::stringstream Debug;
    Debug << "type=func_call*file=" << I.getModule()->getSourceFileName()
      << "*line1=" << I.getDebugLoc()->getLine() << "*name1=" << 
      I.getCalledFunction()->getSubprogram()->getName().data() << "*rank=" << 
      I.getCalledFunction()->getFunctionType()->getNumParams() << "**";
    auto DICall = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
    auto Fun = getDeclaration(I.getModule(),tsar::IntrinsicId::func_call_begin);
    llvm::CallInst::Create(Fun, {DICall}, "", &I);
    Fun = getDeclaration(I.getModule(), tsar::IntrinsicId::func_call_end);
    auto Call = llvm::CallInst::Create(Fun, {DICall}, "");
    Call->insertAfter(&I);
  }

  //visitBasicBlock depends on LoopInfo which is different for
  //different Functions. Calling it from wherever except visitFunction
  //could lead to using unappropriate LoopInfo. So moved it to private.
  void visitBasicBlock(llvm::BasicBlock &B);

  void loopBeginInstr(llvm::Loop *L, llvm::BasicBlock& Header, unsigned);
  void loopEndInstr(llvm::Loop const *L, llvm::BasicBlock& Header, unsigned);
  void loopIterInstr(llvm::Loop *L, llvm::BasicBlock& Header, unsigned);

  unsigned regDbgStr(const std::string& S, llvm::Module& M);
  void regTypes(llvm::Module& M);
  llvm::GetElementPtrInst* prepareStrParam(const std::string& S, 
    llvm::Instruction &I);
  llvm::LoadInst* getDbgPoolElem(unsigned Val, llvm::Instruction& I); 
  void regGlobals(llvm::Module& M);
  void regBaseTypes(llvm::Module& M);
  void instrumentateMain(llvm::Module& M); 
};

#endif // INSTRUMENTATION_H