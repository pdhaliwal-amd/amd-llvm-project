//===----------------------- SIFrameLowering.cpp --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//==-----------------------------------------------------------------------===//

#include "SIFrameLowering.h"
#include "AMDGPU.h"
#include "GCNSubtarget.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "SIMachineFunctionInfo.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "frame-info"

static cl::opt<bool> EnableSpillVGPRToAGPR(
  "amdgpu-spill-vgpr-to-agpr",
  cl::desc("Enable spilling VGPRs to AGPRs"),
  cl::ReallyHidden,
  cl::init(true));

// Find a scratch register that we can use in the prologue. We avoid using
// callee-save registers since they may appear to be free when this is called
// from canUseAsPrologue (during shrink wrapping), but then no longer be free
// when this is called from emitPrologue.
static MCRegister findScratchNonCalleeSaveRegister(MachineRegisterInfo &MRI,
                                                   LivePhysRegs &LiveRegs,
                                                   const TargetRegisterClass &RC,
                                                   bool Unused = false) {
  // Mark callee saved registers as used so we will not choose them.
  const MCPhysReg *CSRegs = MRI.getCalleeSavedRegs();
  for (unsigned i = 0; CSRegs[i]; ++i)
    LiveRegs.addReg(CSRegs[i]);

  if (Unused) {
    // We are looking for a register that can be used throughout the entire
    // function, so any use is unacceptable.
    for (MCRegister Reg : RC) {
      if (!MRI.isPhysRegUsed(Reg) && LiveRegs.available(MRI, Reg))
        return Reg;
    }
  } else {
    for (MCRegister Reg : RC) {
      if (LiveRegs.available(MRI, Reg))
        return Reg;
    }
  }

  return MCRegister();
}

static void getVGPRSpillLaneOrTempRegister(MachineFunction &MF,
                                           LivePhysRegs &LiveRegs,
                                           Register &TempSGPR,
                                           Optional<int> &FrameIndex,
                                           bool IsFP) {
  SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  MachineFrameInfo &FrameInfo = MF.getFrameInfo();

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();

  // We need to save and restore the current FP/BP.

  // 1: If there is already a VGPR with free lanes, use it. We
  // may already have to pay the penalty for spilling a CSR VGPR.
  if (MFI->haveFreeLanesForSGPRSpill(MF, 1)) {
    int NewFI = FrameInfo.CreateStackObject(4, Align(4), true, nullptr,
                                            TargetStackID::SGPRSpill);

    if (!MFI->allocateSGPRSpillToVGPR(MF, NewFI))
      llvm_unreachable("allocate SGPR spill should have worked");

    FrameIndex = NewFI;

    LLVM_DEBUG(auto Spill = MFI->getSGPRToVGPRSpills(NewFI).front();
               dbgs() << "Spilling " << (IsFP ? "FP" : "BP") << " to  "
                      << printReg(Spill.VGPR, TRI) << ':' << Spill.Lane
                      << '\n');
    return;
  }

  // 2: Next, try to save the FP/BP in an unused SGPR.
  TempSGPR = findScratchNonCalleeSaveRegister(
      MF.getRegInfo(), LiveRegs, AMDGPU::SReg_32_XM0_XEXECRegClass, true);

  if (!TempSGPR) {
    int NewFI = FrameInfo.CreateStackObject(4, Align(4), true, nullptr,
                                            TargetStackID::SGPRSpill);

    if (TRI->spillSGPRToVGPR() && MFI->allocateSGPRSpillToVGPR(MF, NewFI)) {
      // 3: There's no free lane to spill, and no free register to save FP/BP,
      // so we're forced to spill another VGPR to use for the spill.
      FrameIndex = NewFI;

      LLVM_DEBUG(
          auto Spill = MFI->getSGPRToVGPRSpills(NewFI).front();
          dbgs() << (IsFP ? "FP" : "BP") << " requires fallback spill to "
                 << printReg(Spill.VGPR, TRI) << ':' << Spill.Lane << '\n';);
    } else {
      // Remove dead <NewFI> index
      MF.getFrameInfo().RemoveStackObject(NewFI);
      // 4: If all else fails, spill the FP/BP to memory.
      FrameIndex = FrameInfo.CreateSpillStackObject(4, Align(4));
      LLVM_DEBUG(dbgs() << "Reserved FI " << FrameIndex << " for spilling "
                        << (IsFP ? "FP" : "BP") << '\n');
    }
  } else {
    LLVM_DEBUG(dbgs() << "Saving " << (IsFP ? "FP" : "BP") << " with copy to "
                      << printReg(TempSGPR, TRI) << '\n');
  }
}

// We need to specially emit stack operations here because a different frame
// register is used than in the rest of the function, as getFrameRegister would
// use.
static void buildPrologSpill(const GCNSubtarget &ST, const SIRegisterInfo &TRI,
                             const SIMachineFunctionInfo &FuncInfo,
                             LivePhysRegs &LiveRegs, MachineFunction &MF,
                             MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator I, const DebugLoc &DL,
                             Register SpillReg, int FI, int64_t DwordOff = 0) {
  unsigned Opc = ST.enableFlatScratch() ? AMDGPU::SCRATCH_STORE_DWORD_SADDR
                                        : AMDGPU::BUFFER_STORE_DWORD_OFFSET;

  MachineFrameInfo &FrameInfo = MF.getFrameInfo();
  MachinePointerInfo PtrInfo = MachinePointerInfo::getFixedStack(MF, FI);
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      PtrInfo, MachineMemOperand::MOStore, FrameInfo.getObjectSize(FI),
      FrameInfo.getObjectAlign(FI));
  LiveRegs.addReg(SpillReg);
  TRI.buildSpillLoadStore(MBB, I, DL, Opc, FI, SpillReg, true,
                          FuncInfo.getStackPtrOffsetReg(), DwordOff, MMO,
                          nullptr, &LiveRegs);
  LiveRegs.removeReg(SpillReg);
}

static void buildEpilogRestore(const GCNSubtarget &ST,
                               const SIRegisterInfo &TRI,
                               const SIMachineFunctionInfo &FuncInfo,
                               LivePhysRegs &LiveRegs, MachineFunction &MF,
                               MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator I,
                               const DebugLoc &DL, Register SpillReg, int FI) {
  unsigned Opc = ST.enableFlatScratch() ? AMDGPU::SCRATCH_LOAD_DWORD_SADDR
                                        : AMDGPU::BUFFER_LOAD_DWORD_OFFSET;

  MachineFrameInfo &FrameInfo = MF.getFrameInfo();
  MachinePointerInfo PtrInfo = MachinePointerInfo::getFixedStack(MF, FI);
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      PtrInfo, MachineMemOperand::MOLoad, FrameInfo.getObjectSize(FI),
      FrameInfo.getObjectAlign(FI));
  TRI.buildSpillLoadStore(MBB, I, DL, Opc, FI, SpillReg, false,
                          FuncInfo.getStackPtrOffsetReg(), 0, MMO, nullptr,
                          &LiveRegs);
}

static void buildGitPtr(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                        const DebugLoc &DL, const SIInstrInfo *TII,
                        Register TargetReg) {
  MachineFunction *MF = MBB.getParent();
  const SIMachineFunctionInfo *MFI = MF->getInfo<SIMachineFunctionInfo>();
  const SIRegisterInfo *TRI = &TII->getRegisterInfo();
  const MCInstrDesc &SMovB32 = TII->get(AMDGPU::S_MOV_B32);
  Register TargetLo = TRI->getSubReg(TargetReg, AMDGPU::sub0);
  Register TargetHi = TRI->getSubReg(TargetReg, AMDGPU::sub1);

  if (MFI->getGITPtrHigh() != 0xffffffff) {
    BuildMI(MBB, I, DL, SMovB32, TargetHi)
        .addImm(MFI->getGITPtrHigh())
        .addReg(TargetReg, RegState::ImplicitDefine);
  } else {
    const MCInstrDesc &GetPC64 = TII->get(AMDGPU::S_GETPC_B64);
    BuildMI(MBB, I, DL, GetPC64, TargetReg);
  }
  Register GitPtrLo = MFI->getGITPtrLoReg(*MF);
  MF->getRegInfo().addLiveIn(GitPtrLo);
  MBB.addLiveIn(GitPtrLo);
  BuildMI(MBB, I, DL, SMovB32, TargetLo)
    .addReg(GitPtrLo);
}

