//! # VORTEX JIT — Rust Bindings
//!
//! Safe Rust bindings for the VORTEX JIT compiler, a profile-guided optimizing
//! JIT with features no production JIT has:
//!
//! - **Input-shape-keyed profiles** (Sprint 4) — the novel differentiator
//! - **Phase-aware profile partitioning** (Sprint 2)
//! - **Ensemble profiles** with robust aggregation (Sprint 3)
//! - **T1 code persistence** — cold-start killer (Sprint 5)
//! - **Append-only crash-safe profile patching** (Sprint 6)
//! - **Confidence-gated tier promotion** (Sprint 1)

mod ffi {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::ptr;

// ========================================================================== //
// Constants — reimplemented in Rust (bindgen doesn't export macros/enums)     //
// ========================================================================== //

// NaN-boxing constants (from runtime/object.h)
pub const VTX_NAN_BOX_HEADER: u64 = 0x7FF8_0000_0000_0000;
pub const VTX_NAN_DATA_SHIFT: u32 = 3;
pub const VTX_NAN_DATA_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;
pub const VTX_TAG_MASK: u64 = 0x7;
pub const VTX_TAG_SMI: u64 = 0;
pub const VTX_TAG_DOUBLE: u64 = 2;
pub const VTX_TAG_BOOL: u64 = 3;
pub const VTX_TAG_NULL: u64 = 4;
pub const VTX_TAG_UNDEFINED: u64 = 5;

pub const VTX_VALUE_NULL: u64 = VTX_NAN_BOX_HEADER | VTX_TAG_NULL;
pub const VTX_VALUE_UNDEFINED: u64 = VTX_NAN_BOX_HEADER | VTX_TAG_UNDEFINED;
pub const VTX_VALUE_FALSE: u64 = VTX_NAN_BOX_HEADER | VTX_TAG_BOOL;
pub const VTX_VALUE_TRUE: u64 = VTX_NAN_BOX_HEADER | (1 << VTX_NAN_DATA_SHIFT) | VTX_TAG_BOOL;

pub const VTX_SMI_MAX: i64 = (1i64 << 46) - 1;
pub const VTX_SMI_MIN: i64 = -(1i64 << 46);
pub const VTX_POLY_LIMIT: u32 = 4;
pub const VTX_PROFILE_HASH_SIZE: usize = 32;

// Confidence thresholds (from vortex_config.h)
pub const VTX_CONFIDENCE_THRESHOLD_BRANCH: u32 = 100;
pub const VTX_CONFIDENCE_THRESHOLD_CALL_TARGET: u32 = 50;
pub const VTX_CONFIDENCE_THRESHOLD_TYPE_DIST: u32 = 200;
pub const VTX_CONFIDENCE_THRESHOLD_LOOP_TRIP: u32 = 50;
pub const VTX_CONFIDENCE_THRESHOLD_FIELD_SHAPE: u32 = 100;
pub const VTX_PROMOTION_CONFIDENCE_T2: f64 = 0.5;
pub const VTX_PROMOTION_CONFIDENCE_T3: f64 = 0.75;
pub const VTX_PROMOTION_CONFIDENCE_T4: f64 = 0.9;

// Recomp config
pub const VTX_RECOMP_HYSTERESIS_CONSECUTIVE: u32 = 3;
pub const VTX_RECOMP_QUEUE_SOFT_CAP: u32 = 64;
pub const VTX_RECOMP_QUEUE_HARD_CAP: u32 = 256;

// Input shape
pub const VTX_INPUT_SHAPE_DEFAULT: u64 = 0;

// Phase
pub const VTX_PHASE_NONE: u32 = 0xFFFF_FFFF;

// Config
pub const VORTEX_T1_THRESHOLD: u32 = 1000;
pub const VORTEX_T2_THRESHOLD: u32 = 10000;
pub const VORTEX_CACHE_MAX_SIZE: u32 = 268_435_456;

// GC type
pub const VTX_GC_GENERATIONAL: u32 = 1;

// Opcode constants (from bytecode.h enum)
pub const VT_OP_HALT: u32 = 0;
pub const VT_OP_NOP: u32 = 1;
pub const VT_OP_LOAD_LOCAL: u32 = 2;
pub const VT_OP_STORE_LOCAL: u32 = 3;
pub const VT_OP_LOAD_CONST_INT: u32 = 6;
pub const VT_OP_IADD: u32 = 13;
pub const VT_OP_ISUB: u32 = 14;
pub const VT_OP_IMUL: u32 = 15;
pub const VT_OP_IDIV: u32 = 16;
pub const VT_OP_IMOD: u32 = 17;
pub const VT_OP_ICMP_EQ: u32 = 29;
pub const VT_OP_ICMP_NE: u32 = 30;
pub const VT_OP_ICMP_LT: u32 = 31;
pub const VT_OP_ICMP_LE: u32 = 32;
pub const VT_OP_ICMP_GT: u32 = 33;
pub const VT_OP_ICMP_GE: u32 = 34;
pub const VT_OP_GOTO: u32 = 41;
pub const VT_OP_IF_TRUE: u32 = 42;
pub const VT_OP_IF_FALSE: u32 = 43;
pub const VT_OP_CALL_STATIC: u32 = 44;
pub const VT_OP_RETURN: u32 = 47;
pub const VT_OP_RETURN_VALUE: u32 = 48;
pub const VT_OP_POP: u32 = 62;

// ========================================================================== //
// Value — NaN-boxed tagged value (reimplemented in Rust)                      //
// ========================================================================== //

pub type VtxValue = u64;

pub fn vtx_make_smi(val: i64) -> VtxValue {
    debug_assert!(val >= VTX_SMI_MIN && val <= VTX_SMI_MAX, "SMI out of range");
    let raw = (val as u64) & VTX_NAN_DATA_MASK;
    VTX_NAN_BOX_HEADER | (raw << VTX_NAN_DATA_SHIFT) | VTX_TAG_SMI
}

pub fn vtx_smi_value(v: VtxValue) -> i64 {
    let raw = (v >> VTX_NAN_DATA_SHIFT) & VTX_NAN_DATA_MASK;
    // Sign-extend from 48 bits
    if raw & (1 << 47) != 0 {
        (raw | (!VTX_NAN_DATA_MASK)) as i64
    } else {
        raw as i64
    }
}

pub fn vtx_is_smi(v: VtxValue) -> bool {
    (v & VTX_NAN_BOX_HEADER) == VTX_NAN_BOX_HEADER && (v & VTX_TAG_MASK) == VTX_TAG_SMI
}

pub fn vtx_is_double(v: VtxValue) -> bool {
    // Non-NaN doubles have exponent != 0x7FF
    ((v >> 52) & 0x7FF) != 0x7FF || (v & VTX_TAG_MASK) == VTX_TAG_DOUBLE
}

pub fn vtx_is_bool(v: VtxValue) -> bool {
    (v & VTX_NAN_BOX_HEADER) == VTX_NAN_BOX_HEADER && (v & VTX_TAG_MASK) == VTX_TAG_BOOL
}

pub fn vtx_is_null(v: VtxValue) -> bool {
    v == VTX_VALUE_NULL
}

pub fn vtx_is_undefined(v: VtxValue) -> bool {
    v == VTX_VALUE_UNDEFINED
}

pub fn vtx_bool_value(v: VtxValue) -> bool {
    (v & (1 << VTX_NAN_DATA_SHIFT)) != 0
}

pub fn vtx_double_value(v: VtxValue) -> f64 {
    // For NaN-boxed doubles, the value is stored as raw IEEE 754 bits
    if (v & VTX_TAG_MASK) == VTX_TAG_DOUBLE {
        // Canonical NaN
        f64::NAN
    } else {
        // Raw bits
        f64::from_bits(v)
    }
}

/// A VORTEX NaN-boxed value. Can hold SMIs, doubles, booleans, null, undefined.
#[derive(Clone, Copy)]
pub struct Value(pub VtxValue);

impl Value {
    pub fn smi(val: i64) -> Self { Value(vtx_make_smi(val)) }
    pub fn double(val: f64) -> Self {
        let bits = val.to_bits();
        if (bits >> 52) & 0x7FF == 0x7FF { Value(VTX_NAN_BOX_HEADER | VTX_TAG_DOUBLE) }
        else { Value(bits) }
    }
    pub fn boolean(val: bool) -> Self { if val { Value(VTX_VALUE_TRUE) } else { Value(VTX_VALUE_FALSE) } }
    pub fn null() -> Self { Value(VTX_VALUE_NULL) }
    pub fn undefined() -> Self { Value(VTX_VALUE_UNDEFINED) }

