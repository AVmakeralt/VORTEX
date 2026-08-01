//! # VORTEX JIT — Rust Bindings
//!
//! Safe Rust bindings for the VORTEX JIT compiler.
//!
//! ## Quick Start
//!
//! ```no_run
//! // See examples/ for full usage
//! ```

mod ffi {
    #![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code, unused_imports)]
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::ptr;

/// A VORTEX runtime instance. Bundles the type system, GC, interpreter,
/// code cache, and compilation threadpool.
pub struct Runtime {
    inner: ffi::vtx_runtime_t,
}

/// A bytecode module loaded from a file or constructed in memory.
pub struct Bytecode {
    inner: *mut ffi::vtx_bytecode_t,
}

/// A VORTEX value (NaN-boxed: SMI, double, heap pointer, null, bool, undefined).
pub type Value = u64;

impl Runtime {
    /// Create a new runtime with default settings.
    pub fn new() -> Result<Self, String> {
        let mut inner: ffi::vtx_runtime_t = unsafe { std::mem::zeroed() };
        let rc = unsafe { ffi::vtx_runtime_create(&mut inner) };
        if rc != 0 {
            return Err("failed to create runtime".to_string());
        }
        Ok(Runtime { inner })
    }

    /// Enable the JIT: starts background compilation threads.
    /// The interpreter will tier-up hot methods to T1 baseline, then T2 optimizing.
    ///
    /// `nthreads` = number of compilation threads (0 = auto-detect).
    pub fn enable_jit(&mut self, nthreads: u32) {
        unsafe { ffi::vtx_runtime_enable_jit(&mut self.inner, nthreads); }
    }

    /// Eagerly compile a method at T1 (baseline JIT).
    /// After this, `run` dispatches to native code.
    pub fn compile_t1(&mut self, method: &mut ffi::vtx_method_desc_t) -> Result<(), String> {
        let rc = unsafe { ffi::vtx_runtime_compile(&mut self.inner, method, 1) };
        if rc != 0 { Err("T1 compilation failed".to_string()) } else { Ok(()) }
    }

    /// Eagerly compile a method at T2 (optimizing JIT with full SoN IR pipeline).
    /// Falls back to T1 if T2 can't handle the method (e.g. float ops).
    pub fn compile_t2(&mut self, method: &mut ffi::vtx_method_desc_t) -> Result<(), String> {
        let rc = unsafe { ffi::vtx_runtime_compile(&mut self.inner, method, 2) };
        if rc != 0 { Err("T2 compilation failed".to_string()) } else { Ok(()) }
    }

    /// Run a bytecode method through the runtime.
    /// If the method has been compiled (via `compile_t1/t2` or tier-up),
    /// the interpreter dispatches to the JIT-compiled native code.
    pub fn run(&mut self, bc: &Bytecode) -> Value {
        unsafe { ffi::vtx_runtime_run(&mut self.inner, bc.inner) }
    }

    /// Run with arguments.
    pub fn run_with_args(&mut self, bc: &Bytecode, args: &[Value]) -> Value {
        unsafe {
            ffi::vtx_runtime_run_with_args(
                &mut self.inner, bc.inner,
                args.as_ptr(), args.len() as u32,
            )
        }
    }

    /// Get a raw pointer to the internal runtime (for advanced FFI).
    pub fn as_ptr(&mut self) -> *mut ffi::vtx_runtime_t { &mut self.inner }

    /// Load a .vtbc bytecode file from disk.
    pub fn load_bytecode(path: &str) -> Result<Bytecode, String> {
        let c_path = CString::new(path).map_err(|e| e.to_string())?;
        let bc = unsafe { ffi::vtx_bytecode_load(c_path.as_ptr()) };
        if bc.is_null() {
            Err("failed to load bytecode".to_string())
        } else {
            Ok(Bytecode { inner: bc })
        }
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        unsafe { ffi::vtx_runtime_destroy(&mut self.inner); }
    }
}

impl Drop for Bytecode {
    fn drop(&mut self) {
        if !self.inner.is_null() {
            unsafe {
                let bc = &mut *self.inner;
                if !bc.code.is_null() { libc::free(bc.code as *mut c_void); }
                if !bc.constant_pool.is_null() { libc::free(bc.constant_pool as *mut c_void); }
                libc::free(self.inner as *mut c_void);
            }
        }
    }
}

/// Helper: create an SMI (small integer) value.
pub fn make_smi(val: i64) -> Value {
    // VTX_NAN_BOX_HEADER = 0x7FF8000000000000, VTX_TAG_SMI = 0
    // SMI(val) = HEADER | ((val & DATA_MASK) << 3)
    const HEADER: u64 = 0x7FF8_0000_0000_0000;
    const DATA_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;
    HEADER | (((val as u64) & DATA_MASK) << 3)
}

/// Helper: extract an SMI value.
pub fn smi_value(val: Value) -> i64 {
    // SHL 13 + SAR 16 to extract the signed integer from NaN-boxed SMI
    ((val << 13) as i64) >> 16
}

/// Helper: check if a value is an SMI.
pub fn is_smi(val: Value) -> bool {
    // NaN-boxed with tag 0 (SMI tag)
    const HEADER: u64 = 0x7FF8_0000_0000_0000;
    const TAG_MASK: u64 = 0x0007;
    (val & HEADER) == HEADER && (val & TAG_MASK) == 0
}

/// Helper: create a double value.
pub fn make_double(val: f64) -> Value {
    let bits = val.to_bits();
    // Check for NaN
    if ((bits >> 52) & 0x7FF) == 0x7FF && (bits & 0x000F_FFFF_FFFF_FFFF) != 0 {
        // Canonical NaN: VTX_NAN_BOX_HEADER | VTX_TAG_DOUBLE (2)
        return 0x7FF8_0000_0000_0002;
    }
    bits
}

/// Helper: extract a double value.
pub fn double_value(val: Value) -> f64 {
    const HEADER: u64 = 0x7FF8_0000_0000_0000;
    if (val & HEADER) != HEADER {
        // Raw non-NaN double — bits are the double
        return f64::from_bits(val);
    }
    // NaN-boxed: return canonical quiet NaN
    f64::from_bits(0x7FF8_0000_0000_0000)
}

/// Helper: check if a value is a double.
pub fn is_double(val: Value) -> bool {
    const HEADER: u64 = 0x7FF8_0000_0000_0000;
    const TAG_MASK: u64 = 0x0007;
    const TAG_DOUBLE: u64 = 2;
    if (val & HEADER) != HEADER {
        return true; // raw non-NaN double
    }
    (val & TAG_MASK) == TAG_DOUBLE
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_smi() {
        let v = make_smi(42);
        assert!(is_smi(v));
        assert_eq!(smi_value(v), 42);
    }

    #[test]
    fn test_double() {
        let v = make_double(3.14);
        assert!(is_double(v));
        assert!((double_value(v) - 3.14).abs() < 1e-10);
    }

    #[test]
    fn test_runtime_create() {
        let rt = Runtime::new();
        assert!(rt.is_ok());
    }
}
