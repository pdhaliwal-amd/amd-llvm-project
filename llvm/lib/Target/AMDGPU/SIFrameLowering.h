//===--------------------- SIFrameLowering.h --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_SIFRAMELOWERING_H
#define LLVM_LIB_TARGET_AMDGPU_SIFRAMELOWERING_H

#include "AMDGPUFrameLowering.h"
#include "SIMachineFunctionInfo.h"

namespace llvm {

class SIInstrInfo;
class SIRegisterInfo;
class GCNSubtarget;

class SIFrameLowering final : public AMDGPUFrameLowering {
public:
  SIFrameLowering(StackDirection D, Align StackAl, int LAO,
                  Align TransAl = Align(1))
      : AMDGPUFrameLowering(D, StackAl, LAO, TransAl) {}
  ~SIFrameLowering() override = default;

  void emitEntryFunctionPrologue(MachineFunction &MF,
                                 MachineBasicBlock &MBB) const;
  void emitPrologue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;
  StackOffset getFrameIndexReference(const MachineFunction &MF, int FI,
                                     Register &FrameReg) const override;

  void determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                            RegScavenger *RS = nullptr) const override;
  void determineCalleeSavesSGPR(MachineFunction &MF, BitVector &SavedRegs,
                                RegScavenger *RS = nullptr) const;
  bool
  assignCalleeSavedSpillSlots(MachineFunction &MF,
                              const TargetRegisterInfo *TRI,
                              std::vector<CalleeSavedInfo> &CSI) const override;

  bool allocateScavengingFrameIndexesNearIncomingSP(
    const MachineFunction &MF) const override;

  bool isSupportedStackID(TargetStackID::Value ID) const override;

  void processFunctionBeforeFrameFinalized(
    MachineFunction &MF,
    RegScavenger *RS = nullptr) const override;

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF,
                                MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MI) const override;

  bool spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MBBI,
                                 const ArrayRef<CalleeSavedInfo> CSI,
                                 const TargetRegisterInfo *TRI) const override;

private:
  void emitEntryFunctionFlatScratchInit(MachineFunction &MF,
                                        MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator I,
                                        const DebugLoc &DL,
                                        Register ScratchWaveOffsetReg) const;

  Register getEntryFunctionReservedScratchRsrcReg(MachineFunction &MF) const;

  void emitEntryFunctionScratchRsrcRegSetup(
      MachineFunction &MF, MachineBasicBlock &MBB,
      MachineBasicBlock::iterator I, const DebugLoc &DL,
      Register PreloadedPrivateBufferReg, Register ScratchRsrcReg,
      Register ScratchWaveOffsetReg) const;

  void emitPrologueEntryCFI(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MBBI,
                            const DebugLoc &DL) const;

public:
  bool hasFP(const MachineFunction &MF) const override;

  bool requiresStackPointerReference(const MachineFunction &MF) const;

  /// If '-amdgpu-spill-cfi-saved-regs' is enabled, emit RA/EXEC spills to
  /// a free VGPR (lanes) or memory and corresponding CFI rules.
  void emitCFISavedRegSpills(MachineFunction &MF, MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator MBBI,
                             LivePhysRegs &LiveRegs,
                             bool emitSpillsToMem) const;

  /// Create a CFI index for CFIInst and build a MachineInstr around it.
  MachineInstr *buildCFI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                const DebugLoc &DL, const MCCFIInstruction &CFIInst) const;
  /// Create a CFI index describing a spill of the register \p Reg to another
  /// register \p RegCopy and build a MachineInstr around it.
  MachineInstr *buildCFIForRegToRegSpill(MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator MBBI,
                                         const DebugLoc &DL, const Register Reg,
                                         const Register RegCopy) const;
  /// Create a CFI index describing a spill of an SGPR to a single lane of
  /// a VGPR and build a MachineInstr around it.
  MachineInstr *buildCFIForSGPRToVGPRSpill(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MBBI,
                                           const DebugLoc &DL, const Register SGPR,
                                           const Register VGPR, const int Lane) const;
  /// Create a CFI index describing a spill of an SGPR to multiple lanes of
  /// VGPRs and build a MachineInstr around it.
  void buildCFIForSGPRToVGPRSpill(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
      const DebugLoc &DL, Register SGPR,
      ArrayRef<SIMachineFunctionInfo::SpilledReg> VGPRSpills) const;
  /// Create a CFI index describing a spill of a VGPR to VMEM and
  /// build a MachineInstr around it.
  void buildCFIForVGPRToVMEMSpill(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator MBBI,
                                  const DebugLoc &DL, unsigned VGPR,
                                  int64_t Offset) const;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_AMDGPU_SIFRAMELOWERING_H