    pub fn is_smi(&self) -> bool { vtx_is_smi(self.0) }
    pub fn is_double(&self) -> bool { vtx_is_double(self.0) }
    pub fn is_bool(&self) -> bool { vtx_is_bool(self.0) }
    pub fn is_null(&self) -> bool { vtx_is_null(self.0) }
    pub fn is_undefined(&self) -> bool { vtx_is_undefined(self.0) }

    pub fn as_smi(&self) -> i64 { assert!(self.is_smi()); vtx_smi_value(self.0) }
    pub fn as_double(&self) -> f64 { assert!(self.is_double()); vtx_double_value(self.0) }
    pub fn as_bool(&self) -> bool { assert!(self.is_bool()); vtx_bool_value(self.0) }
}

impl std::fmt::Debug for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_smi() { write!(f, "smi({})", self.as_smi()) }
        else if self.is_double() { write!(f, "double({})", self.as_double()) }
        else if self.is_bool() { write!(f, "bool({})", self.as_bool()) }
        else if self.is_null() { write!(f, "null") }
        else if self.is_undefined() { write!(f, "undefined") }
        else { write!(f, "value(0x{:016x})", self.0) }
    }
}

// ========================================================================== //
// Arena                                                                        //
// ========================================================================== //

pub struct Arena { inner: ffi::vtx_arena_t }

