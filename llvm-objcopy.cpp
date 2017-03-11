//===-- llvm-objcopy.cpp - Object file copying utility for llvm -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under  University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that mimics a tiny subset of binutils "objcopy".
//
//===----------------------------------------------------------------------===//

#include "llvm-objcopy.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace object;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input object file>"), cl::Required);
static cl::opt<std::string>
OutputFilename(cl::Positional, cl::desc("<output object file>"), cl::Required);


namespace {
    enum OutputFormatTy { binary };
    cl::opt<OutputFormatTy>
        OutputTarget("O",
                cl::desc("Specify output target"),
                cl::values(clEnumVal(binary, "raw binary")),
                cl::init(binary));
    cl::alias OutputTarget2("output-target", cl::desc("Alias for -O"),
            cl::aliasopt(OutputTarget));

    static StringRef ToolName;
}

bool error(std::error_code ec) {
    if (!ec) return false;
    return true;
}
class ObjectCopyBase {
public:
    ObjectCopyBase(StringRef InputFilename) 
        : mBinaryOutput(false)
        , mFillGaps(false)
    {
    }
    virtual ~ObjectCopyBase() {}

    void CopyTo(ObjectFile *o, StringRef OutputFilename) const {
        if (o == NULL) {
            return;
        }

        std::error_code ErrorCode;

        tool_output_file Out(OutputFilename.data(), ErrorCode,  sys::fs::F_None);
        
        std::error_code  ec;
        bool        FillNextGap = false;
        uint64_t    LastAddress = 0;
        StringRef   LastSectionName;

        for (section_iterator si = o->section_begin(), se = o->section_end(); si != se; ++si) {
                       StringRef SectionName;
            StringRef SectionContents;
            uint64_t  SectionAddress;
            bool      BSS;
            bool      Required;

   
            if (error(si->getName(SectionName))) return;
            if (error(si->getContents(SectionContents))) return;
            SectionAddress = si->getAddress();
            BSS = si->isBSS();
            
            if ( BSS
                || SectionContents.size() == 0) {
                continue;
            }

            if (FillNextGap) {
                if (SectionAddress < LastAddress) {
                    errs() << "Trying to fill gaps between sections " << LastSectionName << " and " << SectionName << " in invalid order\n";
                    continue;//return;
                } else if (SectionAddress == LastAddress) {
                    // No gap, do nothing
                } else if (SectionAddress - LastAddress > (1<<16)) {
                    // Gap size limit reached
                    errs() << "Gap between sections is too large\n";
                    return;
                } else {
                    FillGap(Out, 0x00, SectionAddress - LastAddress);
                }
            }

            PrintSection(Out, SectionName, SectionContents, SectionAddress);

            if (mFillGaps) {
                FillNextGap     = true;
                LastSectionName = SectionName;
                LastAddress     = SectionAddress + SectionContents.size();
            }
        }

        Out.keep();
    }


protected:
    virtual void PrintSection(tool_output_file &Out, const StringRef &SectionName,
                              const StringRef &SectionContents, uint64_t SectionAddress) const = 0;
    virtual void FillGap(tool_output_file &Out, unsigned char Value, uint64_t Size) const { }

    bool                  mBinaryOutput;
    bool                  mFillGaps;
};



class ObjectCopyBinary : public ObjectCopyBase {
public:
    ObjectCopyBinary(StringRef InputFilename) 
        : ObjectCopyBase(InputFilename)
    {
        mBinaryOutput = true;
        mFillGaps     = true;
    }
    virtual ~ObjectCopyBinary() {}

protected:
    virtual void PrintSection(tool_output_file &Out, const StringRef &SectionName,
                              const StringRef &SectionContents, uint64_t SectionAddress) const
    {
        uint64_t addr;
        uint64_t end;
        for (addr = 0, end = SectionContents.size(); addr < end; ++addr) {
            Out.os() << SectionContents[addr];
        }
    }

    virtual void FillGap(tool_output_file &Out, unsigned char Value, uint64_t Size) const
    {
        uint64_t i;
        for (i = 0; i < Size; ++i) {
            Out.os() << Value;
        }
    }
};

int main(int argc, char **argv) {
    // Print a stack trace if we signal out.
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram X(argc, argv);
    llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

    cl::ParseCommandLineOptions(argc, argv, "llvm object file copy utility\n");

    ToolName = argv[0];
    std::unique_ptr<ObjectCopyBase> ObjectCopy;

    switch (OutputTarget) {
    case OutputFormatTy::binary:
        ObjectCopy.reset(new ObjectCopyBinary(InputFilename));
        break;
    default:
        return 1;
    }


    // If file isn't stdin, check that it exists.
    if (InputFilename != "-" && !sys::fs::exists(InputFilename)) {
        errs() << ToolName << ": '" << InputFilename << "': " << "No such file\n";
        return 1;
    }

    // Attempt to open the binary.
    Expected<OwningBinary<Binary>> BinaryOrErr = createBinary(InputFilename);
    if (!BinaryOrErr)
        return 1;
    Binary &Binary = *BinaryOrErr.get().getBinary();
    
    ObjectFile *Obj = dyn_cast<ObjectFile>(&Binary);
    if (!Obj) {
        return 1;
    }

    ObjectCopy->CopyTo(Obj, OutputFilename);

    return 0;
}