// Emit flat scratch setup code, assuming `MFI->hasFlatScratchInit()`
void SIFrameLowering::emitEntryFunctionFlatScratchInit(
    MachineFunction &MF, MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
    const DebugLoc &DL, Register ScratchWaveOffsetReg) const {
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo *TRI = &TII->getRegisterInfo();
  const SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();

  // We don't need this if we only have spills since there is no user facing
  // scratch.

  // TODO: If we know we don't have flat instructions earlier, we can omit
  // this from the input registers.
  //
  // TODO: We only need to know if we access scratch space through a flat
  // pointer. Because we only detect if flat instructions are used at all,
  // this will be used more often than necessary on VI.

  Register FlatScrInitLo;
  Register FlatScrInitHi;

  if (ST.isAmdPalOS()) {
    // Extract the scratch offset from the descriptor in the GIT
    LivePhysRegs LiveRegs;
    LiveRegs.init(*TRI);
    LiveRegs.addLiveIns(MBB);

    // Find unused reg to load flat scratch init into
    MachineRegisterInfo &MRI = MF.getRegInfo();
    Register FlatScrInit = AMDGPU::NoRegister;
    ArrayRef<MCPhysReg> AllSGPR64s = TRI->getAllSGPR64(MF);
    unsigned NumPreloaded = (MFI->getNumPreloadedSGPRs() + 1) / 2;
    AllSGPR64s = AllSGPR64s.slice(
        std::min(static_cast<unsigned>(AllSGPR64s.size()), NumPreloaded));
    Register GITPtrLoReg = MFI->getGITPtrLoReg(MF);
    for (MCPhysReg Reg : AllSGPR64s) {
      if (LiveRegs.available(MRI, Reg) && MRI.isAllocatable(Reg) &&
          !TRI->isSubRegisterEq(Reg, GITPtrLoReg)) {
        FlatScrInit = Reg;
        break;
      }
    }
    assert(FlatScrInit && "Failed to find free register for scratch init");

    FlatScrInitLo = TRI->getSubReg(FlatScrInit, AMDGPU::sub0);
    FlatScrInitHi = TRI->getSubReg(FlatScrInit, AMDGPU::sub1);

    buildGitPtr(MBB, I, DL, TII, FlatScrInit);

    // We now have the GIT ptr - now get the scratch descriptor from the entry
    // at offset 0 (or offset 16 for a compute shader).
    MachinePointerInfo PtrInfo(AMDGPUAS::CONSTANT_ADDRESS);
    const MCInstrDesc &LoadDwordX2 = TII->get(AMDGPU::S_LOAD_DWORDX2_IMM);
    auto *MMO = MF.getMachineMemOperand(
        PtrInfo,
        MachineMemOperand::MOLoad | MachineMemOperand::MOInvariant |
            MachineMemOperand::MODereferenceable,
        8, Align(4));
    unsigned Offset =
        MF.getFunction().getCallingConv() == CallingConv::AMDGPU_CS ? 16 : 0;
    const GCNSubtarget &Subtarget = MF.getSubtarget<GCNSubtarget>();
    unsigned EncodedOffset = AMDGPU::convertSMRDOffsetUnits(Subtarget, Offset);
    BuildMI(MBB, I, DL, LoadDwordX2, FlatScrInit)
        .addReg(FlatScrInit)
        .addImm(EncodedOffset) // offset
        .addImm(0)             // cpol
        .addMemOperand(MMO);

    // Mask the offset in [47:0] of the descriptor
    const MCInstrDesc &SAndB32 = TII->get(AMDGPU::S_AND_B32);
    auto And = BuildMI(MBB, I, DL, SAndB32, FlatScrInitHi)
        .addReg(FlatScrInitHi)
        .addImm(0xffff);
    And->getOperand(3).setIsDead(); // Mark SCC as dead.
  } else {
    Register FlatScratchInitReg =
        MFI->getPreloadedReg(AMDGPUFunctionArgInfo::FLAT_SCRATCH_INIT);
    assert(FlatScratchInitReg);

    MachineRegisterInfo &MRI = MF.getRegInfo();
    MRI.addLiveIn(FlatScratchInitReg);
    MBB.addLiveIn(FlatScratchInitReg);

    FlatScrInitLo = TRI->getSubReg(FlatScratchInitReg, AMDGPU::sub0);
    FlatScrInitHi = TRI->getSubReg(FlatScratchInitReg, AMDGPU::sub1);
  }

  // Do a 64-bit pointer add.
  if (ST.flatScratchIsPointer()) {
    if (ST.getGeneration() >= AMDGPUSubtarget::GFX10) {
      BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADD_U32), FlatScrInitLo)
        .addReg(FlatScrInitLo)
        .addReg(ScratchWaveOffsetReg);
      auto Addc = BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADDC_U32),
                          FlatScrInitHi)
        .addReg(FlatScrInitHi)
        .addImm(0);
      Addc->getOperand(3).setIsDead(); // Mark SCC as dead.

      BuildMI(MBB, I, DL, TII->get(AMDGPU::S_SETREG_B32)).
        addReg(FlatScrInitLo).
        addImm(int16_t(AMDGPU::Hwreg::ID_FLAT_SCR_LO |
                       (31 << AMDGPU::Hwreg::WIDTH_M1_SHIFT_)));
      BuildMI(MBB, I, DL, TII->get(AMDGPU::S_SETREG_B32)).
        addReg(FlatScrInitHi).
        addImm(int16_t(AMDGPU::Hwreg::ID_FLAT_SCR_HI |
                       (31 << AMDGPU::Hwreg::WIDTH_M1_SHIFT_)));
      return;
    }

    // For GFX9.
    BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADD_U32), AMDGPU::FLAT_SCR_LO)
      .addReg(FlatScrInitLo)
      .addReg(ScratchWaveOffsetReg);
    auto Addc = BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADDC_U32),
                        AMDGPU::FLAT_SCR_HI)
      .addReg(FlatScrInitHi)
      .addImm(0);
    Addc->getOperand(3).setIsDead(); // Mark SCC as dead.

    return;
  }

  assert(ST.getGeneration() < AMDGPUSubtarget::GFX9);

  // Copy the size in bytes.
  BuildMI(MBB, I, DL, TII->get(AMDGPU::COPY), AMDGPU::FLAT_SCR_LO)
    .addReg(FlatScrInitHi, RegState::Kill);

  // Add wave offset in bytes to private base offset.
  // See comment in AMDKernelCodeT.h for enable_sgpr_flat_scratch_init.
  BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADD_I32), FlatScrInitLo)
      .addReg(FlatScrInitLo)
      .addReg(ScratchWaveOffsetReg);

  // Convert offset to 256-byte units.
  auto LShr = BuildMI(MBB, I, DL, TII->get(AMDGPU::S_LSHR_B32),
                      AMDGPU::FLAT_SCR_HI)
    .addReg(FlatScrInitLo, RegState::Kill)
    .addImm(8);
  LShr->getOperand(3).setIsDead(true); // Mark SCC as dead.
}

// Note SGPRSpill stack IDs should only be used for SGPR spilling to VGPRs, not
// memory. They should have been removed by now, except CFI Saved Reg spills.
static bool allStackObjectsAreDead(const MachineFunction &MF) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();
  const SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();
  for (int I = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd();
       I != E; ++I) {
    if (!MFI.isDeadObjectIndex(I)) {
      // determineCalleeSaves() might have added the SGPRSpill stack IDs for
      // CFI saves into scratch VGPR, ignore them
      if (MFI.getStackID(I) == TargetStackID::SGPRSpill &&
          TRI->isCFISavedRegsSpillEnabled() &&
          (I == FuncInfo->ReturnAddressSaveIndex ||
           I == FuncInfo->EXECSaveIndex)) {
        continue;
      }
      return false;
    }
  }

  return true;
}

// Shift down registers reserved for the scratch RSRC.
Register SIFrameLowering::getEntryFunctionReservedScratchRsrcReg(
    MachineFunction &MF) const {

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo *TRI = &TII->getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();

  assert(MFI->isEntryFunction());

  Register ScratchRsrcReg = MFI->getScratchRSrcReg();

  if (!ScratchRsrcReg ||
      (!MRI.isPhysRegUsed(ScratchRsrcReg) && allStackObjectsAreDead(MF)))
    return Register();

  if (ST.hasSGPRInitBug() ||
      ScratchRsrcReg != TRI->reservedPrivateSegmentBufferReg(MF))
    return ScratchRsrcReg;

  // We reserved the last registers for this. Shift it down to the end of those
  // which were actually used.
  //
  // FIXME: It might be safer to use a pseudoregister before replacement.

  // FIXME: We should be able to eliminate unused input registers. We only
  // cannot do this for the resources required for scratch access. For now we
  // skip over user SGPRs and may leave unused holes.

  unsigned NumPreloaded = (MFI->getNumPreloadedSGPRs() + 3) / 4;
  ArrayRef<MCPhysReg> AllSGPR128s = TRI->getAllSGPR128(MF);
  AllSGPR128s = AllSGPR128s.slice(std::min(static_cast<unsigned>(AllSGPR128s.size()), NumPreloaded));

  // Skip the last N reserved elements because they should have already been
  // reserved for VCC etc.
  Register GITPtrLoReg = MFI->getGITPtrLoReg(MF);
  for (MCPhysReg Reg : AllSGPR128s) {
    // Pick the first unallocated one. Make sure we don't clobber the other
    // reserved input we needed. Also for PAL, make sure we don't clobber
    // the GIT pointer passed in SGPR0 or SGPR8.
    if (!MRI.isPhysRegUsed(Reg) && MRI.isAllocatable(Reg) &&
        !TRI->isSubRegisterEq(Reg, GITPtrLoReg)) {
      MRI.replaceRegWith(ScratchRsrcReg, Reg);
      MFI->setScratchRSrcReg(Reg);
      return Reg;
    }
  }

  return ScratchRsrcReg;
}

static unsigned getScratchScaleFactor(const GCNSubtarget &ST) {
  return ST.enableFlatScratch() ? 1 : ST.getWavefrontSize();
}

void SIFrameLowering::emitEntryFunctionPrologue(MachineFunction &MF,
                                                MachineBasicBlock &MBB) const {
  assert(&MF.front() == &MBB && "Shrink-wrapping not yet supported");

  // FIXME: If we only have SGPR spills, we won't actually be using scratch
  // memory since these spill to VGPRs. We should be cleaning up these unused
  // SGPR spill frame indices somewhere.

  // FIXME: We still have implicit uses on SGPR spill instructions in case they
  // need to spill to vector memory. It's likely that will not happen, but at
  // this point it appears we need the setup. This part of the prolog should be
  // emitted after frame indices are eliminated.

  // FIXME: Remove all of the isPhysRegUsed checks

  SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo *TRI = &TII->getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const Function &F = MF.getFunction();
  const MCRegisterInfo *MCRI = MF.getMMI().getContext().getRegisterInfo();
  MachineFrameInfo &FrameInfo = MF.getFrameInfo();

  assert(MFI->isEntryFunction());

  // Debug location must be unknown since the first debug location is used to
  // determine the end of the prologue.
  DebugLoc DL;
  MachineBasicBlock::iterator I = MBB.begin();

  // FIXME: Switch to using MF.needsFrameMoves() later
  const bool needsFrameMoves = true;

  if (needsFrameMoves) {
    // On entry the SP/FP are not set up, so we need to define the CFA in terms
    // of a literal location expression.
    static const char CFAEncodedInst[] = {
        dwarf::DW_CFA_def_cfa_expression,
        3, // length
        static_cast<char>(dwarf::DW_OP_lit0),
        static_cast<char>(
            dwarf::DW_OP_lit6), // DW_ASPACE_AMDGPU_private_wave FIXME:
                                // should be defined elsewhere
        static_cast<char>(dwarf::DW_OP_LLVM_form_aspace_address)};
    buildCFI(MBB, I, DL,
             MCCFIInstruction::createEscape(
                 nullptr, StringRef(CFAEncodedInst, sizeof(CFAEncodedInst))));
    // Unwinding halts when the return address (PC) is undefined.
    buildCFI(MBB, I, DL,
             MCCFIInstruction::createUndefined(
                 nullptr, MCRI->getDwarfRegNum(AMDGPU::PC_REG, false)));
  }

  Register PreloadedScratchWaveOffsetReg = MFI->getPreloadedReg(
      AMDGPUFunctionArgInfo::PRIVATE_SEGMENT_WAVE_BYTE_OFFSET);

  // We need to do the replacement of the private segment buffer register even
  // if there are no stack objects. There could be stores to undef or a
  // constant without an associated object.
  //
  // This will return `Register()` in cases where there are no actual
  // uses of the SRSRC.
  Register ScratchRsrcReg;
  if (!ST.enableFlatScratch())
    ScratchRsrcReg = getEntryFunctionReservedScratchRsrcReg(MF);

  // Make the selected register live throughout the function.
  if (ScratchRsrcReg) {
    for (MachineBasicBlock &OtherBB : MF) {
      if (&OtherBB != &MBB) {
        OtherBB.addLiveIn(ScratchRsrcReg);
      }
    }
  }

  // Now that we have fixed the reserved SRSRC we need to locate the
  // (potentially) preloaded SRSRC.
  Register PreloadedScratchRsrcReg;
  if (ST.isAmdHsaOrMesa(F)) {
    PreloadedScratchRsrcReg =
        MFI->getPreloadedReg(AMDGPUFunctionArgInfo::PRIVATE_SEGMENT_BUFFER);
    if (ScratchRsrcReg && PreloadedScratchRsrcReg) {
      // We added live-ins during argument lowering, but since they were not
      // used they were deleted. We're adding the uses now, so add them back.
      MRI.addLiveIn(PreloadedScratchRsrcReg);
      MBB.addLiveIn(PreloadedScratchRsrcReg);
    }
  }

  // We found the SRSRC first because it needs four registers and has an
  // alignment requirement. If the SRSRC that we found is clobbering with
  // the scratch wave offset, which may be in a fixed SGPR or a free SGPR
  // chosen by SITargetLowering::allocateSystemSGPRs, COPY the scratch
  // wave offset to a free SGPR.
  Register ScratchWaveOffsetReg;
  if (PreloadedScratchWaveOffsetReg &&
      TRI->isSubRegisterEq(ScratchRsrcReg, PreloadedScratchWaveOffsetReg)) {
    ArrayRef<MCPhysReg> AllSGPRs = TRI->getAllSGPR32(MF);
    unsigned NumPreloaded = MFI->getNumPreloadedSGPRs();
    AllSGPRs = AllSGPRs.slice(
        std::min(static_cast<unsigned>(AllSGPRs.size()), NumPreloaded));
    Register GITPtrLoReg = MFI->getGITPtrLoReg(MF);
    for (MCPhysReg Reg : AllSGPRs) {
      if (!MRI.isPhysRegUsed(Reg) && MRI.isAllocatable(Reg) &&
          !TRI->isSubRegisterEq(ScratchRsrcReg, Reg) && GITPtrLoReg != Reg) {
        ScratchWaveOffsetReg = Reg;
        BuildMI(MBB, I, DL, TII->get(AMDGPU::COPY), ScratchWaveOffsetReg)
            .addReg(PreloadedScratchWaveOffsetReg, RegState::Kill);
        break;
      }
    }
  } else {
    ScratchWaveOffsetReg = PreloadedScratchWaveOffsetReg;
  }
  assert(ScratchWaveOffsetReg || !PreloadedScratchWaveOffsetReg);

  if (requiresStackPointerReference(MF)) {
    Register SPReg = MFI->getStackPtrOffsetReg();
    assert(SPReg != AMDGPU::SP_REG);
    BuildMI(MBB, I, DL, TII->get(AMDGPU::S_MOV_B32), SPReg)
        .addImm(FrameInfo.getStackSize() * getScratchScaleFactor(ST));
  }

  if (hasFP(MF)) {
    Register FPReg = MFI->getFrameOffsetReg();
    assert(FPReg != AMDGPU::FP_REG);
    BuildMI(MBB, I, DL, TII->get(AMDGPU::S_MOV_B32), FPReg).addImm(0);
  }

  bool NeedsFlatScratchInit =
      MFI->hasFlatScratchInit() &&
      (MRI.isPhysRegUsed(AMDGPU::FLAT_SCR) || FrameInfo.hasCalls() ||
       (!allStackObjectsAreDead(MF) && ST.enableFlatScratch()));

  if ((NeedsFlatScratchInit || ScratchRsrcReg) &&
      PreloadedScratchWaveOffsetReg && !ST.flatScratchIsArchitected()) {
    MRI.addLiveIn(PreloadedScratchWaveOffsetReg);
    MBB.addLiveIn(PreloadedScratchWaveOffsetReg);
  }

  if (NeedsFlatScratchInit) {
    emitEntryFunctionFlatScratchInit(MF, MBB, I, DL, ScratchWaveOffsetReg);
  }

  if (ScratchRsrcReg) {
    emitEntryFunctionScratchRsrcRegSetup(MF, MBB, I, DL,
                                         PreloadedScratchRsrcReg,
                                         ScratchRsrcReg, ScratchWaveOffsetReg);
  }
}

