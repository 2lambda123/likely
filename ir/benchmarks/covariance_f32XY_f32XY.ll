; ModuleID = 'likely'

%u0CXYT = type { i32, i32, i32, i32, i32, i32, [0 x i8] }
%f32XY = type { i32, i32, i32, i32, i32, i32, [0 x float] }

; Function Attrs: nounwind readonly
declare noalias %u0CXYT* @likely_new(i32 zeroext, i32 zeroext, i32 zeroext, i32 zeroext, i32 zeroext, i8* noalias nocapture) #0

; Function Attrs: nounwind
declare void @llvm.assume(i1) #1

define %f32XY* @covariance(%f32XY*) {
entry:
  %1 = getelementptr inbounds %f32XY, %f32XY* %0, i64 0, i32 3
  %columns = load i32, i32* %1, align 4, !range !0
  %2 = call %u0CXYT* @likely_new(i32 8480, i32 1, i32 %columns, i32 1, i32 1, i8* null)
  %3 = zext i32 %columns to i64
  %4 = getelementptr inbounds %u0CXYT, %u0CXYT* %2, i64 1
  %5 = bitcast %u0CXYT* %4 to float*
  %6 = ptrtoint %u0CXYT* %4 to i64
  %7 = and i64 %6, 31
  %8 = icmp eq i64 %7, 0
  call void @llvm.assume(i1 %8)
  %scevgep1 = bitcast %u0CXYT* %4 to i8*
  %9 = shl nuw nsw i64 %3, 2
  call void @llvm.memset.p0i8.i64(i8* %scevgep1, i8 0, i64 %9, i32 4, i1 false)
  %10 = getelementptr inbounds %f32XY, %f32XY* %0, i64 0, i32 4
  %rows = load i32, i32* %10, align 4, !range !0
  %11 = zext i32 %rows to i64
  %12 = getelementptr inbounds %f32XY, %f32XY* %0, i64 0, i32 6, i64 0
  %13 = ptrtoint float* %12 to i64
  %14 = and i64 %13, 31
  %15 = icmp eq i64 %14, 0
  call void @llvm.assume(i1 %15)
  br label %y_body

y_body:                                           ; preds = %x_exit8, %entry
  %y = phi i64 [ 0, %entry ], [ %y_increment, %x_exit8 ]
  %16 = mul nuw nsw i64 %y, %3
  br label %x_body7

x_body7:                                          ; preds = %y_body, %x_body7
  %x9 = phi i64 [ %x_increment10, %x_body7 ], [ 0, %y_body ]
  %17 = getelementptr float, float* %5, i64 %x9
  %18 = load float, float* %17, align 4
  %19 = add nuw nsw i64 %x9, %16
  %20 = getelementptr %f32XY, %f32XY* %0, i64 0, i32 6, i64 %19
  %21 = load float, float* %20, align 4
  %22 = fadd fast float %21, %18
  store float %22, float* %17, align 4
  %x_increment10 = add nuw nsw i64 %x9, 1
  %x_postcondition11 = icmp eq i64 %x_increment10, %3
  br i1 %x_postcondition11, label %x_exit8, label %x_body7

x_exit8:                                          ; preds = %x_body7
  %y_increment = add nuw nsw i64 %y, 1
  %y_postcondition = icmp eq i64 %y_increment, %11
  br i1 %y_postcondition, label %y_exit, label %y_body

y_exit:                                           ; preds = %x_exit8
  %23 = uitofp i32 %rows to float
  %norm = fdiv fast float 1.000000e+00, %23
  br label %x_body16

x_body16:                                         ; preds = %x_body16, %y_exit
  %x18 = phi i64 [ 0, %y_exit ], [ %x_increment19, %x_body16 ]
  %24 = getelementptr float, float* %5, i64 %x18
  %25 = load float, float* %24, align 4, !llvm.mem.parallel_loop_access !1
  %26 = fmul fast float %25, %norm
  store float %26, float* %24, align 4, !llvm.mem.parallel_loop_access !1
  %x_increment19 = add nuw nsw i64 %x18, 1
  %x_postcondition20 = icmp eq i64 %x_increment19, %3
  br i1 %x_postcondition20, label %x_exit17, label %x_body16

x_exit17:                                         ; preds = %x_body16
  %27 = call %u0CXYT* @likely_new(i32 24864, i32 1, i32 %columns, i32 %rows, i32 1, i8* null)
  %28 = getelementptr inbounds %u0CXYT, %u0CXYT* %27, i64 1
  %29 = bitcast %u0CXYT* %28 to float*
  %30 = ptrtoint %u0CXYT* %28 to i64
  %31 = and i64 %30, 31
  %32 = icmp eq i64 %31, 0
  call void @llvm.assume(i1 %32)
  br label %y_body32

y_body32:                                         ; preds = %x_exit36, %x_exit17
  %y34 = phi i64 [ 0, %x_exit17 ], [ %y_increment40, %x_exit36 ]
  %33 = mul nuw nsw i64 %y34, %3
  br label %x_body35

x_body35:                                         ; preds = %y_body32, %x_body35
  %x37 = phi i64 [ %x_increment38, %x_body35 ], [ 0, %y_body32 ]
  %34 = add nuw nsw i64 %x37, %33
  %35 = getelementptr %f32XY, %f32XY* %0, i64 0, i32 6, i64 %34
  %36 = load float, float* %35, align 4, !llvm.mem.parallel_loop_access !2
  %37 = getelementptr float, float* %5, i64 %x37
  %38 = load float, float* %37, align 4, !llvm.mem.parallel_loop_access !2
  %39 = fsub fast float %36, %38
  %40 = getelementptr float, float* %29, i64 %34
  store float %39, float* %40, align 4, !llvm.mem.parallel_loop_access !2
  %x_increment38 = add nuw nsw i64 %x37, 1
  %x_postcondition39 = icmp eq i64 %x_increment38, %3
  br i1 %x_postcondition39, label %x_exit36, label %x_body35

x_exit36:                                         ; preds = %x_body35
  %y_increment40 = add nuw nsw i64 %y34, 1
  %y_postcondition41 = icmp eq i64 %y_increment40, %11
  br i1 %y_postcondition41, label %y_exit33, label %y_body32

y_exit33:                                         ; preds = %x_exit36
  %41 = call %u0CXYT* @likely_new(i32 24864, i32 1, i32 %columns, i32 %columns, i32 1, i8* null)
  %42 = getelementptr inbounds %u0CXYT, %u0CXYT* %41, i64 1
  %43 = bitcast %u0CXYT* %42 to float*
  %44 = ptrtoint %u0CXYT* %42 to i64
  %45 = and i64 %44, 31
  %46 = icmp eq i64 %45, 0
  call void @llvm.assume(i1 %46)
  br label %y_body53

y_body53:                                         ; preds = %x_exit57, %y_exit33
  %y55 = phi i64 [ 0, %y_exit33 ], [ %y_increment63, %x_exit57 ]
  %47 = mul nuw nsw i64 %y55, %3
  br label %x_body56

x_body56:                                         ; preds = %y_body53, %Flow
  %x58 = phi i64 [ %x_increment61, %Flow ], [ 0, %y_body53 ]
  %48 = icmp ugt i64 %y55, %x58
  br i1 %48, label %Flow, label %true_entry59

x_exit57:                                         ; preds = %Flow
  %y_increment63 = add nuw nsw i64 %y55, 1
  %y_postcondition64 = icmp eq i64 %y_increment63, %3
  br i1 %y_postcondition64, label %y_exit54, label %y_body53

y_exit54:                                         ; preds = %x_exit57
  %dst = bitcast %u0CXYT* %41 to %f32XY*
  %49 = bitcast %u0CXYT* %2 to i8*
  call void @likely_release_mat(i8* %49)
  %50 = bitcast %u0CXYT* %27 to i8*
  call void @likely_release_mat(i8* %50)
  ret %f32XY* %dst

true_entry59:                                     ; preds = %x_body56, %true_entry59
  %51 = phi i32 [ %65, %true_entry59 ], [ 0, %x_body56 ]
  %52 = phi double [ %64, %true_entry59 ], [ 0.000000e+00, %x_body56 ]
  %53 = sext i32 %51 to i64
  %54 = mul nuw nsw i64 %53, %3
  %55 = add nuw nsw i64 %54, %x58
  %56 = getelementptr float, float* %29, i64 %55
  %57 = load float, float* %56, align 4, !llvm.mem.parallel_loop_access !3
  %58 = fpext float %57 to double
  %59 = add nuw nsw i64 %54, %y55
  %60 = getelementptr float, float* %29, i64 %59
  %61 = load float, float* %60, align 4, !llvm.mem.parallel_loop_access !3
  %62 = fpext float %61 to double
  %63 = fmul fast double %62, %58
  %64 = fadd fast double %63, %52
  %65 = add nuw nsw i32 %51, 1
  %66 = icmp eq i32 %65, %rows
  br i1 %66, label %exit60, label %true_entry59

Flow:                                             ; preds = %x_body56, %exit60
  %x_increment61 = add nuw nsw i64 %x58, 1
  %x_postcondition62 = icmp eq i64 %x_increment61, %3
  br i1 %x_postcondition62, label %x_exit57, label %x_body56

exit60:                                           ; preds = %true_entry59
  %67 = add nuw nsw i64 %x58, %47
  %68 = getelementptr float, float* %43, i64 %67
  %69 = fptrunc double %64 to float
  store float %69, float* %68, align 4, !llvm.mem.parallel_loop_access !3
  %70 = mul nuw nsw i64 %x58, %3
  %71 = add nuw nsw i64 %70, %y55
  %72 = getelementptr float, float* %43, i64 %71
  store float %69, float* %72, align 4, !llvm.mem.parallel_loop_access !3
  br label %Flow
}

; Function Attrs: nounwind
declare void @llvm.memset.p0i8.i64(i8* nocapture, i8, i64, i32, i1) #1

declare void @likely_release_mat(i8* noalias nocapture)

attributes #0 = { nounwind readonly }
attributes #1 = { nounwind }

!0 = !{i32 1, i32 -1}
!1 = distinct !{!1}
!2 = distinct !{!2}
!3 = distinct !{!3}
