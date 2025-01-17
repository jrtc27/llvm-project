//===-- AsmPrinterDwarf.cpp - AsmPrinter Dwarf Support --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Dwarf emissions parts of AsmPrinter.
//
//===----------------------------------------------------------------------===//

#include "ByteStreamer.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/DIE.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MachineLocation.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetMachine.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

//===----------------------------------------------------------------------===//
// Dwarf Emission Helper Routines
//===----------------------------------------------------------------------===//

/// EmitSLEB128 - emit the specified signed leb128 value.
void AsmPrinter::emitSLEB128(int64_t Value, const char *Desc) const {
  if (isVerbose() && Desc)
    OutStreamer->AddComment(Desc);

  OutStreamer->emitSLEB128IntValue(Value);
}

void AsmPrinter::emitULEB128(uint64_t Value, const char *Desc,
                             unsigned PadTo) const {
  if (isVerbose() && Desc)
    OutStreamer->AddComment(Desc);

  OutStreamer->emitULEB128IntValue(Value, PadTo);
}

/// Emit something like ".uleb128 Hi-Lo".
void AsmPrinter::emitLabelDifferenceAsULEB128(const MCSymbol *Hi,
                                              const MCSymbol *Lo) const {
  OutStreamer->emitAbsoluteSymbolDiffAsULEB128(Hi, Lo);
}

static const char *DecodeDWARFEncoding(unsigned Encoding) {
  switch (Encoding) {
  case dwarf::DW_EH_PE_absptr:
    return "absptr";
  case dwarf::DW_EH_PE_omit:
    return "omit";
  case dwarf::DW_EH_PE_pcrel:
    return "pcrel";
  case dwarf::DW_EH_PE_uleb128:
    return "uleb128";
  case dwarf::DW_EH_PE_sleb128:
    return "sleb128";
  case dwarf::DW_EH_PE_udata4:
    return "udata4";
  case dwarf::DW_EH_PE_udata8:
    return "udata8";
  case dwarf::DW_EH_PE_sdata4:
    return "sdata4";
  case dwarf::DW_EH_PE_sdata8:
    return "sdata8";
  case dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_udata4:
    return "pcrel udata4";
  case dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_sdata4:
    return "pcrel sdata4";
  case dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_udata8:
    return "pcrel udata8";
  case dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_sdata8:
    return "pcrel sdata8";
  case dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_udata4
      :
    return "indirect pcrel udata4";
  case dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_sdata4
      :
    return "indirect pcrel sdata4";
  case dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_udata8
      :
    return "indirect pcrel udata8";
  case dwarf::DW_EH_PE_indirect | dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_sdata8
      :
    return "indirect pcrel sdata8";
  }

  return "<unknown encoding>";
}

/// EmitEncodingByte - Emit a .byte 42 directive that corresponds to an
/// encoding.  If verbose assembly output is enabled, we output comments
/// describing the encoding.  Desc is an optional string saying what the
/// encoding is specifying (e.g. "LSDA").
void AsmPrinter::emitEncodingByte(unsigned Val, const char *Desc) const {
  if (isVerbose()) {
    if (Desc)
      OutStreamer->AddComment(Twine(Desc) + " Encoding = " +
                              Twine(DecodeDWARFEncoding(Val)));
    else
      OutStreamer->AddComment(Twine("Encoding = ") + DecodeDWARFEncoding(Val));
  }

  OutStreamer->emitIntValue(Val, 1);
}

/// GetSizeOfEncodedValue - Return the size of the encoding in bytes.
unsigned AsmPrinter::GetSizeOfEncodedValue(unsigned Encoding) const {
  if (Encoding == dwarf::DW_EH_PE_omit)
    return 0;

  switch (Encoding & 0x07) {
  default:
    llvm_unreachable("Invalid encoded value.");
  case dwarf::DW_EH_PE_absptr:
    assert(!MF->getDataLayout().isFatPointer(0u) &&
           "Should not use GetSizeOfEncodedValue when emitting capabilities");
    return MF->getDataLayout().getPointerSize();
  case dwarf::DW_EH_PE_udata2:
    return 2;
  case dwarf::DW_EH_PE_udata4:
    return 4;
  case dwarf::DW_EH_PE_udata8:
    return 8;
  }
}