// Emit scratch RSRC setup code, assuming `ScratchRsrcReg != AMDGPU::NoReg`
void SIFrameLowering::emitEntryFunctionScratchRsrcRegSetup(
    MachineFunction &MF, MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
    const DebugLoc &DL, Register PreloadedScratchRsrcReg,
    Register ScratchRsrcReg, Register ScratchWaveOffsetReg) const {

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo *TRI = &TII->getRegisterInfo();
  const SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  const Function &Fn = MF.getFunction();

  if (ST.isAmdPalOS()) {
    // The pointer to the GIT is formed from the offset passed in and either
    // the amdgpu-git-ptr-high function attribute or the top part of the PC
    Register Rsrc01 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub0_sub1);
    Register Rsrc03 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub3);

    buildGitPtr(MBB, I, DL, TII, Rsrc01);

    // We now have the GIT ptr - now get the scratch descriptor from the entry
    // at offset 0 (or offset 16 for a compute shader).
    MachinePointerInfo PtrInfo(AMDGPUAS::CONSTANT_ADDRESS);
    const MCInstrDesc &LoadDwordX4 = TII->get(AMDGPU::S_LOAD_DWORDX4_IMM);
    auto MMO = MF.getMachineMemOperand(PtrInfo,
                                       MachineMemOperand::MOLoad |
                                           MachineMemOperand::MOInvariant |
                                           MachineMemOperand::MODereferenceable,
                                       16, Align(4));
    unsigned Offset = Fn.getCallingConv() == CallingConv::AMDGPU_CS ? 16 : 0;
    const GCNSubtarget &Subtarget = MF.getSubtarget<GCNSubtarget>();
    unsigned EncodedOffset = AMDGPU::convertSMRDOffsetUnits(Subtarget, Offset);
    BuildMI(MBB, I, DL, LoadDwordX4, ScratchRsrcReg)
      .addReg(Rsrc01)
      .addImm(EncodedOffset) // offset
      .addImm(0) // cpol
      .addReg(ScratchRsrcReg, RegState::ImplicitDefine)
      .addMemOperand(MMO);

    // The driver will always set the SRD for wave 64 (bits 118:117 of
    // descriptor / bits 22:21 of third sub-reg will be 0b11)
    // If the shader is actually wave32 we have to modify the const_index_stride
    // field of the descriptor 3rd sub-reg (bits 22:21) to 0b10 (stride=32). The
    // reason the driver does this is that there can be cases where it presents
    // 2 shaders with different wave size (e.g. VsFs).
    // TODO: convert to using SCRATCH instructions or multiple SRD buffers
    if (ST.isWave32()) {
      const MCInstrDesc &SBitsetB32 = TII->get(AMDGPU::S_BITSET0_B32);
      BuildMI(MBB, I, DL, SBitsetB32, Rsrc03)
          .addImm(21)
          .addReg(Rsrc03);
    }
  } else if (ST.isMesaGfxShader(Fn) || !PreloadedScratchRsrcReg) {
    assert(!ST.isAmdHsaOrMesa(Fn));
    const MCInstrDesc &SMovB32 = TII->get(AMDGPU::S_MOV_B32);

    Register Rsrc2 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub2);
    Register Rsrc3 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub3);

    // Use relocations to get the pointer, and setup the other bits manually.
    uint64_t Rsrc23 = TII->getScratchRsrcWords23();

    if (MFI->hasImplicitBufferPtr()) {
      Register Rsrc01 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub0_sub1);

      if (AMDGPU::isCompute(MF.getFunction().getCallingConv())) {
        const MCInstrDesc &Mov64 = TII->get(AMDGPU::S_MOV_B64);

        BuildMI(MBB, I, DL, Mov64, Rsrc01)
          .addReg(MFI->getImplicitBufferPtrUserSGPR())
          .addReg(ScratchRsrcReg, RegState::ImplicitDefine);
      } else {
        const MCInstrDesc &LoadDwordX2 = TII->get(AMDGPU::S_LOAD_DWORDX2_IMM);

        MachinePointerInfo PtrInfo(AMDGPUAS::CONSTANT_ADDRESS);
        auto MMO = MF.getMachineMemOperand(
            PtrInfo,
            MachineMemOperand::MOLoad | MachineMemOperand::MOInvariant |
                MachineMemOperand::MODereferenceable,
            8, Align(4));
        BuildMI(MBB, I, DL, LoadDwordX2, Rsrc01)
          .addReg(MFI->getImplicitBufferPtrUserSGPR())
          .addImm(0) // offset
          .addImm(0) // cpol
          .addMemOperand(MMO)
          .addReg(ScratchRsrcReg, RegState::ImplicitDefine);

        MF.getRegInfo().addLiveIn(MFI->getImplicitBufferPtrUserSGPR());
        MBB.addLiveIn(MFI->getImplicitBufferPtrUserSGPR());
      }
    } else {
      Register Rsrc0 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub0);
      Register Rsrc1 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub1);

      BuildMI(MBB, I, DL, SMovB32, Rsrc0)
        .addExternalSymbol("SCRATCH_RSRC_DWORD0")
        .addReg(ScratchRsrcReg, RegState::ImplicitDefine);

      BuildMI(MBB, I, DL, SMovB32, Rsrc1)
        .addExternalSymbol("SCRATCH_RSRC_DWORD1")
        .addReg(ScratchRsrcReg, RegState::ImplicitDefine);

    }

    BuildMI(MBB, I, DL, SMovB32, Rsrc2)
      .addImm(Rsrc23 & 0xffffffff)
      .addReg(ScratchRsrcReg, RegState::ImplicitDefine);

    BuildMI(MBB, I, DL, SMovB32, Rsrc3)
      .addImm(Rsrc23 >> 32)
      .addReg(ScratchRsrcReg, RegState::ImplicitDefine);
  } else if (ST.isAmdHsaOrMesa(Fn)) {
    assert(PreloadedScratchRsrcReg);

    if (ScratchRsrcReg != PreloadedScratchRsrcReg) {
      BuildMI(MBB, I, DL, TII->get(AMDGPU::COPY), ScratchRsrcReg)
          .addReg(PreloadedScratchRsrcReg, RegState::Kill);
    }
  }

  // Add the scratch wave offset into the scratch RSRC.
  //
  // We only want to update the first 48 bits, which is the base address
  // pointer, without touching the adjacent 16 bits of flags. We know this add
  // cannot carry-out from bit 47, otherwise the scratch allocation would be
  // impossible to fit in the 48-bit global address space.
  //
  // TODO: Evaluate if it is better to just construct an SRD using the flat
  // scratch init and some constants rather than update the one we are passed.
  Register ScratchRsrcSub0 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub0);
  Register ScratchRsrcSub1 = TRI->getSubReg(ScratchRsrcReg, AMDGPU::sub1);

  // We cannot Kill ScratchWaveOffsetReg here because we allow it to be used in
  // the kernel body via inreg arguments.
  BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADD_U32), ScratchRsrcSub0)
      .addReg(ScratchRsrcSub0)
      .addReg(ScratchWaveOffsetReg)
      .addReg(ScratchRsrcReg, RegState::ImplicitDefine);
  auto Addc = BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADDC_U32), ScratchRsrcSub1)
      .addReg(ScratchRsrcSub1)
      .addImm(0)
      .addReg(ScratchRsrcReg, RegState::ImplicitDefine);
  Addc->getOperand(3).setIsDead(); // Mark SCC as dead.
}

bool SIFrameLowering::isSupportedStackID(TargetStackID::Value ID) const {
  switch (ID) {
  case TargetStackID::Default:
  case TargetStackID::NoAlloc:
  case TargetStackID::SGPRSpill:
    return true;
  case TargetStackID::ScalableVector:
  case TargetStackID::WasmLocal:
    return false;
  }
  llvm_unreachable("Invalid TargetStackID::Value");
}