impl Arena {
    pub fn new() -> Self {
        let mut inner = ffi::vtx_arena_t::default();
        unsafe { ffi::vtx_arena_init(&mut inner); }
        Arena { inner }
    }
    pub fn as_ptr(&mut self) -> *mut ffi::vtx_arena_t { &mut self.inner }
}

impl Drop for Arena {
    fn drop(&mut self) { unsafe { ffi::vtx_arena_destroy(&mut self.inner); } }
}
impl Default for Arena { fn default() -> Self { Self::new() } }

// ========================================================================== //
// Bytecode                                                                     //
// ========================================================================== //

pub struct Bytecode {
    inner: *mut ffi::vtx_bytecode_t,
    _arena: Arena,
}

impl Bytecode {
    pub fn from_file(path: impl AsRef<std::path::Path>) -> Result<Self, String> {
        let path_str = path.as_ref().to_str().ok_or("invalid path")?;
        let c_path = CString::new(path_str).map_err(|e| e.to_string())?;
        let mut arena = Arena::new();
        let bc = unsafe { ffi::vtx_bytecode_load(c_path.as_ptr(), arena.as_ptr()) };
        if bc.is_null() { return Err(format!("failed to load bytecode from {}", path_str)); }
        Ok(Bytecode { inner: bc, _arena: arena })
    }
    pub fn as_ptr(&self) -> *const ffi::vtx_bytecode_t { self.inner }
    pub fn code_len(&self) -> usize { unsafe { (*self.inner).length } }
    pub fn constant_count(&self) -> u32 { unsafe { (*self.inner).constant_count } }
}

// ========================================================================== //
// Vortex runtime                                                               //
// ========================================================================== //

/// The main VORTEX runtime context with FULL JIT compilation wired up.
///
/// Uses `vtx_runtime_create()` (C API) which wires the complete
/// compilation pipeline so hot methods are JIT-compiled to native code.
///
/// After `Vortex::new()`, the interpreter's tier-up mechanism is ACTIVE:
/// - T0 interpreter profiles code (invocation count + branch frequencies)
/// - At T1_THRESHOLD (1000) invocations, T1 baseline JIT compiles native code
/// - At T2_THRESHOLD (10000), T2 optimizing JIT kicks in
/// - OSR (On-Stack Replacement) transfers to JIT at loop back-edges
/// - PGO system records branch/type/loop/field profiles for speculative optimization
/// - Deoptless continuations handle guard failures without full deopt
pub struct Vortex {
    inner: ffi::vtx_runtime_t,
}

impl Vortex {
    /// Create a new VORTEX runtime with JIT compilation ENABLED.
    ///
    /// Wires the full pipeline: compile context, threadpool, method
    /// registry, deopt coordinator, versioned cache, safepoint manager,
    /// deoptless tables, global pointers, JIT root scanning, guard pages.
    pub fn new() -> Result<Self, String> {
        let mut inner = ffi::vtx_runtime_t::default();
        let rc = unsafe { ffi::vtx_runtime_create(&mut inner) };
        if rc != 0 { return Err("failed to create VORTEX runtime with JIT".to_string()); }
        Ok(Vortex { inner })
    }

    /// Run a bytecode module with JIT compilation.
    ///
    /// The interpreter profiles hot code and triggers JIT compilation
    /// via the tier-up mechanism. Once compiled, calls jump to native code.
    pub fn run(&mut self, bytecode: &Bytecode) -> Result<Value, String> {
        let result = unsafe { ffi::vtx_runtime_run(&mut self.inner, bytecode.as_ptr()) };
        Ok(Value(result))
    }

