#!/usr/bin/env python3
"""Compare firmware math wrappers with host libm on selected edge cases."""
import ctypes
import math
import pathlib
import subprocess

project=pathlib.Path(__file__).resolve().parents[1]
source=project.parents[1]/'work/chromium-build/v8/third_party/llvm-libc/src'
build=project/'build'
subprocess.run(['clang','-O2','-fPIC','-c',str(project/'src/math_hyperbolic.c'),'-o',str(build/'math-hyperbolic-host.o')],check=True)
subprocess.run(['clang++','-std=c++20','-O2','-fPIC','-shared','-Wl,-Bsymbolic','-DLIBC_NAMESPACE=__llvm_libc_uefi','-I'+str(source),str(project/'src/math_exact.cpp'),str(build/'math-hyperbolic-host.o'),'-o',str(build/'math-host.so')],check=True)
firmware=ctypes.CDLL(str(build/'math-host.so'))
host=ctypes.CDLL('libm.so.6')
values=[-math.inf,-710,-100,-20,-2,-1,-0.5,-1e-10,-0.0,0.0,1e-10,0.5,1,2,20,100,710,math.inf,math.nan]
count=0
for name in ['sqrt','sin','cos','tan','exp','log','log2','log1p','expm1','cbrt','sinh','cosh','tanh','asinh','acosh','atanh']:
    actual_function=getattr(firmware,name)
    expected_function=getattr(host,name)
    for function in [actual_function,expected_function]:
        function.argtypes=[ctypes.c_double]
        function.restype=ctypes.c_double
    for value in values:
        actual=actual_function(value)
        expected=expected_function(value)
        if math.isnan(expected):
            assert math.isnan(actual),(name,value,actual,expected)
        elif math.isinf(expected):
            assert actual==expected,(name,value,actual,expected)
        else:
            assert math.isfinite(actual) and abs(actual-expected)<=max(12*math.ulp(expected),1e-15),(name,value,actual,expected)
            if expected==0:
                assert math.copysign(1,actual)==math.copysign(1,expected),(name,value,'zero sign')
        count+=1
print(f'PASS: {count} selected math comparisons, including signed zero, NaN, infinity and overflow boundaries')
