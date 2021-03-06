//===-- SIRegisterInfo.cpp - SI Register Information ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief SI implementation of the TargetRegisterInfo class.
//
//===----------------------------------------------------------------------===//


#include "SIRegisterInfo.h"
#include "AMDGPUSubtarget.h"
#include "SIInstrInfo.h"
#include "SIMachineFunctionInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"

using namespace llvm;

SIRegisterInfo::SIRegisterInfo(const AMDGPUSubtarget &st)
: AMDGPURegisterInfo(st)
  { }

BitVector SIRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(AMDGPU::EXEC);
  Reserved.set(AMDGPU::INDIRECT_BASE_ADDR);
  Reserved.set(AMDGPU::FLAT_SCR);
  return Reserved;
}

unsigned SIRegisterInfo::getRegPressureLimit(const TargetRegisterClass *RC,
                                             MachineFunction &MF) const {
  return RC->getNumRegs();
}

bool SIRegisterInfo::requiresRegisterScavenging(const MachineFunction &Fn) const {
  return Fn.getFrameInfo()->hasStackObjects();
}

static unsigned getNumSubRegsForSpillOp(unsigned Op) {

  switch (Op) {
  case AMDGPU::SI_SPILL_S512_SAVE:
  case AMDGPU::SI_SPILL_S512_RESTORE:
    return 16;
  case AMDGPU::SI_SPILL_S256_SAVE:
  case AMDGPU::SI_SPILL_S256_RESTORE:
    return 8;
  case AMDGPU::SI_SPILL_S128_SAVE:
  case AMDGPU::SI_SPILL_S128_RESTORE:
    return 4;
  case AMDGPU::SI_SPILL_S64_SAVE:
  case AMDGPU::SI_SPILL_S64_RESTORE:
    return 2;
  case AMDGPU::SI_SPILL_S32_SAVE:
  case AMDGPU::SI_SPILL_S32_RESTORE:
    return 1;
  default: llvm_unreachable("Invalid spill opcode");
  }
}

void SIRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator MI,
                                        int SPAdj, unsigned FIOperandNum,
                                        RegScavenger *RS) const {
  MachineFunction *MF = MI->getParent()->getParent();
  MachineBasicBlock *MBB = MI->getParent();
  SIMachineFunctionInfo *MFI = MF->getInfo<SIMachineFunctionInfo>();
  MachineFrameInfo *FrameInfo = MF->getFrameInfo();
  const SIInstrInfo *TII = static_cast<const SIInstrInfo*>(ST.getInstrInfo());
  DebugLoc DL = MI->getDebugLoc();

  MachineOperand &FIOp = MI->getOperand(FIOperandNum);
  int Index = MI->getOperand(FIOperandNum).getIndex();

  switch (MI->getOpcode()) {
    // SGPR register spill
    case AMDGPU::SI_SPILL_S512_SAVE:
    case AMDGPU::SI_SPILL_S256_SAVE:
    case AMDGPU::SI_SPILL_S128_SAVE:
    case AMDGPU::SI_SPILL_S64_SAVE:
    case AMDGPU::SI_SPILL_S32_SAVE: {
      unsigned NumSubRegs = getNumSubRegsForSpillOp(MI->getOpcode());

      for (unsigned i = 0, e = NumSubRegs; i < e; ++i) {
        unsigned SubReg = getPhysRegSubReg(MI->getOperand(0).getReg(),
                                           &AMDGPU::SGPR_32RegClass, i);
        struct SIMachineFunctionInfo::SpilledReg Spill =
            MFI->getSpilledReg(MF, Index, i);

        if (Spill.VGPR == AMDGPU::NoRegister) {
           LLVMContext &Ctx = MF->getFunction()->getContext();
           Ctx.emitError("Ran out of VGPRs for spilling SGPR");
        }

        BuildMI(*MBB, MI, DL, TII->get(AMDGPU::V_WRITELANE_B32), Spill.VGPR)
                .addReg(SubReg)
                .addImm(Spill.Lane);

      }
      MI->eraseFromParent();
      break;
    }

    // SGPR register restore
    case AMDGPU::SI_SPILL_S512_RESTORE:
    case AMDGPU::SI_SPILL_S256_RESTORE:
    case AMDGPU::SI_SPILL_S128_RESTORE:
    case AMDGPU::SI_SPILL_S64_RESTORE:
    case AMDGPU::SI_SPILL_S32_RESTORE: {
      unsigned NumSubRegs = getNumSubRegsForSpillOp(MI->getOpcode());

      for (unsigned i = 0, e = NumSubRegs; i < e; ++i) {
        unsigned SubReg = getPhysRegSubReg(MI->getOperand(0).getReg(),
                                           &AMDGPU::SGPR_32RegClass, i);
        struct SIMachineFunctionInfo::SpilledReg Spill =
            MFI->getSpilledReg(MF, Index, i);

        if (Spill.VGPR == AMDGPU::NoRegister) {
           LLVMContext &Ctx = MF->getFunction()->getContext();
           Ctx.emitError("Ran out of VGPRs for spilling SGPR");
        }

        BuildMI(*MBB, MI, DL, TII->get(AMDGPU::V_READLANE_B32), SubReg)
                .addReg(Spill.VGPR)
                .addImm(Spill.Lane);

      }
      TII->insertNOPs(MI, 3);
      MI->eraseFromParent();
      break;
    }

    default: {
      int64_t Offset = FrameInfo->getObjectOffset(Index);
      FIOp.ChangeToImmediate(Offset);
      if (!TII->isImmOperandLegal(MI, FIOperandNum, FIOp)) {
        unsigned TmpReg = RS->scavengeRegister(&AMDGPU::VReg_32RegClass, MI, SPAdj);
        BuildMI(*MBB, MI, MI->getDebugLoc(),
                TII->get(AMDGPU::V_MOV_B32_e32), TmpReg)
                .addImm(Offset);
        FIOp.ChangeToRegister(TmpReg, false);
      }
    }
  }
}

