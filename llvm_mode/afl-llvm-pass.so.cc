/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   Copyright 2015 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.

 */

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <list>
#include <string>
#include <fstream>
#include "llvm/DebugInfo.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

using namespace llvm;

namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;
      AFLCoverage() : ModulePass(ID) { 
        char* instWhiteListFilename = getenv("AFL_INST_WHITELIST");
        if (instWhiteListFilename) {
          std::ifstream fileStream;
          fileStream.open(instWhiteListFilename);
          if (!fileStream) report_fatal_error("Unable to open AFL_INST_WHITELIST");

          std::string line;
          getline(fileStream, line);
          while (fileStream) {
            myWhitelist.push_back(line);
            getline(fileStream, line);
          }
        }
      }

      bool runOnModule(Module &M) override;

      const char *getPassName() const override {
        return "American Fuzzy Lop Instrumentation";
      }

    protected:

      std::list<std::string> myWhitelist;
  };

}


char AFLCoverage::ID = 0;


bool AFLCoverage::runOnModule(Module &M) {

  LLVMContext &C = M.getContext();

  IntegerType *Int8Ty  = IntegerType::getInt8Ty(C);
  IntegerType *Int16Ty = IntegerType::getInt16Ty(C);
  IntegerType *Int64Ty = IntegerType::getInt64Ty(C);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  /* Get globals for the SHM region and the previous location. */

  GlobalVariable *AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

  GlobalVariable *AFLPrevLoc = new GlobalVariable(
      M, Int16Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc");

  /* Instrument all the things! */

  int inst_blocks = 0;

  for (auto &F : M)
    for (auto &BB : F) {

      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(IP);

      if (!myWhitelist.empty()) {
          bool instrumentBlock = false;

          /* Get the current location using debug information.
           * For now, just instrument the block if we are not able
           * to determine our location. */
          DebugLoc Loc = IP->getDebugLoc();
          if ( !Loc.isUnknown() ) {
              DILocation cDILoc(Loc.getAsMDNode(M.getContext()));
              DILocation oDILoc = cDILoc.getOrigLocation();

              unsigned int instLine = oDILoc.getLineNumber();
              StringRef instFilename = oDILoc.getFilename();

              if (instFilename.str().empty()) {
                  /* If the original location is empty, use the actual location */
                  instFilename = cDILoc.getFilename();
                  instLine = cDILoc.getLineNumber();
              }

              /* Continue only if we know where we actually are */
              if (!instFilename.str().empty()) {
                  for (std::list<std::string>::iterator it = myWhitelist.begin(); it != myWhitelist.end(); ++it) {
                      /* We don't check for filename equality here because
                       * filenames might actually be full paths. Instead we
                       * check that the actual filename ends in the filename
                       * specified in the list. */
                      if (instFilename.str().length() >= it->length()) {
                          if (instFilename.str().compare(instFilename.str().length() - it->length(), it->length(), *it) == 0) {
                              instrumentBlock = true;
                              break;
                          }
                      }
                  }
              }
          }

          /* Either we couldn't figure out our location or the location is
           * not whitelisted, so we skip instrumentation. */
          if (!instrumentBlock) continue;
      }

      if (R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int cur_loc = R(MAP_SIZE);
      ConstantInt *CurLoc = ConstantInt::get(Int64Ty, cur_loc);

      /* Load prev_loc */

      LoadInst *PrevLoc = IRB.CreateLoad(AFLPrevLoc);
      PrevLoc->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt64Ty());

      /* Load SHM pointer */

      LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
      MapPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *MapPtrIdx =
          IRB.CreateGEP(MapPtr, IRB.CreateXor(PrevLocCasted, CurLoc));

      /* Update bitmap */

      LoadInst *Counter = IRB.CreateLoad(MapPtrIdx);
      Counter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
      IRB.CreateStore(Incr, MapPtrIdx)
          ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      /* Set prev_loc to cur_loc >> 1 */

      StoreInst *Store =
          IRB.CreateStore(ConstantInt::get(Int16Ty, cur_loc >> 1), AFLPrevLoc);
      Store->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      inst_blocks++;

    }

  /* Say something nice. */

  if (!be_quiet) {

    if (!inst_blocks) WARNF("No instrumentation targets found.");
    else OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks,
             getenv("AFL_HARDEN") ? "hardened" : "non-hardened",
             inst_ratio);

  }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}


static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