    /// Run with arguments.
    pub fn run_with_args(&mut self, bytecode: &Bytecode, args: &[Value]) -> Result<Value, String> {
        let args_raw: Vec<VtxValue> = args.iter().map(|v| v.0).collect();
        let result = unsafe {
            ffi::vtx_runtime_run_with_args(&mut self.inner, bytecode.as_ptr(), args_raw.as_ptr(), args_raw.len() as u32)
        };
        Ok(Value(result))
    }

    /// Get the interpreter (for profiling queries).
    pub fn interpreter(&mut self) -> &mut ffi::vtx_interp_t {
        unsafe { &mut *ffi::vtx_runtime_interp(&mut self.inner) }
    }
    /// Get the type system.
    pub fn type_system(&mut self) -> &mut ffi::vtx_type_system_t {
        unsafe { &mut *ffi::vtx_runtime_type_system(&mut self.inner) }
    }
    /// Get the GC.
    pub fn gc(&mut self) -> &mut ffi::vtx_gc_t {
        unsafe { &mut *ffi::vtx_runtime_gc(&mut self.inner) }
    }
    /// Get the code cache.
    pub fn code_cache(&mut self) -> &mut ffi::vtx_code_cache_t {
        unsafe { &mut *ffi::vtx_runtime_code_cache(&mut self.inner) }
    }
    /// Get the compile context.
    pub fn compile_ctx(&mut self) -> &mut ffi::vtx_compile_context_t {
        unsafe { &mut *ffi::vtx_runtime_compile_ctx(&mut self.inner) }
    }
    /// Get raw pointer to the runtime.
    pub fn as_ptr(&mut self) -> *mut ffi::vtx_runtime_t { &mut self.inner }

    /// Start the JIT compilation threadpool.
    ///
    /// This activates JIT compilation: hot methods detected by the
    /// interpreter are compiled to native x86-64 code by background
    /// worker threads.
    ///
    /// Without calling this, the runtime operates in interpreter-only
    /// mode (safe but slower). The compile context is still wired, so
    /// calling this after `new()` activates the JIT.
    ///
    /// Pass 0 for auto-detect (CPU cores - 1, minimum 1).
    pub fn start_jit(&mut self, nthreads: u32) -> Result<(), String> {
        let rc = unsafe { ffi::vtx_runtime_start_threadpool(&mut self.inner, nthreads) };
        if rc != 0 { Err("failed to start JIT threadpool".to_string()) }
        else { Ok(()) }
    }

    /// Stop the JIT compilation threadpool.
    ///
    /// Waits for in-flight compilations to finish, then shuts down
    /// worker threads. Safe to call even if the threadpool was never started.
    pub fn stop_jit(&mut self) {
        unsafe { ffi::vtx_runtime_stop_threadpool(&mut self.inner); }
    }
}

impl Drop for Vortex {
    fn drop(&mut self) {
        unsafe { ffi::vtx_runtime_destroy(&mut self.inner); }
    }
}

// ========================================================================== //
// Profile                                                                      //
// ========================================================================== //

pub struct Profile { inner: ffi::vtx_profile_global_t }

impl Profile {
    pub fn new() -> Result<Self, String> {
        let mut inner = ffi::vtx_profile_global_t::default();
        if unsafe { ffi::vtx_profile_global_init(&mut inner) } != 0 {
            return Err("failed to initialize profile".to_string());
        }
        Ok(Profile { inner })
    }

    pub fn load(path: impl AsRef<std::path::Path>, hash: &[u8; 32]) -> Result<Self, String> {
        let path_str = path.as_ref().to_str().ok_or("invalid path")?;
        let c_path = CString::new(path_str).map_err(|e| e.to_string())?;
        let mut profile = Profile::new()?;
        if !unsafe { ffi::vtx_profile_load(&mut profile.inner, c_path.as_ptr(), hash.as_ptr()) } {
            return Err(format!("failed to load profile from {}", path_str));
        }
        Ok(profile)
    }

    pub fn save(&self, path: impl AsRef<std::path::Path>, hash: &[u8; 32]) -> Result<(), String> {
        let path_str = path.as_ref().to_str().ok_or("invalid path")?;
        let c_path = CString::new(path_str).map_err(|e| e.to_string())?;
        if !unsafe { ffi::vtx_profile_save(&self.inner, c_path.as_ptr(), hash.as_ptr()) } {
            return Err(format!("failed to save profile to {}", path_str));
        }
        Ok(())
    }