const TargetRegisterClass * SIRegisterInfo::getCFGStructurizerRegClass(
                                                                   MVT VT) const {
  switch(VT.SimpleTy) {
    default:
    case MVT::i32: return &AMDGPU::VReg_32RegClass;
  }
}

unsigned SIRegisterInfo::getHWRegIndex(unsigned Reg) const {
  return getEncodingValue(Reg) & 0xff;
}

const TargetRegisterClass *SIRegisterInfo::getPhysRegClass(unsigned Reg) const {
  assert(!TargetRegisterInfo::isVirtualRegister(Reg));

  const TargetRegisterClass *BaseClasses[] = {
    &AMDGPU::VReg_32RegClass,
    &AMDGPU::SReg_32RegClass,
    &AMDGPU::VReg_64RegClass,
    &AMDGPU::SReg_64RegClass,
    &AMDGPU::SReg_128RegClass,
    &AMDGPU::SReg_256RegClass
  };

  for (const TargetRegisterClass *BaseClass : BaseClasses) {
    if (BaseClass->contains(Reg)) {
      return BaseClass;
    }
  }
  return nullptr;
}

bool SIRegisterInfo::isSGPRClass(const TargetRegisterClass *RC) const {
  if (!RC) {
    return false;
  }
  return !hasVGPRs(RC);
}

bool SIRegisterInfo::hasVGPRs(const TargetRegisterClass *RC) const {
  return getCommonSubClass(&AMDGPU::VReg_32RegClass, RC) ||
         getCommonSubClass(&AMDGPU::VReg_64RegClass, RC) ||
         getCommonSubClass(&AMDGPU::VReg_96RegClass, RC) ||
         getCommonSubClass(&AMDGPU::VReg_128RegClass, RC) ||
         getCommonSubClass(&AMDGPU::VReg_256RegClass, RC) ||
         getCommonSubClass(&AMDGPU::VReg_512RegClass, RC);
}

