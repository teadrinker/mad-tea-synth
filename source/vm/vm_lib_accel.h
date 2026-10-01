// vm_lib_accel.h - the interpreter's C versions of the geometry built-ins.
// Declared here because vm_register_lib_defaults names them long before
// vm_lib_accel.c is included at the end of vm.c. See vm_lib_accel.c.
#ifndef VM_LIB_ACCEL_H
#define VM_LIB_ACCEL_H

#if !defined(VM_NO_MATH) && !defined(VM_NO_ARRAYS) && VM_HAS_F32 && VM_HAS_F64
#define VM_LIB_ACCEL_ON 1
#else
#define VM_LIB_ACCEL_ON 0
#endif

#if VM_LIB_ACCEL_ON
static int acc_dot(struct Func *sp, unsigned char *fr);
static int acc_length(struct Func *sp, unsigned char *fr);
static int acc_distance(struct Func *sp, unsigned char *fr);
static int acc_normalize(struct Func *sp, unsigned char *fr);
static int acc_reflect(struct Func *sp, unsigned char *fr);
static int acc_refract(struct Func *sp, unsigned char *fr);
static int acc_cross(struct Func *sp, unsigned char *fr);
static int acc_faceforward(struct Func *sp, unsigned char *fr);
#define VM_LIB_ACCEL(fn) fn
#else
#define VM_LIB_ACCEL(fn) 0
#endif

#endif
