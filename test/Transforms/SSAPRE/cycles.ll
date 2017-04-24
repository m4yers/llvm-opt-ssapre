; RUN: opt < %s -ssapre -S | FileCheck %s
target datalayout = "e-p:64:64:64-p1:16:16:16-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:32:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-n8:16:32:64"

;           ---------------                        ---------------
;                                                    %1 = %0 + 1
;           ---------------                        ---------------
; .-------------.  |                     .-------------.  |
; |       -------------------            |       -------------------
; |          %p = phi(0,%5)         \\   |
; |       -------------------       //   |       -------------------
; |          /           \               |          /           \
; |  -------------   -------------       |  -------------   -------------
; |   %5 = %0 + 1       ret %p           |                     ret %1
; |  -------------   -------------       |  -------------   -------------
; ._______/                              ._______/
;
; CHECK-LABEL: @cycle_1(
; CHECK:       add
; CHECK:       br
; CHECK:       br
; CHECK:       br
; CHECK:       ret
define i32 @cycle_1(i32, i8**) #0 {
  br label %3

  %.0 = phi i32 [ 0, %2 ], [ %5, %4 ]
  br i1 false, label %4, label %6

  %5 = add nsw i32 %0, 1
  br label %3

  ret i32 %.0
}

;           ---------------                        ---------------
;
;           ---------------                        ---------------
; .-------------.  |                     .-------------.  |
; |       -------------------            |       -------------------
; |          %p = phi(0,%5)         \\   |
; |       -------------------       //   |       -------------------
; |          /           \               |          /           \
; |  -------------   -------------       |  -------------   -------------
; |   %5 = %0 + 1       ret 0            |                     ret 0
; |  -------------   -------------       |  -------------   -------------
; ._______/                              ._______/
;
; CHECK-LABEL: @cycle_1_no_ds(
; CHECK-NOT:   add
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       br
; CHECK:       br
; CHECK:       ret
define i32 @cycle_1_no_ds(i32, i8**) #0 {
  br label %3

  %p = phi i32 [ 0, %2 ], [ %5, %4 ]
  br i1 false, label %4, label %6

  %5 = add nsw i32 %0, 1
  br label %3

  ret i32 0
}

;           ---------------                        ---------------
;             %1 = %0 + 1                            %1 = %0 + 1
;           ---------------                        ---------------
; .-------------.  |                     .-------------.  |
; |       -------------------            |       -------------------
; |          %p = phi(0,%5)         \\   |
; |       -------------------       //   |       -------------------
; |          /           \               |          /           \
; |  -------------   -------------       |  -------------   -------------
; |   %5 = %0 + 1       ret %p           |                     ret %1
; |  -------------   -------------       |  -------------   -------------
; ._______/                              ._______/
;
; CHECK-LABEL: @cycle_2(
; CHECK:       add
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       br
; CHECK:       br
; CHECK:       ret
define i32 @cycle_2(i32, i8**) #0 {
  %3 = add nsw i32 %0, 1
  br label %4

  %.0 = phi i32 [ %3, %2 ], [ %6, %5 ]
  br i1 false, label %5, label %7

  %6 = add nsw i32 %0, 1
  br label %4

  ret i32 %.0
}

;           ---------------                        ---------------
;             %1 = %0 + 1
;           ---------------                        ---------------
; .-------------.  |                     .-------------.  |
; |       -------------------            |       -------------------
; |          %p = phi(0,%5)         \\   |
; |       -------------------       //   |       -------------------
; |          /           \               |          /           \
; |  -------------   -------------       |  -------------   -------------
; |   %5 = %0 + 1       ret 0            |                     ret 0
; |  -------------   -------------       |  -------------   -------------
; ._______/                              ._______/
;
; CHECK-LABEL: @cycle_2_no_ds(
; CHECK-NOT:   add
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       br
; CHECK:       br
; CHECK:       ret
define i32 @cycle_2_no_ds(i32, i8**) #0 {
  %3 = add nsw i32 %0, 1
  br label %4

  %.0 = phi i32 [ %3, %2 ], [ %6, %5 ]
  br i1 false, label %5, label %7

  %6 = add nsw i32 %0, 1
  br label %4

  ret i32 0
}

