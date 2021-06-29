//===-- llvm-dis.cpp - The low-level LLVM disassembler --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This utility may be invoked in the following manner:
//  llvm-dis [options]      - Read LLVM bitcode from stdin, write asm to stdout
//  llvm-dis [options] x.bc - Read LLVM bitcode from the x.bc file, write asm
//                            to the x.ll file.
//  Options:
//      --help   - Output information about command line switches
//
//===----------------------------------------------------------------------===//

#include "llvm-c/Core.h"
#include "llvm-c/DebugInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include <iostream>
#include <system_error>
using namespace llvm;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string> FunctionName(cl::Positional,
                                         cl::desc("<function name>"));

static cl::opt<std::int32_t> ArraySize("s", cl::desc("Override array size"),
                                       cl::init(1024));

static cl::opt<std::string> TargetTriple("t",
                                         cl::desc("Override target triple"),
                                         cl::init("x86_64-pc-linux-gnu"));

namespace {

static void printDebugLoc(const DebugLoc &DL, formatted_raw_ostream &OS) {
  OS << DL.getLine() << ":" << DL.getCol();
  if (DILocation *IDL = DL.getInlinedAt()) {
    OS << "@";
    printDebugLoc(IDL, OS);
  }
}
class CommentWriter : public AssemblyAnnotationWriter {
public:
  void emitFunctionAnnot(const Function *F,
                         formatted_raw_ostream &OS) override {
    OS << "; [#uses=" << F->getNumUses() << ']'; // Output # uses
    OS << '\n';
  }
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override {
    bool Padded = false;
    if (!V.getType()->isVoidTy()) {
      OS.PadToColumn(50);
      Padded = true;
      // Output # uses and type
      OS << "; [#uses=" << V.getNumUses() << " type=" << *V.getType() << "]";
    }
    if (const Instruction *I = dyn_cast<Instruction>(&V)) {
      if (const DebugLoc &DL = I->getDebugLoc()) {
        if (!Padded) {
          OS.PadToColumn(50);
          Padded = true;
          OS << ";";
        }
        OS << " [debug line = ";
        printDebugLoc(DL, OS);
        OS << "]";
      }
      if (const DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DDI->getVariable()->getName() << "]";
      } else if (const DbgValueInst *DVI = dyn_cast<DbgValueInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DVI->getVariable()->getName() << "]";
      }
    }
  }
};

struct LLVMDisDiagnosticHandler : public DiagnosticHandler {
  char *Prefix;
  LLVMDisDiagnosticHandler(char *PrefixPtr) : Prefix(PrefixPtr) {}
  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    raw_ostream &OS = errs();
    OS << Prefix << ": ";
    switch (DI.getSeverity()) {
    case DS_Error:
      WithColor::error(OS);
      break;
    case DS_Warning:
      WithColor::warning(OS);
      break;
    case DS_Remark:
      OS << "remark: ";
      break;
    case DS_Note:
      WithColor::note(OS);
      break;
    }

    DiagnosticPrinterRawOStream DP(OS);
    DI.print(DP);
    OS << '\n';

    if (DI.getSeverity() == DS_Error)
      exit(1);
    return true;
  }
};
} // namespace

static ExitOnError ExitOnErr;

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  ExitOnErr.setBanner(std::string(argv[0]) + ": error: ");

  cl::ParseCommandLineOptions(argc, argv, "llvm .bc -> klee bc .kbc \n");

  LLVMContext Context;
  Context.setDiagnosticHandler(
      std::make_unique<LLVMDisDiagnosticHandler>(argv[0]));

  std::unique_ptr<MemoryBuffer> MB =
      ExitOnErr(errorOrToExpected(MemoryBuffer::getFileOrSTDIN(InputFilename)));

  BitcodeFileContents IF = ExitOnErr(llvm::getBitcodeFileContents(*MB));

  const size_t N = IF.Mods.size();

  for (size_t I = 0; I < N; ++I) {
    BitcodeModule MB = IF.Mods[I];
    std::unique_ptr<Module> M =
        ExitOnErr(MB.getLazyModule(Context, false, false));
    ExitOnErr(M->materializeAll());

    M->setTargetTriple(TargetTriple);
    Function *F = M->getFunction(FunctionName);
    unsigned arg_size = F->arg_size();
    unsigned idx = 0;
    std::cout << "#include <stdint.h>" << std::endl;
    std::cout << "#include <stdlib.h>" << std::endl;
    std::cout << std::endl;
    std::cout << "#ifdef __KLEE__" << std::endl;
    std::cout << "#include <klee/klee.h>" << std::endl;
    std::cout << "#endif" << std::endl;
    std::cout << std::endl;

    std::cout << "#define i8 int8_t\n";
    std::cout << "#define i16 int16_t\n";
    std::cout << "#define i32 int32_t\n";
    std::cout << "#define i64 int64_t\n";
    std::cout << "#define i128 int128_t\n";
    std::cout << std::endl;
    std::cout << "int main(int argc, char** argv) {" << std::endl;
    std::cout << "#ifdef __KLEE__" << std::endl;
    BasicBlock *BB = &F->getEntryBlock();
    std::vector<std::string> args;
    for (auto &I : *BB) {
      if (idx >= arg_size)
        break;
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        MDNode *MD = cast<MDNode>(
            cast<MetadataAsValue>(CI->getOperand(1))->getMetadata());
        DILocalVariable *V = cast<DILocalVariable>(MD);
        // get the name of function arg from Metadata
        StringRef name = V->getName();
        args.push_back(name.str());
        unsigned i = 0;
        for (auto &arg : F->args()) {
          if (idx == i) {
            if (arg.getType()->isPointerTy()) {
              // TODO: hack for pointer size now
              std::cout << "  char " << name.str() << "[" << ArraySize
                        << "];\n";
              std::cout << "  klee_make_symbolic(" << name.str() << ", sizeof("
                        << name.str() << "), \"" << name.str() << "\");"
                        << std::endl;
            } else {
              size_t size = 0;
              if (arg.getType()->isIntegerTy()) {
                size = arg.getType()->getIntegerBitWidth();
              }
              std::cout << "  i" << size << " " << name.str() << ";"
                        << std::endl;
              std::cout << "  klee_make_symbolic(&" << name.str() << ", sizeof("
                        << name.str() << "), \"" << name.str() << "\");"
                        << std::endl;
            }
            break;
          }
          i++;
        }
        idx++;
      }
    }
    std::cout << "  " << FunctionName << "(";
    unsigned i = 0;
    for (auto &a : args) {
      std::cout << a;
      if (i < arg_size - 1)
        std::cout << ", ";
      else
        std::cout << ");" << std::endl;
      i++;
    }

    std::cout << "#endif" << std::endl;
    std::cout << std::endl;
    std::cout << "  return 0;" << std::endl;
    std::cout << "}" << std::endl;
  }

  return 0;
}