static void initLiveRegs(LivePhysRegs &LiveRegs, const SIRegisterInfo &TRI,
                         const SIMachineFunctionInfo *FuncInfo,
                         MachineFunction &MF, MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI, bool IsProlog) {
  if (LiveRegs.empty()) {
    LiveRegs.init(TRI);
    if (IsProlog) {
      LiveRegs.addLiveIns(MBB);
    } else {
      // In epilog.
      LiveRegs.addLiveOuts(MBB);
      LiveRegs.stepBackward(*MBBI);
    }
  }
}

void SIFrameLowering::emitPrologueEntryCFI(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MBBI,
                                           const DebugLoc &DL) const {
  const MachineFunction &MF = *MBB.getParent();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const MCRegisterInfo *MCRI = MF.getMMI().getContext().getRegisterInfo();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo &TRI = ST.getInstrInfo()->getRegisterInfo();
  Register StackPtrReg =
      MF.getInfo<SIMachineFunctionInfo>()->getStackPtrOffsetReg();

  // DW_ASPACE_AMDGPU_private_wave FIXME: should be defined elsewhere
  buildCFI(MBB, MBBI, DL,
           MCCFIInstruction::createLLVMDefAspaceCfa(
               nullptr, MCRI->getDwarfRegNum(StackPtrReg, false), 0, 6));

  static const char PCEncodedInst[] = {
      dwarf::DW_CFA_expression,
      16, // PC 64
      8,  // length
      static_cast<char>(dwarf::DW_OP_regx),
      62, // SGPR30
      static_cast<char>(dwarf::DW_OP_piece),
      4, // 32 bits
      static_cast<char>(dwarf::DW_OP_regx),
      63, // SGPR31
      static_cast<char>(dwarf::DW_OP_piece),
      4 // 32 bits
  };
  buildCFI(MBB, MBBI, DL,
           MCCFIInstruction::createEscape(
               nullptr, StringRef(PCEncodedInst, sizeof(PCEncodedInst))));

  BitVector IsCalleeSaved(TRI.getNumRegs());
  const MCPhysReg *CSRegs = MRI.getCalleeSavedRegs();
  for (unsigned I = 0; CSRegs[I]; ++I) {
    IsCalleeSaved.set(CSRegs[I]);
  }
  auto ProcessReg = [&](MCPhysReg Reg) {
    if (IsCalleeSaved.test(Reg) || !MRI.isPhysRegModified(Reg))
      return;
    MCRegister DwarfReg = MCRI->getDwarfRegNum(Reg, false);
    buildCFI(MBB, MBBI, DL,
             MCCFIInstruction::createUndefined(nullptr, DwarfReg));
  };

  // Emit CFI rules for caller saved Arch VGPRs which are clobbered
  for_each(AMDGPU::VGPR_32RegClass.getRegisters(), ProcessReg);

  // Emit CFI rules for caller saved Accum VGPRs which are clobbered
  if (ST.hasMAIInsts()) {
    for_each(AMDGPU::AGPR_32RegClass.getRegisters(), ProcessReg);
  }

  // Emit CFI rules for caller saved SGPRs which are clobbered
  for_each(AMDGPU::SGPR_32RegClass.getRegisters(), ProcessReg);
}

// Activate all lanes, returns saved exec.
static Register buildScratchExecCopy(LivePhysRegs &LiveRegs,
                                     MachineFunction &MF,
                                     MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator MBBI,
                                     bool IsProlog) {
  Register ScratchExecCopy;
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo &TRI = TII->getRegisterInfo();
  SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();
  DebugLoc DL;

  initLiveRegs(LiveRegs, TRI, FuncInfo, MF, MBB, MBBI, IsProlog);

  ScratchExecCopy = findScratchNonCalleeSaveRegister(
      MRI, LiveRegs, *TRI.getWaveMaskRegClass());
  if (!ScratchExecCopy)
    report_fatal_error("failed to find free scratch register");

  LiveRegs.addReg(ScratchExecCopy);

  const unsigned OrSaveExec =
      ST.isWave32() ? AMDGPU::S_OR_SAVEEXEC_B32 : AMDGPU::S_OR_SAVEEXEC_B64;
  auto SaveExec = BuildMI(MBB, MBBI, DL, TII->get(OrSaveExec), ScratchExecCopy)
    .addImm(-1);
  SaveExec->getOperand(3).setIsDead(); // Mark SCC as dead.

  return ScratchExecCopy;
}

// A StackID of SGPRSpill implies that this is a spill from SGPR to VGPR.
// Otherwise we are spilling to memory.
static bool spilledToMemory(const MachineFunction &MF, int SaveIndex) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  return MFI.getStackID(SaveIndex) != TargetStackID::SGPRSpill;
}

void SIFrameLowering::emitCFISavedRegSpills(MachineFunction &MF,
                                            MachineBasicBlock &MBB,
                                            MachineBasicBlock::iterator MBBI,
                                            LivePhysRegs &LiveRegs,
                                            bool emitSpillsToMem) const {
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo &TRI = TII->getRegisterInfo();
  const MCRegisterInfo *MCRI = MF.getMMI().getContext().getRegisterInfo();

  SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  Optional<int> RASaveIndex = FuncInfo->ReturnAddressSaveIndex;
  Optional<int> EXECSaveIndex = FuncInfo->EXECSaveIndex;
  Register RetAddrReg = TRI.getReturnAddressReg(MF);
  DebugLoc DL;

  if (emitSpillsToMem) {
    // Return address is being spilled into memory at the frame
    // index <RASaveIndex> and consumes two double words. And
    // build the corresponding CFI rule.
    if (RASaveIndex && spilledToMemory(MF, *RASaveIndex)) {
      const int FI = *RASaveIndex;
      assert(!MFI.isDeadObjectIndex(FI));

      initLiveRegs(LiveRegs, TRI, FuncInfo, MF, MBB, MBBI, /*IsProlog*/ true);

      MCPhysReg TmpVGPR = findScratchNonCalleeSaveRegister(
          MRI, LiveRegs, AMDGPU::VGPR_32RegClass);

      BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_MOV_B32_e32), TmpVGPR)
          .addReg(TRI.getSubReg(RetAddrReg, AMDGPU::sub0));

      int DwordOff = 0;
      buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, TmpVGPR,
                       FI, DwordOff);

      BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_MOV_B32_e32), TmpVGPR)
          .addReg(TRI.getSubReg(RetAddrReg, AMDGPU::sub1));

      DwordOff = 4;
      buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, TmpVGPR,
                       FI, DwordOff);

      buildCFI(MBB, MBBI, DL,
               MCCFIInstruction::createOffset(
                   nullptr, MCRI->getDwarfRegNum(AMDGPU::PC_REG, false),
                   MFI.getObjectOffset(FI) * ST.getWavefrontSize()));
    }

    // EXEC mask is being spilled into memory at the frame
    // index <EXECSaveIndex> and consumes two double words in
    // wave64 mode and one doble word in wave32 mode. And
    // build the corresponding CFI rule.
    if (EXECSaveIndex && spilledToMemory(MF, *EXECSaveIndex)) {
      const int FI = *EXECSaveIndex;
      assert(!MFI.isDeadObjectIndex(FI));

      initLiveRegs(LiveRegs, TRI, FuncInfo, MF, MBB, MBBI, /*IsProlog*/ true);

      MCPhysReg TmpVGPR = findScratchNonCalleeSaveRegister(
          MRI, LiveRegs, AMDGPU::VGPR_32RegClass);

      BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_MOV_B32_e32), TmpVGPR)
          .addReg(TRI.getSubReg(AMDGPU::EXEC, AMDGPU::sub0));

      int DwordOff = 0;
      buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, TmpVGPR,
                       FI, DwordOff);

      if (!ST.isWave32()) {
        BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_MOV_B32_e32), TmpVGPR)
            .addReg(TRI.getSubReg(AMDGPU::EXEC, AMDGPU::sub1));

        DwordOff = 4;
        buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL,
                         TmpVGPR, FI, DwordOff);
      }

      buildCFI(MBB, MBBI, DL,
               MCCFIInstruction::createOffset(
                   nullptr, MCRI->getDwarfRegNum(AMDGPU::EXEC, false),
                   MFI.getObjectOffset(FI) * ST.getWavefrontSize()));
    }
  }

  if (!emitSpillsToMem) {
    // Return address is being spilled into free VGPR lanes
    // and consumes two lanes, build the corresponding CFI rule.
    if (RASaveIndex && !spilledToMemory(MF, *RASaveIndex)) {
      MCRegister RetAddrReg = TRI.getReturnAddressReg(MF);
      if (!MBB.isLiveIn(RetAddrReg))
        MBB.addLiveIn(RetAddrReg);

      ArrayRef<SIMachineFunctionInfo::SpilledReg> ReturnAddressSpill =
          FuncInfo->getSGPRToVGPRSpills(*RASaveIndex);
      assert(ReturnAddressSpill.size() == 2);
      BuildMI(MBB, MBBI, DL,
              TII->get(AMDGPU::V_WRITELANE_B32),
              ReturnAddressSpill[0].VGPR)
          .addReg(TRI.getSubReg(RetAddrReg, AMDGPU::sub0))
          .addImm(ReturnAddressSpill[0].Lane)
          .addReg(ReturnAddressSpill[0].VGPR, RegState::Undef);
      BuildMI(MBB, MBBI, DL,
              TII->get(AMDGPU::V_WRITELANE_B32),
              ReturnAddressSpill[1].VGPR)
          .addReg(TRI.getSubReg(RetAddrReg, AMDGPU::sub1))
          .addImm(ReturnAddressSpill[1].Lane)
          .addReg(ReturnAddressSpill[1].VGPR, RegState::Undef);
      buildCFIForSGPRToVGPRSpill(MBB, MBBI, DL, AMDGPU::PC_REG,
                                 ReturnAddressSpill);
    }

    // EXEC mask is being spilled into free VGPR lanes and consumes
    // two lanes in wave64 mode and one lane in wave32 mode, build
    // the corresponding CFI rule.
    if (EXECSaveIndex && !spilledToMemory(MF, *EXECSaveIndex)) {
      ArrayRef<SIMachineFunctionInfo::SpilledReg> EXECSpill =
          FuncInfo->getSGPRToVGPRSpills(*EXECSaveIndex);
      assert(EXECSpill.size());
      BuildMI(MBB, MBBI, DL,
              TII->get(AMDGPU::V_WRITELANE_B32),
              EXECSpill[0].VGPR)
          .addReg(AMDGPU::EXEC_LO)
          .addImm(EXECSpill[0].Lane)
          .addReg(EXECSpill[0].VGPR, RegState::Undef);
      if (!ST.isWave32()) {
        assert(EXECSpill.size() == 2);
        BuildMI(MBB, MBBI, DL,
                TII->get(AMDGPU::V_WRITELANE_B32),
                EXECSpill[1].VGPR)
            .addReg(AMDGPU::EXEC_HI)
            .addImm(EXECSpill[1].Lane)
            .addReg(EXECSpill[1].VGPR, RegState::Undef);
      }
      buildCFIForSGPRToVGPRSpill(MBB, MBBI, DL, AMDGPU::EXEC, EXECSpill);
    }
  }
}