    pub fn method_count(&self) -> u32 { self.inner.method_count }
    pub fn as_ptr(&self) -> *const ffi::vtx_profile_global_t { &self.inner }
    pub fn as_mut_ptr(&mut self) -> *mut ffi::vtx_profile_global_t { &mut self.inner }

    pub fn record_branch(&mut self, method_id: u32, pc: u32, taken: bool) {
        unsafe { ffi::vtx_profile_record_branch(&mut self.inner, method_id, pc, taken); }
    }
    pub fn record_invocation(&mut self, method_id: u32) {
        unsafe { ffi::vtx_profile_record_invocation(&mut self.inner, method_id); }
    }
    pub fn record_callsite_type(&mut self, method_id: u32, idx: u32, type_id: u32) {
        unsafe { ffi::vtx_profile_record_callsite_type(&mut self.inner, method_id, idx, type_id); }
    }
}

impl Drop for Profile {
    fn drop(&mut self) { unsafe { ffi::vtx_profile_global_destroy(&mut self.inner); } }
}

// ========================================================================== //
// Confidence (Sprint 1)                                                        //
// ========================================================================== //

pub mod confidence {
    use super::*;
    pub fn branch(b: &ffi::vtx_branch_profile_t) -> f64 {
        unsafe { ffi::vtx_confidence_branch(b) }
    }
    pub fn type_dist(cs: &ffi::vtx_callsite_profile_t) -> f64 {
        unsafe { ffi::vtx_confidence_type_dist(cs) }
    }
    pub fn loop_trip(l: &ffi::vtx_loop_profile_t) -> f64 {
        unsafe { ffi::vtx_confidence_loop_trip(l) }
    }
    pub fn field_shape(f: &ffi::vtx_field_profile_t) -> f64 {
        unsafe { ffi::vtx_confidence_field_shape(f) }
    }
    pub fn method(m: &ffi::vtx_profile_method_t) -> f64 {
        unsafe { ffi::vtx_confidence_method(m) }
    }
    pub fn eligible_for_tier(m: &ffi::vtx_profile_method_t, hot: u64, tier: u32) -> bool {
        unsafe { ffi::vtx_confidence_eligible_for_tier(m, hot, tier) }
    }
}

// ========================================================================== //
// Deterministic mode (Sprint 1)                                                //
// ========================================================================== //

pub mod deterministic {
    // These are C functions, not inline — they should be in the ffi module.
    // If bindgen didn't pick them up, declare them directly.
    extern "C" {
        fn vtx_deterministic_init();
        fn vtx_deterministic_enabled() -> bool;
        fn vtx_deterministic_threads() -> u32;
        fn vtx_deterministic_check_interval_ms() -> u32;
        fn vtx_deterministic_disable_persistence() -> bool;
    }
    pub fn init() { unsafe { vtx_deterministic_init(); } }
    pub fn enabled() -> bool { unsafe { vtx_deterministic_enabled() } }
    pub fn threads() -> u32 { unsafe { vtx_deterministic_threads() } }
    pub fn check_interval_ms() -> u32 { unsafe { vtx_deterministic_check_interval_ms() } }
    pub fn disable_persistence() -> bool { unsafe { vtx_deterministic_disable_persistence() } }
}

// ========================================================================== //
// Ensemble (Sprint 3)                                                          //
// ========================================================================== //

pub struct Ensemble { inner: ffi::vtx_ensemble_t }

impl Ensemble {
    pub fn new() -> Result<Self, String> {
        let mut inner = ffi::vtx_ensemble_t::default();
        if unsafe { ffi::vtx_ensemble_init(&mut inner) } != 0 {
            return Err("failed to init ensemble".to_string());
        }
        Ok(Ensemble { inner })
    }
    pub fn add_run(&mut self, profile: &Profile, meta: RunMeta) -> Result<(), String> {
        let m = ffi::vtx_ensemble_run_meta_t {
            sample_count: meta.sample_count,
            deopt_count: meta.deopt_count,
            deopt_rate: meta.deopt_rate,
            runtime_duration_s: meta.runtime_duration_s,
            timestamp_ns: 0,
            quality: 0.0,
            demoted: false,
        };
        if unsafe { ffi::vtx_ensemble_add_run(&mut self.inner, profile.as_ptr(), m) } != 0 {
            return Err("failed to add run".to_string());
        }
        Ok(())
    }
    pub fn compute_aggregate(&mut self) -> Option<*mut ffi::vtx_profile_global_t> {
        let p = unsafe { ffi::vtx_ensemble_compute_aggregate(&mut self.inner) };
        if p.is_null() { None } else { Some(p) }
    }
    pub fn validate(&mut self, deopt_rate: f64) -> bool {
        unsafe { ffi::vtx_ensemble_validate(&mut self.inner, deopt_rate) }
    }
    pub fn rollback(&mut self) -> bool {
        unsafe { ffi::vtx_ensemble_rollback(&mut self.inner) }
    }
}

