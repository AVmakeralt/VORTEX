// Pre-generated VORTEX FFI bindings stub.
// This file provides minimal type declarations so the crate compiles
// without libclang. For full bindings, enable the 'runtime-bindgen'
// feature (requires libclang installed).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code)]

pub type vtx_value_t = u64;
pub type vtx_typeid_t = u32;
pub type vtx_nodeid_t = u32;
pub type vtx_cond_t = i32;

// Opaque struct types
#[repr(C)]
pub struct vtx_arena_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_type_system_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_gc_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_interp_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_graph_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_code_cache_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_method_registry_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_bytecode_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_method_desc_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_compile_result_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_pipeline_config_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_assembler_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_node_table_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_node_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_schedule_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_inst_stream_t { _opaque: [u8; 0] }
#[repr(C)]
pub struct vtx_regalloc_result_t { _opaque: [u8; 0] }