void SIFrameLowering::emitPrologue(MachineFunction &MF,
                                   MachineBasicBlock &MBB) const {
  SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();
  if (FuncInfo->isEntryFunction()) {
    emitEntryFunctionPrologue(MF, MBB);
    return;
  }

  const MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo &TRI = TII->getRegisterInfo();
  const MCRegisterInfo *MCRI = MF.getMMI().getContext().getRegisterInfo();

  Register StackPtrReg = FuncInfo->getStackPtrOffsetReg();
  Register FramePtrReg = FuncInfo->getFrameOffsetReg();
  Register BasePtrReg =
      TRI.hasBasePointer(MF) ? TRI.getBaseRegister() : Register();
  LivePhysRegs LiveRegs;

  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL;

  bool HasFP = false;
  bool HasBP = false;
  uint32_t NumBytes = MFI.getStackSize();
  uint32_t RoundedSize = NumBytes;
  // To avoid clobbering VGPRs in lanes that weren't active on function entry,
  // turn on all lanes before doing the spill to memory.
  Register ScratchExecCopy;

  // FIXME: Switch to using MF.needsFrameMoves() later
  const bool needsFrameMoves = true;

  if (needsFrameMoves)
    emitPrologueEntryCFI(MBB, MBBI, DL);

  Optional<int> FPSaveIndex = FuncInfo->FramePointerSaveIndex;
  Optional<int> BPSaveIndex = FuncInfo->BasePointerSaveIndex;

  // VGPRs used for SGPR->VGPR spills
  for (const SIMachineFunctionInfo::SGPRSpillVGPR &Reg :
       FuncInfo->getSGPRSpillVGPRs()) {
    if (!Reg.FI)
      continue;

    if (!ScratchExecCopy)
      ScratchExecCopy = buildScratchExecCopy(LiveRegs, MF, MBB, MBBI,
                                             /*IsProlog*/ true);

    buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, Reg.VGPR,
                     *Reg.FI);

    if (needsFrameMoves)
      // We spill the entire VGPR, so we can get away with just cfi_offset
      buildCFI(MBB, MBBI, DL,
               MCCFIInstruction::createOffset(
                   nullptr, MCRI->getDwarfRegNum(Reg.VGPR, false),
                   MFI.getObjectOffset(*Reg.FI) * ST.getWavefrontSize()));
  }

  // VGPRs used for Whole Wave Mode
  for (const auto &Reg : FuncInfo->WWMReservedRegs) {
    auto VGPR = Reg.first;
    auto FI = Reg.second;
    if (!FI)
      continue;

    if (!ScratchExecCopy)
      ScratchExecCopy =
          buildScratchExecCopy(LiveRegs, MF, MBB, MBBI, /*IsProlog*/ true);

    buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, VGPR,
                     *FI);
  }

  if (ScratchExecCopy) {
    // FIXME: Split block and make terminator.
    unsigned ExecMov = ST.isWave32() ? AMDGPU::S_MOV_B32 : AMDGPU::S_MOV_B64;
    MCRegister Exec = ST.isWave32() ? AMDGPU::EXEC_LO : AMDGPU::EXEC;
    BuildMI(MBB, MBBI, DL, TII->get(ExecMov), Exec)
        .addReg(ScratchExecCopy, RegState::Kill);
    LiveRegs.addReg(ScratchExecCopy);
  }

  if (TRI.isCFISavedRegsSpillEnabled()) {
    bool emitSpillsToMem = true;
    emitCFISavedRegSpills(MF, MBB, MBBI, LiveRegs, emitSpillsToMem);
  }

  if (FPSaveIndex && spilledToMemory(MF, *FPSaveIndex)) {
    const int FramePtrFI = *FPSaveIndex;
    assert(!MFI.isDeadObjectIndex(FramePtrFI));

    initLiveRegs(LiveRegs, TRI, FuncInfo, MF, MBB, MBBI, /*IsProlog*/ true);

    MCPhysReg TmpVGPR = findScratchNonCalleeSaveRegister(
        MRI, LiveRegs, AMDGPU::VGPR_32RegClass);
    if (!TmpVGPR)
      report_fatal_error("failed to find free scratch register");

    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_MOV_B32_e32), TmpVGPR)
        .addReg(FramePtrReg);

    buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, TmpVGPR,
                     FramePtrFI);
    if (needsFrameMoves)
      buildCFI(MBB, MBBI, DL,
               MCCFIInstruction::createOffset(
                   nullptr, MCRI->getDwarfRegNum(FramePtrReg, false),
                   MFI.getObjectOffset(FramePtrFI) * ST.getWavefrontSize()));
  }

  if (BPSaveIndex && spilledToMemory(MF, *BPSaveIndex)) {
    const int BasePtrFI = *BPSaveIndex;
    assert(!MFI.isDeadObjectIndex(BasePtrFI));

    initLiveRegs(LiveRegs, TRI, FuncInfo, MF, MBB, MBBI, /*IsProlog*/ true);

    MCPhysReg TmpVGPR = findScratchNonCalleeSaveRegister(
        MRI, LiveRegs, AMDGPU::VGPR_32RegClass);
    if (!TmpVGPR)
      report_fatal_error("failed to find free scratch register");

    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_MOV_B32_e32), TmpVGPR)
        .addReg(BasePtrReg);

    buildPrologSpill(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, TmpVGPR,
                     BasePtrFI);
    if (needsFrameMoves)
      buildCFI(MBB, MBBI, DL,
               MCCFIInstruction::createOffset(
                   nullptr, MCRI->getDwarfRegNum(BasePtrReg, false),
                   MFI.getObjectOffset(BasePtrFI) * ST.getWavefrontSize()));
  }

  if (TRI.isCFISavedRegsSpillEnabled()) {
    bool emitSpillsToMem = false;
    emitCFISavedRegSpills(MF, MBB, MBBI, LiveRegs, emitSpillsToMem);
  }

  // In this case, spill the FP to a reserved VGPR.
  if (FPSaveIndex && !spilledToMemory(MF, *FPSaveIndex)) {
    const int FramePtrFI = *FPSaveIndex;
    assert(!MFI.isDeadObjectIndex(FramePtrFI));

    assert(MFI.getStackID(FramePtrFI) == TargetStackID::SGPRSpill);
    ArrayRef<SIMachineFunctionInfo::SpilledReg> Spill =
        FuncInfo->getSGPRToVGPRSpills(FramePtrFI);
    assert(Spill.size() == 1);

    // Save FP before setting it up.
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_WRITELANE_B32), Spill[0].VGPR)
        .addReg(FramePtrReg)
        .addImm(Spill[0].Lane)
        .addReg(Spill[0].VGPR, RegState::Undef);

    if (needsFrameMoves)
      buildCFIForSGPRToVGPRSpill(MBB, MBBI, DL, FramePtrReg, Spill[0].VGPR,
                                 Spill[0].Lane);
  }

  // In this case, spill the BP to a reserved VGPR.
  if (BPSaveIndex && !spilledToMemory(MF, *BPSaveIndex)) {
    const int BasePtrFI = *BPSaveIndex;
    assert(!MFI.isDeadObjectIndex(BasePtrFI));

    assert(MFI.getStackID(BasePtrFI) == TargetStackID::SGPRSpill);
    ArrayRef<SIMachineFunctionInfo::SpilledReg> Spill =
        FuncInfo->getSGPRToVGPRSpills(BasePtrFI);
    assert(Spill.size() == 1);

    // Save BP before setting it up.
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_WRITELANE_B32), Spill[0].VGPR)
        .addReg(BasePtrReg)
        .addImm(Spill[0].Lane)
        .addReg(Spill[0].VGPR, RegState::Undef);
    if (needsFrameMoves)
      buildCFIForSGPRToVGPRSpill(MBB, MBBI, DL, BasePtrReg, Spill[0].VGPR,
                                 Spill[0].Lane);
  }

  // Emit the copy if we need an FP, and are using a free SGPR to save it.
  if (FuncInfo->SGPRForFPSaveRestoreCopy) {
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::COPY),
            FuncInfo->SGPRForFPSaveRestoreCopy)
        .addReg(FramePtrReg)
        .setMIFlag(MachineInstr::FrameSetup);
    if (needsFrameMoves)
      buildCFI(
          MBB, MBBI, DL,
          MCCFIInstruction::createRegister(
              nullptr, MCRI->getDwarfRegNum(FramePtrReg, false),
              MCRI->getDwarfRegNum(FuncInfo->SGPRForFPSaveRestoreCopy, false)));
  }

  // Emit the copy if we need a BP, and are using a free SGPR to save it.
  if (FuncInfo->SGPRForBPSaveRestoreCopy) {
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::COPY),
            FuncInfo->SGPRForBPSaveRestoreCopy)
        .addReg(BasePtrReg)
        .setMIFlag(MachineInstr::FrameSetup);
    if (needsFrameMoves)
      buildCFI(
          MBB, MBBI, DL,
          MCCFIInstruction::createRegister(
              nullptr, MCRI->getDwarfRegNum(BasePtrReg, false),
              MCRI->getDwarfRegNum(FuncInfo->SGPRForBPSaveRestoreCopy, false)));
  }

  // If a copy has been emitted for FP and/or BP, Make the SGPRs
  // used in the copy instructions live throughout the function.
  SmallVector<MCPhysReg, 2> TempSGPRs;
  if (FuncInfo->SGPRForFPSaveRestoreCopy)
    TempSGPRs.push_back(FuncInfo->SGPRForFPSaveRestoreCopy);

  if (FuncInfo->SGPRForBPSaveRestoreCopy)
    TempSGPRs.push_back(FuncInfo->SGPRForBPSaveRestoreCopy);

  if (!TempSGPRs.empty()) {
    for (MachineBasicBlock &MBB : MF) {
      for (MCPhysReg Reg : TempSGPRs)
        MBB.addLiveIn(Reg);

      MBB.sortUniqueLiveIns();
    }
    if (!LiveRegs.empty()) {
      LiveRegs.addReg(FuncInfo->SGPRForFPSaveRestoreCopy);
      LiveRegs.addReg(FuncInfo->SGPRForBPSaveRestoreCopy);
    }
  }

  if (TRI.hasStackRealignment(MF)) {
    HasFP = true;
    const unsigned Alignment = MFI.getMaxAlign().value();

    RoundedSize += Alignment;
    if (LiveRegs.empty()) {
      LiveRegs.init(TRI);
      LiveRegs.addLiveIns(MBB);
    }

    // s_add_i32 s33, s32, NumBytes
    // s_and_b32 s33, s33, 0b111...0000
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::S_ADD_I32), FramePtrReg)
        .addReg(StackPtrReg)
        .addImm((Alignment - 1) * getScratchScaleFactor(ST))
        .setMIFlag(MachineInstr::FrameSetup);
    auto And = BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::S_AND_B32), FramePtrReg)
        .addReg(FramePtrReg, RegState::Kill)
        .addImm(-Alignment * getScratchScaleFactor(ST))
        .setMIFlag(MachineInstr::FrameSetup);
    And->getOperand(3).setIsDead(); // Mark SCC as dead.
    FuncInfo->setIsStackRealigned(true);
  } else if ((HasFP = hasFP(MF))) {
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::COPY), FramePtrReg)
        .addReg(StackPtrReg)
        .setMIFlag(MachineInstr::FrameSetup);
  }

  // If we need a base pointer, set it up here. It's whatever the value of
  // the stack pointer is at this point. Any variable size objects will be
  // allocated after this, so we can still use the base pointer to reference
  // the incoming arguments.
  if ((HasBP = TRI.hasBasePointer(MF))) {
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::COPY), BasePtrReg)
        .addReg(StackPtrReg)
        .setMIFlag(MachineInstr::FrameSetup);
  }

  if (HasFP) {
    if (needsFrameMoves)
      buildCFI(MBB, MBBI, DL,
               MCCFIInstruction::createDefCfaRegister(
                   nullptr, MCRI->getDwarfRegNum(FramePtrReg, false)));
  }

  if (HasFP && RoundedSize != 0) {
    auto Add = BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::S_ADD_I32), StackPtrReg)
        .addReg(StackPtrReg)
        .addImm(RoundedSize * getScratchScaleFactor(ST))
        .setMIFlag(MachineInstr::FrameSetup);
    Add->getOperand(3).setIsDead(); // Mark SCC as dead.
  }

  assert((!HasFP || (FuncInfo->SGPRForFPSaveRestoreCopy ||
                     FuncInfo->FramePointerSaveIndex)) &&
         "Needed to save FP but didn't save it anywhere");

  // If we allow spilling to AGPRs we may have saved FP but then spill
  // everything into AGPRs instead of the stack.
  assert((HasFP || (!FuncInfo->SGPRForFPSaveRestoreCopy &&
                    !FuncInfo->FramePointerSaveIndex) ||
                   EnableSpillVGPRToAGPR) &&
         "Saved FP but didn't need it");

  assert((!HasBP || (FuncInfo->SGPRForBPSaveRestoreCopy ||
                     FuncInfo->BasePointerSaveIndex)) &&
         "Needed to save BP but didn't save it anywhere");

  assert((HasBP || (!FuncInfo->SGPRForBPSaveRestoreCopy &&
                    !FuncInfo->BasePointerSaveIndex)) &&
         "Saved BP but didn't need it");
}

