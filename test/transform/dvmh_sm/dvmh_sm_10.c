double A[100][100][100];

void foo() {  
  for (int I = 99; I >= 3; --I)
    for (int J = 97; J >= 0; --J)
      for (int K = 2; K < 100; ++K)
        A[I][J][K] = A[I + 3][J + 2][K - 2] + A[I][J][K - 1];
  for (int I = 3; I < 100; ++I)
    for (int J = 2; J < 100; ++J)
      for (int K = 97; K >= 0; --K)
        A[I][J][K] = A[I - 3][J - 2][K + 2] + A[I - 2][J - 2][K + 2];
}
//CHECK: dvmh_sm_10.c:8:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 3; I < 100; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_10.c:9:5: remark: parallel execution of loop is possible
//CHECK:     for (int J = 2; J < 100; ++J)
//CHECK:     ^
//CHECK: dvmh_sm_10.c:10:7: remark: parallel execution of loop is possible
//CHECK:       for (int K = 97; K >= 0; --K)
//CHECK:       ^
//CHECK: dvmh_sm_10.c:4:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 99; I >= 3; --I)
//CHECK:   ^
//CHECK: dvmh_sm_10.c:5:5: remark: parallel execution of loop is possible
//CHECK:     for (int J = 97; J >= 0; --J)
//CHECK:     ^
//CHECK: dvmh_sm_10.c:6:7: remark: parallel execution of loop is possible
//CHECK:       for (int K = 2; K < 100; ++K)
//CHECK:       ^