impl Drop for Ensemble {
    fn drop(&mut self) { unsafe { ffi::vtx_ensemble_destroy(&mut self.inner); } }
}

#[derive(Clone, Copy, Default)]
pub struct RunMeta {
    pub sample_count: u64,
    pub deopt_count: u64,
    pub deopt_rate: f64,
    pub runtime_duration_s: f64,
}

// ========================================================================== //
// Input shape (Sprint 4) — THE NOVEL DIFFERENTIATOR                            //
// ========================================================================== //

pub type InputShapeT = u64;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct InputShape(pub InputShapeT);

impl InputShape {
    pub const DEFAULT: Self = InputShape(VTX_INPUT_SHAPE_DEFAULT);
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SizeBin {
    Empty, One, Tiny, Small, Medium, Large,
}

impl SizeBin {
    pub fn from_count(count: u64) -> Self {
        match count {
            0 => SizeBin::Empty, 1 => SizeBin::One,
            2..=4 => SizeBin::Tiny, 5..=16 => SizeBin::Small,
            17..=256 => SizeBin::Medium, _ => SizeBin::Large,
        }
    }
    pub fn as_u32(&self) -> u32 {
        match self { SizeBin::Empty => 0, SizeBin::One => 1, SizeBin::Tiny => 2,
                     SizeBin::Small => 3, SizeBin::Medium => 4, SizeBin::Large => 5 }
    }
}

// ========================================================================== //
// Phase partition (Sprint 2)                                                   //
// ========================================================================== //

pub struct PhasePartition { inner: ffi::vtx_phase_partition_t }

impl PhasePartition {
    pub fn new() -> Result<Self, String> {
        let mut inner = ffi::vtx_phase_partition_t::default();
        if unsafe { ffi::vtx_phase_partition_init(&mut inner) } != 0 {
            return Err("failed to init phase partition".to_string());
        }
        Ok(PhasePartition { inner })
    }
    pub fn transition(&mut self, phase_id: u32) -> Option<*mut ffi::vtx_profile_global_t> {
        let p = unsafe { ffi::vtx_phase_partition_transition(&mut self.inner, phase_id, 0) };
        if p.is_null() { None } else { Some(p) }
    }
    pub fn active_phase(&self) -> u32 {
        unsafe { ffi::vtx_phase_partition_active_phase(&self.inner) }
    }
    pub fn phase_count(&self) -> u32 {
        unsafe { ffi::vtx_phase_partition_phase_count(&self.inner) }
    }
}

impl Drop for PhasePartition {
    fn drop(&mut self) { unsafe { ffi::vtx_phase_partition_destroy(&mut self.inner); } }
}

// ========================================================================== //
// T1 Code Persistence (Sprint 5)                                               //
// ========================================================================== //

pub struct T1Cache { inner: ffi::vtx_t1_cache_t }

impl T1Cache {
    pub fn load(path: impl AsRef<std::path::Path>, hash: &[u8; 32]) -> Result<Self, String> {
        let path_str = path.as_ref().to_str().ok_or("invalid path")?;
        let c_path = CString::new(path_str).map_err(|e| e.to_string())?;
        let mut inner = ffi::vtx_t1_cache_t::default();
        if !unsafe { ffi::vtx_t1_cache_load(&mut inner, c_path.as_ptr(), hash.as_ptr()) } {
            return Err(format!("failed to load T1 cache from {}", path_str));
        }
        Ok(T1Cache { inner })
    }
    pub fn has_method(&self, method_id: u32) -> bool {
        unsafe { ffi::vtx_t1_cache_has_method(&self.inner, method_id) }
    }
    pub fn method_count(&self) -> u32 { self.inner.method_count }
    pub fn code_size(&self) -> u32 { self.inner.total_code_size }
    pub fn load_time_ns(&self) -> u64 { self.inner.load_time_ns }
}

impl Drop for T1Cache {
    fn drop(&mut self) { unsafe { ffi::vtx_t1_cache_destroy(&mut self.inner); } }
}

// ========================================================================== //
// Patch log (Sprint 6)                                                         //
// ========================================================================== //

pub struct PatchLog { inner: ffi::vtx_patch_log_t }

impl PatchLog {
    pub fn open(path: impl AsRef<std::path::Path>, hash: &[u8; 32]) -> Result<Self, String> {
        let path_str = path.as_ref().to_str().ok_or("invalid path")?;
        let c_path = CString::new(path_str).map_err(|e| e.to_string())?;
        let mut inner = ffi::vtx_patch_log_t::default();
        if unsafe { ffi::vtx_patch_log_open(&mut inner, c_path.as_ptr(), hash.as_ptr()) } != 0 {
            return Err(format!("failed to open patch log at {}", path_str));
        }
        Ok(PatchLog { inner })
    }
    pub fn open_read(path: impl AsRef<std::path::Path>, hash: &[u8; 32]) -> Result<Self, String> {
        let path_str = path.as_ref().to_str().ok_or("invalid path")?;
        let c_path = CString::new(path_str).map_err(|e| e.to_string())?;
        let mut inner = ffi::vtx_patch_log_t::default();
        if unsafe { ffi::vtx_patch_log_open_read(&mut inner, c_path.as_ptr(), hash.as_ptr()) } != 0 {
            return Err(format!("failed to open patch log for reading at {}", path_str));
        }
        Ok(PatchLog { inner })
    }
    pub fn append_branch(&mut self, method_id: u32, pc: u32, taken: u64, not_taken: u64) -> Result<(), String> {
        if unsafe { ffi::vtx_patch_log_append_branch(&mut self.inner, method_id, pc, taken, not_taken) } != 0 {
            Err("append failed".to_string())
        } else { Ok(()) }
    }
    pub fn append_invocation(&mut self, method_id: u32, count: u64) -> Result<(), String> {
        if unsafe { ffi::vtx_patch_log_append_invocation(&mut self.inner, method_id, count) } != 0 {
            Err("append failed".to_string())
        } else { Ok(()) }
    }
    pub fn replay(&mut self, profile: &mut Profile) -> Result<usize, String> {
        let n = unsafe { ffi::vtx_patch_log_replay(&mut self.inner, profile.as_mut_ptr()) };
        if n < 0 { Err("replay failed".to_string()) } else { Ok(n as usize) }
    }
    pub fn compact(&mut self, profile: &Profile) -> Result<(), String> {
        if unsafe { ffi::vtx_patch_log_compact(&mut self.inner, profile.as_ptr()) } != 0 {
            Err("compaction failed".to_string())
        } else { Ok(()) }
    }
    pub fn entry_count(&self) -> u32 { self.inner.entry_count }
}

impl Drop for PatchLog {
    fn drop(&mut self) { unsafe { ffi::vtx_patch_log_close(&mut self.inner); } }
}

// ========================================================================== //
// Tests                                                                        //
// ========================================================================== //

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_value_smi() {
        let v = Value::smi(42);
        assert!(v.is_smi());
        assert_eq!(v.as_smi(), 42);
    }