;           ---------------                      -------------------
;             %1 = %0 + 1                         %1 = %0 + 1
;           ---------------                       %p = inttoptr %1
;                  |                             -------------------
; .-------------.  |                     .-------------.  |
; |       -------------------            |       -------------------
; |        %p = phi(%1,%2)         \\    |        %val = load %p
; |        %ptr = inttoptr %p      //    |       -------------------
; |        %val = load %ptr              |            /       \
; |       -------------------            |           /         \
; |          /           \               |          /           \
; |  -------------   -------------       |  -------------   -------------
; |   %2 = %0 + 1       ret %p           |                     ret %1
; |  -------------   -------------       |  -------------   -------------
; ._______/                              ._______/
;
; CHECK-LABEL: @cycle_3(
; CHECK:       add
; CHECK:       inttoptr
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       load
; CHECK:       br
; CHECK:       br
; CHECK:       ret
define i64 @cycle_3(i64, i8**) #0 {
  %3 = add nsw i64 %0, 1
  br label %4

  %p = phi i64 [ %3, %2 ], [ %6, %5 ]
  %ptr = inttoptr i64 %p to i64*
  %val = load i64, i64* %ptr
  br i1 false, label %5, label %7

  %6 = add nsw i64 %0, 1
  br label %4

  ret i64 %p
}

;           ---------------                      -------------------
;             %1 = %0 + 1                         %1 = %0 + 1
;           ---------------                       %p = inttoptr %1
;                  |                             -------------------
; .-------------.  |                     .-------------.  |
; |       -------------------            |       -------------------
; |        %p = phi(%1,%2)         \\    |        %val = load %p
; |        %ptr = inttoptr %p      //    |       -------------------
; |        %val = load %ptr              |            /       \
; |       -------------------            |           /         \
; |          /           \               |          /           \
; |  -------------   -------------       |  -------------   -------------
; |   %2 = %0 + 1       ret 0            |                     ret 0
; |  -------------   -------------       |  -------------   -------------
; ._______/                              ._______/
;
; CHECK-LABEL: @cycle_3_no_ds(
; CHECK:       add
; CHECK:       inttoptr
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       load
; CHECK:       br
; CHECK:       br
; CHECK:       ret
define i64 @cycle_3_no_ds(i64, i8**) #0 {
  %3 = add nsw i64 %0, 1
  br label %4

  %p = phi i64 [ %3, %2 ], [ %6, %5 ]
  %ptr = inttoptr i64 %p to i64*
  %val = load i64, i64* %ptr
  br i1 false, label %5, label %7

  %6 = add nsw i64 %0, 1
  br label %4

  ret i64 0
}

;               -------------------                                -------------------
;                %1 = %0 + 1                                        %1 = %0 + 1
;               -------------------                                 %a = inttoptr %1
;                        |                                         -------------------
; .-------------------.  |                            .-------------------. |
; |             -------------------                  |             -------------------
; |              %p = phi(%1,%2)                     |
; |             -------------------              \\  |             -------------------
; |               /             \                //  |               /             \
; |  -------------------    -------------------      |  -------------------    -------------------
; |   %2 = %0 + 1            ret %p                  |   %v = load %p           ret %p
; |   %p = inttoptr $2      -------------------      |  -------------------    -------------------
; |   $v = load %p                                   .________/
; |  -------------------
; ._______/
;
; CHECK-LABEL: @cycle_4(
; CHECK:       add
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       br
; CHECK-NOT:   add
; CHECK:       load
; CHECK:       ret
define i64 @cycle_4(i64, i8**) #0 {
  %3 = add nsw i64 %0, 1
  br label %4

  %p = phi i64 [ %3, %2 ], [ %6, %5 ]
  br i1 false, label %5, label %7

  %6 = add nsw i64 %0, 1
  %ptr = inttoptr i64 %6 to i64*
  %val = load i64, i64* %ptr
  br label %4

  ret i64 %p
}

