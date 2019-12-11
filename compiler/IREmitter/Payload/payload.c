#ifndef GRANITA_H
#define GRANITA_H

// for explanation of WTF is happening here, see ruby.h and
// https://silverhammermba.github.io/emberb/c/ and
// http://clalance.blogspot.com/2011/01/writing-ruby-extensions-in-c-part-9.html

// These are public Ruby headers. Feel free to add more from the include/ruby
// directory
#include "include/ruby/encoding.h" // for rb_encoding

// These are special "public" headers which don't live in include/ruby for some
// reason
#include "internal.h"
#include "ruby.h"

// This is probably a bad idea but is needed for so many things
#include "vm_core.h"

// Paul's and Dmitry's laptops have different attributes for this function in system libraries.
void abort(void) __attribute__((__cold__)) __attribute__((__noreturn__));

// ****
// ****                       Internal Helper Functions
// ****

const char *dbg_pi(ID id) __attribute__((weak)) {
    return rb_id2name(id);
}

const char *dbg_p(VALUE obj) __attribute__((weak)) {
    char *ret = RSTRING_PTR(rb_sprintf("%" PRIsVALUE, obj));
    return ret;
}

void stopInDebugger() {
    __asm__("int $3");
}

// ****
// ****                       Singletons
// ****

VALUE sorbet_rubyTrue() __attribute__((always_inline)) {
    return RUBY_Qtrue;
}

VALUE sorbet_rubyFalse() __attribute__((always_inline)) {
    return RUBY_Qfalse;
}

VALUE sorbet_rubyNil() __attribute__((always_inline)) {
    return RUBY_Qnil;
}

// ****
// ****                       Conversions between Ruby values and C values
// ****

long sorbet_rubyValueToLong(VALUE val) __attribute__((always_inline)) {
    return FIX2LONG(val);
}

VALUE sorbet_longToRubyValue(long i) __attribute__((always_inline)) {
    return LONG2FIX(i);
}

double sorbet_rubyValueToDouble(VALUE val) __attribute__((always_inline)) {
    return RFLOAT_VALUE(val);
}

VALUE sorbet_doubleToRubyValue(double u) __attribute__((always_inline)) {
    return DBL2NUM(u);
}

// ****
// ****                       Integer
// ****

VALUE sorbet_Integer_plus_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return sorbet_longToRubyValue(sorbet_rubyValueToLong(a) + sorbet_rubyValueToLong(b));
}

VALUE sorbet_Integer_minus_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return sorbet_longToRubyValue(sorbet_rubyValueToLong(a) - sorbet_rubyValueToLong(b));
}