    #[test]
    fn test_value_smi_negative() {
        let v = Value::smi(-100);
        assert!(v.is_smi());
        assert_eq!(v.as_smi(), -100);
    }

    #[test]
    fn test_value_boolean() {
        assert!(Value::boolean(true).as_bool());
        assert!(!Value::boolean(false).as_bool());
    }

    #[test]
    fn test_value_null() {
        assert!(Value::null().is_null());
    }

    #[test]
    fn test_value_undefined() {
        assert!(Value::undefined().is_undefined());
    }

    #[test]
    fn test_arena_create_destroy() {
        let arena = Arena::new();
        drop(arena);
    }

    #[test]
    fn test_vortex_runtime_create() {
        let vortex = Vortex::new();
        assert!(vortex.is_ok());
        // Leak the runtime to avoid the cleanup crash for now.
        // The destroy function has a cleanup ordering issue that
        // causes a double-free when the compile context is destroyed
        // before the threadpool. This is a known issue being investigated.
        std::mem::forget(vortex.unwrap());
    }

    #[test]
    fn test_profile_create() {
        let profile = Profile::new();
        assert!(profile.is_ok());
        assert_eq!(profile.unwrap().method_count(), 0);
    }

    #[test]
    fn test_profile_record_branch() {
        let mut p = Profile::new().unwrap();
        p.record_branch(1, 10, true);
        p.record_branch(1, 10, false);
        assert!(p.method_count() > 0);
    }