void AsmPrinter::emitTTypeReference(const GlobalValue *GV,
                                    unsigned Encoding) const {
  if (GV) {
    const TargetLoweringObjectFile &TLOF = getObjFileLowering();

    const MCExpr *Exp =
        TLOF.getTTypeGlobalReference(GV, Encoding, TM, MMI, *OutStreamer);
    OutStreamer->emitValue(Exp, GetSizeOfEncodedValue(Encoding));
  } else
    OutStreamer->emitIntValue(0, GetSizeOfEncodedValue(Encoding));
}

void AsmPrinter::emitDwarfSymbolReference(const MCSymbol *Label,
                                          bool ForceOffset) const {
  if (!ForceOffset) {
    // On COFF targets, we have to emit the special .secrel32 directive.
    if (MAI->needsDwarfSectionOffsetDirective()) {
      OutStreamer->EmitCOFFSecRel32(Label, /*Offset=*/0);
      return;
    }

    // If the format uses relocations with dwarf, refer to the symbol directly.
    if (MAI->doesDwarfUseRelocationsAcrossSections()) {
      OutStreamer->emitSymbolValue(Label, 4);
      return;
    }
  }

  // Otherwise, emit it as a label difference from the start of the section.
  emitLabelDifference(Label, Label->getSection().getBeginSymbol(), 4);
}

void AsmPrinter::emitDwarfStringOffset(DwarfStringPoolEntry S) const {
  if (MAI->doesDwarfUseRelocationsAcrossSections()) {
    assert(S.Symbol && "No symbol available");
    emitDwarfSymbolReference(S.Symbol);
    return;
  }

  // Just emit the offset directly; no need for symbol math.
  emitInt32(S.Offset);
}

void AsmPrinter::emitDwarfOffset(const MCSymbol *Label, uint64_t Offset) const {
  // TODO: Support DWARF64
  emitLabelPlusOffset(Label, Offset, 4);
}

void AsmPrinter::emitCallSiteOffset(const MCSymbol *Hi, const MCSymbol *Lo,
                                    unsigned Encoding) const {
  // The least significant 3 bits specify the width of the encoding
  if ((Encoding & 0x7) == dwarf::DW_EH_PE_uleb128)
    emitLabelDifferenceAsULEB128(Hi, Lo);
  else
    emitLabelDifference(Hi, Lo, GetSizeOfEncodedValue(Encoding));
}

void AsmPrinter::emitCallSiteValue(uint64_t Value, unsigned Encoding) const {
  // The least significant 3 bits specify the width of the encoding
  if ((Encoding & 0x7) == dwarf::DW_EH_PE_uleb128)
    emitULEB128(Value);
  else
    OutStreamer->emitIntValue(Value, GetSizeOfEncodedValue(Encoding));
}

void AsmPrinter::emitCallSiteCheriCapability(const MCSymbol *Hi,
                                             const MCSymbol *Lo) const {
  const TargetLoweringObjectFile &TLOF = getObjFileLowering();
  // Get the Hi-Lo expression. We use (and need) Lo since the offset needs to
  // be a constant expression, whereas CurrentFnSym is preemptible.
  const MCExpr *DiffToStart = MCBinaryExpr::createSub(
      MCSymbolRefExpr::create(Hi, OutContext),
      MCSymbolRefExpr::create(Lo, OutContext),
      OutContext);
  // Note: we cannot use Lo here since that is a local symbol and then
  // EmitCheriCapability() will create a relocation against section plus offset
  // rather than function + offset. We need the right bounds and permissions
  // info.
  OutStreamer->EmitCheriCapability(CurrentFnSym, DiffToStart,
                                   TLOF.getCheriCapabilitySize(TM));
}

