# RUN: llvm-mc -triple mips64el-unknown-linux -show-encoding -print-imm-hex %s | FileCheck %s
# XFAIL: *
  .text
text_label:
# CHECK: text_label:
  nor $4, $5, 0
# CHECK:      addiu   $4, $zero, 0x0          # encoding: [0x00,0x00,0x04,0x24]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 1
# CHECK-NEXT: addiu   $4, $zero, 0x1          # encoding: [0x01,0x00,0x04,0x24]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0x8000
# CHECK-NEXT: ori     $4, $zero, 0x8000       # encoding: [0x00,0x80,0x04,0x34]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, -0x8000
# CHECK-NEXT: addiu   $4, $zero, -0x8000      # encoding: [0x00,0x80,0x04,0x24]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0x10000
# CHECK-NEXT: lui     $4, 0x1                 # encoding: [0x01,0x00,0x04,0x3c]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0x1a5a5
# CHECK-NEXT: lui     $4, 0x1                 # encoding: [0x01,0x00,0x04,0x3c]
# CHECK-NEXT: ori     $4, $4, 0xa5a5          # encoding: [0xa5,0xa5,0x84,0x34]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0xFFFFFFFF
# CHECK-NEXT: lui     $4, 0xffff              # encoding: [0xff,0xff,0x04,0x3c]
# CHECK-NEXT: dsrl32  $4, $4, 0x0             # encoding: [0x3e,0x20,0x04,0x00]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0xF0000000
# CHECK-NEXT: ori     $4, $zero, 0xf000       # encoding: [0x00,0xf0,0x04,0x34]
# CHECK-NEXT: dsll    $4, $4, 0x10            # encoding: [0x38,0x24,0x04,0x00]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0x7FFFFFFF
# CHECK-NEXT: lui     $4, 0x7fff              # encoding: [0xff,0x7f,0x04,0x3c]
# CHECK-NEXT: ori     $4, $4, 0xffff          # encoding: [0xff,0xff,0x84,0x34]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0x7FFFFFFFFFFFFFFF
# FIXME: this is awfully inefficient...
# CHECK-NEXT: lui     $4, 0x7fff              # encoding: [0xff,0x7f,0x04,0x3c]
# CHECK-NEXT: ori     $4, $4, 0xffff          # encoding: [0xff,0xff,0x84,0x34]
# CHECK-NEXT: dsll    $4, $4, 0x10            # encoding: [0x38,0x24,0x04,0x00]
# CHECK-NEXT: ori     $4, $4, 0xffff          # encoding: [0xff,0xff,0x84,0x34]
# CHECK-NEXT: dsll    $4, $4, 0x10            # encoding: [0x38,0x24,0x04,0x00]
# CHECK-NEXT: ori     $4, $4, 0xffff          # encoding: [0xff,0xff,0x84,0x34]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0xFFFFFFFFFFFFFFFF
# CHECK-NEXT: addiu   $4, $zero, -0x1         # encoding: [0xff,0xff,0x04,0x24]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0xF000000000000000
# CHECK-NEXT: ori     $4, $zero, 0xf000       # encoding: [0x00,0xf0,0x04,0x34]
# CHECK-NEXT: dsll    $4, $4, 0x30            # encoding: [0x3c,0x24,0x04,0x00]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, ~(0xf0000000|0x0f000000|0x000000f0)
# CHECK-NEXT: addiu   $4, $zero, -0x1         # encoding: [0xff,0xff,0x04,0x24]
# CHECK-NEXT: dsll    $4, $4, 0x10            # encoding: [0x38,0x24,0x04,0x00]
# CHECK-NEXT: ori     $4, $4, 0xff            # encoding: [0xff,0x00,0x84,0x34]
# CHECK-NEXT: dsll    $4, $4, 0x10            # encoding: [0x38,0x24,0x04,0x00]
# CHECK-NEXT: ori     $4, $4, 0xff0f          # encoding: [0x0f,0xff,0x84,0x34]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]
  nor $4, $5, 0xff00ff00