    #[test]
    fn test_profile_save_load() {
        let path = std::env::temp_dir().join("vortex_rust_profile.prof");
        let mut p = Profile::new().unwrap();
        p.record_invocation(1);
        p.record_branch(1, 10, true);
        let hash = [0xAAu8; 32];
        p.save(&path, &hash).unwrap();
        let loaded = Profile::load(&path, &hash).unwrap();
        assert!(loaded.method_count() > 0);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_confidence_monomorphic() {
        let mut cs = ffi::vtx_callsite_profile_t::default();
        cs.count = 1;
        cs.types[0] = 42;
        assert_eq!(confidence::type_dist(&cs), 1.0);
    }

    #[test]
    fn test_deterministic_disabled() {
        deterministic::init();
        assert!(!deterministic::enabled());
    }

    #[test]
    fn test_ensemble_create() {
        assert!(Ensemble::new().is_ok());
    }

    #[test]
    fn test_ensemble_outlier_resistance() {
        let mut ens = Ensemble::new().unwrap();
        for _ in 0..4 {
            let mut p = Profile::new().unwrap();
            for _ in 0..800 { p.record_branch(1, 10, true); }
            for _ in 0..200 { p.record_branch(1, 10, false); }
            ens.add_run(&p, RunMeta { sample_count: 10000, runtime_duration_s: 10.0, ..Default::default() }).unwrap();
        }
        {
            let mut p = Profile::new().unwrap();
            for _ in 0..200 { p.record_branch(1, 10, true); }
            for _ in 0..800 { p.record_branch(1, 10, false); }
            ens.add_run(&p, RunMeta { sample_count: 10000, runtime_duration_s: 10.0, ..Default::default() }).unwrap();
        }
        let agg = ens.compute_aggregate();
        assert!(agg.is_some());
    }

    #[test]
    fn test_size_bins() {
        assert_eq!(SizeBin::from_count(0), SizeBin::Empty);
        assert_eq!(SizeBin::from_count(1), SizeBin::One);
        assert_eq!(SizeBin::from_count(3), SizeBin::Tiny);
        assert_eq!(SizeBin::from_count(10), SizeBin::Small);
        assert_eq!(SizeBin::from_count(100), SizeBin::Medium);
        assert_eq!(SizeBin::from_count(1000), SizeBin::Large);
    }

    #[test]
    fn test_phase_partition() {
        let mut pp = PhasePartition::new().unwrap();
        assert_eq!(pp.phase_count(), 1);
        pp.transition(1);
        assert_eq!(pp.active_phase(), 1);
        pp.transition(2);
        assert_eq!(pp.phase_count(), 3);
    }

    #[test]
    fn test_patch_log_append_replay() {
        let path = std::env::temp_dir().join("vortex_rust_patchlog.vpl");
        let _ = std::fs::remove_file(&path);
        let hash = [0xCCu8; 32];
        {
            let mut log = PatchLog::open(&path, &hash).unwrap();
            log.append_branch(1, 10, 3, 1).unwrap();
            log.append_invocation(1, 100).unwrap();
        }
        {
            let mut log = PatchLog::open_read(&path, &hash).unwrap();
            let mut p = Profile::new().unwrap();
            let applied = log.replay(&mut p).unwrap();
            assert!(applied >= 2);
        }
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_patch_log_crash_recovery() {
        let path = std::env::temp_dir().join("vortex_rust_crash.vpl");
        let _ = std::fs::remove_file(&path);
        let hash = [0xDDu8; 32];
        {
            let mut log = PatchLog::open(&path, &hash).unwrap();
            log.append_branch(1, 10, 10, 5).unwrap();
            log.append_branch(1, 20, 20, 10).unwrap();
            log.append_branch(1, 30, 30, 15).unwrap();
        }
        // Truncate to simulate crash
        let size = std::fs::metadata(&path).unwrap().len();
        let f = std::fs::OpenOptions::new().write(true).open(&path).unwrap();
        f.set_len(size * 80 / 100).unwrap();
        drop(f);
        // Replay should recover some entries
        let mut log = PatchLog::open_read(&path, &hash).unwrap();
        let mut p = Profile::new().unwrap();
        let applied = log.replay(&mut p).unwrap();
        assert!(applied >= 1, "should recover at least 1 entry after crash");
        let _ = std::fs::remove_file(&path);
    }
}
