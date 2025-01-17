// NOTE: Assertions have been autogenerated by utils/update_cc_test_checks.py UTC_ARGS: --function-signature
// RUN: %cheri_purecap_cc1 -fcxx-exceptions -fexceptions -o - -emit-llvm %s | FileCheck %s
// This code previously triggered a verification error due to a mismerge (missing call to @llvm.cheri.cap.address.get.i64):
// GEP indexes must be integers
//   %add.ptr = getelementptr inbounds i8, i8 addrspace(200)* %2, %vbase.offset
class a {
public:
  void b();
};
class c : virtual public a {};
// CHECK-LABEL: define {{[^@]+}}@_Z1dv() addrspace(200) #0
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[E:%.*]] = alloca [[CLASS_C:%.*]], align 16, addrspace(200)
// CHECK-NEXT:    call void @_ZN1cC1Ev(%class.c addrspace(200)* [[E]]) #4
// CHECK-NEXT:    [[TMP0:%.*]] = bitcast [[CLASS_C]] addrspace(200)* [[E]] to i8 addrspace(200)* addrspace(200)*
// CHECK-NEXT:    [[VTABLE:%.*]] = load i8 addrspace(200)*, i8 addrspace(200)* addrspace(200)* [[TMP0]], align 16
// CHECK-NEXT:    [[VBASE_OFFSET_PTR:%.*]] = getelementptr i8, i8 addrspace(200)* [[VTABLE]], i64 -48
// CHECK-NEXT:    [[TMP1:%.*]] = bitcast i8 addrspace(200)* [[VBASE_OFFSET_PTR]] to i8 addrspace(200)* addrspace(200)*
// CHECK-NEXT:    [[VBASE_OFFSET:%.*]] = load i8 addrspace(200)*, i8 addrspace(200)* addrspace(200)* [[TMP1]], align 16
// CHECK-NEXT:    [[TMP2:%.*]] = call i64 @llvm.cheri.cap.address.get.i64(i8 addrspace(200)* [[VBASE_OFFSET]])
// CHECK-NEXT:    [[TMP3:%.*]] = bitcast [[CLASS_C]] addrspace(200)* [[E]] to i8 addrspace(200)*
// CHECK-NEXT:    [[ADD_PTR:%.*]] = getelementptr inbounds i8, i8 addrspace(200)* [[TMP3]], i64 [[TMP2]]
// CHECK-NEXT:    [[TMP4:%.*]] = bitcast i8 addrspace(200)* [[ADD_PTR]] to [[CLASS_A:%.*]] addrspace(200)*
// CHECK-NEXT:    call void @_ZN1a1bEv(%class.a addrspace(200)* [[TMP4]])
// CHECK-NEXT:    ret void
//
void d() {
  c e;
  e.b();
}