void SIFrameLowering::emitEpilogue(MachineFunction &MF,
                                   MachineBasicBlock &MBB) const {
  const SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();
  if (FuncInfo->isEntryFunction())
    return;

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const MCRegisterInfo *MCRI = MF.getMMI().getContext().getRegisterInfo();
  const SIRegisterInfo &TRI = TII->getRegisterInfo();
  MachineBasicBlock::iterator MBBI = MBB.getFirstTerminator();
  LivePhysRegs LiveRegs;
  DebugLoc DL;

  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint32_t NumBytes = MFI.getStackSize();
  uint32_t RoundedSize = FuncInfo->isStackRealigned()
                             ? NumBytes + MFI.getMaxAlign().value()
                             : NumBytes;
  const Register StackPtrReg = FuncInfo->getStackPtrOffsetReg();
  const Register FramePtrReg = FuncInfo->getFrameOffsetReg();
  const Register BasePtrReg =
      TRI.hasBasePointer(MF) ? TRI.getBaseRegister() : Register();

  Optional<int> FPSaveIndex = FuncInfo->FramePointerSaveIndex;
  Optional<int> BPSaveIndex = FuncInfo->BasePointerSaveIndex;

  if (RoundedSize != 0 && hasFP(MF)) {
    auto Add = BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::S_ADD_I32), StackPtrReg)
        .addReg(StackPtrReg)
        .addImm(-static_cast<int64_t>(RoundedSize * getScratchScaleFactor(ST)))
        .setMIFlag(MachineInstr::FrameDestroy);
    Add->getOperand(3).setIsDead(); // Mark SCC as dead.
  }

  if (FuncInfo->SGPRForFPSaveRestoreCopy) {
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::COPY), FramePtrReg)
        .addReg(FuncInfo->SGPRForFPSaveRestoreCopy)
        .setMIFlag(MachineInstr::FrameDestroy);
  }

  if (FuncInfo->SGPRForBPSaveRestoreCopy) {
    BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::COPY), BasePtrReg)
        .addReg(FuncInfo->SGPRForBPSaveRestoreCopy)
        .setMIFlag(MachineInstr::FrameDestroy);
  }

  if (FPSaveIndex) {
    const int FramePtrFI = *FPSaveIndex;
    assert(!MFI.isDeadObjectIndex(FramePtrFI));
    if (spilledToMemory(MF, FramePtrFI)) {
      initLiveRegs(LiveRegs, TRI, FuncInfo, MF, MBB, MBBI, /*IsProlog*/ false);

      MCPhysReg TmpVGPR = findScratchNonCalleeSaveRegister(
          MRI, LiveRegs, AMDGPU::VGPR_32RegClass);
      if (!TmpVGPR)
        report_fatal_error("failed to find free scratch register");
      buildEpilogRestore(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL,
                         TmpVGPR, FramePtrFI);
      BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_READFIRSTLANE_B32), FramePtrReg)
          .addReg(TmpVGPR, RegState::Kill);
    } else {
      // Reload from VGPR spill.
      assert(MFI.getStackID(FramePtrFI) == TargetStackID::SGPRSpill);
      ArrayRef<SIMachineFunctionInfo::SpilledReg> Spill =
          FuncInfo->getSGPRToVGPRSpills(FramePtrFI);
      assert(Spill.size() == 1);
      BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_READLANE_B32), FramePtrReg)
          .addReg(Spill[0].VGPR)
          .addImm(Spill[0].Lane);
    }
  }

  // FIXME: Switch to using MF.needsFrameMoves() later
  const bool needsFrameMoves = true;
  if (hasFP(MF)) {
    if (needsFrameMoves)
      buildCFI(MBB, MBBI, DL,
               MCCFIInstruction::createDefCfaRegister(
                   nullptr, MCRI->getDwarfRegNum(StackPtrReg, false)));
  }

  if (BPSaveIndex) {
    const int BasePtrFI = *BPSaveIndex;
    assert(!MFI.isDeadObjectIndex(BasePtrFI));
    if (spilledToMemory(MF, BasePtrFI)) {
      initLiveRegs(LiveRegs, TRI, FuncInfo, MF, MBB, MBBI, /*IsProlog*/ false);

      MCPhysReg TmpVGPR = findScratchNonCalleeSaveRegister(
          MRI, LiveRegs, AMDGPU::VGPR_32RegClass);
      if (!TmpVGPR)
        report_fatal_error("failed to find free scratch register");
      buildEpilogRestore(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL,
                         TmpVGPR, BasePtrFI);
      BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_READFIRSTLANE_B32), BasePtrReg)
          .addReg(TmpVGPR, RegState::Kill);
    } else {
      // Reload from VGPR spill.
      assert(MFI.getStackID(BasePtrFI) == TargetStackID::SGPRSpill);
      ArrayRef<SIMachineFunctionInfo::SpilledReg> Spill =
          FuncInfo->getSGPRToVGPRSpills(BasePtrFI);
      assert(Spill.size() == 1);
      BuildMI(MBB, MBBI, DL, TII->get(AMDGPU::V_READLANE_B32), BasePtrReg)
          .addReg(Spill[0].VGPR)
          .addImm(Spill[0].Lane);
    }
  }

  Register ScratchExecCopy;
  for (const SIMachineFunctionInfo::SGPRSpillVGPR &Reg :
       FuncInfo->getSGPRSpillVGPRs()) {
    if (!Reg.FI)
      continue;

    if (!ScratchExecCopy)
      ScratchExecCopy =
          buildScratchExecCopy(LiveRegs, MF, MBB, MBBI, /*IsProlog*/ false);

    buildEpilogRestore(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL,
                       Reg.VGPR, *Reg.FI);
  }

  for (const auto &Reg : FuncInfo->WWMReservedRegs) {
    auto VGPR = Reg.first;
    auto FI = Reg.second;
    if (!FI)
      continue;

    if (!ScratchExecCopy)
      ScratchExecCopy =
          buildScratchExecCopy(LiveRegs, MF, MBB, MBBI, /*IsProlog*/ false);

    buildEpilogRestore(ST, TRI, *FuncInfo, LiveRegs, MF, MBB, MBBI, DL, VGPR,
                       *FI);
  }

  if (ScratchExecCopy) {
    // FIXME: Split block and make terminator.
    unsigned ExecMov = ST.isWave32() ? AMDGPU::S_MOV_B32 : AMDGPU::S_MOV_B64;
    MCRegister Exec = ST.isWave32() ? AMDGPU::EXEC_LO : AMDGPU::EXEC;
    BuildMI(MBB, MBBI, DL, TII->get(ExecMov), Exec)
        .addReg(ScratchExecCopy, RegState::Kill);
  }
}

#ifndef NDEBUG
static bool allSGPRSpillsAreDead(const MachineFunction &MF) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();
  const SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();
  for (int I = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd();
       I != E; ++I) {
    if (!MFI.isDeadObjectIndex(I) &&
        MFI.getStackID(I) == TargetStackID::SGPRSpill &&
        (I != FuncInfo->FramePointerSaveIndex &&
         I != FuncInfo->BasePointerSaveIndex &&
         (!TRI->isCFISavedRegsSpillEnabled() ||
          (I != FuncInfo->ReturnAddressSaveIndex &&
           I != FuncInfo->EXECSaveIndex)))) {
      return false;
    }
  }

  return true;
}
#endif

StackOffset SIFrameLowering::getFrameIndexReference(const MachineFunction &MF,
                                                    int FI,
                                                    Register &FrameReg) const {
  const SIRegisterInfo *RI = MF.getSubtarget<GCNSubtarget>().getRegisterInfo();

  FrameReg = RI->getFrameRegister(MF);
  return StackOffset::getFixed(MF.getFrameInfo().getObjectOffset(FI));
}

void SIFrameLowering::processFunctionBeforeFrameFinalized(
  MachineFunction &MF,
  RegScavenger *RS) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();

  const bool SpillVGPRToAGPR = ST.hasMAIInsts() && FuncInfo->hasSpilledVGPRs()
                               && EnableSpillVGPRToAGPR;

  if (SpillVGPRToAGPR) {
    // To track the spill frame indices handled in this pass.
    BitVector SpillFIs(MFI.getObjectIndexEnd(), false);

    bool SeenDbgInstr = false;

    for (MachineBasicBlock &MBB : MF) {
      for (MachineInstr &MI : llvm::make_early_inc_range(MBB)) {
        if (MI.isDebugInstr())
          SeenDbgInstr = true;

        if (TII->isVGPRSpill(MI)) {
          // Try to eliminate stack used by VGPR spills before frame
          // finalization.
          unsigned FIOp = AMDGPU::getNamedOperandIdx(MI.getOpcode(),
                                                     AMDGPU::OpName::vaddr);
          int FI = MI.getOperand(FIOp).getIndex();
          Register VReg =
            TII->getNamedOperand(MI, AMDGPU::OpName::vdata)->getReg();
          if (FuncInfo->allocateVGPRSpillToAGPR(MF, FI,
                                                TRI->isAGPR(MRI, VReg))) {
            // FIXME: change to enterBasicBlockEnd()
            RS->enterBasicBlock(MBB);
            TRI->eliminateFrameIndex(MI, 0, FIOp, RS);
            SpillFIs.set(FI);
            continue;
          }
        }
      }
    }

    for (MachineBasicBlock &MBB : MF) {
      for (MCPhysReg Reg : FuncInfo->getVGPRSpillAGPRs())
        MBB.addLiveIn(Reg);

      for (MCPhysReg Reg : FuncInfo->getAGPRSpillVGPRs())
        MBB.addLiveIn(Reg);

      MBB.sortUniqueLiveIns();

      if (!SpillFIs.empty() && SeenDbgInstr) {
        // FIXME: The dead frame indices are replaced with a null register from
        // the debug value instructions. We should instead, update it with the
        // correct register value. But not sure the register value alone is
        for (MachineInstr &MI : MBB) {
          if (MI.isDebugValue() && MI.getOperand(0).isFI() &&
              SpillFIs[MI.getOperand(0).getIndex()]) {
            MI.getOperand(0).ChangeToRegister(Register(), false /*isDef*/);
          }
        }
      }
    }
  }

  FuncInfo->removeDeadFrameIndices(MF);
  assert(allSGPRSpillsAreDead(MF) &&
         "SGPR spill should have been removed in SILowerSGPRSpills");

  // FIXME: The other checks should be redundant with allStackObjectsAreDead,
  // but currently hasNonSpillStackObjects is set only from source
  // allocas. Stack temps produced from legalization are not counted currently.
  if (!allStackObjectsAreDead(MF)) {
    assert(RS && "RegScavenger required if spilling");

    // Add an emergency spill slot
    RS->addScavengingFrameIndex(FuncInfo->getScavengeFI(MFI, *TRI));
  }
}

