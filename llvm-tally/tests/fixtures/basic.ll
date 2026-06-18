; LLVM fixture for the tally-instrument FunctionPass.

declare void @external()

define i32 @foo(i32 %x) {
entry:
  %is_positive = icmp sgt i32 %x, 0
  br i1 %is_positive, label %then, label %else

then:
  %a = add i32 %x, 1
  br label %merge

else:
  %b = sub i32 0, %x
  br label %merge

merge:
  %p = phi i32 [ %a, %then ], [ %b, %else ]
  ret i32 %p
}

define void @__tally_internal() {
entry:
  call void @external()
  ret void
}