//===----------------------------------------------------------------------===//
// Dwarf Lowering Routines
//===----------------------------------------------------------------------===//

void AsmPrinter::emitCFIInstruction(const MCCFIInstruction &Inst) const {
  switch (Inst.getOperation()) {
  default:
    llvm_unreachable("Unexpected instruction");
  case MCCFIInstruction::OpDefCfaOffset:
    OutStreamer->emitCFIDefCfaOffset(Inst.getOffset());
    break;
  case MCCFIInstruction::OpAdjustCfaOffset:
    OutStreamer->emitCFIAdjustCfaOffset(Inst.getOffset());
    break;
  case MCCFIInstruction::OpDefCfa:
    OutStreamer->emitCFIDefCfa(Inst.getRegister(), Inst.getOffset());
    break;
  case MCCFIInstruction::OpDefCfaRegister:
    OutStreamer->emitCFIDefCfaRegister(Inst.getRegister());
    break;
  case MCCFIInstruction::OpOffset:
    OutStreamer->emitCFIOffset(Inst.getRegister(), Inst.getOffset());
    break;
  case MCCFIInstruction::OpRegister:
    OutStreamer->emitCFIRegister(Inst.getRegister(), Inst.getRegister2());
    break;
  case MCCFIInstruction::OpWindowSave:
    OutStreamer->emitCFIWindowSave();
    break;
  case MCCFIInstruction::OpNegateRAState:
    OutStreamer->emitCFINegateRAState();
    break;
  case MCCFIInstruction::OpSameValue:
    OutStreamer->emitCFISameValue(Inst.getRegister());
    break;
  case MCCFIInstruction::OpGnuArgsSize:
    OutStreamer->emitCFIGnuArgsSize(Inst.getOffset());
    break;
  case MCCFIInstruction::OpEscape:
    OutStreamer->emitCFIEscape(Inst.getValues());
    break;
  case MCCFIInstruction::OpRestore:
    OutStreamer->emitCFIRestore(Inst.getRegister());
    break;
  case MCCFIInstruction::OpUndefined:
    OutStreamer->emitCFIUndefined(Inst.getRegister());
    break;
  }
}

void AsmPrinter::emitDwarfDIE(const DIE &Die) const {
  // Emit the code (index) for the abbreviation.
  if (isVerbose())
    OutStreamer->AddComment("Abbrev [" + Twine(Die.getAbbrevNumber()) + "] 0x" +
                            Twine::utohexstr(Die.getOffset()) + ":0x" +
                            Twine::utohexstr(Die.getSize()) + " " +
                            dwarf::TagString(Die.getTag()));
  emitULEB128(Die.getAbbrevNumber());

  // Emit the DIE attribute values.
  for (const auto &V : Die.values()) {
    dwarf::Attribute Attr = V.getAttribute();
    assert(V.getForm() && "Too many attributes for DIE (check abbreviation)");

    if (isVerbose()) {
      OutStreamer->AddComment(dwarf::AttributeString(Attr));
      if (Attr == dwarf::DW_AT_accessibility)
        OutStreamer->AddComment(
            dwarf::AccessibilityString(V.getDIEInteger().getValue()));
    }

    // Emit an attribute using the defined form.
    V.emitValue(this);
  }

  // Emit the DIE children if any.
  if (Die.hasChildren()) {
    for (auto &Child : Die.children())
      emitDwarfDIE(Child);

    OutStreamer->AddComment("End Of Children Mark");
    emitInt8(0);
  }
}

void AsmPrinter::emitDwarfAbbrev(const DIEAbbrev &Abbrev) const {
  // Emit the abbreviations code (base 1 index.)
  emitULEB128(Abbrev.getNumber(), "Abbreviation Code");

  // Emit the abbreviations data.
  Abbrev.Emit(this);
}