static void allocateCFISave(MachineFunction &MF, Optional<int> &FI,
                            Register Reg) {
  SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();
  const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(Reg);
  if (MFI->haveFreeLanesForSGPRSpill(MF, TRI->getSpillSize(*RC) / 4)) {
    int NewFI = MF.getFrameInfo().CreateStackObject(
        TRI->getSpillSize(*RC), TRI->getSpillAlign(*RC), true, nullptr,
        TargetStackID::SGPRSpill);
    if (MFI->allocateSGPRSpillToVGPR(MF, NewFI)) {
      FI = NewFI;
    }
  } else {
    int NewFI = MF.getFrameInfo().CreateStackObject(
        TRI->getSpillSize(*RC), TRI->getSpillAlign(*RC), true, nullptr,
        TargetStackID::SGPRSpill);
    if (TRI->spillSGPRToVGPR() && MFI->allocateSGPRSpillToVGPR(MF, NewFI)) {
      FI = NewFI;
    } else {
      // Remove dead <NewFI> index
      MF.getFrameInfo().RemoveStackObject(NewFI);
      FI = MF.getFrameInfo().CreateSpillStackObject(
          TRI->getSpillSize(*RC), Align(TRI->getSpillAlign(*RC)));
    }
  }
  return;
}

// Only report VGPRs to generic code.
void SIFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                           BitVector &SavedVGPRs,
                                           RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedVGPRs, RS);
  SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  if (MFI->isEntryFunction())
    return;

  MachineFrameInfo &FrameInfo = MF.getFrameInfo();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();

  // Ignore the SGPRs the default implementation found.
  SavedVGPRs.clearBitsNotInMask(TRI->getAllVectorRegMask());

  // Do not save AGPRs prior to GFX90A because there was no easy way to do so.
  // In gfx908 there was do AGPR loads and stores and thus spilling also
  // require a temporary VGPR.
  if (!ST.hasGFX90AInsts())
    SavedVGPRs.clearBitsInMask(TRI->getAllAGPRRegMask());

  if (TRI->isCFISavedRegsSpillEnabled()) {
    allocateCFISave(MF, MFI->ReturnAddressSaveIndex,
                    TRI->getReturnAddressReg(MF));
    allocateCFISave(MF, MFI->EXECSaveIndex,
                    ST.isWave32() ? AMDGPU::EXEC_LO : AMDGPU::EXEC);
  }

  // hasFP only knows about stack objects that already exist. We're now
  // determining the stack slots that will be created, so we have to predict
  // them. Stack objects force FP usage with calls.
  //
  // Note a new VGPR CSR may be introduced if one is used for the spill, but we
  // don't want to report it here.
  //
  // FIXME: Is this really hasReservedCallFrame?
  const bool WillHaveFP =
      FrameInfo.hasCalls() && (SavedVGPRs.any() || !allStackObjectsAreDead(MF));

  // VGPRs used for SGPR spilling need to be specially inserted in the prolog,
  // so don't allow the default insertion to handle them.
  for (auto SSpill : MFI->getSGPRSpillVGPRs())
    SavedVGPRs.reset(SSpill.VGPR);

  LivePhysRegs LiveRegs;
  LiveRegs.init(*TRI);

  if (WillHaveFP || hasFP(MF)) {
    assert(!MFI->SGPRForFPSaveRestoreCopy && !MFI->FramePointerSaveIndex &&
           "Re-reserving spill slot for FP");
    getVGPRSpillLaneOrTempRegister(MF, LiveRegs, MFI->SGPRForFPSaveRestoreCopy,
                                   MFI->FramePointerSaveIndex, true);
  }

  if (TRI->hasBasePointer(MF)) {
    if (MFI->SGPRForFPSaveRestoreCopy)
      LiveRegs.addReg(MFI->SGPRForFPSaveRestoreCopy);

    assert(!MFI->SGPRForBPSaveRestoreCopy &&
           !MFI->BasePointerSaveIndex && "Re-reserving spill slot for BP");
    getVGPRSpillLaneOrTempRegister(MF, LiveRegs, MFI->SGPRForBPSaveRestoreCopy,
                                   MFI->BasePointerSaveIndex, false);
  }
}

void SIFrameLowering::determineCalleeSavesSGPR(MachineFunction &MF,
                                               BitVector &SavedRegs,
                                               RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
  const SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  if (MFI->isEntryFunction())
    return;

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();

  // The SP is specifically managed and we don't want extra spills of it.
  SavedRegs.reset(MFI->getStackPtrOffsetReg());

  const BitVector AllSavedRegs = SavedRegs;
  SavedRegs.clearBitsInMask(TRI->getAllVectorRegMask());

  // If clearing VGPRs changed the mask, we will have some CSR VGPR spills.
  const bool HaveAnyCSRVGPR = SavedRegs != AllSavedRegs;

  // We have to anticipate introducing CSR VGPR spills or spill of caller
  // save VGPR reserved for SGPR spills as we now always create stack entry
  // for it, if we don't have any stack objects already, since we require
  // an FP if there is a call and stack.
  MachineFrameInfo &FrameInfo = MF.getFrameInfo();
  const bool WillHaveFP =
      FrameInfo.hasCalls() && (HaveAnyCSRVGPR || MFI->VGPRReservedForSGPRSpill);

  // FP will be specially managed like SP.
  if (WillHaveFP || hasFP(MF))
    SavedRegs.reset(MFI->getFrameOffsetReg());
}

bool SIFrameLowering::assignCalleeSavedSpillSlots(
    MachineFunction &MF, const TargetRegisterInfo *TRI,
    std::vector<CalleeSavedInfo> &CSI) const {
  if (CSI.empty())
    return true; // Early exit if no callee saved registers are modified!

  const SIMachineFunctionInfo *FuncInfo = MF.getInfo<SIMachineFunctionInfo>();
  if (!FuncInfo->SGPRForFPSaveRestoreCopy &&
      !FuncInfo->SGPRForBPSaveRestoreCopy)
    return false;

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIRegisterInfo *RI = ST.getRegisterInfo();
  Register FramePtrReg = FuncInfo->getFrameOffsetReg();
  Register BasePtrReg = RI->getBaseRegister();
  unsigned NumModifiedRegs = 0;

  if (FuncInfo->SGPRForFPSaveRestoreCopy)
    NumModifiedRegs++;
  if (FuncInfo->SGPRForBPSaveRestoreCopy)
    NumModifiedRegs++;

  for (auto &CS : CSI) {
    if (CS.getReg() == FramePtrReg && FuncInfo->SGPRForFPSaveRestoreCopy) {
      CS.setDstReg(FuncInfo->SGPRForFPSaveRestoreCopy);
      if (--NumModifiedRegs)
        break;
    } else if (CS.getReg() == BasePtrReg &&
               FuncInfo->SGPRForBPSaveRestoreCopy) {
      CS.setDstReg(FuncInfo->SGPRForBPSaveRestoreCopy);
      if (--NumModifiedRegs)
        break;
    }
  }

  return false;
}

bool SIFrameLowering::allocateScavengingFrameIndexesNearIncomingSP(
  const MachineFunction &MF) const {

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t EstStackSize = MFI.estimateStackSize(MF);
  uint64_t MaxOffset = EstStackSize - 1;

  // We need the emergency stack slots to be allocated in range of the
  // MUBUF/flat scratch immediate offset from the base register, so assign these
  // first at the incoming SP position.
  //
  // TODO: We could try sorting the objects to find a hole in the first bytes
  // rather than allocating as close to possible. This could save a lot of space
  // on frames with alignment requirements.
  if (ST.enableFlatScratch()) {
    const SIInstrInfo *TII = ST.getInstrInfo();
    if (TII->isLegalFLATOffset(MaxOffset, AMDGPUAS::PRIVATE_ADDRESS,
                               SIInstrFlags::FlatScratch))
      return false;
  } else {
    if (SIInstrInfo::isLegalMUBUFImmOffset(MaxOffset))
      return false;
  }

  return true;
}

MachineBasicBlock::iterator SIFrameLowering::eliminateCallFramePseudoInstr(
  MachineFunction &MF,
  MachineBasicBlock &MBB,
  MachineBasicBlock::iterator I) const {
  int64_t Amount = I->getOperand(0).getImm();
  if (Amount == 0)
    return MBB.erase(I);

  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const DebugLoc &DL = I->getDebugLoc();
  unsigned Opc = I->getOpcode();
  bool IsDestroy = Opc == TII->getCallFrameDestroyOpcode();
  uint64_t CalleePopAmount = IsDestroy ? I->getOperand(1).getImm() : 0;

  if (!hasReservedCallFrame(MF)) {
    Amount = alignTo(Amount, getStackAlign());
    assert(isUInt<32>(Amount) && "exceeded stack address space size");
    const SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
    Register SPReg = MFI->getStackPtrOffsetReg();

    Amount *= getScratchScaleFactor(ST);
    if (IsDestroy)
      Amount = -Amount;
    auto Add = BuildMI(MBB, I, DL, TII->get(AMDGPU::S_ADD_I32), SPReg)
        .addReg(SPReg)
        .addImm(Amount);
    Add->getOperand(3).setIsDead(); // Mark SCC as dead.
  } else if (CalleePopAmount != 0) {
    llvm_unreachable("is this used?");
  }

  return MBB.erase(I);
}

/// Returns true if the frame will require a reference to the stack pointer.
///
/// This is the set of conditions common to setting up the stack pointer in a
/// kernel, and for using a frame pointer in a callable function.
///
/// FIXME: Should also check hasOpaqueSPAdjustment and if any inline asm
/// references SP.
static bool frameTriviallyRequiresSP(const MachineFrameInfo &MFI) {
  return MFI.hasVarSizedObjects() || MFI.hasStackMap() || MFI.hasPatchPoint();
}

