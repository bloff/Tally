/*
 * LLVM FunctionPass for the Rust/LLVM Tally prototype. It inserts calls to the
 * runtime budget hook at basic-block boundaries.
 */
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

class TallyInstrumentPass : public PassInfoMixin<TallyInstrumentPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    (void)AM;

    if (shouldSkipFunction(F)) {
      return PreservedAnalyses::all();
    }

    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();
    FunctionCallee Charge = M->getOrInsertFunction(
        "__tally_charge",
        FunctionType::get(Type::getVoidTy(Ctx), {Type::getInt64Ty(Ctx)}, false));

    bool Changed = false;
    for (BasicBlock &BB : F) {
      uint64_t Cost = blockCost(BB);
      if (Cost == 0) {
        continue;
      }

      Instruction *InsertBefore = insertionPoint(BB);
      if (!InsertBefore) {
        continue;
      }

      IRBuilder<> Builder(InsertBefore);
      Builder.CreateCall(Charge, ConstantInt::get(Type::getInt64Ty(Ctx), Cost));
      Changed = true;
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

private:
  static bool shouldSkipFunction(const Function &F) {
    if (F.isDeclaration() || F.isIntrinsic()) {
      return true;
    }

    StringRef Name = F.getName();
    return Name.starts_with("__tally_") || Name.starts_with("llvm.");
  }

  static uint64_t instructionCost(const Instruction &I) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I)) {
      return 0;
    }

    if (const auto *II = dyn_cast<IntrinsicInst>(&I)) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
      case Intrinsic::dbg_declare:
      case Intrinsic::dbg_value:
      case Intrinsic::dbg_label:
        return 0;
      default:
        return 1;
      }
    }

    if (const auto *CB = dyn_cast<CallBase>(&I)) {
      return 2 + CB->arg_size();
    }

    if (isa<SwitchInst>(I)) {
      return 2;
    }

    if (I.isTerminator()) {
      return 1;
    }

    return 1;
  }

  static uint64_t blockCost(const BasicBlock &BB) {
    uint64_t Cost = 0;
    for (const Instruction &I : BB) {
      Cost += instructionCost(I);
    }
    return Cost;
  }

  static bool shouldSkipPrologueInstruction(const Instruction &I) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I)) {
      return true;
    }

    const auto *II = dyn_cast<IntrinsicInst>(&I);
    if (!II) {
      return false;
    }

    switch (II->getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_value:
    case Intrinsic::dbg_label:
      return true;
    default:
      return false;
    }
  }

  static Instruction *insertionPoint(BasicBlock &BB) {
    auto It = BB.getFirstInsertionPt();
    while (It != BB.end() && shouldSkipPrologueInstruction(*It)) {
      ++It;
    }

    if (It == BB.end()) {
      return BB.getTerminator();
    }

    return &*It;
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION,
      "llvm-tally-pass",
      LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, FunctionPassManager &FPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name != "tally-instrument") {
                return false;
              }
              FPM.addPass(TallyInstrumentPass());
              return true;
            });
      },
  };
}