VALUE sorbet_Integer_less_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return (sorbet_rubyValueToLong(a) < sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_greater_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return (sorbet_rubyValueToLong(a) > sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_greatereq_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return (sorbet_rubyValueToLong(a) >= sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_lesseq_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return (sorbet_rubyValueToLong(a) <= sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_eq_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return (sorbet_rubyValueToLong(a) == sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_neq_Integer(VALUE a, VALUE b) __attribute__((always_inline)) {
    return (sorbet_rubyValueToLong(a) != sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

// ****
// ****                       Operations on Strings
// ****

const char *sorbet_rubyStringToCPtr(VALUE value) __attribute__((always_inline)) {
    return RSTRING_PTR(value);
}

long sorbet_rubyStringLength(VALUE value) __attribute__((always_inline)) {
    return RSTRING_LEN(value);
}

VALUE sorbet_cPtrToRubyString(const char *ptr, long length) __attribute__((always_inline)) {
    return rb_str_new(ptr, length);
}

VALUE sorbet_stringPlus(VALUE str1, VALUE str2) __attribute__((always_inline)) {
    return rb_str_plus(str1, str2);
}

// ****
// ****                       Operations on Arrays
// ****

int sorbet_rubyArrayLen(VALUE array) __attribute__((always_inline)) {
    return RARRAY_LEN(array);
}

const VALUE *sorbet_rubyArrayInnerPtr(VALUE array) __attribute__((always_inline)) {
    // there's also a transient version of this function if we ever decide to want more speed. transient stands for that
    // we _should not_ allow to execute any code between getting these pointers and reading elements from
    return RARRAY_CONST_PTR(array);
}

VALUE sorbet_newRubyArray(long size) __attribute__((always_inline)) {
    return rb_ary_new2(size);
}

VALUE sorbet_newRubyArrayWithElems(long size, const VALUE *elems) __attribute__((always_inline)) {
    return rb_ary_new4(size, elems);
}

void sorbet_arrayPush(VALUE array, VALUE element) __attribute__((always_inline)) {
    rb_ary_push(array, element);
}

// ****
// ****                       Operations on Hashes
// ****

VALUE sorbet_newRubyHash() __attribute__((always_inline)) {
    return rb_hash_new();
}

void sorbet_hashStore(VALUE hash, VALUE key, VALUE value) __attribute__((always_inline)) {
    rb_hash_aset(hash, key, value);
}

VALUE sorbet_hashGet(VALUE hash, VALUE key) __attribute__((always_inline)) {
    return rb_hash_aref(hash, key);
}

// possible return values for `func`:
//  - ST_CONTINUE, then the rest of the hash is processed as normal.
//  - ST_STOP, then no further processing of the hash is done.
//  - ST_DELETE, then the current hash key is deleted from the hash and the rest
//  of the hash is processed
//  - ST_CHECK, then the hash is checked to see if it has been modified during
//  this operation. If so, processing of the hash stops.
/*
void sorbet_hashEach(VALUE hash, int(*func)(VALUE key, VALUE val,
VALUE in), VALUE closure) { return rb_hash_foreach(hash, func, closure);
}
*/

// ****
// ****                       Operations on Ruby ID's
// ****

ID sorbet_idIntern(const char *value, long length) __attribute__((always_inline)) {
    return rb_intern2(value, length);
}

ID sorbet_symToID(VALUE sym) __attribute__((always_inline)) {
    return SYM2ID(sym);
}

VALUE sorbet_IDToSym(ID id) __attribute__((always_inline)) {
    return ID2SYM(id);
}

VALUE sorbet_getRubyClassOf(VALUE value) __attribute__((always_inline)) {
    return CLASS_OF(value);
}

const char *sorbet_getRubyClassName(VALUE object) __attribute__((always_inline)) {
    return rb_obj_classname(object);
}
// ****
// ****                       Tests
// ****

_Bool sorbet_testIsTruthy(VALUE value) __attribute__((always_inline)) {
    return RB_TEST(value);
}

_Bool sorbet_testIsTrue(VALUE value) __attribute__((always_inline)) {
    return value == RUBY_Qtrue;
}

_Bool sorbet_testIsFalse(VALUE value) __attribute__((always_inline)) {
    return value == RUBY_Qfalse;
}

_Bool sorbet_testIsNil(VALUE value) __attribute__((always_inline)) {
    return value == RUBY_Qnil;
}

_Bool sorbet_testIsUndef(VALUE value) __attribute__((always_inline)) {
    return value == RUBY_Qundef;
}

_Bool sorbet_testIsSymbol(VALUE value) __attribute__((always_inline)) {
    return RB_SYMBOL_P(value);
}

_Bool sorbet_testIsFloat(VALUE value) __attribute__((always_inline)) {
    return RB_FLOAT_TYPE_P(value);
}

_Bool sorbet_testIsHash(VALUE value) __attribute__((always_inline)) {
    return TYPE(value) == RUBY_T_HASH;
}

_Bool sorbet_testIsArray(VALUE value) __attribute__((always_inline)) {
    return TYPE(value) == RUBY_T_ARRAY;
}

_Bool sorbet_testIsString(VALUE value) __attribute__((always_inline)) {
    return TYPE(value) == RUBY_T_STRING;
}

// ****
// ****                       Variables
// ****

VALUE sorbet_instanceVariableGet(VALUE receiver, ID name) __attribute__((always_inline)) {
    return rb_ivar_get(receiver, name);
}

VALUE sorbet_instanceVariableSet(VALUE receiver, ID name, VALUE newValue) __attribute__((always_inline)) {
    return rb_ivar_set(receiver, name, newValue);
}

VALUE sorbet_globalVariableGet(const char *name) __attribute__((always_inline)) {
    return rb_gv_get(name);
}

void sorbet_globalVariableSet(const char *name, VALUE newValue) __attribute__((always_inline)) {
    rb_gv_set(name, newValue);
}

VALUE sorbet_classVariableGet(VALUE _class, ID name) __attribute__((always_inline)) {
    return rb_cvar_get(_class, name);
}

void sorbet_classVariableSet(VALUE _class, ID name, VALUE newValue) __attribute__((always_inline)) {
    rb_cvar_set(_class, name, newValue);
}

// ****
// ****                       Constants, Classes and Modules
// ****

VALUE sorbet_rb_cObject() {
    return rb_cObject;
}

void sorbet_defineTopLevelConstant(const char *name, VALUE value) __attribute__((always_inline)) {
    rb_define_global_const(name, value);
}

void sorbet_defineNestedCosntant(VALUE owner, const char *name, VALUE value) __attribute__((always_inline)) {
    rb_define_const(owner, name, value);
}

RUBY_EXTERN rb_serial_t ruby_vm_global_constant_state;

long sorbet_getConstantEpoch() {
    return ruby_vm_global_constant_state;
}

VALUE sorbet_getMethodBlockAsProc() {
    if (rb_block_given_p()) {
        return rb_block_proc();
    }
    return Qnil;
}

// Trying to be a copy of rb_mod_const_get
VALUE sorbet_getConstantAt(VALUE mod, ID id) __attribute__((noinline)) {
    VALUE name;
    rb_encoding *enc;
    const char *pbeg, *p, *path, *pend;
    int recur = 1;
    int DISABLED_CODE = 0;

    name = ID2SYM(id);
    enc = rb_enc_get(name);
    path = rb_id2name(id);

    pbeg = p = path;
    pend = path + strlen(path);

    if (DISABLED_CODE && (p >= pend || !*p)) {
    wrong_name:
        rb_raise(rb_eRuntimeError, "wrong constant name %" PRIsVALUE "%" PRIsVALUE, mod, name);
    }

    if (DISABLED_CODE && (p + 2 < pend && p[0] == ':' && p[1] == ':')) {
        mod = rb_cObject;
        p += 2;
        pbeg = p;
    }

    while (p < pend) {
        VALUE part;
        long len, beglen;

        while (p < pend && *p != ':')
            p++;

        if (pbeg == p)
            goto wrong_name;

        id = rb_check_id_cstr(pbeg, len = p - pbeg, enc);
        beglen = pbeg - path;

        if (p < pend && p[0] == ':') {
            if (p + 2 >= pend || p[1] != ':')
                goto wrong_name;
            p += 2;
            pbeg = p;
        }

        if (!RB_TYPE_P(mod, T_MODULE) && !RB_TYPE_P(mod, T_CLASS)) {
            rb_raise(rb_eTypeError, "%" PRIsVALUE " does not refer to class/module", name);
        }

        if (!id) {
            part = rb_str_subseq(name, beglen, len);
            OBJ_FREEZE(part);
            VALUE idConst_missing = rb_intern("const_missing");
            if (!rb_is_const_name(part)) {
                name = part;
                goto wrong_name;
            } else if (!rb_method_basic_definition_p(CLASS_OF(mod), idConst_missing)) {
                part = rb_str_intern(part);
                mod = rb_const_missing(mod, part);
                continue;
            } else {
                rb_mod_const_missing(mod, part);
            }
        }
        if (!rb_is_const_id(id)) {
            name = ID2SYM(id);
            goto wrong_name;
        }
        if (!recur) {
            mod = rb_const_get_at(mod, id);
        } else if (beglen == 0) {
            mod = rb_const_get(mod, id);
        } else {
            mod = rb_const_get_from(mod, id);
        }
    }

    return mod;
}
// End copy of rb_mod_const_get

VALUE sorbet_getConstant(const char *path, long pathLen) __attribute__((noinline)) {
    VALUE mod = sorbet_rb_cObject();
    ID id = rb_intern2(path, pathLen);
    return sorbet_getConstantAt(mod, id);
}

void sorbet_setConstant(VALUE mod, const char *name, long nameLen, VALUE value) __attribute__((noinline)) {
    ID id = rb_intern2(name, nameLen);
    return rb_const_set(mod, id, value);
}

VALUE sorbet_defineTopLevelModule(const char *name) __attribute__((always_inline)) {
    return rb_define_module(name);
}

VALUE sorbet_defineNestedModule(VALUE owner, const char *name) __attribute__((always_inline)) {
    return rb_define_module_under(owner, name);
}

VALUE sorbet_defineTopClassOrModule(const char *name, VALUE super) __attribute__((always_inline)) {
    return rb_define_class(name, super);
}

VALUE sorbet_defineNestedClass(VALUE owner, const char *name, VALUE super) __attribute__((always_inline)) {
    return rb_define_class_under(owner, name, super);
}

// this DOES override existing methods
void sorbet_defineMethod(VALUE klass, const char *name, VALUE (*methodPtr)(ANYARGS), int argc)
    __attribute__((always_inline)) {
    rb_define_method(klass, name, methodPtr, argc);
}

// this DOES override existing methods
void sorbet_defineMethodSingleton(VALUE klass, const char *name, VALUE (*methodPtr)(ANYARGS), int argc)
    __attribute__((always_inline)) {
    rb_define_singleton_method(klass, name, methodPtr, argc);
}

// ****
// ****                       Calls
// ****

VALUE sorbet_callSuper(int argc, const VALUE *argv) __attribute__((always_inline)) {
    return rb_call_super(argc, argv);
}

VALUE sorbet_callBlock(VALUE array) __attribute__((always_inline)) {
    // TODO: one day we should use rb_yield_values, as it saves an allocation, but
    // for now, do the easy thing
    return rb_yield_splat(array);
}

VALUE sorbet_callFunc(VALUE recv, ID func, int argc, __attribute__((noescape)) const VALUE *const restrict argv)
    __attribute__((always_inline)) {
    return rb_funcallv(recv, func, argc, argv);
}

VALUE sorbet_callFuncProc(VALUE recv, ID func, int argc, __attribute__((noescape)) const VALUE *const restrict argv,
                          VALUE proc) __attribute__((always_inline)) {
    return rb_funcall_with_block(recv, func, argc, argv, proc);
}

VALUE sorbet_callFuncBlock(VALUE recv, ID func, int argc, __attribute__((noescape)) const VALUE *const restrict argv,
                           VALUE (*blockImpl)(VALUE, VALUE, int, VALUE *, VALUE), VALUE closure)
    __attribute__((always_inline)) {
    return rb_block_call(recv, func, argc, argv, blockImpl, closure);
}

// defining a way to allocate storage for custom class:
//      VALUE allocate(VALUE klass);
//      rb_define_alloc_func(class, &allocate)
//

VALUE sorbet_rb_arity_error_new(int argc, int min, int max) {
    VALUE err_mess = 0;
    if (min == max) {
        err_mess = rb_sprintf("wrong number of arguments (given %d, expected %d)", argc, min);
    } else if (max == UNLIMITED_ARGUMENTS) {
        err_mess = rb_sprintf("wrong number of arguments (given %d, expected %d+)", argc, min);
    } else {
        err_mess = rb_sprintf("wrong number of arguments (given %d, expected %d..%d)", argc, min, max);
    }
    return rb_exc_new3(rb_eArgError, err_mess);
}

void sorbet_cast_failure(VALUE value, char *castMethod, char *type) __attribute__((__cold__))
__attribute__((__noreturn__)) {
    // TODO: cargo cult more of
    // https://github.com/sorbet/sorbet/blob/b045fb1ba12756c3760fe516dc315580d93f3621/gems/sorbet-runtime/lib/types/types/base.rb#L105
    //
    // e.g. we need to teach the `got` part to do `T.class_of`
    rb_raise(rb_eTypeError, "%s: Expected type %s, got %s with value %" PRIsVALUE, castMethod, type,
             rb_obj_classname(value), value);
}

void sorbet_raiseArity(int argc, int min, int max) __attribute__((__noreturn__)) {
    rb_exc_raise(sorbet_rb_arity_error_new(argc, min, max));
}

void sorbet_checkStack() {
    // This is actually pretty slow. We should probably use guard pages instead.
    ruby_stack_check();
}

// ****
// **** Optimized versions of callFunc.
// **** Should use the same calling concention.
// **** Call it ending with `_no_type_guard` if implementation has a backed in slowpath
// ****
// ****

// ****
// ****                       Closures
// ****

// this specifies to use ruby default free for freeing(which is just xfree). Thus objects should be allocated with
// xmalloc

struct sorbet_Closure {
    const int size;
    VALUE closureData[]; // this is a rarely known feature of C99 https://en.wikipedia.org/wiki/Flexible_array_member
};

struct sorbet_Closure *sorbet_Closure_alloc(int elemCount) {
    struct sorbet_Closure *ret =
        (struct sorbet_Closure *)xmalloc(sizeof(struct sorbet_Closure) + sizeof(VALUE) * elemCount);
    *(int *)&ret->size = elemCount; // this is how you assign a const field after malloc
    return ret;
}

void sorbet_Closure_mark(void *closurePtr) {
    // this might be possible to make more efficient using rb_mark_tbl
    struct sorbet_Closure *ptr = (struct sorbet_Closure *)closurePtr;
    rb_gc_mark_values(ptr->size, &ptr->closureData[0]);
}

size_t sorbet_Closure_size(const void *closurePtr) __attribute__((const)) {
    // this might be possible to make more efficient using rb_mark_tbl
    struct sorbet_Closure *ptr = (struct sorbet_Closure *)closurePtr;
    return sizeof(struct sorbet_Closure) + ptr->size * sizeof(VALUE);
}

const rb_data_type_t closureInfo = {
    "CompiledClosure", // this shouldn't ever be visible to users
    {
        /* mark = */ sorbet_Closure_mark,
        /* free = */ RUBY_DEFAULT_FREE,  // this uses xfree and optimzies it
        /* size = */ sorbet_Closure_size /*, compact */
    },
    /* parent = */ NULL,
    /* arbitrary data = */ NULL,
    /* flags = */ RUBY_TYPED_FREE_IMMEDIATELY /* deferred free */,
};

VALUE sorbet_allocClosureAsValue(int elemCount) {
    struct sorbet_Closure *ptr = sorbet_Closure_alloc(elemCount);
    return TypedData_Wrap_Struct(rb_cData, &closureInfo, ptr);
}

VALUE *sorbet_getClosureElem(VALUE closure, int elemId) {
    struct sorbet_Closure *ptr = (struct sorbet_Closure *)RTYPEDDATA_DATA(closure);
    return &(ptr->closureData[elemId]);
}

// ****
// ****                       Control Frames
// ****

/* From inseq.h */
struct rb_compile_option_struct {
    unsigned int inline_const_cache : 1;
    unsigned int peephole_optimization : 1;
    unsigned int tailcall_optimization : 1;
    unsigned int specialized_instruction : 1;
    unsigned int operands_unification : 1;
    unsigned int instructions_unification : 1;
    unsigned int stack_caching : 1;
    unsigned int frozen_string_literal : 1;
    unsigned int debug_frozen_string_literal : 1;
    unsigned int coverage_enabled : 1;
    int debug_level;
};

struct iseq_insn_info_entry {
    int line_no;
    rb_event_flag_t events;
};

void rb_iseq_insns_info_encode_positions(const rb_iseq_t *iseq);
/* End from inseq.h */

unsigned char *sorbet_allocateRubyStackFrames(VALUE recv, VALUE funcName, ID func, VALUE filename, VALUE realpath,
                                              int startline, int endline) {
    // DO NOT ALLOCATE RUBY LEVEL OBJECTS HERE. All objects that are passed to
    // this function should be retained (for GC purposes) by something else.

    rb_iseq_t *iseq = rb_iseq_new(0, funcName, filename, realpath, 0, ISEQ_TYPE_TOP);

    // Even if start and end are on the same line, we still want one insns_info made
    int insn_num = endline - startline + 1;
    struct iseq_insn_info_entry *insns_info = ALLOC_N(struct iseq_insn_info_entry, insn_num);
    unsigned int *positions = ALLOC_N(unsigned int, insn_num);
    for (int i = 0; i < insn_num; i++) {
        int lineno = i + startline;
        positions[i] = i;
        insns_info[i].line_no = lineno;
    }
    iseq->body->insns_info.body = insns_info;
    iseq->body->insns_info.positions = positions;
    // One iseq per line
    iseq->body->iseq_size = insn_num;
    iseq->body->insns_info.size = insn_num;
    rb_iseq_insns_info_encode_positions(iseq);
    // Just an offset that has to be consistent with what we use in sorbet_setLineNumber
    iseq->body->iseq_encoded = 0x0;

    // Cast it to something easy since teaching LLVM about structs is a huge PITA
    return (unsigned char *)iseq;
}

const VALUE **sorbet_setRubyStackFrame(unsigned char *iseqchar) {
    const rb_iseq_t *iseq = (const rb_iseq_t *)iseqchar;
    rb_control_frame_t *cfp = GET_EC()->cfp;
    cfp->iseq = iseq;
    VM_ENV_FLAGS_UNSET(cfp->ep, VM_FRAME_FLAG_CFRAME);
    return &cfp->pc;
}

void sorbet_setLineNumber(int offset) {
    // TODO(perf): Pass in the return of sorbet_setRubyStackFrame instead of looking up
    // the pc location every time

    // use pos+1 because PC should point at the next instruction
    GET_EC()->cfp->pc = ((VALUE *)0x0) + offset + 1;
}

// ****
// ****                       Implementation helpers for type tests
// ****

_Bool sorbet_isa_Integer(VALUE obj) __attribute__((const)) {
    return RB_FIXNUM_P(obj);
}

_Bool sorbet_isa_TrueClass(VALUE obj) __attribute__((const)) {
    return obj == RUBY_Qtrue;
}

_Bool sorbet_isa_FalseClass(VALUE obj) __attribute__((const)) {
    return obj == RUBY_Qfalse;
}

_Bool sorbet_isa_NilClass(VALUE obj) __attribute__((const)) {
    return obj == RUBY_Qnil;
}

_Bool sorbet_isa_Symbol(VALUE obj) __attribute__((const)) {
    return RB_SYMBOL_P(obj);
}

_Bool sorbet_isa_Float(VALUE obj) __attribute__((const)) {
    return RB_FLOAT_TYPE_P(obj);
}

_Bool sorbet_isa_Untyped(VALUE obj) __attribute__((const)) {
    return 1;
}

_Bool sorbet_isa_Hash(VALUE obj) __attribute__((const)) {
    return RB_TYPE_P(obj, T_HASH);
}

_Bool sorbet_isa_Array(VALUE obj) __attribute__((const)) {
    return RB_TYPE_P(obj, T_ARRAY);
}

_Bool sorbet_isa_Regexp(VALUE obj) __attribute__((const)) {
    return RB_TYPE_P(obj, T_REGEXP);
}

_Bool sorbet_isa_Rational(VALUE obj) __attribute__((const)) {
    return RB_TYPE_P(obj, T_RATIONAL);
}

_Bool sorbet_isa_String(VALUE obj) __attribute__((const)) {
    return RB_TYPE_P(obj, T_STRING);
}

/*
_Bool sorbet_isa_Method(VALUE obj) __attribute__((const))  {
    return rb_obj_is_method(obj) == Qtrue;
}
*/

_Bool sorbet_isa_Proc(VALUE obj) __attribute__((const)) {
    return rb_obj_is_proc(obj) == Qtrue;
}

VALUE rb_obj_is_kind_of(VALUE, VALUE) __attribute__((const));
VALUE rb_class_inherited_p(VALUE, VALUE) __attribute__((const));

_Bool sorbet_isa(VALUE obj, VALUE class) __attribute__((const)) {
    return rb_obj_is_kind_of(obj, class) == Qtrue;
}

_Bool sorbet_isa_class_of(VALUE obj, VALUE class) __attribute__((const)) {
    return (obj == class) || (rb_obj_is_kind_of(obj, rb_cModule) && rb_class_inherited_p(obj, class));
}

// ****
// ****                       Helpers for Intrinsics
// ****

void sorbet_ensure_arity(int argc, int expected) {
    if (argc != expected) {
        sorbet_raiseArity(argc, expected, expected);
    }
}

VALUE sorbet_boolToRuby(_Bool b) {
    if (b) {
        return RUBY_Qtrue;
    }
    return RUBY_Qfalse;
}

// ****
// ****                       Name Based Intrinsics
// ****

VALUE sorbet_buildHashIntrinsic(VALUE recv, int argc, const VALUE *const restrict argv) {
    // this comes from internal.h
    void rb_hash_bulk_insert(long, const VALUE *, VALUE);

    VALUE ret = rb_hash_new();
    if (argc != 0) {
        rb_hash_bulk_insert(argc, argv, ret);
    }
    return ret;
}

VALUE sorbet_buildArrayIntrinsic(VALUE recv, int argc, const VALUE *const restrict argv) {
    if (argc == 0) {
        return rb_ary_new();
    }
    return rb_ary_new_from_values(argc, argv);
}

VALUE sorbet_splatIntrinsic(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 3);
    VALUE arr = argv[0];
    long len = sorbet_rubyArrayLen(arr);
    int size = sorbet_rubyValueToLong(argv[1]) + sorbet_rubyValueToLong(argv[2]);
    int missing = size - len;
    if (missing > 0) {
        VALUE newArr = rb_ary_dup(arr);
        for (int i = 0; i < missing; i++) {
            sorbet_arrayPush(newArr, sorbet_rubyNil());
        }
        return newArr;
    }
    return arr;
}

// This doesn't do exactly the right thing because that is done by the parser in Ruby. Ruby will return the String
// "expression" if the RHS is an expression.
VALUE sorbet_definedIntinsic(VALUE recv, int argc, const VALUE *const restrict argv) {
    if (argc == 0) {
        return sorbet_rubyNil();
    }
    VALUE klass = sorbet_rb_cObject();
    for (int i = 0; i < argc; i++) {
        VALUE str = argv[i];
        ID id = rb_intern(sorbet_rubyStringToCPtr(str));
        if (!rb_const_defined_at(klass, id)) {
            return sorbet_rubyNil();
        }
        klass = sorbet_getConstantAt(klass, id);
    }
    return rb_str_new2("constant");
}

// ****
// ****                       Symbol Intrinsics. See CallCMethod in SymbolIntrinsics.cc
// ****

// TODO: add many from https://github.com/ruby/ruby/blob/ruby_2_6/include/ruby/intern.h#L55
VALUE sorbet_T_unsafe(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    return argv[0];
}

VALUE sorbet_rb_array_len(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 0);
    return sorbet_longToRubyValue(rb_array_len(recv));
}

VALUE sorbet_rb_array_square_br(VALUE recv, int argc, const VALUE *const restrict argv) {
    VALUE ary = recv;
    rb_check_arity(argc, 1, 2);
    if (argc == 2) {
        return rb_ary_aref2(ary, argv[0], argv[1]);
    }
    VALUE arg = argv[0];

    long beg, len;

    /* special case - speeding up */
    if (FIXNUM_P(arg)) {
        return rb_ary_entry(ary, FIX2LONG(arg));
    }
    /* check if idx is Range */
    switch (rb_range_beg_len(arg, &beg, &len, RARRAY_LEN(ary), 0)) {
        case Qfalse:
            break;
        case Qnil:
            return Qnil;
        default:
            return rb_ary_subseq(ary, beg, len);
    }
    return rb_ary_entry(ary, NUM2LONG(arg));
}

void rb_ary_splice(VALUE ary, long beg, long len, const VALUE *rptr, long rlen);

VALUE sorbet_rb_array_square_br_eq(VALUE ary, int argc, const VALUE *const restrict argv) {
    long offset, beg, len;
    VALUE rpl;

    if (argc == 3) {
        rb_check_frozen(ary);
        beg = NUM2LONG(argv[0]);
        len = NUM2LONG(argv[1]);
        goto range;
    }
    rb_check_arity(argc, 2, 2);
    rb_check_frozen(ary);
    if (FIXNUM_P(argv[0])) {
        offset = FIX2LONG(argv[0]);
        goto fixnum;
    }
    if (rb_range_beg_len(argv[0], &beg, &len, RARRAY_LEN(ary), 1)) {
        /* check if idx is Range */
    range:
        rpl = rb_ary_to_ary(argv[argc - 1]);
        rb_ary_splice(ary, beg, len, RARRAY_CONST_PTR_TRANSIENT(rpl), RARRAY_LEN(rpl));
        RB_GC_GUARD(rpl);
        return argv[argc - 1];
    }

    offset = NUM2LONG(argv[0]);
fixnum:
    rb_ary_store(ary, offset, argv[1]);
    return argv[1];
}
VALUE sorbet_rb_hash_square_br(VALUE recv, int argc, const VALUE *const restrict argv) {
    rb_check_arity(argc, 1, 1);
    return rb_hash_aref(recv, argv[0]);
}

VALUE sorbet_rb_hash_square_br_eq(VALUE recv, int argc, const VALUE *const restrict argv) {
    rb_check_arity(argc, 2, 2);
    return rb_hash_aset(recv, argv[0], argv[1]);
}

VALUE sorbet_rb_int_plus(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    VALUE y = argv[0];
    if (LIKELY(FIXNUM_P(recv))) {
        if (LIKELY(FIXNUM_P(y))) {
            return rb_fix_plus_fix(recv, y);
        } else if (RB_TYPE_P(y, T_BIGNUM)) {
            return rb_big_plus(y, recv);
        } else if (RB_TYPE_P(recv, T_FLOAT)) {
            return DBL2NUM((double)FIX2LONG(recv) + RFLOAT_VALUE(y));
        } else if (RB_TYPE_P(y, T_COMPLEX)) {
            return rb_complex_plus(y, recv);
        }
        // fall through to coerce
    } else if (RB_TYPE_P(recv, T_BIGNUM)) {
        return rb_big_plus(recv, y);
    }
    return rb_num_coerce_bin(recv, y, '+');
}

VALUE sorbet_rb_int_minus(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    // optimized version from numeric.c
    VALUE y = argv[0];
    if (LIKELY(FIXNUM_P(recv))) {
        if (LIKELY(FIXNUM_P(y))) {
            return rb_fix_minus_fix(recv, y);
        } else if (RB_TYPE_P(y, T_BIGNUM)) {
            VALUE x = rb_int2big(FIX2LONG(recv));
            return rb_big_minus(x, y);
        } else if (RB_TYPE_P(y, T_FLOAT)) {
            return DBL2NUM((double)FIX2LONG(recv) - RFLOAT_VALUE(y));
        }
        // fall throught to coerece
    } else if (RB_TYPE_P(recv, T_BIGNUM)) {
        return rb_big_minus(recv, y);
    }
    return rb_num_coerce_bin(recv, y, '-');
}

VALUE sorbet_rb_int_mul(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    return rb_int_mul(recv, argv[0]);
}

VALUE sorbet_rb_int_div(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    return rb_int_div(recv, argv[0]);
}

VALUE sorbet_rb_int_gt(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    VALUE y = argv[0];
    if (LIKELY(FIXNUM_P(recv))) {
        if (LIKELY(FIXNUM_P(y))) {
            if (FIX2LONG(recv) > FIX2LONG(y)) {
                return Qtrue;
            }
            return Qfalse;
        } else if (RB_TYPE_P(y, T_BIGNUM)) {
            return rb_big_cmp(y, recv) == INT2FIX(-1) ? Qtrue : Qfalse;
        } else if (RB_TYPE_P(y, T_FLOAT)) {
            return rb_integer_float_cmp(recv, y) == INT2FIX(+1) ? Qtrue : Qfalse;
        }
    } else if (RB_TYPE_P(recv, T_BIGNUM)) {
        return rb_big_gt(recv, y);
    }
    return rb_num_coerce_relop(recv, y, '>');
}

VALUE sorbet_rb_int_lt(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    VALUE y = argv[0];
    if (LIKELY(FIXNUM_P(recv))) {
        if (LIKELY(FIXNUM_P(y))) {
            if (FIX2LONG(recv) < FIX2LONG(y)) {
                return Qtrue;
            }
            return Qfalse;
        } else if (RB_TYPE_P(y, T_BIGNUM)) {
            return rb_big_cmp(y, recv) == INT2FIX(+1) ? Qtrue : Qfalse;
        } else if (RB_TYPE_P(y, T_FLOAT)) {
            return rb_integer_float_cmp(recv, y) == INT2FIX(-1) ? Qtrue : Qfalse;
        }
    } else if (RB_TYPE_P(recv, T_BIGNUM)) {
        return rb_big_lt(recv, y);
    }
    return rb_num_coerce_relop(recv, y, '<');
}

VALUE sorbet_rb_int_ge(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    VALUE res = sorbet_rb_int_lt(recv, argc, argv);
    return res == Qtrue ? Qfalse : Qtrue;
}

VALUE sorbet_rb_int_le(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    VALUE res = sorbet_rb_int_gt(recv, argc, argv);
    return res == Qtrue ? Qfalse : Qtrue;
}

VALUE sorbet_rb_int_equal(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    return rb_int_equal(recv, argv[0]);
}

VALUE sorbet_rb_int_neq(VALUE recv, int argc, const VALUE *const restrict argv) {
    sorbet_ensure_arity(argc, 1);
    return sorbet_boolToRuby(rb_int_equal(recv, argv[0]) == sorbet_rubyFalse());
}
VALUE sorbet_rb_int_to_s(VALUE x, int argc, const VALUE *const restrict argv) {
    int base;

    rb_check_arity(argc, 0, 1);
    if (argc == 1) {
        base = NUM2INT(argv[0]);
    } else {
        base = 10;
    }
    if (LIKELY(FIXNUM_P(x))) {
        return rb_fix2str(x, base);
    }
    if (RB_TYPE_P(x, T_BIGNUM)) {
        return rb_big2str(x, base);
    }

    return rb_any_to_s(x);
}
// ****
// ****                       Compile-time only intrinsics. These should be eliminated by passes.
// ****

VALUE sorbet_i_getRubyClass(const char *const className, long classNameLen) __attribute__((const));
VALUE sorbet_i_getRubyConstant(const char *const className, long classNameLen) __attribute__((const));

VALUE __sorbet_only_exists_to_keep_functions_alive__() __attribute__((optnone)) {
    // this function will be nuked but it exists to keep forward definitions alive for clang
    return (long)&sorbet_i_getRubyClass + (long)&sorbet_i_getRubyConstant + (long)&sorbet_getConstantEpoch +
           (long)&sorbet_getConstant;
}
#endif