// The FP for kernels is always known 0, so we never really need to setup an
// explicit register for it. However, DisableFramePointerElim will force us to
// use a register for it.
bool SIFrameLowering::hasFP(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();

  // For entry functions we can use an immediate offset in most cases, so the
  // presence of calls doesn't imply we need a distinct frame pointer.
  if (MFI.hasCalls() &&
      !MF.getInfo<SIMachineFunctionInfo>()->isEntryFunction()) {
    // All offsets are unsigned, so need to be addressed in the same direction
    // as stack growth.

    // FIXME: This function is pretty broken, since it can be called before the
    // frame layout is determined or CSR spills are inserted.
    return MFI.getStackSize() != 0;
  }

  return frameTriviallyRequiresSP(MFI) || MFI.isFrameAddressTaken() ||
         MF.getSubtarget<GCNSubtarget>().getRegisterInfo()->hasStackRealignment(
             MF) ||
         MF.getTarget().Options.DisableFramePointerElim(MF);
}

// This is essentially a reduced version of hasFP for entry functions. Since the
// stack pointer is known 0 on entry to kernels, we never really need an FP
// register. We may need to initialize the stack pointer depending on the frame
// properties, which logically overlaps many of the cases where an ordinary
// function would require an FP.
bool SIFrameLowering::requiresStackPointerReference(
    const MachineFunction &MF) const {
  // Callable functions always require a stack pointer reference.
  assert(MF.getInfo<SIMachineFunctionInfo>()->isEntryFunction() &&
         "only expected to call this for entry points");

  const MachineFrameInfo &MFI = MF.getFrameInfo();

  // Entry points ordinarily don't need to initialize SP. We have to set it up
  // for callees if there are any. Also note tail calls are impossible/don't
  // make any sense for kernels.
  if (MFI.hasCalls())
    return true;

  // We still need to initialize the SP if we're doing anything weird that
  // references the SP, like variable sized stack objects.
  return frameTriviallyRequiresSP(MFI);
}

bool SIFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  MachineFunction &MF = *MBB.getParent();
  const SIInstrInfo *TII = MF.getSubtarget<GCNSubtarget>().getInstrInfo();

  for (const CalleeSavedInfo &CS : CSI) {
    // Insert the spill to the stack frame.
    unsigned Reg = CS.getReg();

    if (CS.isSpilledToReg()) {
      BuildMI(MBB, MBBI, DebugLoc(), TII->get(TargetOpcode::COPY),
              CS.getDstReg())
          .addReg(Reg, getKillRegState(true));
    } else {
      const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(Reg);
      const MachineRegisterInfo &MRI = MF.getRegInfo();
      // If this value was already livein, we probably have a direct use of the
      // incoming register value, so don't kill at the spill point. This happens
      // since we pass some special inputs (workgroup IDs) in the callee saved
      // range.
      const bool IsLiveIn = MRI.isLiveIn(Reg);
      TII->storeRegToStackSlotCFI(MBB, MBBI, Reg, !IsLiveIn, CS.getFrameIdx(),
                                  RC, TRI);
    }
  }

  return true;
}

MachineInstr *SIFrameLowering::buildCFI(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator MBBI,
                               const DebugLoc &DL,
                               const MCCFIInstruction &CFIInst) const {
  MachineFunction &MF = *MBB.getParent();
  const SIInstrInfo *TII = MF.getSubtarget<GCNSubtarget>().getInstrInfo();
  return BuildMI(MBB, MBBI, DL, TII->get(TargetOpcode::CFI_INSTRUCTION))
      .addCFIIndex(MF.addFrameInst(CFIInst))
      .setMIFlag(MachineInstr::FrameSetup);
}

MachineInstr *SIFrameLowering::buildCFIForRegToRegSpill(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const DebugLoc &DL, const Register Reg, const Register RegCopy) const {
  MachineFunction &MF = *MBB.getParent();
  const MCRegisterInfo &MCRI = *MF.getMMI().getContext().getRegisterInfo();
  return buildCFI(
      MBB, MBBI, DL,
      MCCFIInstruction::createRegister(nullptr, MCRI.getDwarfRegNum(Reg, false),
                                       MCRI.getDwarfRegNum(RegCopy, false)));
}

static void encodeDwarfRegisterLocation(int DwarfReg, raw_ostream &OS) {
  if (DwarfReg < 32) {
    OS << uint8_t(dwarf::DW_OP_reg0 + DwarfReg);
  } else {
    OS << uint8_t(dwarf::DW_OP_regx);
    encodeULEB128(DwarfReg, OS);
  }
}

MachineInstr *SIFrameLowering::buildCFIForSGPRToVGPRSpill(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const DebugLoc &DL, const Register SGPR, const Register VGPR,
    const int Lane) const {
  MachineFunction &MF = *MBB.getParent();
  const MCRegisterInfo &MCRI = *MF.getMMI().getContext().getRegisterInfo();
  int DwarfSGPR = MCRI.getDwarfRegNum(SGPR, false);
  int DwarfVGPR = MCRI.getDwarfRegNum(VGPR, false);

  // CFI for an SGPR spilled to a single lane of a VGPR is implemented as an
  // expression(E) rule where E is a register location description referencing
  // a VGPR register location storage at a byte offset of the lane index
  // multiplied by the size of an SGPR (4 bytes). In other words we generate
  // the following DWARF:
  //
  // DW_CFA_expression: <SGPR>,
  //    (DW_OP_regx <VGPR>) (DW_OP_LLVM_offset_uconst <Lane>*4)
  //
  // The memory location description for the current CFA is pushed on the
  // stack before E is evaluated, but we choose not to drop it as it would
  // require a longer expression E and DWARF defines the result of the
  // evaulation to be the location description on the top of the stack (i.e. the
  // implictly pushed one is just ignored.)
  SmallString<20> CFIInst;
  raw_svector_ostream OSCFIInst(CFIInst);
  SmallString<20> Block;
  raw_svector_ostream OSBlock(Block);

  OSCFIInst << uint8_t(dwarf::DW_CFA_expression);
  encodeULEB128(DwarfSGPR, OSCFIInst);

  encodeDwarfRegisterLocation(DwarfVGPR, OSBlock);
  OSBlock << uint8_t(dwarf::DW_OP_LLVM_offset_uconst);
  // FIXME:
  const unsigned SGPRByteSize = 4;
  encodeULEB128(Lane * SGPRByteSize, OSBlock);

  encodeULEB128(Block.size(), OSCFIInst);
  OSCFIInst << Block;

  return buildCFI(MBB, MBBI, DL,
           MCCFIInstruction::createEscape(nullptr, OSCFIInst.str()));
}

void SIFrameLowering::buildCFIForSGPRToVGPRSpill(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const DebugLoc &DL, Register SGPR,
    ArrayRef<SIMachineFunctionInfo::SpilledReg> VGPRSpills) const {
  MachineFunction &MF = *MBB.getParent();
  const MCRegisterInfo &MCRI = *MF.getMMI().getContext().getRegisterInfo();
  int DwarfSGPR = MCRI.getDwarfRegNum(SGPR, false);

  // CFI for an SGPR spilled to a multiple lanes of VGPRs is implemented as an
  // expression(E) rule where E is a composite location description
  // with multiple parts each referencing
  // VGPR register location storage with a bit offset of the lane index
  // multiplied by the size of an SGPR (32 bits). In other words we generate
  // the following DWARF:
  //
  // DW_CFA_expression: <SGPR>,
  //    (DW_OP_regx <VGPR[0]>) (DW_OP_bit_piece 32, <Lane[0]>*32)
  //    (DW_OP_regx <VGPR[1]>) (DW_OP_bit_piece 32, <Lane[1]>*32)
  //    ...
  //    (DW_OP_regx <VGPR[N]>) (DW_OP_bit_piece 32, <Lane[N]>*32)
  //
  // The memory location description for the current CFA is pushed on the
  // stack before E is evaluated, but we choose not to drop it as it would
  // require a longer expression E and DWARF defines the result of the
  // evaulation to be the location description on the top of the stack (i.e. the
  // implictly pushed one is just ignored.)
  SmallString<20> CFIInst;
  raw_svector_ostream OSCFIInst(CFIInst);
  SmallString<20> Block;
  raw_svector_ostream OSBlock(Block);

  OSCFIInst << uint8_t(dwarf::DW_CFA_expression);
  encodeULEB128(DwarfSGPR, OSCFIInst);

  // TODO: Detect when we can merge multiple adjacent pieces, or even reduce
  // this to a register location description (when all pieces are adjacent).
  for (SIMachineFunctionInfo::SpilledReg Spill : VGPRSpills) {
    encodeDwarfRegisterLocation(MCRI.getDwarfRegNum(Spill.VGPR, false),
                                OSBlock);
    OSBlock << uint8_t(dwarf::DW_OP_bit_piece);
    // FIXME:Can this be a function of the SGPR?
    const unsigned SGPRBitSize = 32;
    encodeULEB128(SGPRBitSize, OSBlock);
    encodeULEB128(SGPRBitSize * Spill.Lane, OSBlock);
  }

  encodeULEB128(Block.size(), OSCFIInst);
  OSCFIInst << Block;

  buildCFI(MBB, MBBI, DL,
           MCCFIInstruction::createEscape(nullptr, OSCFIInst.str()));
}

void SIFrameLowering::buildCFIForVGPRToVMEMSpill(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    const DebugLoc &DL, unsigned VGPR, int64_t Offset) const {
  MachineFunction &MF = *MBB.getParent();
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  const MCRegisterInfo &MCRI = *MF.getMMI().getContext().getRegisterInfo();
  int DwarfVGPR = MCRI.getDwarfRegNum(VGPR, false);

  SmallString<20> CFIInst;
  raw_svector_ostream OSCFIInst(CFIInst);
  SmallString<20> Block;
  raw_svector_ostream OSBlock(Block);

  OSCFIInst << uint8_t(dwarf::DW_CFA_expression);
  encodeULEB128(DwarfVGPR, OSCFIInst);

  encodeDwarfRegisterLocation(DwarfVGPR, OSBlock);
  OSBlock << uint8_t(dwarf::DW_OP_swap);
  OSBlock << uint8_t(dwarf::DW_OP_LLVM_offset_uconst);
  encodeULEB128(Offset, OSBlock);
  OSBlock << uint8_t(dwarf::DW_OP_LLVM_call_frame_entry_reg);
  encodeULEB128(MCRI.getDwarfRegNum(
                    ST.isWave32() ? AMDGPU::EXEC_LO : AMDGPU::EXEC, false),
                OSBlock);
  OSBlock << uint8_t(dwarf::DW_OP_deref_size);
  OSBlock << uint8_t(ST.getWavefrontSize() / CHAR_BIT);
  OSBlock << uint8_t(dwarf::DW_OP_LLVM_select_bit_piece);
  // FIXME: Can this be a function of the VGPR?
  const unsigned VGPRLaneBitSize = 32;
  encodeULEB128(VGPRLaneBitSize, OSBlock);
  encodeULEB128(ST.getWavefrontSize(), OSBlock);

  encodeULEB128(Block.size(), OSCFIInst);
  OSCFIInst << Block;

  buildCFI(MBB, MBBI, DL,
           MCCFIInstruction::createEscape(nullptr, OSCFIInst.str()));
}
