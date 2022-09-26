// RUN: python -m triton.tools.aot %s --target=ptx --sm=80 --ptx-version=63 | FileCheck %s
// CHECK-LABEL: // Generated by LLVM NVPTX Back-End
// CHECK: .version 6.3
// CHECK: .target sm_80
// CHECK: .address_size 64

module attributes {"triton_gpu.num-warps" = 4 : i32} {

func @test_empty_kernel(%lb : index, %A : !tt.ptr<f16>) {

  return
}

}