;               -------------------                                -------------------
;                %1 = %0 + 1                                        %1 = %0 + 1
;               -------------------                                -------------------
; .-------------------.  |                            .-------------------. |
; |             -------------------                  |             -------------------
; |              %p = phi(%1,%2)                     |
; |             -------------------              \\  |             -------------------
; |               /             \                //  |               /             \
; |  -------------------    -------------------      |  -------------------    -------------------
; |   %2 = %0 + 1            ret 0                   |   %v = load %1           ret 0
; |   %p = inttoptr $2      -------------------      |  -------------------    -------------------
; |   $v = load %p                                   .________/
; |  -------------------
; ._______/
;
; CHECK-LABEL: @cycle_4_no_ds(
; CHECK:       add
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       br
; CHECK-NOT:   add
; CHECK:       load
; CHECK:       ret
define i64 @cycle_4_no_ds(i64, i8**) #0 {
  %3 = add nsw i64 %0, 1
  br label %4

  %p = phi i64 [ %3, %2 ], [ %6, %5 ]
  br i1 false, label %5, label %7

  %6 = add nsw i64 %0, 1
  %ptr = inttoptr i64 %6 to i64*
  %val = load i64, i64* %ptr
  br label %4

  ret i64 0
}

;               -------------------                                -------------------
;                                                                   %1 = %0 + 1
;               -------------------                                -------------------
; .-------------------.  |                            .-------------------. |
; |             -------------------                  |             -------------------
; |              %p = phi(0,%2)                      |
; |             -------------------              \\  |             -------------------
; |               /             \                //  |               /             \
; |  -------------------    -------------------      |  -------------------    -------------------
; |   %2 = %0 + 1            ret %p                  |   %v = load %1           ret %1
; |   %p = inttoptr $2      -------------------      |  -------------------    -------------------
; |   $v = load %p                                   .________/
; |  -------------------
; ._______/
;
; CHECK-LABEL: @cycle_5(
; CHECK:       add
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       br
; CHECK-NOT:   add
; CHECK:       load
; CHECK:       ret
define i64 @cycle_5(i64, i8**) #0 {
  br label %3

  %p = phi i64 [ 1, %2 ], [ %5, %4 ]
  br i1 false, label %4, label %6

  %5 = add nsw i64 %0, 1
  %ptr = inttoptr i64 %5 to i64*
  %val = load i64, i64* %ptr
  br label %3

  ret i64 %p
}

;               -------------------                                -------------------
;                                                                   %1 = %0 + 1
;               -------------------                                -------------------
; .-------------------.  |                            .-------------------. |
; |             -------------------                  |             -------------------
; |              %p = phi(0,%2)                      |
; |             -------------------              \\  |             -------------------
; |               /             \                //  |               /             \
; |  -------------------    -------------------      |  -------------------    -------------------
; |   %2 = %0 + 1            ret 0                   |   %v = load %1           ret 0
; |   %p = inttoptr $2      -------------------      |  -------------------    -------------------
; |   $v = load %p                                   .________/
; |  -------------------
; ._______/
;
; CHECK-LABEL: @cycle_5_no_ds(
; CHECK:       add
; CHECK:       br
; CHECK-NOT:   phi
; CHECK:       br
; CHECK-NOT:   add
; CHECK:       load
; CHECK:       ret
define i64 @cycle_5_no_ds(i64, i8**) #0 {
  br label %3

  %p = phi i64 [ 1, %2 ], [ %5, %4 ]
  br i1 false, label %4, label %6

  %5 = add nsw i64 %0, 1
  %ptr = inttoptr i64 %5 to i64*
  %val = load i64, i64* %ptr
  br label %3

  ret i64 0
}

; CHECK-LABEL: @cycle_6(
; CHECK-NOT:   getelementptr
define void @cycle_6(i8** %argv) #0 {
  br label %1

; <label>:1:                                      ; preds = %10, %6, %0
  br i1 undef, label %2, label %11

; <label>:2:                                      ; preds = %1
  br i1 undef, label %3, label %7

; <label>:3:                                      ; preds = %2
  br label %4

; <label>:4:                                      ; preds = %5, %3
  br i1 undef, label %5, label %6

; <label>:5:                                      ; preds = %4
  br label %4

; <label>:6:                                      ; preds = %4
  br label %1

; <label>:7:                                      ; preds = %2
  br label %8

; <label>:8:                                      ; preds = %9, %7
  br i1 undef, label %9, label %10

; <label>:9:                                      ; preds = %8
  %arrayidx35 = getelementptr inbounds i8*, i8** %argv, i64 undef
  br label %8

; <label>:10:                                     ; preds = %8
  br label %1

; <label>:11:                                     ; preds = %1
  ret void
}