const TargetRegisterClass *SIRegisterInfo::getEquivalentVGPRClass(
                                         const TargetRegisterClass *SRC) const {
    if (hasVGPRs(SRC)) {
      return SRC;
    } else if (SRC == &AMDGPU::SCCRegRegClass) {
      return &AMDGPU::VCCRegRegClass;
    } else if (getCommonSubClass(SRC, &AMDGPU::SGPR_32RegClass)) {
      return &AMDGPU::VReg_32RegClass;
    } else if (getCommonSubClass(SRC, &AMDGPU::SGPR_64RegClass)) {
      return &AMDGPU::VReg_64RegClass;
    } else if (getCommonSubClass(SRC, &AMDGPU::SReg_128RegClass)) {
      return &AMDGPU::VReg_128RegClass;
    } else if (getCommonSubClass(SRC, &AMDGPU::SReg_256RegClass)) {
      return &AMDGPU::VReg_256RegClass;
    } else if (getCommonSubClass(SRC, &AMDGPU::SReg_512RegClass)) {
      return &AMDGPU::VReg_512RegClass;
    }
    return nullptr;
}

const TargetRegisterClass *SIRegisterInfo::getSubRegClass(
                         const TargetRegisterClass *RC, unsigned SubIdx) const {
  if (SubIdx == AMDGPU::NoSubRegister)
    return RC;

  // If this register has a sub-register, we can safely assume it is a 32-bit
  // register, because all of SI's sub-registers are 32-bit.
  if (isSGPRClass(RC)) {
    return &AMDGPU::SGPR_32RegClass;
  } else {
    return &AMDGPU::VGPR_32RegClass;
  }
}

unsigned SIRegisterInfo::getPhysRegSubReg(unsigned Reg,
                                          const TargetRegisterClass *SubRC,
                                          unsigned Channel) const {

  switch (Reg) {
    case AMDGPU::VCC:
      switch(Channel) {
        case 0: return AMDGPU::VCC_LO;
        case 1: return AMDGPU::VCC_HI;
        default: llvm_unreachable("Invalid SubIdx for VCC");
      }
      break;

  case AMDGPU::FLAT_SCR:
    switch (Channel) {
    case 0:
      return AMDGPU::FLAT_SCR_LO;
    case 1:
      return AMDGPU::FLAT_SCR_HI;
    default:
      llvm_unreachable("Invalid SubIdx for FLAT_SCR");
    }
    break;

  case AMDGPU::EXEC:
    switch (Channel) {
    case 0:
      return AMDGPU::EXEC_LO;
    case 1:
      return AMDGPU::EXEC_HI;
    default:
      llvm_unreachable("Invalid SubIdx for EXEC");
    }
    break;
  }

  unsigned Index = getHWRegIndex(Reg);
  return SubRC->getRegister(Index + Channel);
}

bool SIRegisterInfo::regClassCanUseImmediate(int RCID) const {
  switch (RCID) {
  default: return false;
  case AMDGPU::SSrc_32RegClassID:
  case AMDGPU::SSrc_64RegClassID:
  case AMDGPU::VSrc_32RegClassID:
  case AMDGPU::VSrc_64RegClassID:
    return true;
  }
}

bool SIRegisterInfo::regClassCanUseImmediate(
                             const TargetRegisterClass *RC) const {
  return regClassCanUseImmediate(RC->getID());
}

unsigned SIRegisterInfo::getPreloadedValue(const MachineFunction &MF,
                                           enum PreloadedValue Value) const {

  const SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  switch (Value) {
  case SIRegisterInfo::TGID_X:
    return AMDGPU::SReg_32RegClass.getRegister(MFI->NumUserSGPRs + 0);
  case SIRegisterInfo::TGID_Y:
    return AMDGPU::SReg_32RegClass.getRegister(MFI->NumUserSGPRs + 1);
  case SIRegisterInfo::TGID_Z:
    return AMDGPU::SReg_32RegClass.getRegister(MFI->NumUserSGPRs + 2);
  case SIRegisterInfo::SCRATCH_WAVE_OFFSET:
    return AMDGPU::SReg_32RegClass.getRegister(MFI->NumUserSGPRs + 4);
  case SIRegisterInfo::SCRATCH_PTR:
    return AMDGPU::SGPR2_SGPR3;
  }
  llvm_unreachable("unexpected preloaded value type");
}
