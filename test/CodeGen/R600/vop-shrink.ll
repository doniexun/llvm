; RUN: llc -march=r600 -mcpu=SI -verify-machineinstrs< %s | FileCheck -check-prefix=SI -check-prefix=FUNC %s

; Test that we correctly commute a sub instruction
; FUNC-LABEL: @sub_rev
; SI-NOT: V_SUB_I32_e32 v{{[0-9]+}}, s
; SI: V_SUBREV_I32_e32 v{{[0-9]+}}, s

; ModuleID = 'vop-shrink.ll'

define void @sub_rev(i32 addrspace(1)* %out, <4 x i32> %sgpr, i32 %cond) {
entry:
  %vgpr = call i32 @llvm.r600.read.tidig.x() #1
  %tmp = icmp eq i32 %cond, 0
  br i1 %tmp, label %if, label %else

if:                                               ; preds = %entry
  %tmp1 = getelementptr i32 addrspace(1)* %out, i32 1
  %tmp2 = extractelement <4 x i32> %sgpr, i32 1
  store i32 %tmp2, i32 addrspace(1)* %out
  br label %endif

else:                                             ; preds = %entry
  %tmp3 = extractelement <4 x i32> %sgpr, i32 2
  %tmp4 = sub i32 %vgpr, %tmp3
  store i32 %tmp4, i32 addrspace(1)* %out
  br label %endif

endif:                                            ; preds = %else, %if
  ret void
}

; Test that we fold an immediate that was illegal for a 64-bit op into the
; 32-bit op when we shrink it.

; FUNC-LABEL: @add_fold
; SI: V_ADD_F32_e32 v{{[0-9]+}}, 0x44800000
define void @add_fold(float addrspace(1)* %out) {
entry:
  %tmp = call i32 @llvm.r600.read.tidig.x()
  %tmp1 = uitofp i32 %tmp to float
  %tmp2 = fadd float %tmp1, 1.024000e+03
  store float %tmp2, float addrspace(1)* %out
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @llvm.r600.read.tidig.x() #0

attributes #0 = { nounwind readnone }
attributes #1 = { readnone }