# CHECK-NEXT: ori     $4, $zero, 0xff00       # encoding: [0x00,0xff,0x04,0x34]
# CHECK-NEXT: dsll    $4, $4, 0x10            # encoding: [0x38,0x24,0x04,0x00]
# CHECK-NEXT: ori     $4, $4, 0xff00          # encoding: [0x00,0xff,0x84,0x34]
# CHECK-NEXT: nor     $4, $4, $5              # encoding: [0x27,0x20,0x85,0x00]


  slt $4, $5, -0x80000000
# CHECK:      lui     $4, 0x8000              # encoding: [0x00,0x80,0x04,0x3c]
# CHECK-NEXT: slt     $4, $4, $5              # encoding: [0x2a,0x20,0x85,0x00]
  slt $4, $5, -0x8001
# CHECK-NEXT: lui     $4, 0xffff              # encoding: [0xff,0xff,0x04,0x3c]
# CHECK-NEXT: ori     $4, $4, 0x7fff          # encoding: [0xff,0x7f,0x84,0x34]
# CHECK-NEXT: slt     $4, $4, $5              # encoding: [0x2a,0x20,0x85,0x00]
  slt $4, $5, -0x8000
# CHECK-NEXT: slti    $4, $5, -0x8000         # encoding: [0x00,0x80,0xa4,0x28]
  slt $4, $5, 0
# CHECK-NEXT: slti    $4, $5, 0x0             # encoding: [0x00,0x00,0xa4,0x28]
  slt $4, $5, 0xFFFF
# CHECK-NEXT: ori     $4, $zero, 0xffff       # encoding: [0xff,0xff,0x04,0x34]
# CHECK-NEXT: slt     $4, $4, $5              # encoding: [0x2a,0x20,0x85,0x00]
  slt $4, $5, 0x10000
# CHECK-NEXT: lui     $4, 0x1                 # encoding: [0x01,0x00,0x04,0x3c]
# CHECK-NEXT: slt     $4, $4, $5              # encoding: [0x2a,0x20,0x85,0x00]
  slt $4, $5, 0xFFFFFFFF
# CHECK-NOT: slti    $4, $5, -0x1             # encoding: [0xff,0xff,0xa4,0x28]

  sltu $4, $5, -0x80000000
# CHECK:      lui     $4, 0x8000              # encoding: [0x00,0x80,0x04,0x3c]
# CHECK-NEXT: sltu    $4, $4, $5              # encoding: [0x2b,0x20,0x85,0x00]
  sltu $4, $5, -0x8001
# CHECK-NEXT: lui     $4, 0xffff              # encoding: [0xff,0xff,0x04,0x3c]
# CHECK-NEXT: ori     $4, $4, 0x7fff          # encoding: [0xff,0x7f,0x84,0x34]
# CHECK-NEXT: sltu    $4, $4, $5              # encoding: [0x2b,0x20,0x85,0x00]
  sltu $4, $5, -0x8000
# CHECK-NEXT: sltiu   $4, $5, -0x8000         # encoding: [0x00,0x80,0xa4,0x2c]
  sltu $4, $5, 0
# CHECK-NEXT: sltiu   $4, $5, 0x0             # encoding: [0x00,0x00,0xa4,0x2c]
  sltu $4, $5, 0xFFFF
# CHECK-NEXT: ori     $4, $zero, 0xffff       # encoding: [0xff,0xff,0x04,0x34]
# CHECK-NEXT: sltu    $4, $4, $5              # encoding: [0x2b,0x20,0x85,0x00]
  sltu $4, $5, 0x10000
# CHECK-NEXT: lui     $4, 0x1                 # encoding: [0x01,0x00,0x04,0x3c]
# CHECK-NEXT: sltu    $4, $4, $5              # encoding: [0x2b,0x20,0x85,0x00]
# FIXME: should this be sign extended or not???
  sltu $4, $5, 0xFFFFFFFF
# CHECK-NOT: sltiu   $4, $5, -0x1            # encoding: [0xff,0xff,0xa4,0x2c]
